#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/clock.h>
#include <nuttx/semaphore.h>

#include <arch/board/board.h>

#include "robot_audio_playback.h"
#include "robot_music_buffer_policy.h"
#include "robot_voice_config.h"
#include "robot_voice_diag.h"
#include "robot_voice_threads.h"

/* The OLED is optional at runtime. Keep the audio path linkable in builds
 * that omit the contest display, while still exposing music rebuffer state
 * when the display is present. */
extern int robot_oled_set_loading(bool loading) __attribute__((weak));

/*
 * Playback format mirrors the original board codec:
 *
 *   source: PCM16LE / 24000 Hz / mono
 *   wire:   24000 Hz / 32 bit / mono-left (two physical I2S slots)
 *
 * No resampling and no time-domain sample repetition are performed.
 * Each mono source sample is left-aligned into one 32-bit mono I2S word:
 *
 *   source: A B C ...
 *   wire:   (A<<16) (B<<16) (C<<16) ...
 *
 * The TX lower-half selects the left slot and maintains the second physical
 * 32-bit slot required for the 64-BCLK LRCK frame.
 */
#define RV_TTS_SAMPLE_RATE          24000
#define RV_WIRE_SAMPLE_RATE         24000
#define RV_TTS_SOURCE_CHANNELS      1
#define RV_TTS_BITS                 16
#define RV_WIRE_CHANNELS            1
#define RV_WIRE_BITS                32
#define RV_WIRE_BYTES_PER_SAMPLE    (RV_WIRE_BITS / 8)
/* User-selected output volume 80 with the original quadratic volume curve:
 * (80 / 100)^2 * 65536 = 41943. */
#define RV_OUTPUT_GAIN_Q16          41943

#define RV_SOURCE_BLOCK_BYTES       1024
#define RV_WIRE_BLOCK_BYTES \
  ((unsigned int)((RV_SOURCE_BLOCK_BYTES / sizeof(int16_t)) * RV_WIRE_BYTES_PER_SAMPLE))
/*
 * Must match contest_i2s.c:
 *   CONTEST_I2S_TX_RING_SLOTS = 16
 *   CONTEST_I2S_TX_SLOT_BYTES = 2048
 *
 * Sixteen is the maximum. START negotiates 16/8/4 slots according to actual
 * internal memory; GETBUFFERINFO reports the selected count. Every priming,
 * wrap and drain operation must use that same count.
 */
#define RV_LOWER_DMA_RING_SLOTS      16
#define RV_LOWER_DMA_SLOT_BYTES      2048
#define RV_MAX_INFLIGHT              RV_LOWER_DMA_RING_SLOTS
#define RV_SEND_TIMEOUT_MS           1000
#if ROBOT_VOICE_TTS_PREBUFFER < (RV_MAX_INFLIGHT + 1) * RV_SOURCE_BLOCK_BYTES || ROBOT_VOICE_TTS_PREBUFFER > ROBOT_VOICE_TTS_STREAM_RING_BYTES
#  error Live voice prebuffer must prime DMA and fit its bounded ring
#endif

/* A released cyclic DMA slot must be refilled immediately. Waiting for the
 * producer here allows old payloads to circulate. Voice instead waits for
 * complete PCM before priming; streaming music retains its existing policy. */
#define RV_REFILL_WINDOW_MS(pb) \
  (((pb)->slot_count - 1) * RV_SOURCE_BLOCK_BYTES * 1000 / \
   (RV_TTS_SAMPLE_RATE * sizeof(int16_t)))

/*
 * Diagnostic switches.  Keep both at zero for normal TTS playback.
 *
 * RV_FULL_CACHE_AB_TEST=1
 *   Do not start I2S until the complete WAV PCM has reached EOS.  This keeps
 *   the exact same PCM -> DMA path but removes producer/consumer overlap.
 *
 * RV_I2S_OUTPUT_TONE_TEST=1
 *   Keep the same ring, worker, DMA blocks and I2S configuration, but replace
 *   every source sample at the final PCM -> wire boundary with a 1 kHz tone.
 *   A clean tone proves that the echo is upstream of I2S; a doubled/rough
 *   tone proves that it is in the DMA/I2S/codec path.
 *
 * Enable one switch at a time.  Neither switch is a production setting.
 */
#define RV_FULL_CACHE_AB_TEST        0
#define RV_I2S_OUTPUT_TONE_TEST      0
#define RV_I2S_OUTPUT_TONE_PEAK       8192

/*
 * Ring-buffer values from tts_pcm_playback_guide.md:
 *
 *   Music Ring      = 2 MiB (PSRAM jitter cushion)
 *   Music Prebuffer = 512 KiB; voice SSE prebuffer = 24 KiB
 *   Rebuffer        =  48 KiB (legacy threshold; F7I does not rebuffer once PLAYING)
 *
 * The worker waits in PREBUFFER while PCM arrives. Cached voice waits for
 * EOS; live voice starts at its prebuffer and retains a real final block.
 *
 * Legacy streaming-music invariant for the immutable contest I2S DMA ring:
 * once PLAYING has started, the worker must keep every released DMA slot
 * refreshed.  If the producer temporarily runs dry, submit silence instead
 * of entering REBUFFER and leaving old slot payloads circulating in hardware.
 */
#if RV_FULL_CACHE_AB_TEST
#  define RV_RING_BYTES             (2 * 1024 * 1024)
#  define RV_START_PREBUFFER_BYTES  RV_RING_BYTES
#  define RV_MUSIC_RING_BYTES       RV_RING_BYTES
#  define RV_MUSIC_START_PREBUFFER_BYTES RV_RING_BYTES
#else
#  define RV_RING_BYTES             (512 * 1024)
#  define RV_START_PREBUFFER_BYTES  (128 * 1024)
#  define RV_MUSIC_RING_BYTES       (4 * 1024 * 1024)
#  define RV_MUSIC_START_PREBUFFER_BYTES (512 * 1024)
#endif
#define RV_REBUFFER_BYTES           (48 * 1024)
/* Native Media's URL input is bursty. Keep the large music ring for jitter
 * headroom and wait for a useful amount of PCM before enabling I2S. One DMA
 * ring (4 KiB) is only about 85 ms at 24 kHz mono and starts an immediate
 * underrun when the redirected URL is still filling its first TCP buffers. */
#define RV_NATIVE_MEDIA_RING_BYTES  (512 * 1024)
#define RV_NATIVE_MEDIA_PREBUFFER_BYTES (32 * 1024)

/* The proven version used priority 120.  Keep that real-time bias.  Use a
 * larger stack than the old 4 KiB implementation because this contest-local
 * wrapper has additional logging/call depth, while the DMA cadence remains
 * identical to the proven code.
 */
#define RV_WORKER_STACK             (16 * 1024)
#define RV_WORKER_PRIORITY          ROBOT_VOICE_PLAY_PRIORITY

static unsigned long long rv_audio_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum rv_play_state_e
{
  RV_STATE_PREBUFFER = 0,
  RV_STATE_PLAYING,
  RV_STATE_REBUFFER,
  RV_STATE_DRAINING,
  RV_STATE_STOPPED
};

struct rv_ring_s
{
  uint8_t *data;
  size_t head;
  size_t tail;
  size_t count;
  size_t capacity;
};

struct rv_playback_s;

struct rv_slot_s
{
  struct rv_playback_s *pb;
  struct ap_buffer_s *apb;
  sem_t done;
  int result;
  int inflight;
  unsigned int index;
  uint32_t submit_count;
  uint32_t done_count;
  unsigned long long done_ms;
  size_t source_bytes;
};

struct rv_playback_s
{
  struct i2s_dev_s *i2s;

  sem_t i2s_done;
  sem_t ring_lock;
  sem_t ring_data;
  sem_t ring_space;

  pthread_t worker;
  int worker_started;
  int worker_exited;

  volatile int i2s_inflight;
  int i2s_result;
  int i2s_started;

  volatile int stopped;
  int eos;
  int stop_on_short_underrun;
  int terminal_tail_done;
  int eos_silence_rounds;
  enum rv_play_state_e state;
  unsigned int start_prebuffer;
  uint32_t underrun_count;
  size_t ring_min_play;
  size_t ring_max_play;

  struct rv_ring_s ring;
  unsigned int slot_count;
  struct rv_slot_s slots[RV_MAX_INFLIGHT];

  size_t total_written;
  size_t total_read;
  uint32_t input_hash, output_hash;
  unsigned int id;
  int cache_until_eos;
  int voice_live;
  bool native_media;
  bool media_started;
  bool media_paused;
  bool music_buffering;
  unsigned int music_rebuffers;
  uint64_t completed_source_bytes;
  unsigned long long open_ms, first_pcm_ms, last_pcm_ms, max_pcm_gap_ms;
  unsigned long long dma_start_ms, dma_done_ms;
  unsigned long long last_diag_ms;
  unsigned long long commit_ms, user_end_ms, tts_start_ms, tts_ready_ms;
  unsigned long run_id;
  unsigned int sentence;
  unsigned long long max_refill_ms, max_send_ms, last_callback_ms, max_callback_gap_ms;
  uint32_t late_refills, callback_errors, silence_blocks, tail_pad_bytes;
  uint32_t gap_run, max_gap_run;

  uint8_t diagnostic_tone_phase;
  int opened;
  int finished;
};

static struct rv_playback_s g_pb;
static int g_stop_on_short_underrun;
static volatile bool g_media_voice_mode;
static unsigned int g_playback_id;

static void rv_signal_all(struct rv_playback_s *pb);

static const char *rv_play_state_name(enum rv_play_state_e state)
{
  switch (state)
    {
      case RV_STATE_PREBUFFER:
        return "prebuffer";
      case RV_STATE_PLAYING:
        return "playing";
      case RV_STATE_REBUFFER:
        return "rebuffer";
      case RV_STATE_DRAINING:
        return "draining";
      case RV_STATE_STOPPED:
        return "stopped";
      default:
        return "unknown";
    }
}

static void rv_report_playback_diag(struct rv_playback_s *pb,
                                    size_t available,
                                    unsigned int slot_index,
                                    int primed_slots)
{
  unsigned long long now;

  if (pb == NULL)
    {
      return;
    }

  now = rv_audio_now_ms();
  if (pb->last_diag_ms != 0 && now - pb->last_diag_ms < 1000)
    {
      return;
    }

  pb->last_diag_ms = now;
  printf("[RV-PB-DIAG] id=%u state=%s buffered=%zu/%zu written=%zu "
         "read=%zu underruns=%lu silence=%lu inflight=%d slots=%u "
         "primed=%d next=%u pcm_gap_max_ms=%llu dma_gap_max_ms=%llu\n",
         pb->id, rv_play_state_name(pb->state), available,
         pb->ring.capacity, pb->total_written, pb->total_read,
         (unsigned long)pb->underrun_count,
         (unsigned long)pb->silence_blocks, pb->i2s_inflight,
         pb->slot_count, primed_slots, slot_index, pb->max_pcm_gap_ms,
         pb->max_callback_gap_ms);
}

static int rv_lock(struct rv_playback_s *pb)
{
  return nxsem_wait_uninterruptible(&pb->ring_lock);
}

static void rv_unlock(struct rv_playback_s *pb)
{
  nxsem_post(&pb->ring_lock);
}

static size_t rv_ring_count(struct rv_playback_s *pb)
{
  size_t count;

  if (rv_lock(pb) < 0)
    {
      return pb->ring.count;
    }

  count = pb->ring.count;
  rv_unlock(pb);
  return count;
}

static int rv_get_inflight(struct rv_playback_s *pb)
{
  int value;

  if (rv_lock(pb) < 0)
    {
      return pb->i2s_inflight;
    }

  value = pb->i2s_inflight;
  rv_unlock(pb);
  return value;
}

static int rv_get_result(struct rv_playback_s *pb)
{
  int value;

  if (rv_lock(pb) < 0)
    {
      return pb->i2s_result;
    }

  value = pb->i2s_result;
  rv_unlock(pb);
  return value;
}

/* The task owner can cancel while the playback worker reaches EOS. Reuse
 * i2s_started's existing word: -1 marks STOP in progress, avoiding duplicate
 * driver STOP calls or reading a checkpoint before its callbacks finish. */
static void rv_stop_i2s(struct rv_playback_s *pb)
{
  int started = 1;
  if (pb->i2s == NULL) return;
  if (__atomic_compare_exchange_n(&pb->i2s_started, &started, -1, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
      I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
      __atomic_store_n(&pb->i2s_started, 0, __ATOMIC_RELEASE);
    }
  else
    {
      while (__atomic_load_n(&pb->i2s_started, __ATOMIC_ACQUIRE) == -1)
        usleep(100);
    }
}

static int rv_ring_write(struct rv_playback_s *pb,
                         const uint8_t *src, size_t len)
{
  size_t first;
  size_t second;
  size_t space;
  int ret;

  if (pb == NULL || src == NULL || len == 0)
    {
      return -EINVAL;
    }

  if (pb->stopped)
    {
      return -ECANCELED;
    }

  ret = rv_lock(pb);
  if (ret < 0)
    {
      return ret;
    }

  if (pb->eos)
    {
      rv_unlock(pb);
      return -EPIPE;
    }

  space = pb->ring.capacity - pb->ring.count;
  if (len > space)
    {
      size_t buffered = pb->ring.count;
      rv_unlock(pb);

      RV_VERBOSE("[RV-PB] PCM cache full buffered=%zu incoming=%zu capacity=%u\n",
             buffered, len, (unsigned int)pb->ring.capacity);
      return -ENOSPC;
    }

  first = pb->ring.capacity - pb->ring.head;
  if (first > len)
    {
      first = len;
    }

  memcpy(pb->ring.data + pb->ring.head, src, first);
  pb->ring.head = (pb->ring.head + first) % pb->ring.capacity;
  pb->ring.count += first;

  second = len - first;
  if (second > 0)
    {
      memcpy(pb->ring.data + pb->ring.head, src + first, second);
      pb->ring.head = (pb->ring.head + second) % pb->ring.capacity;
      pb->ring.count += second;
    }

  if (pb->ring.count > pb->ring_max_play)
    {
      pb->ring_max_play = pb->ring.count;
    }

  {
    unsigned long long now = rv_audio_now_ms();
    if (!pb->first_pcm_ms) pb->first_pcm_ms = now;
    if (pb->last_pcm_ms && now - pb->last_pcm_ms > pb->max_pcm_gap_ms)
      pb->max_pcm_gap_ms = now - pb->last_pcm_ms;
    pb->last_pcm_ms = now;
    pb->input_hash = rv_pcm_hash(pb->input_hash, src, len);
    pb->total_written += len;
  }
  rv_unlock(pb);
  nxsem_post(&pb->ring_data);
  return 0;
}

static size_t rv_ring_read(struct rv_playback_s *pb,
                           uint8_t *dst, size_t wanted)
{
  size_t n;
  size_t first;

  if (rv_lock(pb) < 0)
    {
      return 0;
    }

  n = pb->ring.count;
  if (n > wanted)
    {
      n = wanted;
    }

  first = pb->ring.capacity - pb->ring.tail;
  if (first > n)
    {
      first = n;
    }

  if (first > 0)
    {
      memcpy(dst, pb->ring.data + pb->ring.tail, first);
      pb->ring.tail = (pb->ring.tail + first) % pb->ring.capacity;
      pb->ring.count -= first;
    }

  if (first < n)
    {
      size_t second = n - first;

      memcpy(dst + first, pb->ring.data + pb->ring.tail, second);
      pb->ring.tail = (pb->ring.tail + second) % pb->ring.capacity;
      pb->ring.count -= second;
    }

  if ((pb->state == RV_STATE_PLAYING || pb->state == RV_STATE_DRAINING) &&
      pb->ring.count < pb->ring_min_play)
    {
      pb->ring_min_play = pb->ring.count;
    }

  if (n > 0)
    {
      pb->output_hash = rv_pcm_hash(pb->output_hash, dst, n);
      pb->total_read += n;
    }
  rv_unlock(pb);

  if (n > 0)
    {
      nxsem_post(&pb->ring_space);
    }

  return n;
}

static void rv_i2s_done(struct i2s_dev_s *dev,
                        struct ap_buffer_s *apb,
                        void *arg, int result)
{
  struct rv_slot_s *slot = arg;
  struct rv_playback_s *pb;

  (void)dev;

  if (slot == NULL || slot->pb == NULL)
    {
      return;
    }

  pb = slot->pb;

  if (rv_lock(pb) == 0)
    {
      if (!slot->inflight || (apb && apb != slot->apb))
        {
          pb->callback_errors++;
          if (pb->i2s_result >= 0) pb->i2s_result = -EPROTO;
          rv_unlock(pb);
          nxsem_post(&slot->done);
          nxsem_post(&pb->i2s_done);
          return;
        }
      slot->done_ms = rv_audio_now_ms();
      if (pb->last_callback_ms &&
          slot->done_ms - pb->last_callback_ms > pb->max_callback_gap_ms)
        pb->max_callback_gap_ms = slot->done_ms - pb->last_callback_ms;
      pb->last_callback_ms = slot->done_ms;
      slot->result = result;
      if (result == 0) pb->completed_source_bytes += slot->source_bytes;
      slot->source_bytes = 0;
      slot->inflight = 0;
      slot->done_count++;

      if (result < 0 && result != -ECANCELED && pb->i2s_result >= 0)
        {
          pb->i2s_result = result;
        }

      if (pb->i2s_inflight > 0)
        {
          pb->i2s_inflight--;
        }

      rv_unlock(pb);
    }
  else
    {
      slot->result = result;
      if (result == 0) pb->completed_source_bytes += slot->source_bytes;
      slot->source_bytes = 0;
      slot->inflight = 0;
      slot->done_count++;

      if (result < 0 && result != -ECANCELED && pb->i2s_result >= 0)
        {
          pb->i2s_result = result;
        }

      if (pb->i2s_inflight > 0)
        {
          pb->i2s_inflight--;
        }
    }

  nxsem_post(&slot->done);
  nxsem_post(&pb->i2s_done);
}

static int rv_wait_slot(struct rv_playback_s *pb,
                        struct rv_slot_s *slot)
{
  int ret;

  if (pb == NULL || slot == NULL)
    {
      return -EINVAL;
    }

  for (;;)
    {
      int inflight;
      int result;

      ret = rv_lock(pb);
      if (ret < 0)
        {
          return ret;
        }

      inflight = slot->inflight;
      result = slot->result;
      rv_unlock(pb);

      if (!inflight)
        {
          if (result < 0 && result != -ECANCELED)
            {
              return result;
            }
          return 0;
        }

      if (pb->stopped)
        {
          return -ECANCELED;
        }

      ret = nxsem_wait_uninterruptible(&slot->done);
      if (ret < 0)
        {
          return ret;
        }
    }
}

static int rv_queue_slot(struct rv_playback_s *pb,
                         struct rv_slot_s *slot,
                         const uint8_t *src, size_t len,
                         bool final_block, size_t source_bytes)
{
  struct audio_buf_desc_s desc;
  struct ap_buffer_s *apb;
  uint8_t *dst;
  size_t frames;
  size_t tx_len;
  size_t i;
  int ret;

  if (pb == NULL || slot == NULL || src == NULL || len == 0 ||
      len > RV_SOURCE_BLOCK_BYTES)
    {
      return -EINVAL;
    }

  len &= ~(size_t)1;
  if (len == 0)
    {
      return -EINVAL;
    }

  frames = len / sizeof(int16_t);
  tx_len = frames * RV_WIRE_BYTES_PER_SAMPLE;
  if (tx_len == 0 || tx_len > RV_WIRE_BLOCK_BYTES)
    {
      return -EINVAL;
    }

  /*
   * Normal blocks keep the proven 2048-byte wire contract.
   *
   * F7P exception: Media may submit one terminal short tail.  Its APB nbytes
   * is the exact converted wire length (for example 512 source bytes become
   * 1024 wire bytes).  contest_i2s shortens that descriptor and terminates
   * the cyclic ring after the real final samples.
   */
  if (RV_WIRE_BLOCK_BYTES != RV_LOWER_DMA_SLOT_BYTES)
    {
      return -EINVAL;
    }

  ret = rv_wait_slot(pb, slot);
  if (ret < 0)
    {
      return ret;
    }

  if (pb->stopped) return -ECANCELED;

  /* One stable APB per upper playback slot.  The lower half copies every
   * submitted block into its private immutable DMA ring payload. */
  if (slot->apb == NULL)
    {
      memset(&desc, 0, sizeof(desc));
      desc.numbytes = RV_WIRE_BLOCK_BYTES;
      desc.u.pbuffer = &slot->apb;

      ret = apb_alloc(&desc);
      if (ret < 0 || slot->apb == NULL)
        {
          slot->apb = NULL;
          return ret < 0 ? ret : -ENOMEM;
        }

      RV_VERBOSE("[RV-PB] fixed APB slot=%u ptr=%p bytes=%u\n",
             slot->index, slot->apb->samp, RV_WIRE_BLOCK_BYTES);
    }

  apb = slot->apb;

  /*
   * Legacy/TTS short blocks still use zero padding.  Media terminal tails
   * do not pad beyond tx_len: the lower-half descriptor itself is shortened.
   */
  if (tx_len < RV_WIRE_BLOCK_BYTES &&
      !(pb->stop_on_short_underrun &&
        ((!pb->voice_live && !pb->native_media) || final_block)))
    {
      memset(apb->samp, 0, RV_WIRE_BLOCK_BYTES);
    }

  dst = apb->samp;
  for (i = 0; i < frames; i++)
    {
      /* Match NoAudioCodec::Write() from the original firmware exactly:
       * PCM16 is multiplied by its Q16 volume factor, producing a
       * left-aligned signed 32-bit word.
       * The I2S lower half selects the left slot in mono mode. */

#if RV_I2S_OUTPUT_TONE_TEST
      /* 24 samples/cycle at 24 kHz = exactly 1 kHz.  Do not use floating
       * point in the playback worker. */
      static const int16_t tone_1khz[24] =
      {
        0, 2120, 4096, 5793, 7094, 7913,
        8192, 7913, 7094, 5793, 4096, 2120,
        0, -2120, -4096, -5793, -7094, -7913,
        -8192, -7913, -7094, -5793, -4096, -2120
      };
      int32_t sample = (int32_t)tone_1khz[pb->diagnostic_tone_phase] *
                       RV_OUTPUT_GAIN_Q16;

      pb->diagnostic_tone_phase++;
      if (pb->diagnostic_tone_phase == 24)
        {
          pb->diagnostic_tone_phase = 0;
        }

      dst[i * 4]     = (uint8_t)(sample & 0xff);
      dst[i * 4 + 1] = (uint8_t)((uint32_t)sample >> 8);
      dst[i * 4 + 2] = (uint8_t)((uint32_t)sample >> 16);
      dst[i * 4 + 3] = (uint8_t)((uint32_t)sample >> 24);
#else
      int16_t pcm16 = (int16_t)((uint16_t)src[i * 2] |
                                ((uint16_t)src[i * 2 + 1] << 8));

      /*
       * Match the clean reference NoAudioCodec::Write() path exactly:
       *
       *   int32_sample = pcm16 * q16_volume_factor
       *
       * Keep ALL 32 result bits.  The previous code rounded then cleared the
       * low 16 bits, effectively quantizing the signal back to 16-bit
       * precision after volume scaling.  That creates avoidable low-level
       * quantization steps in speech tails and quiet consonants.
       *
       * With RV_OUTPUT_GAIN_Q16 <= 65536, int16 * Q16 fits in int32_t.
       */
      int32_t sample = (int32_t)pcm16 * RV_OUTPUT_GAIN_Q16;

      dst[i * 4]     = (uint8_t)(sample & 0xff);
      dst[i * 4 + 1] = (uint8_t)((uint32_t)sample >> 8);
      dst[i * 4 + 2] = (uint8_t)((uint32_t)sample >> 16);
      dst[i * 4 + 3] = (uint8_t)((uint32_t)sample >> 24);

#endif
    }




  apb->curbyte = 0;
  apb->flags &= ~AUDIO_APB_FINAL;
  if (final_block)
    {
      apb->flags |= AUDIO_APB_FINAL;
    }
  if (pb->stop_on_short_underrun && tx_len < RV_WIRE_BLOCK_BYTES &&
      ((!pb->voice_live && !pb->native_media) || final_block))
    {
      apb->nbytes = tx_len;
      apb->nsamples = frames;
      RV_VERBOSE("[RV-PB] terminal tail source=%zu wire=%zu samples=%zu\n",
             len, tx_len, frames);
    }
  else
    {
      apb->nbytes = RV_WIRE_BLOCK_BYTES;
      apb->nsamples =
        RV_WIRE_BLOCK_BYTES /
        RV_WIRE_BYTES_PER_SAMPLE;
    }

  ret = rv_lock(pb);
  if (ret < 0)
    {
      return ret;
    }

  if (slot->done_ms)
    {
      unsigned long long delay = rv_audio_now_ms() - slot->done_ms;
      if (delay > pb->max_refill_ms) pb->max_refill_ms = delay;
      if (delay >= RV_REFILL_WINDOW_MS(pb)) pb->late_refills++;
    }
  if (len < RV_SOURCE_BLOCK_BYTES && !pb->stop_on_short_underrun)
    pb->tail_pad_bytes += RV_SOURCE_BLOCK_BYTES - len;
  slot->result = -EINPROGRESS;
  slot->source_bytes = source_bytes;
  slot->inflight = 1;
  slot->submit_count++;
  pb->i2s_inflight++;
  rv_unlock(pb);

  {
    unsigned long long send_at = rv_audio_now_ms();
    ret = I2S_SEND(pb->i2s, apb, rv_i2s_done, slot,
                   MSEC2TICK(RV_SEND_TIMEOUT_MS));
    unsigned long long elapsed = rv_audio_now_ms() - send_at;
    if (elapsed > pb->max_send_ms) pb->max_send_ms = elapsed;

    if (pb->native_media && slot->submit_count == 1)
      {
        printf("[RV-PB] native DMA submit slot=%u bytes=%zu rc=%d\n",
               slot->index, (size_t)apb->nbytes, ret);
      }
  }
  if (ret < 0)
    {
      if (rv_lock(pb) == 0)
        {
          slot->result = ret;
          slot->source_bytes = 0;
          slot->inflight = 0;

          if (pb->i2s_inflight > 0)
            {
              pb->i2s_inflight--;
            }

          if (ret != -EBUSY && pb->i2s_result >= 0)
            {
              pb->i2s_result = ret;
            }

          rv_unlock(pb);
        }

      nxsem_post(&slot->done);

      /* slot->apb owns its base reference until rv_destroy(). */
      return ret;
    }

  /* Lower-half owns only its per-submit reference. */
  return 0;
}


static int rv_drain(struct rv_playback_s *pb)
{
  int result = 0;
  int ret;

  for (;;)
    {
      ret = rv_get_result(pb);
      if (ret < 0 && result >= 0)
        {
          result = ret;
        }

      if (rv_get_inflight(pb) == 0)
        {
          return result;
        }

      ret = nxsem_wait_uninterruptible(&pb->i2s_done);
      if (ret < 0)
        {
          return ret;
        }
    }
}

static int rv_wait_buffer(struct rv_playback_s *pb, size_t threshold)
{
#if RV_FULL_CACHE_AB_TEST
  (void)threshold;
#endif

  for (;;)
    {
      size_t available = rv_ring_count(pb);

#if RV_FULL_CACHE_AB_TEST
      /* For the A/B run, EOS rather than a fixed water mark authorizes the
       * first DMA submission.  The 2 MiB ring holds the maximum configured
       * decoded response, so producer/consumer overlap is eliminated. */
      if (pb->eos)
        {
          return 0;
        }
#else
      if ((!pb->native_media ||
           __atomic_load_n(&pb->media_started, __ATOMIC_ACQUIRE)) &&
          ((pb->cache_until_eos && pb->eos) ||
           (!pb->cache_until_eos && available >= threshold) || pb->eos))
        {
          return 0;
        }

      if (pb->stopped)
        {
          return 0;
        }
#endif

      if (pb->stopped)
        {
          return 0;
        }

      if (rv_get_result(pb) < 0)
        {
          return rv_get_result(pb);
        }

      if (nxsem_wait_uninterruptible(&pb->ring_data) < 0)
        {
          return -EINTR;
        }
    }
}

static void *rv_worker(void *arg)
{
  struct rv_playback_s *pb = arg;
  uint8_t block[RV_SOURCE_BLOCK_BYTES];
  unsigned int slot_index = 0;
  int primed_slots = 0;
  int ret = 0;
  bool terminal_tail = false;

  pb->state = RV_STATE_PREBUFFER;

  if (pb->native_media)
    {
      printf("[RV-PB] native worker ready slots=%u prebuffer=%u\n",
             pb->slot_count, pb->start_prebuffer);
    }

  while (!pb->stopped)
    {
      struct rv_slot_s *slot;

      terminal_tail = false;
      size_t available;
      size_t want;
      size_t got;
      bool prime_silence = false;
      bool final_block = false;
      bool music_stream = !pb->cache_until_eos && !pb->voice_live &&
                          !pb->native_media;

      ret = rv_get_result(pb);
      if (ret < 0)
        {
          printf("[RV-PB] I2S worker error=%d state=%s buffered=%zu/%zu "
                 "inflight=%d underruns=%lu silence=%lu\n",
                 ret, rv_play_state_name(pb->state), rv_ring_count(pb),
                 pb->ring.capacity, pb->i2s_inflight,
                 (unsigned long)pb->underrun_count,
                 (unsigned long)pb->silence_blocks);
          break;
        }

      if (pb->state == RV_STATE_PREBUFFER || pb->state == RV_STATE_REBUFFER)
        {
          size_t threshold = pb->state == RV_STATE_PREBUFFER ?
                             pb->start_prebuffer : RV_REBUFFER_BYTES;
          const char *from = pb->state == RV_STATE_PREBUFFER ?
                             "PREBUFFER" : "REBUFFER";

          ret = rv_wait_buffer(pb, threshold);
          if (ret < 0)
            {
              break;
            }

          if (pb->stopped) { ret = -ECANCELED; break; }
          available = rv_ring_count(pb);
          if (available >= 2)
            {
              pb->state = RV_STATE_PLAYING;
              primed_slots = 0;
              slot_index = 0;

              if (pb->ring_min_play == (size_t)-1)
                {
                  pb->ring_min_play = available;
                }

              if (available > pb->ring_max_play)
                {
                  pb->ring_max_play = available;
                }

              (void)from;
            }
          else if (pb->eos)
            {
              pb->state = RV_STATE_DRAINING;
            }
          else
            {
              continue;
            }
        }

      if (pb->state == RV_STATE_DRAINING)
        {
          break;
        }

      slot = &pb->slots[slot_index];

      /* Prime the negotiated number of slots, then refill in that same
       * round-robin order. Each source block expands to the fixed wire size;
       * a larger ring gives more callback/worker scheduling headroom. */
      if (primed_slots >= pb->slot_count)
        {
          ret = rv_wait_slot(pb, slot);
          if (ret < 0)
            {
              break;
            }
        }

      available = rv_ring_count(pb);
      rv_report_playback_diag(pb, available, slot_index, primed_slots);

      /* Preserve newly arriving fragments until there is a useful music
       * window. Continue submitting fresh silence at DMA cadence; waiting
       * here would replay old cyclic descriptors. Slot indices never reset. */
      if (music_stream)
        {
          bool old_hold = __atomic_load_n(&pb->music_buffering, __ATOMIC_ACQUIRE);
          bool hold = robot_music_buffer_hold(old_hold, available, pb->eos,
                                               pb->ring.capacity);
          __atomic_store_n(&pb->music_buffering, hold, __ATOMIC_RELEASE);
          if (hold != old_hold)
            {
              if (hold) pb->music_rebuffers++;
              if (robot_oled_set_loading != NULL)
                {
                  (void)robot_oled_set_loading(hold);
                }
              printf("[RV-MUSIC-BUFFER] id=%u %s buffered=%zu recover=%u episodes=%u\n",
                pb->id, hold ? "hold-silence" : "resume-pcm", available,
                ROBOT_MUSIC_BUFFER_RECOVER_BYTES, pb->music_rebuffers);
            }
        }

      /* No producer wait is permitted once the immutable DMA ring runs.
       * cache_until_eos voice has all its PCM here, so no interior gap exists. */
      if (music_stream &&
          __atomic_load_n(&pb->music_buffering, __ATOMIC_ACQUIRE))
        {
          memset(block, 0, sizeof(block));
          want = RV_SOURCE_BLOCK_BYTES;
          prime_silence = true;
          pb->underrun_count++;
        }
      else if (pb->native_media &&
          __atomic_load_n(&pb->media_paused, __ATOMIC_ACQUIRE))
        {
          /* Keep the immutable hardware ring fresh while preserving every
           * queued real sample. A pause is neither EOS nor a short tail. */
          memset(block, 0, sizeof(block));
          want = RV_SOURCE_BLOCK_BYTES;
          prime_silence = true;
        }
      else if (pb->voice_live && !pb->eos &&
          available < 2 * RV_SOURCE_BLOCK_BYTES)
        {
          /* Retain one real block until EOS is known so even an exact block
           * tail receives AUDIO_APB_FINAL. Never wait on a running immutable
           * DMA slot: overwrite it with silence during a producer gap. */
          memset(block, 0, sizeof(block));
          want = RV_SOURCE_BLOCK_BYTES;
          prime_silence = true;
          pb->underrun_count++;
          pb->silence_blocks++;
          pb->gap_run++;
          if (pb->gap_run > pb->max_gap_run) pb->max_gap_run = pb->gap_run;
        }
      else if (available >= RV_SOURCE_BLOCK_BYTES)
        {
          want = RV_SOURCE_BLOCK_BYTES;
          pb->gap_run = 0;
          if (pb->eos && available == RV_SOURCE_BLOCK_BYTES &&
              pb->stop_on_short_underrun)
            {
              final_block = true;
              terminal_tail = true;
            }
        }
      else if (pb->eos && available > 0)
        {
          want = available & ~(size_t)1;
          final_block = pb->stop_on_short_underrun;
          terminal_tail = final_block;
          if (want == 0)
            {
              (void)rv_ring_read(pb, block, available);
              continue;
            }
        }
      else if (pb->eos && available == 0)
        {
          if (pb->stop_on_short_underrun)
            {
              /*
               * Exact finite-file EOF: there is no real PCM left.  Do not
               * manufacture ten extra silent submissions just to walk the
               * cyclic ring.  Drain the already-submitted real blocks and
               * stop.
               */

              pb->eos_silence_rounds = 0;
              pb->state = RV_STATE_DRAINING;
              break;
            }

          if (pb->eos_silence_rounds < pb->slot_count)
            {
              memset(block, 0, sizeof(block));
              want = RV_SOURCE_BLOCK_BYTES;
              prime_silence = true;
              pb->eos_silence_rounds++;
              pb->silence_blocks++;
            }
          else
            {

              pb->eos_silence_rounds = 0;
              pb->state = RV_STATE_DRAINING;
              break;
            }
        }

      else
        {
          /*
           * Legacy streaming-music path only: NEVER stop feeding the immutable cyclic DMA ring after
           * PLAYING has started.
           *
           * The lower half keeps its 16 frozen descriptors circulating even
           * when the upper layer has no logical requests in flight.  The old
           * REBUFFER path stopped submitting new payloads while waiting for
           * 48 KiB of producer data; hardware then replayed the stale tail
           * still stored in those DMA slots ("...变化量量量量...").
           *
           * Keep cadence tied to rv_wait_slot()/DMA completion instead:
           *   - if a short aligned tail exists, play it and let
           *     rv_queue_slot() zero-pad the rest of the 2048-byte wire slot;
           *   - if no complete PCM16 sample exists, submit one full silent
           *     source block.
           *
           * A producer stall can therefore create silence, never stale-audio
           * repetition. Complete-cache voice must never enter this path.
           */
          if (pb->cache_until_eos)
            {
              /* Complete-cache voice can never starve. Stop on invariant
               * failure instead of adding any silence in the utterance. */
              ret = -EPROTO;
              break;
            }
          pb->underrun_count++;
          pb->gap_run++;
          if (pb->gap_run > pb->max_gap_run) pb->max_gap_run = pb->gap_run;

          if ((pb->native_media || !pb->voice_live) &&
              (pb->underrun_count == 1 ||
               (pb->underrun_count % 512) == 0))
            {
              printf("[RV-PB] streaming PCM underflow=%lu buffered=%zu/%zu "
                     "low_water=%zu silence=%lu\n",
                     (unsigned long)pb->underrun_count, available,
                     pb->ring.capacity, pb->ring_min_play,
                     (unsigned long)pb->silence_blocks);
            }

          if (available >= sizeof(int16_t))
            {
              want = available;
              if (want > RV_SOURCE_BLOCK_BYTES)
                {
                  want = RV_SOURCE_BLOCK_BYTES;
                }

              want &= ~(size_t)1;

              if (pb->stop_on_short_underrun && !pb->native_media)
                {
                  terminal_tail = true;
                  final_block = true;
                  pb->eos = 1;
                  (void)0;
                }
              else if (pb->underrun_count <= 4 ||
                       (pb->underrun_count % 32) == 0)
                {
                  (void)0;
                }
            }
          else
            {
              memset(block, 0, sizeof(block));
              want = RV_SOURCE_BLOCK_BYTES;
              prime_silence = true;

              if (available > 0)
                {
                  /* Preserve PCM16 alignment: discard a lone impossible
                   * byte rather than carrying it into the next sample. */
                  (void)rv_ring_read(pb, block, available);
                }

              if (pb->stop_on_short_underrun && !pb->native_media)
                {
                  (void)0;
                  pb->eos = 1;
                  pb->stopped = 1;
                  pb->terminal_tail_done = 1;

                  if (pb->i2s != NULL && pb->i2s_started)
                    {
                      rv_stop_i2s(pb);
                    }

                  rv_signal_all(pb);
                  break;
                }

              if (pb->underrun_count <= 4 ||
                  (pb->underrun_count % 32) == 0)
                {
                  (void)0;
                }
            }
        }

      if (prime_silence)
        {
          if (music_stream) pb->silence_blocks++;
          got = want;
        }
      else
        {
          got = rv_ring_read(pb, block, want);
          if (got == 0)
            {
              continue;
            }
        }

      ret = rv_queue_slot(pb, slot, block, got, final_block,
                          prime_silence ? 0 : got);
      if (ret < 0)
        {
          if (ret != -ECANCELED)
            {
              printf("[RV-PB] queue slot=%u failed=%d\n",
                     slot_index, ret);
            }
          break;
        }

      if (!pb->dma_start_ms &&
          (primed_slots + 1 >= pb->slot_count || final_block)) {
        /* A short finite chain starts on its FINAL send too. This software
         * submission timestamp is not an acoustic measurement. */
        pb->dma_start_ms = rv_audio_now_ms();
        if (pb->voice_live && pb->sentence)
          RV_EVENT("[耗时][TTS播放] run=%lu seq=%u 请求到播放提交=%lldms 音频交付到播放提交=%lldms\n",
            pb->run_id, pb->sentence, rv_elapsed_ms(pb->tts_start_ms, pb->dma_start_ms),
            rv_elapsed_ms(pb->tts_ready_ms, pb->dma_start_ms));
      }

      if (terminal_tail)
        {
          int tail_ret = rv_wait_slot(pb, slot);

          if (tail_ret < 0 && tail_ret != -ECANCELED && ret >= 0)
            {
              ret = tail_ret;
            }



          pb->terminal_tail_done = 1;
          pb->stopped = 1;
          pb->state = RV_STATE_STOPPED;

          if (pb->i2s != NULL && pb->i2s_started)
            {
              rv_stop_i2s(pb);
            }

          rv_signal_all(pb);
          break;
        }

      if (primed_slots < pb->slot_count)
        {
          primed_slots++;
        }

      slot_index = (slot_index + 1) % pb->slot_count;
    }

  /* Do not stop I2S before the logical slots have completed. */
  {
    unsigned int i;

    for (i = 0; i < pb->slot_count; i++)
      {
        int slot_ret = rv_wait_slot(pb, &pb->slots[i]);
        if (slot_ret < 0 && slot_ret != -ECANCELED && ret >= 0)
          {
            ret = slot_ret;
          }
      }
  }

  if (!pb->stopped)
    {
      int drain_ret = rv_drain(pb);
      if (drain_ret < 0 && ret >= 0)
        {
          ret = drain_ret;
        }
    }

  if (rv_lock(pb) == 0)
    {
      if (ret < 0 && ret != -ECANCELED && pb->i2s_result >= 0)
        {
          pb->i2s_result = ret;
        }
      pb->worker_exited = 1;
      pb->state = RV_STATE_STOPPED;
      rv_unlock(pb);
    }
  else
    {
      pb->worker_exited = 1;
      pb->state = RV_STATE_STOPPED;
    }

  pb->dma_done_ms = rv_audio_now_ms();
  rv_signal_all(pb); /* Wake a bounded producer if the DMA worker failed. */
  nxsem_post(&pb->i2s_done);
  return NULL;
}

static void rv_signal_all(struct rv_playback_s *pb)
{
  unsigned int i;

  nxsem_post(&pb->ring_data);
  nxsem_post(&pb->ring_space);
  nxsem_post(&pb->i2s_done);

  for (i = 0; i < RV_MAX_INFLIGHT; i++)
    {
      nxsem_post(&pb->slots[i].done);
    }
}

static void rv_release_slot_buffers(struct rv_playback_s *pb)
{
  unsigned int i;
  for (i = 0; i < RV_MAX_INFLIGHT; i++)
    {
      if (pb->slots[i].apb != NULL)
        {
          apb_free(pb->slots[i].apb);
          pb->slots[i].apb = NULL;
        }

    }
}

static int rv_prepare_slot_buffers(struct rv_playback_s *pb, unsigned int count)
{
  unsigned int i;
  rv_release_slot_buffers(pb);
  for (i = 0; i < count; i++)
    {
      struct audio_buf_desc_s desc;
      memset(&desc, 0, sizeof(desc));
      desc.numbytes = RV_WIRE_BLOCK_BYTES;
      desc.u.pbuffer = &pb->slots[i].apb;
      int ret = apb_alloc(&desc);
      if (ret < 0 || !pb->slots[i].apb)
        {
          rv_release_slot_buffers(pb);
          return ret < 0 ? ret : -ENOMEM;
        }
    }
  return 0;
}

static void rv_destroy(struct rv_playback_s *pb)
{
  unsigned int i;
  rv_release_slot_buffers(pb);
  for (i = 0; i < RV_MAX_INFLIGHT; i++)
    {
      nxsem_destroy(&pb->slots[i].done);
    }

  nxsem_destroy(&pb->ring_space);
  nxsem_destroy(&pb->ring_data);
  nxsem_destroy(&pb->ring_lock);
  nxsem_destroy(&pb->i2s_done);

  free(pb->ring.data);
  pb->ring.data = NULL;
}

static int rv_start_i2s_playback(struct rv_playback_s *pb)
{
  pthread_attr_t attr;
  struct ap_buffer_info_s info;
  static const unsigned int choices[] = {16, 8, 4};
  int actual_rate;
  int actual_width;
  int ret;

  if (pb == NULL || !pb->opened)
    {
      return -EINVAL;
    }

  if (pb->i2s_started || pb->worker_started)
    {
      return -EBUSY;
    }

  /*
   * Hard guard against another upper/lower mismatch.  The current
   * contest_i2s.c accepts only fixed 2048-byte TX requests and starts its
   * immutable ring only after all slots have been populated.
   */
  if (RV_WIRE_BLOCK_BYTES != RV_LOWER_DMA_SLOT_BYTES ||
      RV_MAX_INFLIGHT != RV_LOWER_DMA_RING_SLOTS)
    {
      printf("[RV-PB] lower contract mismatch wire_block=%u/%u slots=%u/%u\n",
             RV_WIRE_BLOCK_BYTES, RV_LOWER_DMA_SLOT_BYTES,
             RV_MAX_INFLIGHT, RV_LOWER_DMA_RING_SLOTS);
      return -EINVAL;
    }

  pb->i2s = board_voice_speaker_i2s();
  if (pb->i2s == NULL)
    {
      return -ENODEV;
    }

  /*
   * Original board wire format:
   *   24000 Hz / 2 physical slots / 32-bit / mono-left.
   *
   * Source and wire have the same 24 kHz frame rate. rv_queue_slot() shifts
   * PCM16 into the high half of each 32-bit mono sample; it never resamples.
   */
  ret = I2S_TXCHANNELS(pb->i2s, RV_WIRE_CHANNELS);
  if (ret < 0)
    {
      printf("[RV-PB] I2S_TXCHANNELS(1) failed=%d\n", ret);
      goto fail;
    }

  actual_rate = (int)I2S_TXSAMPLERATE(pb->i2s, RV_WIRE_SAMPLE_RATE);
  if (actual_rate != RV_WIRE_SAMPLE_RATE)
    {
      RV_VERBOSE("[RV-PB] I2S wire rate request=%u actual=%d\n",
             RV_WIRE_SAMPLE_RATE, actual_rate);
      ret = actual_rate < 0 ? actual_rate : -ENOTSUP;
      goto fail;
    }

  actual_width = (int)I2S_TXDATAWIDTH(pb->i2s, RV_WIRE_BITS);
  if (actual_width != RV_WIRE_BITS)
    {
      RV_VERBOSE("[RV-PB] I2S width request=%u actual=%d\n",
             RV_WIRE_BITS, actual_width);
      ret = actual_width < 0 ? actual_width : -ENOTSUP;
      goto fail;
    }

  /* Include the upper APBs in the REAL allocation budget before requesting
   * private DMA. apb_alloc can itself use internal memory on some builds.
   * A smaller lower ring releases unused upper APBs before playback starts. */
  ret = -ENOMEM;
  for (unsigned int attempt = 0; attempt < sizeof(choices) / sizeof(choices[0]); attempt++)
    {
      ret = rv_prepare_slot_buffers(pb, choices[attempt]);
      if (ret == -ENOMEM) continue;
      if (ret < 0) goto fail;
      ret = I2S_IOCTL(pb->i2s, AUDIOIOC_START, choices[attempt]);
      if (ret == -ENOMEM)
        {
          rv_release_slot_buffers(pb);
          continue;
        }
      if (ret < 0) goto fail;
      pb->i2s_started = 1;
      memset(&info, 0, sizeof(info));
      ret = I2S_IOCTL(pb->i2s, AUDIOIOC_GETBUFFERINFO, (unsigned long)&info);
      if (ret < 0 || info.buffer_size != RV_WIRE_BLOCK_BYTES ||
          (info.nbuffers != 4 && info.nbuffers != 8 && info.nbuffers != 16) ||
          info.nbuffers > choices[attempt])
        {
          printf("[RV-PB] DMA contract failed rc=%d slots=%u bytes=%u requested=%u\n",
                 ret, (unsigned int)info.nbuffers, (unsigned int)info.buffer_size,
                 choices[attempt]);
          ret = ret < 0 ? ret : -EPROTO;
          goto fail_started;
        }
      pb->slot_count = info.nbuffers;
      if (pb->native_media)
        {
          /* Prime every negotiated slot once. A large fixed startup threshold
           * deadlocks when the Media source waits for GETPOSITION to advance. */
          pb->start_prebuffer = pb->slot_count * RV_SOURCE_BLOCK_BYTES;
        }
      for (unsigned int i = pb->slot_count; i < RV_MAX_INFLIGHT; i++)
        {
          if (pb->slots[i].apb) apb_free(pb->slots[i].apb);
          pb->slots[i].apb = NULL;
        }
      break;
    }
  if (!pb->i2s_started)
    {
      printf("[RV-PB] DMA unavailable rc=%d; minimum ring allocation failed\n", ret);
      goto fail;
    }
  printf("[RV-PB] DMA ready slots=%u slot_bytes=%u\n", pb->slot_count, RV_WIRE_BLOCK_BYTES);
  pb->i2s_result = 0;
  pb->state = RV_STATE_PREBUFFER;
  pb->ring_min_play = (size_t)-1;
  pb->ring_max_play = 0;
  pb->eos_silence_rounds = 0;

  RV_VERBOSE("[RV-PB] I2S ready source=%uHz/mono/%ubit "
         "wire=%dHz/%uch/%dbit mono-left "
         "source_block=%u wire_dma=%u ring_slots=%u "
         "prebuffer=%u rebuffer=%u\n",
         RV_TTS_SAMPLE_RATE, RV_TTS_BITS,
         actual_rate, RV_WIRE_CHANNELS, actual_width,
         RV_SOURCE_BLOCK_BYTES, RV_WIRE_BLOCK_BYTES,
         pb->slot_count,
         pb->start_prebuffer, RV_REBUFFER_BYTES);

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      ret = -ret;
      goto fail_started;
    }

  ret = rv_thread_attributes(&attr, RV_WORKER_STACK, RV_WORKER_PRIORITY);
  if (ret != 0)
    {
      pthread_attr_destroy(&attr);
      ret = -ret;
      goto fail_started;
    }

#if !ROBOT_VOICE_EXPLICIT_SCHED
  /* Match v9 exactly: best-effort parameter, inherited/default policy. */
  struct sched_param param = {0};
  param.sched_priority = RV_WORKER_PRIORITY;
  (void)pthread_attr_setschedparam(&attr, &param);
#endif
  ret = pthread_create(&pb->worker, &attr, rv_worker, pb);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      ret = -ret;
      goto fail_started;
    }

  pb->worker_started = 1;
  return 0;

fail_started:
  if (pb->i2s_started)
    {
      rv_stop_i2s(pb);
    }

fail:
  rv_release_slot_buffers(pb);
  pb->i2s = NULL;
  return ret;
}

void robot_audio_playback_set_stop_on_short_underrun(bool enable)
{
  g_stop_on_short_underrun = enable ? 1 : 0;
  RV_VERBOSE("[RV-PB] short-tail policy=%s\n",
         enable ? "play-real-tail-then-stop" : "normal");
}

static int rv_playback_open(unsigned int prebuffer, bool cache_until_eos,
                            bool voice_live, bool native_media)
{
  struct rv_playback_s *pb = &g_pb;
  unsigned int i;
  int ret;

  if (pb->opened)
    {
      return -EBUSY;
    }

  memset(pb, 0, sizeof(*pb));
  pb->id = ++g_playback_id;
  pb->slot_count = RV_MAX_INFLIGHT;
  pb->cache_until_eos = cache_until_eos;
  pb->voice_live = voice_live;
  pb->native_media = native_media;
  pb->media_started = !native_media;
  pb->ring.capacity = native_media ? RV_NATIVE_MEDIA_RING_BYTES :
                     (voice_live ? ROBOT_VOICE_TTS_STREAM_RING_BYTES :
                      (cache_until_eos ? RV_RING_BYTES : RV_MUSIC_RING_BYTES));
  pb->open_ms = rv_audio_now_ms();
  pb->input_hash = pb->output_hash = RV_PCM_HASH_INIT;
  pb->start_prebuffer = native_media ? prebuffer :
                        (cache_until_eos ?
                         (RV_FULL_CACHE_AB_TEST ? RV_RING_BYTES : prebuffer) :
                         RV_MUSIC_START_PREBUFFER_BYTES);
  /* Complete TTS sentences use AUDIO_APB_FINAL so the lower-half drains the
   * final real PCM and stops without walking the cyclic ring with silence. */
  pb->stop_on_short_underrun = (cache_until_eos || voice_live || native_media) ?
                              1 : g_stop_on_short_underrun;

  if (ROBOT_VOICE_TTS_RATE != RV_TTS_SAMPLE_RATE ||
      ROBOT_VOICE_CHANNELS != RV_TTS_SOURCE_CHANNELS ||
      ROBOT_VOICE_BITS != RV_TTS_BITS)
    {
      printf("[RV-PB] source format mismatch cfg=%uHz/%uch/%ubit "
             "expected=%uHz/%uch/%ubit\n",
             ROBOT_VOICE_TTS_RATE, ROBOT_VOICE_CHANNELS, ROBOT_VOICE_BITS,
             RV_TTS_SAMPLE_RATE, RV_TTS_SOURCE_CHANNELS, RV_TTS_BITS);
      return -ENOTSUP;
    }

  pb->ring.data = malloc(pb->ring.capacity);
  if (pb->ring.data == NULL && !cache_until_eos && !voice_live &&
      !native_media && pb->ring.capacity != RV_RING_BYTES)
    {
      /* Music uses PSRAM for a long jitter cushion when available. Fall back
       * to the original bounded ring if the board has less free PSRAM. */
      pb->ring.capacity = RV_RING_BYTES;
      pb->start_prebuffer = RV_FULL_CACHE_AB_TEST ? RV_RING_BYTES :
                            RV_MUSIC_START_PREBUFFER_BYTES;
      if (pb->start_prebuffer > pb->ring.capacity)
        {
          pb->start_prebuffer = pb->ring.capacity;
        }
      pb->ring.data = malloc(pb->ring.capacity);
      printf("[RV-PB] music ring fallback capacity=%zu prebuffer=%u\n",
             pb->ring.capacity, pb->start_prebuffer);
    }
  if (pb->ring.data == NULL)
    {
      printf("[RV-PB] ring alloc failed bytes=%u mode=%s\n",
             (unsigned int)pb->ring.capacity,
             RV_FULL_CACHE_AB_TEST ? "full-cache" : "streaming");
      return -ENOMEM;
    }

  ret = nxsem_init(&pb->i2s_done, 0, 0);
  if (ret < 0)
    {
      free(pb->ring.data);
      memset(pb, 0, sizeof(*pb));
      return ret;
    }

  ret = nxsem_init(&pb->ring_lock, 0, 1);
  if (ret < 0)
    {
      nxsem_destroy(&pb->i2s_done);
      free(pb->ring.data);
      memset(pb, 0, sizeof(*pb));
      return ret;
    }

  ret = nxsem_init(&pb->ring_data, 0, 0);
  if (ret < 0)
    {
      nxsem_destroy(&pb->ring_lock);
      nxsem_destroy(&pb->i2s_done);
      free(pb->ring.data);
      memset(pb, 0, sizeof(*pb));
      return ret;
    }

  ret = nxsem_init(&pb->ring_space, 0, 0);
  if (ret < 0)
    {
      nxsem_destroy(&pb->ring_data);
      nxsem_destroy(&pb->ring_lock);
      nxsem_destroy(&pb->i2s_done);
      free(pb->ring.data);
      memset(pb, 0, sizeof(*pb));
      return ret;
    }

  for (i = 0; i < RV_MAX_INFLIGHT; i++)
    {
      ret = nxsem_init(&pb->slots[i].done, 0, 0);
      if (ret < 0)
        {
          while (i > 0)
            {
              i--;
              nxsem_destroy(&pb->slots[i].done);
            }

          nxsem_destroy(&pb->ring_space);
          nxsem_destroy(&pb->ring_data);
          nxsem_destroy(&pb->ring_lock);
          nxsem_destroy(&pb->i2s_done);
          free(pb->ring.data);
          memset(pb, 0, sizeof(*pb));
          return ret;
        }

      pb->slots[i].pb = pb;
      pb->slots[i].result = 0;
      pb->slots[i].inflight = 0;
      pb->slots[i].index = i;
    }

  pb->i2s_result = 0;
  pb->state = RV_STATE_PREBUFFER;
  pb->opened = 1;

  /* Complete-cache voice needs no DMA buffers, APBs or hardware worker
   * during the network download. Release this DRAM budget to Wi-Fi until
   * the full validated playback window is ready at finish(). */
  if (!(cache_until_eos && ROBOT_VOICE_TTS_CONTINUITY))
    {
      ret = rv_start_i2s_playback(pb);
      if (ret < 0)
        {
          rv_destroy(pb);
          memset(pb, 0, sizeof(*pb));
          return ret;
        }
    }

  RV_VERBOSE("[RV-DIAG] mode=%s output=%s tone_peak=%u "
         "source_bytes_per_sec=%u wire_bytes_per_sec=%u\n",
         RV_FULL_CACHE_AB_TEST ? "full-cache" : "streaming",
         RV_I2S_OUTPUT_TONE_TEST ? "tone-1khz" : "tts-pcm",
         RV_I2S_OUTPUT_TONE_PEAK,
         RV_TTS_SAMPLE_RATE * RV_TTS_SOURCE_CHANNELS *
           (RV_TTS_BITS / 8),
         RV_WIRE_SAMPLE_RATE * RV_WIRE_BYTES_PER_SAMPLE);
  RV_VERBOSE("[RV-PB] ring open capacity=%zu start=%u rebuffer=%u "
         "source=%uHz/mono/%ubit wire=%uHz/mono-left/%ubit resample=none\n",
         pb->ring.capacity, pb->start_prebuffer, RV_REBUFFER_BYTES,
         RV_TTS_SAMPLE_RATE, RV_TTS_BITS,
         RV_WIRE_SAMPLE_RATE, RV_WIRE_BITS);
  RV_VERBOSE("[RV-PB] open id=%u mode=%s capacity=%zu cache_limit=%zu source=24000/mono16\n",
         pb->id, cache_until_eos ? "voice-complete-cache" :
         (voice_live ? "voice-live" : "music-stream"), pb->ring.capacity,
         cache_until_eos ? ROBOT_VOICE_TTS_CACHE_BYTES : pb->ring.capacity);
  return 0;
}

void robot_audio_playback_set_turn_timing(unsigned long long commit_ms,
    unsigned long long user_end_ms, unsigned long long tts_start_ms)
{
  struct rv_playback_s *pb = &g_pb;
  if (!pb->opened || rv_lock(pb) < 0) return;
  pb->commit_ms = commit_ms;
  pb->user_end_ms = user_end_ms;
  pb->tts_start_ms = tts_start_ms;
  rv_unlock(pb);
}

void robot_audio_playback_set_sentence_timing(unsigned long run_id,
    unsigned int sequence, unsigned long long ready_ms)
{
  struct rv_playback_s *pb = &g_pb;
  if (!pb->opened || rv_lock(pb) < 0) return;
  pb->run_id = run_id;
  pb->sentence = sequence;
  pb->tts_ready_ms = ready_ms;
  rv_unlock(pb);
}

int robot_audio_playback_open(void)
{
  return rv_playback_open(RV_START_PREBUFFER_BYTES, false, false, false);
}

int robot_audio_playback_open_music(void)
{
  RV_VERBOSE("[RV-PB] music open prebuffer=%u ring=%u\n",
             RV_MUSIC_START_PREBUFFER_BYTES, RV_MUSIC_RING_BYTES);
  return rv_playback_open(RV_MUSIC_START_PREBUFFER_BYTES, false, false,
                          false);
}

void robot_audio_playback_set_media_voice_mode(bool enable)
{
  g_media_voice_mode = enable;
}

int robot_audio_playback_open_media(void)
{
  unsigned int prebuffer = g_media_voice_mode ? ROBOT_VOICE_TTS_PREBUFFER :
                                                RV_START_PREBUFFER_BYTES;

  RV_VERBOSE("[RV-PB] media open prebuffer=%u mode=%s\n", prebuffer,
         g_media_voice_mode ? "voice" : "music");

  return rv_playback_open(prebuffer, false, false, false);
}

int robot_audio_playback_open_native_media(void)
{
  int ret = rv_playback_open(RV_NATIVE_MEDIA_PREBUFFER_BYTES,
                             false, false, true);
  if (ret == 0)
    printf("[RV-PB] native Media buffer=%zuB (~%ums) prebuffer=%uB (~%ums) "
           "mode=OpenVela-Media\n",
           g_pb.ring.capacity,
           (unsigned int)(g_pb.ring.capacity * 1000ULL /
                          (RV_TTS_SAMPLE_RATE * sizeof(int16_t))),
           g_pb.start_prebuffer,
           (unsigned int)(g_pb.start_prebuffer * 1000ULL /
                          (RV_TTS_SAMPLE_RATE * sizeof(int16_t))));
  return ret;
}

int robot_audio_playback_start_native_media(void)
{
  struct rv_playback_s *pb = &g_pb;
  if (!pb->opened || !pb->native_media) return -EINVAL;
  if (rv_lock(pb) < 0) return -EINTR;
  __atomic_store_n(&pb->media_started, true, __ATOMIC_RELEASE);
  __atomic_store_n(&pb->media_paused, false, __ATOMIC_RELEASE);
  rv_unlock(pb);
  nxsem_post(&pb->ring_data);
  return 0;
}

int robot_audio_playback_pause_native_media(bool paused)
{
  struct rv_playback_s *pb = &g_pb;
  if (!pb->opened || !pb->native_media) return -EINVAL;
  if (rv_lock(pb) < 0) return -EINTR;
  __atomic_store_n(&pb->media_paused, paused, __ATOMIC_RELEASE);
  rv_unlock(pb);
  nxsem_post(&pb->ring_data);
  return 0;
}

int robot_audio_playback_end_input(void)
{
  struct rv_playback_s *pb = &g_pb;
  if (!pb->opened) return -EINVAL;
  if (rv_lock(pb) < 0) return -EINTR;
  pb->eos = 1;
  rv_unlock(pb);
  rv_signal_all(pb);
  return 0;
}

int robot_audio_playback_progress(uint64_t *completed, uint64_t *pending)
{
  struct rv_playback_s *pb = &g_pb;
  int ret;
  if (!completed || !pending || !pb->opened) return -EINVAL;
  if (rv_lock(pb) < 0) return -EINTR;
  *completed = pb->completed_source_bytes;
  *pending = pb->total_written - pb->completed_source_bytes;
  ret = pb->i2s_result;
  rv_unlock(pb);
  return ret < 0 ? ret : 0;
}

int robot_audio_playback_open_voice(void)
{
  return rv_playback_open(ROBOT_VOICE_TTS_PREBUFFER, true, false, false);
}

int robot_audio_playback_open_voice_live(void)
{
  return rv_playback_open(ROBOT_VOICE_TTS_PREBUFFER, false, true, false);
}

int robot_audio_playback_write(const uint8_t *pcm, size_t len)
{
  struct rv_playback_s *pb = &g_pb;
  size_t offset = 0;

  if (!pb->opened || pcm == NULL || len == 0)
    {
      return -EINVAL;
    }

  if ((len & 1) != 0)
    {
      /* PCM16 producer chunks must preserve sample alignment. */
      printf("[RV-PB] reject odd PCM chunk len=%zu\n", len);
      return -EINVAL;
    }

  if (pb->cache_until_eos)
    {
      size_t required;
      int lock_ret = rv_lock(pb);
      if (lock_ret < 0) return lock_ret;
      if (pb->eos || pb->stopped)
        { rv_unlock(pb); return pb->stopped ? -ECANCELED : -EPIPE; }
      if (len > ROBOT_VOICE_TTS_CACHE_BYTES - pb->total_written)
        {
          rv_unlock(pb);
          printf("[RV-PB] cache overflow id=%u incoming=%zu cached=%zu limit=%u; no audio played\n",
                 pb->id, len, pb->total_written, ROBOT_VOICE_TTS_CACHE_BYTES);
          return -EOVERFLOW;
        }
      required = pb->total_written + len;
      if (required > pb->ring.capacity)
        {
          size_t capacity = pb->ring.capacity;
          uint8_t *data;
          while (capacity < required) capacity *= 2;
          if (capacity > ROBOT_VOICE_TTS_CACHE_BYTES)
            capacity = ROBOT_VOICE_TTS_CACHE_BYTES;
          /* Voice has not consumed any PCM yet. It is contiguous from zero;
           * restore head=count if the old capacity was exactly full. */
          data = realloc(pb->ring.data, capacity);
          if (!data) { rv_unlock(pb); return -ENOMEM; }
          pb->ring.data = data;
          pb->ring.capacity = capacity;
          pb->ring.head = pb->ring.count;
        }
      rv_unlock(pb);
    }

  /*
   * Producer side from tts_pcm_playback_guide.md:
   *
   * Never fail a valid long utterance just because the 256 KiB ring is
   * temporarily full.  Wait for the dedicated playback worker to free space,
   * then continue writing the same PCM chunk.
   */
  while (offset < len)
    {
      size_t buffered;
      size_t space;
      size_t n;
      int ret;

      if (pb->stopped)
        {
          return pb->terminal_tail_done ? -EPIPE : -ECANCELED;
        }

      ret = rv_get_result(pb);
      if (ret < 0)
        {
          return ret;
        }

      buffered = rv_ring_count(pb);
      space = pb->ring.capacity - buffered;
      if (space < 2)
        {
          ret = nxsem_wait_uninterruptible(&pb->ring_space);
          if (ret < 0)
            {
              return ret;
            }
          continue;
        }

      n = len - offset;
      if (n > space)
        {
          n = space;
        }

      n &= ~(size_t)1;
      if (n == 0)
        {
          continue;
        }

      ret = rv_ring_write(pb, pcm + offset, n);
      if (ret == -ENOSPC)
        {
          continue;
        }
      if (ret < 0)
        {
          return ret;
        }

      offset += n;
    }

  return 0;
}

size_t robot_audio_playback_buffered_bytes(void)
{
  struct rv_playback_s *pb = &g_pb;
  return pb->opened ? rv_ring_count(pb) : 0;
}

bool robot_audio_playback_music_audible(void)
{
  struct rv_playback_s *pb = &g_pb;
  return pb->opened && !pb->stopped && pb->dma_start_ms != 0 &&
         !pb->cache_until_eos && !pb->voice_live && !pb->native_media &&
         !__atomic_load_n(&pb->music_buffering, __ATOMIC_ACQUIRE);
}

int robot_audio_playback_finish(void)
{
  struct rv_playback_s *pb = &g_pb;
  int ret = 0;

  if (!pb->opened)
    {
      return -EINVAL;
    }

  if (pb->finished)
    {
      return rv_get_result(pb);
    }

  if (pb->cache_until_eos && ROBOT_VOICE_TTS_CONTINUITY &&
      !pb->worker_started && !pb->stopped && pb->total_written != 0)
    {
      ret = rv_start_i2s_playback(pb);
      if (ret < 0)
        {
          if (rv_lock(pb) == 0)
            {
              pb->i2s_result = ret;
              rv_unlock(pb);
            }
          return ret;
        }
    }

  /*
   * Producer EOS: wake the consumer.  If less than a normal prebuffer/block
   * remains, the worker is allowed to drain that final aligned PCM tail.
   */
  pb->eos = 1;
  RV_VERBOSE("[RV-PB] producer EOS id=%u mono_ms=%llu cached=%zu\n",
         pb->id, rv_audio_now_ms(), pb->total_written);
  rv_signal_all(pb);

  if (pb->worker_started)
    {
      int join_ret = pthread_join(pb->worker, NULL);

      if (join_ret != 0 && ret >= 0)
        {
          ret = -join_ret;
        }

      pb->worker_started = 0;
    }

  if (rv_get_result(pb) < 0 && ret >= 0)
    {
      ret = rv_get_result(pb);
    }

  if (pb->i2s != NULL && pb->i2s_started)
    {
      rv_stop_i2s(pb);
    }

  pb->finished = 1;

  if ((pb->cache_until_eos || pb->voice_live) && ret == 0 && (pb->input_hash != pb->output_hash ||
                   pb->total_written != pb->total_read)) ret = -EPROTO;
  if (ret < 0 && rv_lock(pb) == 0)
    { if (pb->i2s_result >= 0) pb->i2s_result = ret; rv_unlock(pb); }
  if (pb->voice_live && pb->sentence) {
    RV_EVENT("[播放] run=%lu seq=%u 已完成 PCM=%zuB 补静音块=%lu 最大连续补静音块=%lu rc=%d\n",
      pb->run_id, pb->sentence, pb->total_read, (unsigned long)pb->silence_blocks,
      (unsigned long)pb->max_gap_run, ret);
  } else if (pb->sentence && pb->dma_start_ms) {
    RV_EVENT("[耗时][TTS] run=%lu seq=%u 文本送入到播放提交=%lldms 合成=%lldms 就绪后等待及提交=%lldms rc=%d\n",
      pb->run_id, pb->sentence, rv_elapsed_ms(pb->tts_start_ms, pb->dma_start_ms),
      rv_elapsed_ms(pb->tts_start_ms, pb->tts_ready_ms),
      rv_elapsed_ms(pb->tts_ready_ms, pb->dma_start_ms), ret);
  }

  RV_VERBOSE("[RV-PB] PLAY_DONE id=%u pcm=%zu consumed=%zu hash=%08lx/%08lx "
         "gap_blocks=%lu late_refills=%lu max_refill_ms=%llu max_send_ms=%llu "
         "callback_errors=%lu callback_gap_max_ms=%llu tail_pad=%lu end_silence_blocks=%lu rc=%d\n",
         pb->id, pb->total_written, pb->total_read,
         (unsigned long)pb->input_hash, (unsigned long)pb->output_hash,
         (unsigned long)pb->underrun_count, (unsigned long)pb->late_refills,
         pb->max_refill_ms, pb->max_send_ms, (unsigned long)pb->callback_errors,
         pb->max_callback_gap_ms, (unsigned long)pb->tail_pad_bytes, (unsigned long)pb->silence_blocks, ret);
  RV_VERBOSE("[RV-LATENCY] playback_id=%u dma_start_ms=%llu "
         "user_end_to_dma_ms=%llu commit_to_dma_ms=%llu tts_to_dma_ms=%llu "
         "first_pcm_to_dma_ms=%llu source_ms=%zu play_ms=%llu producer_gap_max_ms=%llu\n",
         pb->id, pb->dma_start_ms,
         pb->user_end_ms && pb->dma_start_ms >= pb->user_end_ms ? pb->dma_start_ms - pb->user_end_ms : 0,
         pb->commit_ms && pb->dma_start_ms >= pb->commit_ms ? pb->dma_start_ms - pb->commit_ms : 0,
         pb->tts_start_ms && pb->dma_start_ms >= pb->tts_start_ms ? pb->dma_start_ms - pb->tts_start_ms : 0,
         pb->first_pcm_ms && pb->dma_start_ms >= pb->first_pcm_ms ? pb->dma_start_ms - pb->first_pcm_ms : 0,
         pb->total_written * 1000 / (RV_TTS_SAMPLE_RATE * sizeof(int16_t)),
         pb->dma_start_ms && pb->dma_done_ms >= pb->dma_start_ms ? pb->dma_done_ms - pb->dma_start_ms : 0,
         pb->max_pcm_gap_ms);
  if (!pb->voice_live)
    {
      printf("[RV-PB] stream summary id=%u prebuffer=%u low_water=%zu "
             "underruns=%lu silence_blocks=%lu producer_gap_max_ms=%llu\n",
             pb->id, pb->start_prebuffer, pb->ring_min_play,
             (unsigned long)pb->underrun_count,
             (unsigned long)pb->silence_blocks, pb->max_pcm_gap_ms);
    }
  return ret;
}

uint64_t robot_audio_playback_cancel(void)
{
  struct rv_playback_s *pb = &g_pb;
  uint64_t completed = 0;
  if (!pb->opened) return 0;
  pb->stopped = 1;
  rv_signal_all(pb);
  if (pb->i2s != NULL && pb->i2s_started)
    {
      rv_stop_i2s(pb);
      rv_signal_all(pb);
    }
  if (rv_lock(pb) >= 0)
    {
      completed = pb->completed_source_bytes;
      rv_unlock(pb);
    }
  return completed;
}

void robot_audio_playback_close(void)
{
  struct rv_playback_s *pb = &g_pb;

  if (!pb->opened)
    {
      return;
    }

  if (pb->worker_started)
    {
      pb->stopped = 1;
      rv_signal_all(pb);

      if (pb->i2s != NULL && pb->i2s_started)
        {
          rv_stop_i2s(pb);
          rv_signal_all(pb);
        }

      pthread_join(pb->worker, NULL);
      pb->worker_started = 0;
    }

  (void)rv_drain(pb);

  if (pb->i2s != NULL && pb->i2s_started)
    {
      rv_stop_i2s(pb);
    }

  if (!pb->finished)
    RV_VERBOSE("[RV-PB] cancelled id=%u cached=%zu consumed=%zu dma_start_ms=%llu result=%d\n",
           pb->id, pb->total_written, pb->total_read, pb->dma_start_ms, rv_get_result(pb));
  rv_destroy(pb);
  memset(pb, 0, sizeof(*pb));
}
