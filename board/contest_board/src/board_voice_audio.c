/****************************************************************************
 * board/contest_board/src/board_voice_audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native Media PCM bridge with DMA-backed buffer ownership:
 *
 *   FFmpeg/Media decoded PCM16LE 24 kHz mono
 *       -> /dev/audio/pcm0p
 *       -> robot_audio_playback_write()
 *       -> proven contest I2S1 / MAX98357 playback path
 *
 * The NuttX Audio device in this file intentionally does NOT use audio_i2s
 * or pcm_decode.  Media owns decoding; robot_audio_playback owns the single
 * verified hardware playback path.
 *
 * A copied period is not a played period. The bridge returns APBs on the
 * decoded PCM timeline so Media can continue feeding the private playback
 * ring; I2S callbacks remain the source of actual speaker progress. Never
 * infer EOF from PAUSE or a network gap. FINAL drains; STOP/FLUSH abort and
 * return all owned APBs.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>
#include <nuttx/clock.h>
#include <nuttx/semaphore.h>

#include <arch/board/board.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CONTEST_MEDIA_RATE          24000
#define CONTEST_MEDIA_CHANNELS      1
#define CONTEST_MEDIA_BITS          16
#define CONTEST_MEDIA_NBUFFERS      6
#define CONTEST_MEDIA_BUFFER_BYTES  4096
#define CONTEST_MEDIA_BYTES_PER_SEC \
  (CONTEST_MEDIA_RATE * CONTEST_MEDIA_CHANNELS * (CONTEST_MEDIA_BITS / 8))

/* Pending APBs are bounded by the client's advertised six-period window.
 * The extra slots permit orderly cancellation without allocating per APB. */
#define CONTEST_MEDIA_PACE_QUEUE      16
#ifndef CONTEST_MEDIA_PACE_STACK
#  define CONTEST_MEDIA_PACE_STACK     8192
#endif
#define CONTEST_MEDIA_PACE_SLICE_US   5000
#define CONTEST_MEDIA_HEALTH_POLL_MS  100
#ifndef CONTEST_MEDIA_STALL_TIMEOUT_MS
#  define CONTEST_MEDIA_STALL_TIMEOUT_MS 12000
#endif

/* Music volume is scaled only in the Media PCM bridge.  TTS uses the
 * application playback path directly and is intentionally unaffected. */
#define CONTEST_MEDIA_MUSIC_GAIN_NUM  9
#define CONTEST_MEDIA_MUSIC_GAIN_DEN  32
#define CONTEST_MEDIA_GAIN_CHUNK      512

/****************************************************************************
 * Weak playback backend
 *
 * robot_audio_playback.c is contest-local application code and is already
 * linked by robot_voice.  Weak declarations keep board bring-up independent:
 * if that application is not present, the audio device still registers, but
 * START/ENQUEUE return -ENOSYS instead of dereferencing a missing symbol.
 ****************************************************************************/

extern int robot_audio_playback_open_native_media(void) __attribute__((weak));
extern int robot_audio_playback_start_native_media(void) __attribute__((weak));
extern int robot_audio_playback_pause_native_media(bool paused)
  __attribute__((weak));
extern int robot_audio_playback_end_input(void) __attribute__((weak));
extern int robot_audio_playback_progress(uint64_t *completed, uint64_t *pending)
  __attribute__((weak));
extern int robot_audio_playback_write(const uint8_t *pcm, size_t len)
  __attribute__((weak));
extern int robot_audio_playback_finish(void) __attribute__((weak));
extern void robot_audio_playback_close(void) __attribute__((weak));

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct contest_media_pending_s
{
  FAR struct ap_buffer_s *apb;
  size_t bytes;
  uint64_t endpoint;
  bool final;
};

struct contest_media_sink_s
{
  struct audio_lowerhalf_s dev;
  bool configured;
  bool playback_open;
  bool completion_sent;
  bool started;
  bool paused;
  bool eos;
  int stream_error;
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bits;
  size_t total_pcm;
  size_t dequeued_pcm;
  uint32_t started_ms;
  uint32_t last_pcm_ms;
  uint64_t health_completed;
  uint32_t health_progress_ms;
  bool health_progress_valid;
  unsigned int enqueue_count;
  unsigned int dequeue_count;
  unsigned int position_query_count;

  /* APB ownership follows the Media PCM timeline.  The copied PCM remains
   * owned by the native playback ring until its I2S DMA completion. */
  sem_t pace_lock;
  sem_t pace_data;
  sem_t pace_empty;
  sem_t playback_lock;
  sem_t enqueue_lock;
  pthread_t pace_worker;
  bool pace_worker_started;
  bool pace_abort;
  unsigned int pace_head;
  unsigned int pace_tail;
  unsigned int pace_count;
  bool pace_busy;
  bool pace_clock_valid;
  uint64_t pace_deadline_us;
  struct contest_media_pending_s pending[CONTEST_MEDIA_PACE_QUEUE];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int contest_media_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                                 FAR struct audio_caps_s *caps);

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int contest_media_configure(FAR struct audio_lowerhalf_s *dev,
                                   FAR void *session,
                                   FAR const struct audio_caps_s *caps);
static int contest_media_start(FAR struct audio_lowerhalf_s *dev,
                               FAR void *session);
#  ifndef CONFIG_AUDIO_EXCLUDE_STOP
static int contest_media_stop(FAR struct audio_lowerhalf_s *dev,
                              FAR void *session);
#  endif
#  ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
static int contest_media_pause(FAR struct audio_lowerhalf_s *dev,
                               FAR void *session);
static int contest_media_resume(FAR struct audio_lowerhalf_s *dev,
                                FAR void *session);
#  endif
static int contest_media_reserve(FAR struct audio_lowerhalf_s *dev,
                                 FAR void **session);
static int contest_media_release(FAR struct audio_lowerhalf_s *dev,
                                 FAR void *session);
#else
static int contest_media_configure(FAR struct audio_lowerhalf_s *dev,
                                   FAR const struct audio_caps_s *caps);
static int contest_media_start(FAR struct audio_lowerhalf_s *dev);
#  ifndef CONFIG_AUDIO_EXCLUDE_STOP
static int contest_media_stop(FAR struct audio_lowerhalf_s *dev);
#  endif
#  ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
static int contest_media_pause(FAR struct audio_lowerhalf_s *dev);
static int contest_media_resume(FAR struct audio_lowerhalf_s *dev);
#  endif
static int contest_media_reserve(FAR struct audio_lowerhalf_s *dev);
static int contest_media_release(FAR struct audio_lowerhalf_s *dev);
#endif

static int contest_media_shutdown(FAR struct audio_lowerhalf_s *dev);
static int contest_media_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                       FAR struct ap_buffer_s *apb);
static int contest_media_ioctl(FAR struct audio_lowerhalf_s *dev,
                               int cmd, unsigned long arg);

static int contest_media_pacer_initialize(FAR struct contest_media_sink_s *sink);
static int contest_media_queue_apb(FAR struct contest_media_sink_s *sink,
                                   FAR struct ap_buffer_s *apb,
                                   size_t bytes, bool final);
static int contest_media_abort_pending(FAR struct contest_media_sink_s *sink);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct audio_ops_s g_contest_media_ops =
{
  contest_media_getcaps,       /* getcaps        */
  contest_media_configure,     /* configure      */
  contest_media_shutdown,      /* shutdown       */
  contest_media_start,         /* start          */
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  contest_media_stop,          /* stop           */
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
  contest_media_pause,         /* pause          */
  contest_media_resume,        /* resume         */
#endif
  NULL,                        /* allocbuffer     */
  NULL,                        /* freebuffer      */
  contest_media_enqueuebuffer, /* enqueue_buffer */
  NULL,                        /* cancel_buffer   */
  contest_media_ioctl,         /* ioctl           */
  NULL,                        /* read            */
  NULL,                        /* write           */
  contest_media_reserve,       /* reserve         */
  contest_media_release        /* release         */
};

static struct contest_media_sink_s g_contest_media_sink =
{
  .dev =
  {
    .ops = &g_contest_media_ops,
  },
};
static bool g_contest_media_registered;

static uint64_t contest_media_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool contest_media_backend_available(void)
{
  return robot_audio_playback_open_native_media != NULL &&
         robot_audio_playback_start_native_media != NULL &&
         robot_audio_playback_pause_native_media != NULL &&
         robot_audio_playback_end_input != NULL &&
         robot_audio_playback_progress != NULL &&
         robot_audio_playback_write != NULL &&
         robot_audio_playback_finish != NULL &&
         robot_audio_playback_close != NULL;
}

static int contest_media_write_music_pcm(FAR const uint8_t *pcm, size_t len)
{
  uint8_t scaled[CONTEST_MEDIA_GAIN_CHUNK];
  size_t offset = 0;

  if (pcm == NULL || len == 0 || (len & 1) != 0)
    {
      return -EINVAL;
    }

  while (offset < len)
    {
      size_t chunk = len - offset;
      size_t i;
      int ret;

      if (chunk > sizeof(scaled))
        {
          chunk = sizeof(scaled);
        }

      chunk &= ~(size_t)1u;
      if (chunk == 0)
        {
          return -EINVAL;
        }

      for (i = 0; i < chunk; i += sizeof(int16_t))
        {
          uint16_t raw = (uint16_t)pcm[offset + i] |
                         ((uint16_t)pcm[offset + i + 1] << 8);
          int16_t sample = (int16_t)raw;
          int16_t quieter =
            (int16_t)(((int32_t)sample * CONTEST_MEDIA_MUSIC_GAIN_NUM) /
                      CONTEST_MEDIA_MUSIC_GAIN_DEN);

          scaled[i] = (uint8_t)((uint16_t)quieter & 0xff);
          scaled[i + 1] =
            (uint8_t)(((uint16_t)quieter >> 8) & 0xff);
        }

      ret = robot_audio_playback_write(scaled, chunk);
      if (ret < 0)
        {
          return ret;
        }

      offset += chunk;
    }

  return OK;
}

static int contest_media_open_playback(FAR struct contest_media_sink_s *sink)
{
  int ret;

  ret = nxsem_wait_uninterruptible(&sink->playback_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (sink->playback_open)
    {
      nxsem_post(&sink->playback_lock);
      return OK;
    }

  if (!sink->configured)
    {
      nxsem_post(&sink->playback_lock);
      syslog(LOG_ERR, "[MEDIA-BRIDGE] start rejected: not configured\n");
      return -EINVAL;
    }

  if (!contest_media_backend_available())
    {
      nxsem_post(&sink->playback_lock);
      syslog(LOG_ERR,
             "[MEDIA-BRIDGE] robot_audio_playback backend unavailable\n");
      return -ENOSYS;
    }

  ret = robot_audio_playback_open_native_media();
  if (ret < 0)
    {
      nxsem_post(&sink->playback_lock);
      syslog(LOG_ERR, "[MEDIA-BRIDGE] playback open failed=%d\n", ret);
      return ret;
    }

  sink->playback_open = true;
  __atomic_store_n(&sink->completion_sent, false, __ATOMIC_RELEASE);
  sink->started = false;
  sink->paused = false;
  sink->eos = false;
  sink->stream_error = OK;
  sink->total_pcm = 0;
  sink->dequeued_pcm = 0;
  __atomic_store_n(&sink->started_ms, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&sink->last_pcm_ms, 0, __ATOMIC_RELEASE);
  sink->health_completed = 0;
  sink->health_progress_ms = 0;
  sink->health_progress_valid = false;
  sink->enqueue_count = 0;
  sink->dequeue_count = 0;
  sink->position_query_count = 0;
  sink->pace_clock_valid = false;
  sink->pace_deadline_us = 0;


  nxsem_post(&sink->playback_lock);

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] playback open source=%luHz/%uch/%ubit\n",
         (unsigned long)sink->sample_rate,
         sink->channels, sink->bits);
  return OK;
}

/* Start the contest-local backend only from AUDIOIOC_START.  Media fills
 * several APBs before that command; starting on the first enqueue gives the
 * decoder an 85 ms ring and immediately triggers source-side xrun. */
static int contest_media_start_backend(FAR struct contest_media_sink_s *sink)
{
  int ret;

  ret = nxsem_wait_uninterruptible(&sink->playback_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!sink->playback_open)
    {
      ret = -EINVAL;
    }
  else if (sink->started)
    {
      ret = OK;
    }
  else
    {
      ret = robot_audio_playback_start_native_media();
      if (ret == OK)
        {
          sink->started = true;
          sink->paused = false;
          uint32_t now_ms = (uint32_t)contest_media_now_ms();
          __atomic_store_n(&sink->started_ms, now_ms, __ATOMIC_RELEASE);
          __atomic_store_n(&sink->last_pcm_ms, now_ms, __ATOMIC_RELEASE);
        }
    }

  nxsem_post(&sink->playback_lock);
  return ret;
}

static int contest_media_finish_playback(FAR struct contest_media_sink_s *sink,
                                         bool drain)
{
  int lock_ret;
  int ret = OK;

  lock_ret = nxsem_wait_uninterruptible(&sink->playback_lock);
  if (lock_ret < 0)
    {
      return lock_ret;
    }

  if (!sink->playback_open)
    {
      nxsem_post(&sink->playback_lock);
      return OK;
    }

  if (drain && robot_audio_playback_finish != NULL)
    {
      syslog(LOG_INFO,
             "[MEDIA-BRIDGE] producer EOS -> robot_audio_playback_finish\n");
      ret = robot_audio_playback_finish();
    }

  if (robot_audio_playback_progress != NULL)
    {
      uint64_t completed;
      uint64_t pending;

      if (robot_audio_playback_progress(&completed, &pending) == OK)
        {
          sink->dequeued_pcm = completed;
        }
    }

  if (robot_audio_playback_close != NULL)
    {
      robot_audio_playback_close();
    }

  sink->playback_open = false;
  sink->started = false;
  nxsem_post(&sink->playback_lock);

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] playback close drain=%d pcm=%zu "
         "dequeued_pcm=%zu enqueues=%u dequeues=%u rc=%d\n",
         drain ? 1 : 0, sink->total_pcm, sink->dequeued_pcm,
         sink->enqueue_count, sink->dequeue_count, ret);
  return ret;
}

static void contest_media_dequeue(FAR struct contest_media_sink_s *sink,
                                  FAR struct ap_buffer_s *apb,
                                  uint16_t status)
{
  if (sink->dev.upper == NULL)
    {
      return;
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, status,
                  sink);
#else
  sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, status);
#endif
}

static void contest_media_complete(FAR struct contest_media_sink_s *sink)
{
  if (__atomic_exchange_n(&sink->completion_sent, true, __ATOMIC_ACQ_REL))
    {
      return;
    }

  if (sink->dev.upper == NULL)
    {
      return;
    }

  syslog(LOG_INFO, "[MEDIA-BRIDGE] COMPLETE\n");

  if (sink->stream_error < 0)
    {
#ifdef CONFIG_AUDIO_MULTI_SESSION
      sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_IOERR, NULL,
                       (uint16_t)-sink->stream_error, sink);
#else
      sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_IOERR, NULL,
                       (uint16_t)-sink->stream_error);
#endif
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_COMPLETE, NULL, OK, sink);
#else
  sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_COMPLETE, NULL, OK);
#endif
}

static bool contest_media_pace_aborted(FAR struct contest_media_sink_s *sink)
{
  bool aborted;

  if (nxsem_wait_uninterruptible(&sink->pace_lock) < 0)
    {
      return true;
    }

  aborted = sink->pace_abort;
  nxsem_post(&sink->pace_lock);
  return aborted;
}

/* Snapshot under the lifetime lock; STOP first aborts the queue and waits
 * for this worker, then closes the backend. No polling of a freed ring. */
static int contest_media_progress(FAR struct contest_media_sink_s *sink,
                                  uint64_t *completed, uint64_t *pending,
                                  bool *started)
{
  int ret = nxsem_wait_uninterruptible(&sink->playback_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (started != NULL) *started = sink->started;
  if (!sink->playback_open)
    {
      *completed = sink->dequeued_pcm;
      *pending = 0;
      ret = sink->stream_error;
    }
  else
    {
      ret = robot_audio_playback_progress(completed, pending);
      if (ret == OK)
        {
          sink->dequeued_pcm = *completed;
        }
    }

  nxsem_post(&sink->playback_lock);
  return ret;
}

static uint64_t contest_media_now_us(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000000ULL +
         (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Return Media APBs at the PCM consumption rate.  The native playback ring
 * and I2S DMA continue independently; waiting for each DMA callback here
 * starves adevsink of APBs and makes it report a false xrun after four
 * buffers.  Use one absolute timeline so scheduler delay does not accumulate
 * into audible gaps. */
static void contest_media_pace_delay(FAR struct contest_media_sink_s *sink,
                                     size_t bytes)
{
  uint64_t duration;

  if (bytes == 0)
    {
      return;
    }

  duration = ((uint64_t)bytes * 1000000ULL +
              CONTEST_MEDIA_BYTES_PER_SEC - 1) /
             CONTEST_MEDIA_BYTES_PER_SEC;

  uint64_t now = contest_media_now_us();
  if (!sink->pace_clock_valid || now == 0)
    {
      sink->pace_deadline_us = now + duration;
      sink->pace_clock_valid = true;
    }
  else
    {
      sink->pace_deadline_us += duration;
    }

  for (;;)
    {
      if (contest_media_pace_aborted(sink))
        {
          return;
        }

      now = contest_media_now_us();
      if (now == 0 || now >= sink->pace_deadline_us)
        {
          return;
        }

      uint64_t remain = sink->pace_deadline_us - now;
      unsigned int slice = remain > CONTEST_MEDIA_PACE_SLICE_US ?
                           CONTEST_MEDIA_PACE_SLICE_US :
                           (unsigned int)remain;
      usleep(slice);
    }
}

static bool contest_media_pop_apb(FAR struct contest_media_sink_s *sink,
                                  FAR struct contest_media_pending_s *entry)
{
  if (nxsem_wait_uninterruptible(&sink->pace_lock) < 0)
    {
      return false;
    }

  if (sink->pace_count == 0)
    {
      nxsem_post(&sink->pace_lock);
      return false;
    }

  *entry = sink->pending[sink->pace_head];
  sink->pace_head = (sink->pace_head + 1) % CONTEST_MEDIA_PACE_QUEUE;
  sink->pace_count--;
  sink->pace_busy = true;

  if (sink->pace_count == 0)
    {
      nxsem_post(&sink->pace_empty);
    }

  nxsem_post(&sink->pace_lock);
  return true;
}

/* Detect a stopped I2S DMA worker even when Media has not queued a FINAL
 * buffer.  Without this poll, APBs already copied into the private PCM ring
 * can be returned and the Media thread can then wait forever for an event. */
static void contest_media_check_health(FAR struct contest_media_sink_s *sink)
{
  uint32_t now_ms;
  uint32_t last_pcm_ms;
  uint32_t started_ms;
  uint64_t completed;
  uint64_t pending;
  unsigned int queued;
  bool busy;
  bool started;
  bool progress_stalled;
  bool input_stalled;
  int ret;

  if (__atomic_load_n(&sink->completion_sent, __ATOMIC_ACQUIRE) ||
      sink->stream_error < 0 || contest_media_pace_aborted(sink))
    {
      return;
    }

  ret = contest_media_progress(sink, &completed, &pending, &started);
  if (!started)
    {
      return;
    }

  now_ms = (uint32_t)contest_media_now_ms();
  last_pcm_ms = __atomic_load_n(&sink->last_pcm_ms, __ATOMIC_ACQUIRE);
  started_ms = __atomic_load_n(&sink->started_ms, __ATOMIC_ACQUIRE);

  if (!sink->health_progress_valid || completed != sink->health_completed)
    {
      sink->health_completed = completed;
      sink->health_progress_ms = now_ms;
      sink->health_progress_valid = true;
    }

  if (nxsem_wait_uninterruptible(&sink->pace_lock) < 0)
    {
      return;
    }

  queued = sink->pace_count;
  busy = sink->pace_busy;
  nxsem_post(&sink->pace_lock);

  progress_stalled = now_ms != 0 && sink->health_progress_valid &&
                     (uint32_t)(now_ms - sink->health_progress_ms) >=
                     CONTEST_MEDIA_STALL_TIMEOUT_MS;
  input_stalled = now_ms != 0 &&
                  (uint32_t)(now_ms - (last_pcm_ms ? last_pcm_ms :
                                       started_ms)) >=
                  CONTEST_MEDIA_STALL_TIMEOUT_MS;

  if (ret >= 0 && !sink->eos && queued == 0 && !busy &&
      ((pending == 0 && input_stalled) ||
       (pending != 0 && progress_stalled)))
    {
      ret = -ETIMEDOUT;
      syslog(LOG_ERR,
             "[MEDIA-BRIDGE] no PCM input for %u ms; closing stalled stream\n",
             CONTEST_MEDIA_STALL_TIMEOUT_MS);
    }
  else if (ret >= 0)
    {
      return;
    }

  sink->stream_error = ret;
  sink->eos = true;
  syslog(LOG_ERR,
         "[MEDIA-BRIDGE] playback backend failed while streaming rc=%d; "
         "closing and returning to caller\n", ret);

  if (queued > 0)
    {
      /* The normal worker path returns every copied APB with the error
       * before it emits the terminal IOERR/COMPLETE callbacks. */
      nxsem_post(&sink->pace_data);
      return;
    }

  (void)contest_media_finish_playback(sink, false);
  contest_media_complete(sink);
}

static void *contest_media_pace_worker(FAR void *arg)
{
  FAR struct contest_media_sink_s *sink = arg;

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] DMA completion worker queue=%u\n",
         CONTEST_MEDIA_PACE_QUEUE);

  for (;;)
    {
      struct contest_media_pending_s entry;

      int wait_ret = nxsem_tickwait_uninterruptible(
        &sink->pace_data, MSEC2TICK(CONTEST_MEDIA_HEALTH_POLL_MS));
      if (wait_ret == -ETIMEDOUT || wait_ret == -ETIME)
        {
          contest_media_check_health(sink);
          continue;
        }
      if (wait_ret < 0)
        {
          continue;
        }

      while (contest_media_pop_apb(sink, &entry))
        {
          bool aborted = contest_media_pace_aborted(sink);
          bool dma_done = false;
          uint64_t played = 0;
          uint64_t pending = 0;

          int ret = aborted ? -ECANCELED : sink->stream_error;

          /* Return APBs on the PCM timeline so adevsink can continuously
           * refill its queue.  PCM was already copied into the native ring;
           * the I2S/DMA backend owns actual speaker delivery. */
          if (ret == OK)
            {
              contest_media_pace_delay(sink, entry.bytes);
              if (contest_media_pace_aborted(sink))
                {
                  ret = -ECANCELED;
                }
            }

          /* Keep diagnostics tied to the real backend, while APB ownership
           * follows the Media pacing timeline above. */
          if (contest_media_progress(sink, &played, &pending, NULL) == OK)
            {
              dma_done = played >= entry.endpoint;
            }

          /* STOP can arrive while waiting. Do not turn that cancellation
           * into natural EOS or finish()/drain the queued music. */
          aborted = contest_media_pace_aborted(sink);
          if (ret < 0 && !aborted)
            {
              sink->stream_error = ret;
            }

          contest_media_dequeue(sink, entry.apb,
                                ret < 0 ? (uint16_t)-ret : OK);
          sink->dequeue_count++;

          if (sink->dequeue_count <= 4 ||
              (sink->dequeue_count % 32) == 0 ||
              entry.final)
            {
              syslog(LOG_INFO,
                     "[MEDIA-BRIDGE] PCM dequeue #%u bytes=%zu "
                     "dma_done=%d final=%d played=%zu copied=%zu\n",
                     sink->dequeue_count, entry.bytes,
                     dma_done ? 1 : 0, entry.final ? 1 : 0,
                     (size_t)played, sink->total_pcm);
            }

          if (entry.final && !aborted && ret == OK)
            {
              int finish_ret;

              /* FINAL's actual samples have completed DMA. Finish joins
               * the backend worker and closes before announcing COMPLETE. */
              finish_ret = contest_media_finish_playback(sink, true);
              if (finish_ret < 0) sink->stream_error = finish_ret;
              contest_media_complete(sink);

              if (finish_ret < 0)
                {
                  syslog(LOG_ERR,
                         "[MEDIA-BRIDGE] final drain failed=%d\n", finish_ret);
                }
            }

          /* Mark the APB idle only after its callback and any FINAL handling
           * have finished.  STOP waits for this flag as well as an empty
           * queue, otherwise it can race the current DMA wait operation. */
          if (nxsem_wait_uninterruptible(&sink->pace_lock) == OK)
            {
              bool failed = sink->stream_error < 0 && !sink->pace_abort &&
                            sink->pace_count == 0;
              sink->pace_busy = false;
              nxsem_post(&sink->pace_empty);
              nxsem_post(&sink->pace_lock);
              if (failed)
                {
                  syslog(LOG_ERR, "[MEDIA-BRIDGE] DMA failed rc=%d\n",
                         sink->stream_error);
                  (void)contest_media_finish_playback(sink, false);
                  contest_media_complete(sink);
                }
            }
        }
    }

  return NULL;
}

static int contest_media_queue_apb(FAR struct contest_media_sink_s *sink,
                                   FAR struct ap_buffer_s *apb,
                                   size_t bytes, bool final)
{
  int ret;

  ret = nxsem_wait_uninterruptible(&sink->pace_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (sink->pace_count >= CONTEST_MEDIA_PACE_QUEUE)
    {
      nxsem_post(&sink->pace_lock);
      syslog(LOG_ERR,
             "[MEDIA-BRIDGE] pacer queue full count=%u\n",
             sink->pace_count);
      return -ENOMEM;
    }

  sink->pending[sink->pace_tail].apb = apb;
  sink->pending[sink->pace_tail].bytes = bytes;
  sink->pending[sink->pace_tail].endpoint = sink->total_pcm;
  sink->pending[sink->pace_tail].final = final;
  sink->pace_tail = (sink->pace_tail + 1) % CONTEST_MEDIA_PACE_QUEUE;
  sink->pace_count++;

  nxsem_post(&sink->pace_lock);
  nxsem_post(&sink->pace_data);
  return OK;
}

static int contest_media_abort_pending(FAR struct contest_media_sink_s *sink)
{
  int ret;

  ret = nxsem_wait_uninterruptible(&sink->pace_lock);
  if (ret < 0)
    {
      return ret;
    }

  sink->pace_abort = true;
  nxsem_post(&sink->pace_lock);
  nxsem_post(&sink->pace_data);

  for (;;)
    {
      unsigned int count;
      bool busy;

      ret = nxsem_wait_uninterruptible(&sink->pace_lock);
      if (ret < 0)
        {
          return ret;
        }

      count = sink->pace_count;
      busy = sink->pace_busy;
      nxsem_post(&sink->pace_lock);

      if (count == 0 && !busy)
        {
          break;
        }

      ret = nxsem_wait_uninterruptible(&sink->pace_empty);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = nxsem_wait_uninterruptible(&sink->pace_lock);
  if (ret < 0)
    {
      return ret;
    }

  sink->pace_abort = false;
  sink->pace_clock_valid = false;
  sink->pace_deadline_us = 0;
  nxsem_post(&sink->pace_lock);
  return OK;
}

static int contest_media_pacer_initialize(FAR struct contest_media_sink_s *sink)
{
  pthread_attr_t attr;
  int ret;

  ret = nxsem_init(&sink->pace_lock, 0, 1);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxsem_init(&sink->pace_data, 0, 0);
  if (ret < 0)
    {
      nxsem_destroy(&sink->pace_lock);
      return ret;
    }

  ret = nxsem_init(&sink->pace_empty, 0, 0);
  if (ret < 0)
    {
      nxsem_destroy(&sink->pace_data);
      nxsem_destroy(&sink->pace_lock);
      return ret;
    }

  ret = nxsem_init(&sink->playback_lock, 0, 1);
  if (ret < 0)
    {
      nxsem_destroy(&sink->pace_empty);
      nxsem_destroy(&sink->pace_data);
      nxsem_destroy(&sink->pace_lock);
      return ret;
    }

  ret = nxsem_init(&sink->enqueue_lock, 0, 1);
  if (ret < 0)
    {
      nxsem_destroy(&sink->playback_lock);
      nxsem_destroy(&sink->pace_empty);
      nxsem_destroy(&sink->pace_data);
      nxsem_destroy(&sink->pace_lock);
      return ret;
    }

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      ret = -ret;
      goto fail_sems;
    }

  ret = pthread_attr_setstacksize(&attr, CONTEST_MEDIA_PACE_STACK);
  if (ret != 0)
    {
      pthread_attr_destroy(&attr);
      ret = -ret;
      goto fail_sems;
    }

  ret = pthread_create(&sink->pace_worker, &attr,
                       contest_media_pace_worker, sink);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      ret = -ret;
      goto fail_sems;
    }

  sink->pace_worker_started = true;
  return OK;

fail_sems:
  nxsem_destroy(&sink->enqueue_lock);
  nxsem_destroy(&sink->playback_lock);
  nxsem_destroy(&sink->pace_empty);
  nxsem_destroy(&sink->pace_data);
  nxsem_destroy(&sink->pace_lock);
  return ret;
}

static int contest_media_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                                 FAR struct audio_caps_s *caps)
{
  (void)dev;
  (void)type;

  if (caps == NULL || caps->ac_len < sizeof(struct audio_caps_s))
    {
      return -EINVAL;
    }

  caps->ac_format.hw = 0;
  caps->ac_controls.w = 0;

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_QUERY:
        if (caps->ac_subtype == AUDIO_TYPE_QUERY)
          {
            /* Overall device capability: playback + PCM. */
            caps->ac_controls.b[0] = AUDIO_TYPE_OUTPUT;
            caps->ac_format.hw = 1 << (AUDIO_FMT_PCM - 1);
          }
        else if (caps->ac_subtype == AUDIO_FMT_PCM)
          {
            /* FFmpeg's NuttX outdev performs a second capability query for
             * PCM subformats.  Report the exact format accepted by the
             * proven robot_audio_playback producer interface. */
            caps->ac_controls.b[0] = AUDIO_SUBFMT_PCM_S16_LE;
            caps->ac_controls.b[1] = AUDIO_SUBFMT_END;
            caps->ac_format.hw = 1 << (AUDIO_FMT_PCM - 1);
          }
        else
          {
            caps->ac_controls.b[0] = AUDIO_SUBFMT_END;
          }
        break;

      case AUDIO_TYPE_OUTPUT:
        if (caps->ac_subtype == AUDIO_TYPE_QUERY)
          {
            /* Advertise exactly the format accepted by the bridge. */
            caps->ac_controls.hw[0] = AUDIO_SAMP_RATE_24K;
            caps->ac_channels = CONTEST_MEDIA_CHANNELS;
            caps->ac_format.hw = 1 << (AUDIO_FMT_PCM - 1);
          }
        break;

      default:
        break;
    }

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] getcaps type=%u subtype=%u fmt=0x%x ch=%u ctl=0x%lx\n",
         caps->ac_type, caps->ac_subtype, caps->ac_format.hw,
         caps->ac_channels, (unsigned long)caps->ac_controls.w);
  return caps->ac_len;
}

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int contest_media_configure(FAR struct audio_lowerhalf_s *dev,
                                   FAR void *session,
                                   FAR const struct audio_caps_s *caps)
#else
static int contest_media_configure(FAR struct audio_lowerhalf_s *dev,
                                   FAR const struct audio_caps_s *caps)
#endif
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bits;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  if (caps == NULL)
    {
      return -EINVAL;
    }

  /* Volume/feature control is currently handled by the proven playback
   * path's fixed gain.  Accept feature configure so Media parameter setup
   * cannot tear down an otherwise valid PCM session. */
  if (caps->ac_type == AUDIO_TYPE_FEATURE ||
      caps->ac_type == AUDIO_TYPE_PROCESSING)
    {
      return OK;
    }

  if (caps->ac_type != AUDIO_TYPE_OUTPUT)
    {
      syslog(LOG_ERR, "[MEDIA-BRIDGE] configure unsupported type=%u\n",
             caps->ac_type);
      return -ENOTSUP;
    }

  sample_rate = caps->ac_controls.hw[0] |
                ((uint32_t)caps->ac_controls.b[3] << 16);
  channels = caps->ac_channels;
  bits = caps->ac_controls.b[2];

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] configure fmt=%u rate=%lu ch=%u bits=%u\n",
         caps->ac_subtype, (unsigned long)sample_rate, channels, bits);

  if (sample_rate != CONTEST_MEDIA_RATE ||
      channels != CONTEST_MEDIA_CHANNELS ||
      bits != CONTEST_MEDIA_BITS)
    {
      syslog(LOG_ERR,
             "[MEDIA-BRIDGE] format rejected expected=%uHz/%uch/%ubit\n",
             CONTEST_MEDIA_RATE, CONTEST_MEDIA_CHANNELS, CONTEST_MEDIA_BITS);
      return -EINVAL;
    }

  sink->sample_rate = sample_rate;
  sink->channels = channels;
  sink->bits = bits;
  sink->configured = true;
  /* New configuration is a new stream, even after the preceding FINAL. */
  if (!sink->playback_open)
    {
      __atomic_store_n(&sink->completion_sent, false, __ATOMIC_RELEASE);
      sink->eos = false;
      sink->stream_error = OK;
    }
  return OK;
}

static int contest_media_shutdown(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;
  int ret;

  (void)contest_media_abort_pending(sink);
  ret = contest_media_finish_playback(sink, false);

  sink->configured = false;
  return ret;
}

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int contest_media_start(FAR struct audio_lowerhalf_s *dev,
                               FAR void *session)
#else
static int contest_media_start(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  int ret = contest_media_open_playback(sink);
  if (ret < 0) return ret;
  ret = contest_media_start_backend(sink);
  if (ret == OK && sink->eos)
    {
      ret = robot_audio_playback_end_input();
    }
  syslog(LOG_INFO, "[MEDIA-BRIDGE] START DMA-backed rc=%d\n", ret);
  return ret;
}

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int contest_media_stop(FAR struct audio_lowerhalf_s *dev,
                              FAR void *session)
#else
static int contest_media_stop(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;
  int ret;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  syslog(LOG_INFO, "[MEDIA-BRIDGE] STOP -> immediate abort/close\n");

  /*
   * An explicit AUDIOIOC_STOP is a user/tool stop, not natural end-of-file.
   * Do not call robot_audio_playback_finish() here: finish() drains the
   * playback ring by design, which makes a "stop" command continue playing
   * any already-buffered PCM before the device is closed.
   *
   * First return APBs still waiting in the ownership queue, then close the
   * hardware path without draining the private PCM ring.  COMPLETE tells the
   * Media upper-half that this playback instance is terminal and permits the
   * caller to return to the normal vela> state.
   *
   * Natural EOF remains handled by the FINAL APB / worker path below, where
   * robot_audio_playback_finish() is intentionally used to drain the real
   * tail before COMPLETE.
   */
  (void)contest_media_abort_pending(sink);
  ret = contest_media_finish_playback(sink, false);
  contest_media_complete(sink);
  return ret;
}
#endif

#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int contest_media_pause(FAR struct audio_lowerhalf_s *dev,
                               FAR void *session)
#else
static int contest_media_pause(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;
  int ret = OK;
#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif
  if (nxsem_wait_uninterruptible(&sink->playback_lock) < 0) return -EINTR;
  if (sink->playback_open)
    {
      /* Media's player reports source-side xrun through AUDIOIOC_PAUSE while
       * abufsrc has temporarily run dry.  Freezing the private I2S ring here
       * would prevent its queued PCM from draining, block the graph writer,
       * and make RESUME unreachable.  Keep DMA moving; empty periods are
       * already filled with silence by the playback worker. */
      sink->paused = true;
    }
  nxsem_post(&sink->playback_lock);
  /* Empty input is an underrun or a pause, never a declaration of EOF. */
  return ret;
}

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int contest_media_resume(FAR struct audio_lowerhalf_s *dev,
                                FAR void *session)
#else
static int contest_media_resume(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  int ret = OK;
  if (nxsem_wait_uninterruptible(&sink->playback_lock) < 0) return -EINTR;
  if (sink->playback_open)
    {
      ret = robot_audio_playback_pause_native_media(false);
      if (ret == OK) sink->paused = false;
    }
  nxsem_post(&sink->playback_lock);
  return ret;
}
#endif

static int contest_media_enqueue_internal(FAR struct audio_lowerhalf_s *dev,
                                          FAR struct ap_buffer_s *apb)
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;
  size_t len;
  int ret;
  bool final;

  if (apb == NULL || apb->curbyte > apb->nbytes)
    {
      return -EINVAL;
    }

  len = (size_t)apb->nbytes - (size_t)apb->curbyte;
  final = (apb->flags & AUDIO_APB_FINAL) != 0;
  if ((len & 1) != 0) return -EINVAL;
  /* A bounded APB must fit before START, when DMA intentionally consumes
   * nothing. Reject oversized client buffers rather than blocking forever. */
  if (len > CONTEST_MEDIA_BUFFER_BYTES) return -EMSGSIZE;

  if (__atomic_load_n(&sink->completion_sent, __ATOMIC_ACQUIRE) || sink->eos)
    {
      return -EPIPE;
    }

  /* Fail before committing PCM if no ownership slot is available. */
  ret = nxsem_wait_uninterruptible(&sink->pace_lock);
  if (ret < 0) return ret;
  bool full = sink->pace_count + (sink->pace_busy ? 1 : 0) >=
              CONTEST_MEDIA_PACE_QUEUE;
  nxsem_post(&sink->pace_lock);
  if (full) return -EAGAIN;

  /* Be tolerant of clients that enqueue before AUDIOIOC_START.  The NuttX
   * upper-half still requires CONFIGURE first, and open_playback() enforces
   * that contract. */
  ret = contest_media_open_playback(sink);
  if (ret < 0)
    {
      return ret;
    }

  if (len > 0)
    {
      ret = contest_media_write_music_pcm(&apb->samp[apb->curbyte], len);
      if (ret < 0)
        {
          syslog(LOG_ERR, "[MEDIA-BRIDGE] write failed len=%zu rc=%d\n",
                 len, ret);
          return ret;
        }

      sink->total_pcm += len;
      __atomic_store_n(&sink->last_pcm_ms,
                       (uint32_t)contest_media_now_ms(),
                       __ATOMIC_RELEASE);
      sink->enqueue_count++;
      apb->curbyte = apb->nbytes;

      if (sink->enqueue_count <= 4 || (sink->enqueue_count % 32) == 0)
        {
          syslog(LOG_INFO,
                 "[MEDIA-BRIDGE] PCM enqueue #%u bytes=%zu total=%zu final=%d\n",
                 sink->enqueue_count, len, sink->total_pcm, final ? 1 : 0);
        }
    }

  /* Retain the APB until its PCM-duration slot on the Media timeline. */
  if (final)
    {
      syslog(LOG_INFO,
             "[MEDIA-BRIDGE] FINAL APB received bytes=%zu "
             "pcm_total=%zu dequeued=%zu\n",
             len, sink->total_pcm, sink->dequeued_pcm);
    }

  if (final)
    {
      sink->eos = true;
      /* Nonblocking EOS wakes even a file shorter than the startup water
       * mark. Waiting for FINAL's dequeue before EOS would deadlock. */
      if (nxsem_wait_uninterruptible(&sink->playback_lock) < 0) return -EINTR;
      ret = sink->started ? robot_audio_playback_end_input() : OK;
      nxsem_post(&sink->playback_lock);
      if (ret < 0) return ret;
    }

  ret = contest_media_queue_apb(sink, apb, len, final);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "[MEDIA-BRIDGE] queue APB failed bytes=%zu final=%d rc=%d\n",
             len, final ? 1 : 0, ret);
      return ret;
    }

  return OK;
}

static int contest_media_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                       FAR struct ap_buffer_s *apb)
{
  FAR struct contest_media_sink_s *sink = (FAR void *)dev;
  int ret = nxsem_wait_uninterruptible(&sink->enqueue_lock);
  if (ret < 0) return ret;
  ret = contest_media_enqueue_internal(dev, apb);
  nxsem_post(&sink->enqueue_lock);
  return ret;
}

static int contest_media_ioctl(FAR struct audio_lowerhalf_s *dev,
                               int cmd, unsigned long arg)
{
  FAR struct contest_media_sink_s *sink =
    (FAR struct contest_media_sink_s *)dev;

  switch (cmd)
    {
      case AUDIOIOC_GETBUFFERINFO:
        {
          FAR struct ap_buffer_info_s *info =
            (FAR struct ap_buffer_info_s *)((uintptr_t)arg);

          if (info == NULL)
            {
              return -EINVAL;
            }

          info->nbuffers = CONTEST_MEDIA_NBUFFERS;
          info->buffer_size = CONTEST_MEDIA_BUFFER_BYTES;
          syslog(LOG_INFO,
                 "[MEDIA-BRIDGE] GETBUFFERINFO nbuffers=%u bytes=%u\n",
                 CONTEST_MEDIA_NBUFFERS, CONTEST_MEDIA_BUFFER_BYTES);
          return OK;
        }

      case AUDIOIOC_SETBUFFERINFO:
        /* GETBUFFERINFO is authoritative: the native playback ring and DMA
         * geometry are private to the backend, not set by a Media period. */
        return OK;

      case AUDIOIOC_GETPOSITION:
        {
          FAR long *position =
            (FAR long *)((uintptr_t)arg);
          size_t bytes_per_frame;

          if (position == NULL)
            {
              return -EINVAL;
            }

          bytes_per_frame =
            (size_t)sink->channels * (size_t)sink->bits / 8;

          if (bytes_per_frame == 0)
            {
              bytes_per_frame =
                CONTEST_MEDIA_CHANNELS * (CONTEST_MEDIA_BITS / 8);
            }

          uint64_t completed;
          uint64_t pending;
          int ret = contest_media_progress(sink, &completed, &pending, NULL);
          if (ret < 0) return ret;
          *position = (long)(completed / bytes_per_frame);
          return OK;
        }

      case AUDIOIOC_GETLATENCY:
        {
          FAR long *latency =
            (FAR long *)((uintptr_t)arg);

          if (latency == NULL)
            {
              return -EINVAL;
            }

          uint64_t completed;
          uint64_t pending;
          int ret = contest_media_progress(sink, &completed, &pending, NULL);
          if (ret < 0) return ret;
          *latency = (long)(pending / (CONTEST_MEDIA_CHANNELS *
                                      (CONTEST_MEDIA_BITS / 8)));
          return OK;
        }

      case AUDIOIOC_FLUSH:
        {
          bool restart = sink->started;
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
          bool paused = sink->paused;
#endif
          syslog(LOG_INFO, "[MEDIA-BRIDGE] FLUSH\n");
          (void)contest_media_abort_pending(sink);
          int ret = contest_media_finish_playback(sink, false);
          sink->eos = false;
          __atomic_store_n(&sink->completion_sent, false, __ATOMIC_RELEASE);
          sink->stream_error = OK;
          /* ff_nuttx_flush() keeps its running flag. Re-arm the lower half
           * here; the client will not send a new START after a seek/flush. */
          if (ret == OK && restart)
            {
#ifdef CONFIG_AUDIO_MULTI_SESSION
              ret = contest_media_start(dev, sink);
#else
              ret = contest_media_start(dev);
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
              if (ret == OK && paused)
                {
#ifdef CONFIG_AUDIO_MULTI_SESSION
                  ret = contest_media_pause(dev, sink);
#else
                  ret = contest_media_pause(dev);
#endif
                }
#endif
            }
          return ret;
        }

      default:
        return -ENOTTY;
    }
}

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int contest_media_reserve(FAR struct audio_lowerhalf_s *dev,
                                 FAR void **session)
{
  if (session != NULL)
    {
      *session = dev;
    }

  syslog(LOG_INFO, "[MEDIA-BRIDGE] RESERVE\n");
  return OK;
}

static int contest_media_release(FAR struct audio_lowerhalf_s *dev,
                                 FAR void *session)
{
  (void)dev;
  (void)session;
  syslog(LOG_INFO, "[MEDIA-BRIDGE] RELEASE\n");
  return OK;
}
#else
static int contest_media_reserve(FAR struct audio_lowerhalf_s *dev)
{
  (void)dev;
  syslog(LOG_INFO, "[MEDIA-BRIDGE] RESERVE\n");
  return OK;
}

static int contest_media_release(FAR struct audio_lowerhalf_s *dev)
{
  (void)dev;
  syslog(LOG_INFO, "[MEDIA-BRIDGE] RELEASE\n");
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_voice_audio_initialize(void)
{
#if defined(CONFIG_DRIVERS_AUDIO) && defined(CONFIG_AUDIO_FORMAT_PCM) && \
    defined(CONFIG_CONTEST_BOARD_I2S1_TX)
  int ret;

  if (g_contest_media_registered) return OK;

  /* I2S1 itself is initialized immediately before this function by
   * contest_board_bringup().  Do not create a second audio_i2s owner here:
   * robot_audio_playback will acquire the existing board I2S instance when
   * Media actually starts playback. */

  ret = g_contest_media_sink.pace_worker_started ? OK :
        contest_media_pacer_initialize(&g_contest_media_sink);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[MEDIA-BRIDGE] pacer init failed: %d\n", ret);
      return ret;
    }

  ret = audio_register("pcm0p", &g_contest_media_sink.dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[MEDIA-BRIDGE] register pcm0p failed: %d\n", ret);
      return ret;
    }

  g_contest_media_registered = true;

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] registered /dev/audio/pcm0p "
         "route=Media->DMA-progress-v11 "
         "format=%uHz/%uch/%ubit backend=%s\n",
         CONTEST_MEDIA_RATE, CONTEST_MEDIA_CHANNELS, CONTEST_MEDIA_BITS,
         contest_media_backend_available() ? "ready" : "missing");
  return OK;
#else
  return -ENOSYS;
#endif
}
