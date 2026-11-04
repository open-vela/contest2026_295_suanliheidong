#include "robot_voice_diag.h"
#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "robot_voice_media_stream.h"
#include "robot_audio_playback.h"

extern void robot_audio_playback_set_media_voice_mode(bool enable)
  __attribute__((weak));

#if defined(CONFIG_MEDIA)
#  include <media_player.h>
#  ifndef MEDIA_STREAM_MUSIC
#    define MEDIA_STREAM_MUSIC "music"
#  endif
#endif

#define RV_MEDIA_PCM_OPTIONS       "format=s16le:sample_rate=24000:ch_layout=mono"
#define RV_MEDIA_RETRY_US          5000
#define RV_MEDIA_RETRY_MAX         600

static unsigned long long rv_media_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (unsigned long long)ts.tv_sec * 1000ULL +
         (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static void rv_media_retry_delay(void)
{
  struct timespec delay =
    {
      .tv_sec = 0,
      .tv_nsec = RV_MEDIA_RETRY_US * 1000L,
    };

  (void)nanosleep(&delay, NULL);
}

static void rv_media_reset(struct robot_voice_media_stream_s *stream)
{
  if (stream != NULL)
    {
      memset(stream, 0, sizeof(*stream));
    }
}

int robot_voice_media_stream_open(struct robot_voice_media_stream_s *stream)
{
#if defined(CONFIG_MEDIA)
  int ret;

  if (stream == NULL)
    {
      return -EINVAL;
    }

  rv_media_reset(stream);
  stream->open_ms = rv_media_now_ms();
  if (robot_audio_playback_set_media_voice_mode != NULL)
    {
      robot_audio_playback_set_media_voice_mode(true);
    }
  stream->handle = media_player_open(MEDIA_STREAM_MUSIC);
  if (stream->handle == NULL)
    {
      printf("[RV-MEDIA-TTS] open failed\n");
      if (robot_audio_playback_set_media_voice_mode != NULL)
        {
          robot_audio_playback_set_media_voice_mode(false);
        }
      rv_media_reset(stream);
      return -ENOMEM;
    }

  /* NULL URL selects the documented raw buffer mode.  The graph supplied by
   * contest_board is 24 kHz/s16/mono, exactly matching MiMo pcm16 output. */
  ret = media_player_prepare(stream->handle, NULL, RV_MEDIA_PCM_OPTIONS);
  if (ret < 0)
    {
      printf("[RV-MEDIA-TTS] prepare failed rc=%d\n", ret);
      media_player_close(stream->handle, 0);
      if (robot_audio_playback_set_media_voice_mode != NULL)
        {
          robot_audio_playback_set_media_voice_mode(false);
        }
      rv_media_reset(stream);
      return ret;
    }

  stream->prepared = true;
  RV_VERBOSE("[RV-MEDIA-TTS] prepared raw PCM options=%s\n",
         RV_MEDIA_PCM_OPTIONS);
  return 0;
#else
  (void)stream;
  return -ENOSYS;
#endif
}

int robot_voice_media_stream_start(struct robot_voice_media_stream_s *stream)
{
#if defined(CONFIG_MEDIA)
  int ret;

  if (stream == NULL || stream->handle == NULL || !stream->prepared)
    {
      return -EINVAL;
    }

  if (stream->started)
    {
      return 0;
    }

  ret = media_player_start(stream->handle);
  if (ret < 0)
    {
      stream->failed = true;
      printf("[RV-MEDIA-TTS] start failed rc=%d\n", ret);
      return ret;
    }

  stream->started = true;
  stream->start_ms = rv_media_now_ms();
  RV_VERBOSE("[RV-MEDIA-TTS] started after_prepare_ms=%llu\n",
         stream->open_ms && stream->start_ms >= stream->open_ms ?
         stream->start_ms - stream->open_ms : 0);
  return 0;
#else
  (void)stream;
  return -ENOSYS;
#endif
}

int robot_voice_media_stream_write(struct robot_voice_media_stream_s *stream,
                                   const uint8_t *pcm, size_t len)
{
#if defined(CONFIG_MEDIA)
  size_t offset = 0;

  if (stream == NULL || stream->handle == NULL || !stream->started ||
      pcm == NULL || len == 0 || (len & 1u) != 0)
    {
      return -EINVAL;
    }

  while (offset < len)
    {
      ssize_t written = media_player_write_data(stream->handle,
                                                pcm + offset, len - offset);

      if (written > 0)
        {
          size_t remaining = len - offset;

          if ((size_t)written > remaining)
            {
              stream->failed = true;
              return -EPROTO;
            }

          if (stream->first_write_ms == 0)
            {
              stream->first_write_ms = rv_media_now_ms();
            }

          stream->bytes_written += (size_t)written;
          stream->write_calls++;
          stream->last_write_ms = rv_media_now_ms();
          offset += (size_t)written;
          continue;
        }

      if (written == 0 || written == -EAGAIN || written == -EWOULDBLOCK ||
          written == -EINTR)
        {
          if (stream->retry_count++ >= RV_MEDIA_RETRY_MAX)
            {
              stream->failed = true;
              RV_VERBOSE("[RV-MEDIA-TTS] write stalled offset=%zu/%zu\n",
                     offset, len);
              return -ETIMEDOUT;
            }

          rv_media_retry_delay();
          continue;
        }

      stream->failed = true;
      printf("[RV-MEDIA-TTS] write failed offset=%zu/%zu rc=%ld\n",
             offset, len, (long)written);
      return (int)written;
    }

  return 0;
#else
  (void)stream;
  (void)pcm;
  (void)len;
  return -ENOSYS;
#endif
}

int robot_voice_media_stream_finish(struct robot_voice_media_stream_s *stream)
{
#if defined(CONFIG_MEDIA)
  int ret = 0;
  unsigned long long done_ms;

  if (stream == NULL || stream->handle == NULL)
    {
      return -EINVAL;
    }

  if (!stream->started)
    {
      media_player_close(stream->handle, 0);
      if (robot_audio_playback_set_media_voice_mode != NULL)
        {
          robot_audio_playback_set_media_voice_mode(false);
        }
      stream->handle = NULL;
      stream->prepared = false;
      return stream->bytes_written == 0 ? -ENODATA : -EINVAL;
    }

  /* pending_stop=1 lets the buffer-mode player mark EOS, drain its queued
   * APBs, and let the contest media bridge stop the I2S path at a real tail. */
  ret = media_player_close(stream->handle, 1);
  done_ms = rv_media_now_ms();
  RV_VERBOSE("[RV-MEDIA-TTS] finish bytes=%zu writes=%u retries=%u drain_ms=%llu rc=%d\n",
         stream->bytes_written, stream->write_calls, stream->retry_count,
         stream->start_ms && done_ms >= stream->start_ms ?
         done_ms - stream->start_ms : 0, ret);
  stream->handle = NULL;
  stream->prepared = false;
  stream->started = false;
  if (robot_audio_playback_set_media_voice_mode != NULL)
    {
      robot_audio_playback_set_media_voice_mode(false);
    }
  if (ret < 0)
    {
      stream->failed = true;
    }
  return ret;
#else
  (void)stream;
  return -ENOSYS;
#endif
}

void robot_voice_media_stream_abort(struct robot_voice_media_stream_s *stream)
{
#if defined(CONFIG_MEDIA)
  if (stream == NULL || stream->handle == NULL)
    {
      return;
    }

  (void)media_player_stop(stream->handle);
  (void)media_player_close(stream->handle, 0);
  if (robot_audio_playback_set_media_voice_mode != NULL)
    {
      robot_audio_playback_set_media_voice_mode(false);
    }
  RV_VERBOSE("[RV-MEDIA-TTS] aborted bytes=%zu\n", stream->bytes_written);
  stream->handle = NULL;
  stream->prepared = false;
  stream->started = false;
  stream->failed = true;
#else
  (void)stream;
#endif
}

void robot_voice_media_stream_close(struct robot_voice_media_stream_s *stream)
{
#if defined(CONFIG_MEDIA)
  if (stream != NULL && stream->handle != NULL)
    {
      (void)media_player_close(stream->handle, 0);
    }

  if (robot_audio_playback_set_media_voice_mode != NULL)
    {
      robot_audio_playback_set_media_voice_mode(false);
    }
#endif

  rv_media_reset(stream);
}
