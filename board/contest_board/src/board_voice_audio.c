/****************************************************************************
 * board/contest_board/src/board_voice_audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * F7P Media PCM bridge with exact terminal short-tail playback:
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
 * F7H additionally preserves NuttX Audio timing semantics: an APB is returned
 * to FFmpeg only after its PCM duration has elapsed, instead of immediately
 * after copying it into robot_audio_playback's private ring.
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

/* F7H: pace AUDIO_CALLBACK_DEQUEUE at the real PCM consumption rate.
 *
 * The proven robot_audio_playback backend copies PCM into a 512 KiB ring.
 * Returning an APB immediately after that copy makes FFmpeg believe the
 * hardware consumed all periods instantly, which triggers its
 * "playback underflow! pause" path.  Keep the APB owned by this lower-half
 * until its PCM duration has elapsed.
 */
#define CONTEST_MEDIA_BYTES_PER_SEC \
  (CONTEST_MEDIA_RATE * CONTEST_MEDIA_CHANNELS * (CONTEST_MEDIA_BITS / 8))
#define CONTEST_MEDIA_PACE_QUEUE      16
#define CONTEST_MEDIA_PACE_STACK      8192
#define CONTEST_MEDIA_PACE_SLICE_US   5000

/* Music volume is scaled only in the Media PCM bridge.  TTS uses the
 * application playback path directly and is intentionally unaffected. */
#define CONTEST_MEDIA_MUSIC_GAIN_NUM  1
#define CONTEST_MEDIA_MUSIC_GAIN_DEN  2
#define CONTEST_MEDIA_GAIN_CHUNK      512

/****************************************************************************
 * Weak playback backend
 *
 * robot_audio_playback.c is contest-local application code and is already
 * linked by robot_voice.  Weak declarations keep board bring-up independent:
 * if that application is not present, the audio device still registers, but
 * START/ENQUEUE return -ENOSYS instead of dereferencing a missing symbol.
 ****************************************************************************/

extern int robot_audio_playback_open(void) __attribute__((weak));
extern int robot_audio_playback_write(const uint8_t *pcm, size_t len)
  __attribute__((weak));
extern int robot_audio_playback_finish(void) __attribute__((weak));
extern void robot_audio_playback_close(void) __attribute__((weak));
extern void robot_audio_playback_set_stop_on_short_underrun(bool enable)
  __attribute__((weak));

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct contest_media_pending_s
{
  FAR struct ap_buffer_s *apb;
  size_t bytes;
  bool final;
};

struct contest_media_sink_s
{
  struct audio_lowerhalf_s dev;
  bool configured;
  bool playback_open;
  bool completion_sent;
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bits;
  size_t total_pcm;
  size_t dequeued_pcm;
  unsigned int enqueue_count;
  unsigned int dequeue_count;
  unsigned int position_query_count;

  /* Media/APB pacing is deliberately separate from robot_audio_playback's
   * own PCM ring.  The APB remains owned by this lower-half until the pacing
   * worker returns AUDIO_CALLBACK_DEQUEUE. */
  sem_t pace_lock;
  sem_t pace_data;
  sem_t pace_empty;
  sem_t playback_lock;
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

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool contest_media_backend_available(void)
{
  return robot_audio_playback_open != NULL &&
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

  if (robot_audio_playback_set_stop_on_short_underrun != NULL)
    {
      robot_audio_playback_set_stop_on_short_underrun(true);
    }

  ret = robot_audio_playback_open();
  if (ret < 0)
    {
      if (robot_audio_playback_set_stop_on_short_underrun != NULL)
        {
          robot_audio_playback_set_stop_on_short_underrun(false);
        }

      nxsem_post(&sink->playback_lock);
      syslog(LOG_ERR, "[MEDIA-BRIDGE] playback open failed=%d\n", ret);
      return ret;
    }

  sink->playback_open = true;
  sink->completion_sent = false;
  sink->total_pcm = 0;
  sink->dequeued_pcm = 0;
  sink->enqueue_count = 0;
  sink->dequeue_count = 0;
  sink->position_query_count = 0;
  sink->pace_busy = false;
  sink->pace_clock_valid = false;
  sink->pace_deadline_us = 0;

  nxsem_post(&sink->playback_lock);

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] playback open source=%luHz/%uch/%ubit\n",
         (unsigned long)sink->sample_rate,
         sink->channels, sink->bits);
  return OK;
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

  if (robot_audio_playback_close != NULL)
    {
      robot_audio_playback_close();
    }

  if (robot_audio_playback_set_stop_on_short_underrun != NULL)
    {
      robot_audio_playback_set_stop_on_short_underrun(false);
    }

  sink->playback_open = false;
  nxsem_post(&sink->playback_lock);

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] playback close drain=%d pcm=%zu "
         "dequeued_pcm=%zu enqueues=%u dequeues=%u rc=%d\n",
         drain ? 1 : 0, sink->total_pcm, sink->dequeued_pcm,
         sink->enqueue_count, sink->dequeue_count, ret);
  return ret;
}

static void contest_media_dequeue(FAR struct contest_media_sink_s *sink,
                                  FAR struct ap_buffer_s *apb)
{
  if (sink->dev.upper == NULL)
    {
      return;
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK, NULL);
#else
  sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
#endif
}

static void contest_media_complete(FAR struct contest_media_sink_s *sink)
{
  if (sink->completion_sent)
    {
      return;
    }

  sink->completion_sent = true;

  if (sink->dev.upper == NULL)
    {
      return;
    }

  syslog(LOG_INFO, "[MEDIA-BRIDGE] COMPLETE\n");

#ifdef CONFIG_AUDIO_MULTI_SESSION
  sink->dev.upper(sink->dev.priv, AUDIO_CALLBACK_COMPLETE, NULL, OK, NULL);
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

static void contest_media_pace_delay(FAR struct contest_media_sink_s *sink,
                                     size_t bytes)
{
  uint64_t duration;
  uint64_t now;

  if (bytes == 0)
    {
      return;
    }

  duration = ((uint64_t)bytes * 1000000ULL +
              CONTEST_MEDIA_BYTES_PER_SEC - 1) /
             CONTEST_MEDIA_BYTES_PER_SEC;

  now = contest_media_now_us();

  /* F7J:
   * Do NOT implement a period as a chain of relative usleep() calls.
   * Each sleep can overshoot slightly; adding those overshoots to the next
   * APB makes the producer permanently slower than the 48 kB/s consumer.
   * That was the source of the audible "word-(gap)-word-(gap)" behavior.
   *
   * Maintain one absolute PCM timeline instead.  Scheduler oversleep on one
   * APB is automatically recovered by a shorter/no sleep on the next APB.
   */
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
      uint64_t remain;
      unsigned int slice;

      if (contest_media_pace_aborted(sink))
        {
          return;
        }

      now = contest_media_now_us();
      if (now == 0 || now >= sink->pace_deadline_us)
        {
          return;
        }

      remain = sink->pace_deadline_us - now;
      slice = remain > CONTEST_MEDIA_PACE_SLICE_US ?
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

static void *contest_media_pace_worker(FAR void *arg)
{
  FAR struct contest_media_sink_s *sink = arg;

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] pacer worker start rate=%uB/s queue=%u\n",
         CONTEST_MEDIA_BYTES_PER_SEC, CONTEST_MEDIA_PACE_QUEUE);

  for (;;)
    {
      struct contest_media_pending_s entry;

      if (nxsem_wait_uninterruptible(&sink->pace_data) < 0)
        {
          continue;
        }

      while (contest_media_pop_apb(sink, &entry))
        {
          bool aborted = contest_media_pace_aborted(sink);

          if (!aborted)
            {
              contest_media_pace_delay(sink, entry.bytes);
            }

          contest_media_dequeue(sink, entry.apb);
          sink->dequeue_count++;
          sink->dequeued_pcm += entry.bytes;

          if (sink->dequeue_count <= 4 ||
              (sink->dequeue_count % 32) == 0 ||
              entry.final)
            {
              syslog(LOG_INFO,
                     "[MEDIA-BRIDGE] PCM dequeue #%u bytes=%zu "
                     "paced=%d final=%d\n",
                     sink->dequeue_count, entry.bytes,
                     aborted ? 0 : 1, entry.final ? 1 : 0);
            }

          if (entry.final && !aborted)
            {
              int ret;

              /* The final APB has now lived for its PCM duration.  EOS then
               * drains the proven robot_audio_playback ring all the way to
               * the I2S hardware before Media receives COMPLETE. */
              ret = contest_media_finish_playback(sink, true);
              contest_media_complete(sink);

              if (ret < 0)
                {
                  syslog(LOG_ERR,
                         "[MEDIA-BRIDGE] final drain failed=%d\n", ret);
                }
            }

          /* Mark the APB idle only after its callback and any FINAL handling
           * have finished.  STOP waits for this flag as well as an empty
           * queue, otherwise it can race the current pacing operation. */
          if (nxsem_wait_uninterruptible(&sink->pace_lock) == OK)
            {
              sink->pace_busy = false;
              nxsem_post(&sink->pace_empty);
              nxsem_post(&sink->pace_lock);
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

  syslog(LOG_INFO, "[MEDIA-BRIDGE] START\n");
  return contest_media_open_playback(sink);
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
   * First return APBs still waiting in the pacing queue, then close the
   * hardware path without draining the private PCM ring.  COMPLETE tells the
   * Media upper-half that this playback instance is terminal and permits the
   * caller to return to the normal vela> state.
   *
   * Natural EOF remains handled by the FINAL APB / pacer path below, where
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
  size_t total;
  size_t dequeued;
  int ret;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  (void)session;
#endif

  /*
   * F7N EOF fallback:
   *
   * FFmpeg's NuttX outdev calls AUDIOIOC_PAUSE when all playback periods
   * have returned and no partial APB remains to enqueue.  With the contest
   * bridge this is the reliable signal we actually observe at finite-file
   * EOF, while AUDIO_APB_FINAL / AUDIOIOC_STOP are not consistently reached
   * by adevsink.
   *
   * Do NOT turn an arbitrary user pause into EOF.  Only accept this fallback
   * when every byte submitted by Media has already reached the bridge's
   * paced DEQUEUE point.  At that moment there is no outstanding Media PCM;
   * robot_audio_playback may still contain its intentional 64 KiB prebuffer,
   * and finish() will drain that real PCM before sanitizing/stopping I2S.
   */
  total = sink->total_pcm;
  dequeued = sink->dequeued_pcm;

  if (dequeued < total)
    {
      syslog(LOG_INFO,
             "[MEDIA-BRIDGE] PAUSE nonterminal pcm=%zu dequeued=%zu -> ignore\n",
             total, dequeued);
      return OK;
    }

  if (!sink->playback_open)
    {
      syslog(LOG_INFO,
             "[MEDIA-BRIDGE] PAUSE after completion -> ignore\n");
      return OK;
    }

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] PAUSE with all PCM returned -> EOF fallback "
         "pcm=%zu dequeued=%zu\n",
         total, dequeued);

  (void)contest_media_abort_pending(sink);

  ret = contest_media_finish_playback(sink, true);
  contest_media_complete(sink);
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

  /*
   * If PAUSE was the EOF fallback above, playback is already drained and
   * completed.  A late ff_nuttx RESUME must not reopen the hardware path.
   * For a nonterminal pause we intentionally keep the backend running, so
   * RESUME is also a no-op.
   */
  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] RESUME playback_open=%d complete=%d -> no-op\n",
         sink->playback_open ? 1 : 0,
         sink->completion_sent ? 1 : 0);
  return OK;
}
#endif

static int contest_media_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
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

  if (sink->completion_sent && !sink->playback_open)
    {
      syslog(LOG_INFO,
             "[MEDIA-BRIDGE] drop post-COMPLETE APB bytes=%u flags=0x%x\n",
             apb->nbytes, apb->flags);
      contest_media_dequeue(sink, apb);
      return OK;
    }

  /* Be tolerant of clients that enqueue before AUDIOIOC_START.  The NuttX
   * upper-half still requires CONFIGURE first, and open_playback() enforces
   * that contract. */
  ret = contest_media_open_playback(sink);
  if (ret < 0)
    {
      return ret;
    }

  len = (size_t)apb->nbytes - (size_t)apb->curbyte;
  final = (apb->flags & AUDIO_APB_FINAL) != 0;

  if ((len & 1) != 0)
    {
      syslog(LOG_ERR, "[MEDIA-BRIDGE] odd PCM buffer len=%zu\n", len);
      return -EINVAL;
    }

  if (len > 0)
    {
      ret = contest_media_write_music_pcm(&apb->samp[apb->curbyte], len);
      if (ret < 0)
        {
          if (ret == -EPIPE || ret == -ECANCELED)
            {
              syslog(LOG_INFO,
                     "[MEDIA-BRIDGE] terminal tail ended rc=%d -> COMPLETE\n",
                     ret);
              contest_media_dequeue(sink, apb);
              (void)contest_media_finish_playback(sink, false);
              contest_media_complete(sink);
              return OK;
            }

          syslog(LOG_ERR, "[MEDIA-BRIDGE] write failed len=%zu rc=%d\n",
                 len, ret);
          return ret;
        }

      sink->total_pcm += len;
      sink->enqueue_count++;
      apb->curbyte = apb->nbytes;

      if (sink->enqueue_count <= 4 || (sink->enqueue_count % 32) == 0)
        {
          syslog(LOG_INFO,
                 "[MEDIA-BRIDGE] PCM enqueue #%u bytes=%zu total=%zu final=%d\n",
                 sink->enqueue_count, len, sink->total_pcm, final ? 1 : 0);
        }
    }

  /* robot_audio_playback_write() has copied the PCM, but that does NOT mean
   * the hardware has consumed this period.  Keep ownership of the Media APB
   * and return it later at the real 48 kB/s PCM cadence. */
  if (final)
    {
      syslog(LOG_INFO,
             "[MEDIA-BRIDGE] FINAL APB received bytes=%zu "
             "pcm_total=%zu dequeued=%zu\n",
             len, sink->total_pcm, sink->dequeued_pcm);
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
        /* The playback backend owns its own 256 KiB ring and DMA geometry,
         * so Media's requested APB geometry does not need to alter it. */
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

          /*
           * F7L:
           *
           * FFmpeg's AV_APP_TO_DEV_DRAIN explicitly calls
           * AUDIOIOC_GETPOSITION first and refuses to send its final
           * zero-byte AUDIO_APB_FINAL buffer if GETPOSITION fails.
           *
           * The bridge's paced DEQUEUE callback is our approximation of
           * hardware consumption time, so report the number of PCM sample
           * frames whose APBs have reached that point.
           */
          *position = (long)(sink->dequeued_pcm / bytes_per_frame);
          sink->position_query_count++;

          if (sink->position_query_count <= 4 ||
              (sink->position_query_count % 32) == 0)
            {
              syslog(LOG_INFO,
                     "[MEDIA-BRIDGE] GETPOSITION samples=%ld "
                     "dequeued_pcm=%zu query=%u\n",
                     *position, sink->dequeued_pcm,
                     sink->position_query_count);
            }

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

          /*
           * APB completion is already paced against CLOCK_MONOTONIC, so do
           * not ask FFmpeg to add another synthetic lower-half latency.
           */
          *latency = 0;
          return OK;
        }

      case AUDIOIOC_FLUSH:
        syslog(LOG_INFO, "[MEDIA-BRIDGE] FLUSH\n");
        (void)contest_media_abort_pending(sink);
        return contest_media_finish_playback(sink, false);

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

  /* I2S1 itself is initialized immediately before this function by
   * contest_board_bringup().  Do not create a second audio_i2s owner here:
   * robot_audio_playback will acquire the existing board I2S instance when
   * Media actually starts playback. */

  ret = contest_media_pacer_initialize(&g_contest_media_sink);
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

  syslog(LOG_INFO,
         "[MEDIA-BRIDGE] registered /dev/audio/pcm0p "
         "route=Media->robot_audio_playback "
         "format=%uHz/%uch/%ubit backend=%s\n",
         CONTEST_MEDIA_RATE, CONTEST_MEDIA_CHANNELS, CONTEST_MEDIA_BITS,
         contest_media_backend_available() ? "ready" : "missing");
  return OK;
#else
  return -ENOSYS;
#endif
}
