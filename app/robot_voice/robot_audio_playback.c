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

#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/clock.h>
#include <nuttx/semaphore.h>

#include <arch/board/board.h>

#include "robot_audio_playback.h"
#include "robot_voice_config.h"

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
  ((RV_SOURCE_BLOCK_BYTES / sizeof(int16_t)) * RV_WIRE_BYTES_PER_SAMPLE)
/*
 * Must match contest_i2s.c:
 *   CONTEST_I2S_TX_RING_SLOTS = 16
 *   CONTEST_I2S_TX_SLOT_BYTES = 2048
 *
 * The lower half does not start its immutable DMA ring until all sixteen slots
 * have been bound once.  Therefore the upper layer must be able to submit all
 * sixteen initial requests without waiting for a completion.
 */
#define RV_LOWER_DMA_RING_SLOTS      16
#define RV_LOWER_DMA_SLOT_BYTES      2048
#define RV_MAX_INFLIGHT              RV_LOWER_DMA_RING_SLOTS
#define RV_SEND_TIMEOUT_MS           1000

/* F7J transient producer-gap grace.
 *
 * One 1024-byte PCM16 source block is ~21.3 ms.  With sixteen immutable DMA
 * slots, once a slot is released there is far more than 15 ms before that
 * same physical slot is needed again.  Use part of that safety window to
 * wait for late Media PCM instead of inserting an audible silent block.
 */
#define RV_UNDERRUN_GRACE_US          500000
#define RV_UNDERRUN_POLL_US            2000

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
 *   Ring Buffer     = 512 KiB
 *   Start Prebuffer = 128 KiB
 *   Rebuffer        =  48 KiB (legacy threshold; F7I does not rebuffer once PLAYING)
 *
 * The playback worker is created at open time and waits in PREBUFFER while
 * the PCM producer fills the ring.
 *
 * F7K invariant for the immutable contest I2S DMA ring:
 * once PLAYING has started, the worker must keep every released DMA slot
 * refreshed.  If the producer temporarily runs dry, submit silence instead
 * of entering REBUFFER and leaving old slot payloads circulating in hardware.
 */
#if RV_FULL_CACHE_AB_TEST
#  define RV_RING_BYTES             (2 * 1024 * 1024)
#  define RV_START_PREBUFFER_BYTES  RV_RING_BYTES
#else
#  define RV_RING_BYTES             (512 * 1024)
#  define RV_START_PREBUFFER_BYTES  (128 * 1024)
#endif
#define RV_REBUFFER_BYTES           (48 * 1024)

/* The proven version used priority 120.  Keep that real-time bias.  Use a
 * larger stack than the old 4 KiB implementation because this contest-local
 * wrapper has additional logging/call depth, while the DMA cadence remains
 * identical to the proven code.
 */
#define RV_WORKER_STACK             (16 * 1024)
#define RV_WORKER_PRIORITY          120

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
  uint32_t underrun_count;
  size_t ring_min_play;
  size_t ring_max_play;

  struct rv_ring_s ring;
  struct rv_slot_s slots[RV_MAX_INFLIGHT];

  size_t total_written;
  uint8_t diagnostic_tone_phase;
  int opened;
  int finished;
};

static struct rv_playback_s g_pb;
static int g_stop_on_short_underrun;

static void rv_signal_all(struct rv_playback_s *pb);

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

  space = RV_RING_BYTES - pb->ring.count;
  if (len > space)
    {
      size_t buffered = pb->ring.count;
      rv_unlock(pb);

      printf("[RV-PB] PCM cache full buffered=%zu incoming=%zu capacity=%u\n",
             buffered, len, RV_RING_BYTES);
      return -ENOSPC;
    }

  first = RV_RING_BYTES - pb->ring.head;
  if (first > len)
    {
      first = len;
    }

  memcpy(pb->ring.data + pb->ring.head, src, first);
  pb->ring.head = (pb->ring.head + first) % RV_RING_BYTES;
  pb->ring.count += first;

  second = len - first;
  if (second > 0)
    {
      memcpy(pb->ring.data + pb->ring.head, src + first, second);
      pb->ring.head = (pb->ring.head + second) % RV_RING_BYTES;
      pb->ring.count += second;
    }

  if (pb->ring.count > pb->ring_max_play)
    {
      pb->ring_max_play = pb->ring.count;
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

  first = RV_RING_BYTES - pb->ring.tail;
  if (first > n)
    {
      first = n;
    }

  if (first > 0)
    {
      memcpy(dst, pb->ring.data + pb->ring.tail, first);
      pb->ring.tail = (pb->ring.tail + first) % RV_RING_BYTES;
      pb->ring.count -= first;
    }

  if (first < n)
    {
      size_t second = n - first;

      memcpy(dst + first, pb->ring.data + pb->ring.tail, second);
      pb->ring.tail = (pb->ring.tail + second) % RV_RING_BYTES;
      pb->ring.count -= second;
    }

  if ((pb->state == RV_STATE_PLAYING || pb->state == RV_STATE_DRAINING) &&
      pb->ring.count < pb->ring_min_play)
    {
      pb->ring_min_play = pb->ring.count;
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
  (void)apb;

  if (slot == NULL || slot->pb == NULL)
    {
      return;
    }

  pb = slot->pb;

  if (rv_lock(pb) == 0)
    {
      slot->result = result;
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
                         const uint8_t *src, size_t len)
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

      printf("[RV-PB] fixed APB slot=%u ptr=%p bytes=%u\n",
             slot->index, slot->apb->samp, RV_WIRE_BLOCK_BYTES);
    }

  apb = slot->apb;

  /*
   * Legacy/TTS short blocks still use zero padding.  Media terminal tails
   * do not pad beyond tx_len: the lower-half descriptor itself is shortened.
   */
  if (tx_len < RV_WIRE_BLOCK_BYTES && !pb->stop_on_short_underrun)
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
  if (pb->stop_on_short_underrun && tx_len < RV_WIRE_BLOCK_BYTES)
    {
      apb->nbytes = tx_len;
      apb->nsamples = frames;
      printf("[RV-PB] terminal tail source=%zu wire=%zu samples=%zu\n",
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

  slot->result = -EINPROGRESS;
  slot->inflight = 1;
  slot->submit_count++;
  pb->i2s_inflight++;
  rv_unlock(pb);

  ret = I2S_SEND(pb->i2s, apb, rv_i2s_done, slot,
                 MSEC2TICK(RV_SEND_TIMEOUT_MS));
  if (ret < 0)
    {
      if (rv_lock(pb) == 0)
        {
          slot->result = ret;
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
      if (available >= threshold)
        {
          return 0;
        }

      if (pb->eos || pb->stopped)
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

static size_t rv_wait_transient_gap(struct rv_playback_s *pb,
                                    size_t initial_available)
{
  size_t available = initial_available;
  unsigned int waited = 0;

  while (!pb->eos && !pb->stopped &&
         available < RV_SOURCE_BLOCK_BYTES &&
         waited < RV_UNDERRUN_GRACE_US)
    {
      int result = rv_get_result(pb);

      if (result < 0)
        {
          break;
        }

      usleep(RV_UNDERRUN_POLL_US);
      waited += RV_UNDERRUN_POLL_US;
      available = rv_ring_count(pb);
    }

  if (waited > 0 &&
      available >= RV_SOURCE_BLOCK_BYTES)
    {
      printf("[RV-PB] transient gap recovered wait_us=%u buffered=%zu\n",
             waited, available);
    }

  return available;
}

static void *rv_worker(void *arg)
{
  struct rv_playback_s *pb = arg;
  uint8_t block[RV_SOURCE_BLOCK_BYTES];
  unsigned int slot_index = 0;
  int primed_slots = 0;
  int ret = 0;
  bool terminal_tail = false;

  printf("[RV-PB] worker start source_rate=%u wire_rate=%u "
         "source=mono16 wire=mono-left32 gain_q16=%u resample=none "
         "source_block=%u wire_block=%u slots=%u priority=%u "
         "underrun_policy=grace-then-silence grace_us=%u sample_precision=full-q16\n",
         RV_TTS_SAMPLE_RATE, RV_WIRE_SAMPLE_RATE, RV_OUTPUT_GAIN_Q16,
         RV_SOURCE_BLOCK_BYTES, RV_WIRE_BLOCK_BYTES,
         RV_MAX_INFLIGHT, RV_WORKER_PRIORITY, RV_UNDERRUN_GRACE_US);

  pb->state = RV_STATE_PREBUFFER;

  while (!pb->stopped)
    {
      struct rv_slot_s *slot;

      terminal_tail = false;
      size_t available;
      size_t want;
      size_t got;
      bool prime_silence = false;

      ret = rv_get_result(pb);
      if (ret < 0)
        {
          printf("[RV-PB] I2S worker error=%d\n", ret);
          break;
        }

      if (pb->state == RV_STATE_PREBUFFER || pb->state == RV_STATE_REBUFFER)
        {
          size_t threshold = pb->state == RV_STATE_PREBUFFER ?
                             RV_START_PREBUFFER_BYTES : RV_REBUFFER_BYTES;
          const char *from = pb->state == RV_STATE_PREBUFFER ?
                             "PREBUFFER" : "REBUFFER";

          ret = rv_wait_buffer(pb, threshold);
          if (ret < 0)
            {
              break;
            }

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

              printf("[RV-PB] %s -> PLAYING buffered=%zu inflight=%d "
                     "dma_window=%u blocks (~%u ms)\n",
                     from, available, rv_get_inflight(pb),
                     RV_MAX_INFLIGHT,
                     (unsigned int)(RV_MAX_INFLIGHT *
                       RV_SOURCE_BLOCK_BYTES * 1000 /
                       (RV_TTS_SAMPLE_RATE * sizeof(int16_t))));
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

      /* Deeper DMA submit window:
       * prime all fixed slots, then wait/refill round-robin.
       *
       * Sixteen 1024-byte source blocks provide about 341 ms of 24 kHz mono
       * PCM queue depth. Each source block remains 1024 bytes on the wire.
       * while giving HPWORK/callback/worker jitter twice as much time before
       * a zero-filled slot can wrap back to hardware.
       */
      if (primed_slots >= RV_MAX_INFLIGHT)
        {
          ret = rv_wait_slot(pb, slot);
          if (ret < 0)
            {
              break;
            }
        }

      available = rv_ring_count(pb);

      /*
       * Streaming TTS is allowed a producer-jitter grace window.
       *
       * Finite-file Media playback is different: the bridge enables
       * stop_on_short_underrun specifically so the last partial PCM block is
       * the terminal tail.  Waiting here while the immutable DMA ring keeps
       * running can replay already-consumed tail slots several times.
       *
       * Therefore exact-tail mode never waits for more PCM once less than one
       * complete source block remains: play the remaining aligned bytes once,
       * mark that descriptor terminal (next=NULL), then stop after FIFO drain.
       */
      if (!pb->eos &&
          available < RV_SOURCE_BLOCK_BYTES &&
          !pb->stop_on_short_underrun)
        {
          available = rv_wait_transient_gap(pb, available);
        }
      else if (!pb->eos &&
               available < RV_SOURCE_BLOCK_BYTES &&
               pb->stop_on_short_underrun)
        {
          printf("[RV-PB] media exact-tail buffered=%zu -> no grace wait\n",
                 available);
        }

      if (available >= RV_SOURCE_BLOCK_BYTES)
        {
          want = RV_SOURCE_BLOCK_BYTES;
        }
      else if (pb->eos && available > 0)
        {
          want = available & ~(size_t)1;
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
              printf("[RV-PB] media EOF empty -> DRAINING immediately\n");
              pb->eos_silence_rounds = 0;
              pb->state = RV_STATE_DRAINING;
              break;
            }

          if (pb->eos_silence_rounds < RV_MAX_INFLIGHT)
            {
              memset(block, 0, sizeof(block));
              want = RV_SOURCE_BLOCK_BYTES;
              prime_silence = true;
              pb->eos_silence_rounds++;
              printf("[RV-PB] EOS silence fill slot=%u round=%d/%u\n",
                     slot_index, pb->eos_silence_rounds, RV_MAX_INFLIGHT);
            }
          else
            {
              printf("[RV-PB] EOS DMA slots sanitized -> DRAINING\n");
              pb->eos_silence_rounds = 0;
              pb->state = RV_STATE_DRAINING;
              break;
            }
        }

      else
        {
          /*
           * F7I: NEVER stop feeding the immutable cyclic DMA ring after
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
           * repetition.  Initial PREBUFFER behavior is unchanged.
           */
          pb->underrun_count++;

          if (available >= sizeof(int16_t))
            {
              want = available;
              if (want > RV_SOURCE_BLOCK_BYTES)
                {
                  want = RV_SOURCE_BLOCK_BYTES;
                }

              want &= ~(size_t)1;

              if (pb->stop_on_short_underrun)
                {
                  terminal_tail = true;
                  pb->eos = 1;
                  printf("[RV-PB] underrun #%lu short-fill=%zu "
                         "buffered=%zu -> play terminal tail ONCE\n",
                         (unsigned long)pb->underrun_count,
                         want, available);
                }
              else if (pb->underrun_count <= 4 ||
                       (pb->underrun_count % 32) == 0)
                {
                  printf("[RV-PB] underrun #%lu short-fill=%zu "
                         "buffered=%zu -> silence-pad slot\n",
                         (unsigned long)pb->underrun_count,
                         want, available);
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

              if (pb->stop_on_short_underrun)
                {
                  printf("[RV-PB] underrun #%lu buffered=%zu "
                         "-> no tail, stop now\n",
                         (unsigned long)pb->underrun_count, available);
                  pb->eos = 1;
                  pb->stopped = 1;
                  pb->terminal_tail_done = 1;

                  if (pb->i2s != NULL && pb->i2s_started)
                    {
                      pb->i2s_started = 0;
                      I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
                    }

                  rv_signal_all(pb);
                  break;
                }

              if (pb->underrun_count <= 4 ||
                  (pb->underrun_count % 32) == 0)
                {
                  printf("[RV-PB] underrun #%lu buffered=%zu "
                         "-> silent DMA slot\n",
                         (unsigned long)pb->underrun_count, available);
                }
            }
        }

      if (prime_silence)
        {
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

      ret = rv_queue_slot(pb, slot, block, got);
      if (ret < 0)
        {
          if (ret != -ECANCELED)
            {
              printf("[RV-PB] queue slot=%u failed=%d\n",
                     slot_index, ret);
            }
          break;
        }

      if (terminal_tail)
        {
          int tail_ret = rv_wait_slot(pb, slot);

          if (tail_ret < 0 && tail_ret != -ECANCELED && ret >= 0)
            {
              ret = tail_ret;
            }

          printf("[RV-PB] terminal tail complete source=%zu -> STOP\n", got);

          pb->terminal_tail_done = 1;
          pb->stopped = 1;
          pb->state = RV_STATE_STOPPED;

          if (pb->i2s != NULL && pb->i2s_started)
            {
              pb->i2s_started = 0;
              I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
            }

          rv_signal_all(pb);
          break;
        }

      if (primed_slots < RV_MAX_INFLIGHT)
        {
          primed_slots++;
        }

      slot_index = (slot_index + 1) % RV_MAX_INFLIGHT;
    }

  /* Do not stop I2S before the logical slots have completed. */
  {
    unsigned int i;

    for (i = 0; i < RV_MAX_INFLIGHT; i++)
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

  printf("[RV-PB] worker exit buffered=%zu inflight=%d underruns=%lu "
         "result=%d eos=%d stopped=%d "
         "slots=%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu,"
         "%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu\n",
         rv_ring_count(pb), rv_get_inflight(pb),
         (unsigned long)pb->underrun_count, rv_get_result(pb),
         pb->eos, pb->stopped,
         (unsigned long)pb->slots[0].done_count,
         (unsigned long)pb->slots[0].submit_count,
         (unsigned long)pb->slots[1].done_count,
         (unsigned long)pb->slots[1].submit_count,
         (unsigned long)pb->slots[2].done_count,
         (unsigned long)pb->slots[2].submit_count,
         (unsigned long)pb->slots[3].done_count,
         (unsigned long)pb->slots[3].submit_count,
         (unsigned long)pb->slots[4].done_count,
         (unsigned long)pb->slots[4].submit_count,
         (unsigned long)pb->slots[5].done_count,
         (unsigned long)pb->slots[5].submit_count,
         (unsigned long)pb->slots[6].done_count,
         (unsigned long)pb->slots[6].submit_count,
         (unsigned long)pb->slots[7].done_count,
         (unsigned long)pb->slots[7].submit_count);

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

static void rv_destroy(struct rv_playback_s *pb)
{
  unsigned int i;

  for (i = 0; i < RV_MAX_INFLIGHT; i++)
    {
      if (pb->slots[i].apb != NULL)
        {
          apb_free(pb->slots[i].apb);
          pb->slots[i].apb = NULL;
        }

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
  struct sched_param param;
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
      printf("[RV-PB] I2S wire rate request=%u actual=%d\n",
             RV_WIRE_SAMPLE_RATE, actual_rate);
      ret = actual_rate < 0 ? actual_rate : -ENOTSUP;
      goto fail;
    }

  actual_width = (int)I2S_TXDATAWIDTH(pb->i2s, RV_WIRE_BITS);
  if (actual_width != RV_WIRE_BITS)
    {
      printf("[RV-PB] I2S width request=%u actual=%d\n",
             RV_WIRE_BITS, actual_width);
      ret = actual_width < 0 ? actual_width : -ENOTSUP;
      goto fail;
    }

  ret = I2S_IOCTL(pb->i2s, AUDIOIOC_START, 0);
  if (ret < 0)
    {
      printf("[RV-PB] AUDIOIOC_START failed=%d\n", ret);
      goto fail;
    }

  pb->i2s_started = 1;
  pb->i2s_result = 0;
  pb->state = RV_STATE_PREBUFFER;
  pb->ring_min_play = (size_t)-1;
  pb->ring_max_play = 0;
  pb->eos_silence_rounds = 0;

  printf("[RV-PB] I2S ready source=%uHz/mono/%ubit "
         "wire=%dHz/%uch/%dbit mono-left "
         "source_block=%u wire_dma=%u ring_slots=%u "
         "prebuffer=%u rebuffer=%u\n",
         RV_TTS_SAMPLE_RATE, RV_TTS_BITS,
         actual_rate, RV_WIRE_CHANNELS, actual_width,
         RV_SOURCE_BLOCK_BYTES, RV_WIRE_BLOCK_BYTES,
         RV_MAX_INFLIGHT,
         RV_START_PREBUFFER_BYTES, RV_REBUFFER_BYTES);

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      ret = -ret;
      goto fail_started;
    }

  ret = pthread_attr_setstacksize(&attr, RV_WORKER_STACK);
  if (ret != 0)
    {
      pthread_attr_destroy(&attr);
      ret = -ret;
      goto fail_started;
    }

  memset(&param, 0, sizeof(param));
  param.sched_priority = RV_WORKER_PRIORITY;
  (void)pthread_attr_setschedparam(&attr, &param);

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
      pb->i2s_started = 0;
      I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
    }

fail:
  pb->i2s = NULL;
  return ret;
}

void robot_audio_playback_set_stop_on_short_underrun(bool enable)
{
  g_stop_on_short_underrun = enable ? 1 : 0;
  printf("[RV-PB] short-tail policy=%s\n",
         enable ? "play-real-tail-then-stop" : "normal");
}

int robot_audio_playback_open(void)
{
  struct rv_playback_s *pb = &g_pb;
  unsigned int i;
  int ret;

  if (pb->opened)
    {
      return -EBUSY;
    }

  memset(pb, 0, sizeof(*pb));
  pb->stop_on_short_underrun = g_stop_on_short_underrun;

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

  pb->ring.data = malloc(RV_RING_BYTES);
  if (pb->ring.data == NULL)
    {
      printf("[RV-PB] ring alloc failed bytes=%u mode=%s\n",
             RV_RING_BYTES,
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

  /*
   * Start the dedicated playback consumer now.  It remains in PREBUFFER until
   * at least RV_START_PREBUFFER_BYTES of decoded PCM are available (or EOS).
   * The TTS callback is only a producer into the ring.
   */
  ret = rv_start_i2s_playback(pb);
  if (ret < 0)
    {
      rv_destroy(pb);
      memset(pb, 0, sizeof(*pb));
      return ret;
    }

  printf("[RV-DIAG] mode=%s output=%s tone_peak=%u "
         "source_bytes_per_sec=%u wire_bytes_per_sec=%u\n",
         RV_FULL_CACHE_AB_TEST ? "full-cache" : "streaming",
         RV_I2S_OUTPUT_TONE_TEST ? "tone-1khz" : "tts-pcm",
         RV_I2S_OUTPUT_TONE_PEAK,
         RV_TTS_SAMPLE_RATE * RV_TTS_SOURCE_CHANNELS *
           (RV_TTS_BITS / 8),
         RV_WIRE_SAMPLE_RATE * RV_WIRE_BYTES_PER_SAMPLE);
  printf("[RV-PB] ring open capacity=%u start=%u rebuffer=%u "
         "source=%uHz/mono/%ubit wire=%uHz/mono-left/%ubit resample=none\n",
         RV_RING_BYTES, RV_START_PREBUFFER_BYTES, RV_REBUFFER_BYTES,
         RV_TTS_SAMPLE_RATE, RV_TTS_BITS,
         RV_WIRE_SAMPLE_RATE, RV_WIRE_BITS);
  return 0;
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
      space = RV_RING_BYTES - buffered;
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
      pb->total_written += n;
    }

  return 0;
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

  /*
   * Producer EOS: wake the consumer.  If less than a normal prebuffer/block
   * remains, the worker is allowed to drain that final aligned PCM tail.
   */
  pb->eos = 1;
  printf("[RV-PB] producer EOS buffered=%zu total_pcm=%zu\n",
         rv_ring_count(pb), pb->total_written);
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
      pb->i2s_started = 0;
      I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
    }

  pb->finished = 1;

  printf("[RV-PB] PLAY_DONE total_pcm=%zu expected_ms=%zu "
         "underruns=%lu result=%d ring_min=%zu ring_max=%zu "
         "slots=%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu,"
         "%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu\n",
         pb->total_written,
         pb->total_written * 1000 /
           (RV_TTS_SAMPLE_RATE * sizeof(int16_t)),
         (unsigned long)pb->underrun_count,
         ret,
         pb->ring_min_play == (size_t)-1 ? 0 : pb->ring_min_play,
         pb->ring_max_play,
         (unsigned long)pb->slots[0].done_count,
         (unsigned long)pb->slots[0].submit_count,
         (unsigned long)pb->slots[1].done_count,
         (unsigned long)pb->slots[1].submit_count,
         (unsigned long)pb->slots[2].done_count,
         (unsigned long)pb->slots[2].submit_count,
         (unsigned long)pb->slots[3].done_count,
         (unsigned long)pb->slots[3].submit_count,
         (unsigned long)pb->slots[4].done_count,
         (unsigned long)pb->slots[4].submit_count,
         (unsigned long)pb->slots[5].done_count,
         (unsigned long)pb->slots[5].submit_count,
         (unsigned long)pb->slots[6].done_count,
         (unsigned long)pb->slots[6].submit_count,
         (unsigned long)pb->slots[7].done_count,
         (unsigned long)pb->slots[7].submit_count);
  return ret;
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
          pb->i2s_started = 0;
          I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
          rv_signal_all(pb);
        }

      pthread_join(pb->worker, NULL);
      pb->worker_started = 0;
    }

  (void)rv_drain(pb);

  if (pb->i2s != NULL && pb->i2s_started)
    {
      pb->i2s_started = 0;
      I2S_IOCTL(pb->i2s, AUDIOIOC_STOP, 0);
    }

  printf("[RV-PB] close total_pcm=%zu finished=%d underruns=%lu result=%d "
         "slots=%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu,"
         "%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu\n",
         pb->total_written, pb->finished,
         (unsigned long)pb->underrun_count,
         rv_get_result(pb),
         (unsigned long)pb->slots[0].done_count,
         (unsigned long)pb->slots[0].submit_count,
         (unsigned long)pb->slots[1].done_count,
         (unsigned long)pb->slots[1].submit_count,
         (unsigned long)pb->slots[2].done_count,
         (unsigned long)pb->slots[2].submit_count,
         (unsigned long)pb->slots[3].done_count,
         (unsigned long)pb->slots[3].submit_count,
         (unsigned long)pb->slots[4].done_count,
         (unsigned long)pb->slots[4].submit_count,
         (unsigned long)pb->slots[5].done_count,
         (unsigned long)pb->slots[5].submit_count,
         (unsigned long)pb->slots[6].done_count,
         (unsigned long)pb->slots[6].submit_count,
         (unsigned long)pb->slots[7].done_count,
         (unsigned long)pb->slots[7].submit_count);

  rv_destroy(pb);
  memset(pb, 0, sizeof(*pb));
}
