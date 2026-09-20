/****************************************************************************
 * Contest media-player compatibility layer
 *
 * The official music tool is linked even when CONFIG_MEDIA is disabled.  In
 * that configuration ai_agent supplies weak media symbols which return
 * ENOSYS.  This file supplies the contest-local implementation and reuses the
 * already verified PCM16LE -> I2S/DMA playback path.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <netutils/webclient.h>

#include "robot_audio_playback.h"

#ifdef CONFIG_LIB_HELIX_MP3
#  include <mp3dec.h>
#endif

#define RV_MEDIA_TAG                 "RV-MEDIA"
#define RV_MEDIA_URL_CAP             512
#define RV_MP3_INPUT_CAP             (16 * 1024)
#define RV_HTTP_BUFFER_CAP           8192
#define RV_PCM_SAMPLES_CAP           2304
#define RV_OUTPUT_RATE               24000
#define RV_EVENT_PREPARED            1
#define RV_EVENT_STARTED             2
#define RV_EVENT_STOPPED             4
#define RV_EVENT_COMPLETED           6

/* Music/media software gain: 1/2 amplitude ~= -6.02 dB. */
#define RV_MUSIC_GAIN_NUM            1
#define RV_MUSIC_GAIN_DEN            2
#define RV_MUSIC_SCALE_CHUNK         512

typedef void (*rv_media_event_callback)(void *cookie, int event, int result,
                                        const char *extra);

struct rv_media_player;

struct rv_media_event
{
  rv_media_event_callback callback;
  void *cookie;
  int event;
  int result;
};

struct rv_media_player
{
  pthread_mutex_t lock;
  pthread_t worker;
  bool worker_started;
  bool worker_done;
  bool stop_requested;
  bool prepared;
  bool started;
  bool url_mode;
  bool playback_open;
  char url[RV_MEDIA_URL_CAP];
  const char *options;
  rv_media_event_callback callback;
  void *callback_cookie;
};

/* Track the currently active official Media player so contest voice control
 * can stop online/Media playback without owning tool_media's private handle. */
static pthread_mutex_t g_rv_active_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rv_media_player *g_rv_active_player;

static bool rv_player_stop_requested(struct rv_media_player *player)
{
  bool stop = false;

  if (player == NULL)
    {
      return true;
    }

  pthread_mutex_lock(&player->lock);
  stop = player->stop_requested;
  pthread_mutex_unlock(&player->lock);
  return stop;
}

static void *rv_event_worker(void *arg)
{
  struct rv_media_event *event = arg;

  /* media_player_prepare/start are called while tool_media holds its lock.
   * Delivering synchronously would deadlock its callback. */
  usleep(1000);
  if (event->callback)
    {
      event->callback(event->cookie, event->event, event->result, NULL);
    }

  free(event);
  return NULL;
}

static void rv_emit_event(struct rv_media_player *player, int event,
                          int result)
{
  struct rv_media_event *pending;
  pthread_t thread;
  pthread_attr_t attr;

  pthread_mutex_lock(&player->lock);
  if (!player->callback)
    {
      pthread_mutex_unlock(&player->lock);
      return;
    }

  pending = calloc(1, sizeof(*pending));
  if (!pending)
    {
      pthread_mutex_unlock(&player->lock);
      syslog(LOG_ERR, "[%s] event allocation failed event=%d\n",
             RV_MEDIA_TAG, event);
      return;
    }

  pending->callback = player->callback;
  pending->cookie = player->callback_cookie;
  pending->event = event;
  pending->result = result;
  pthread_mutex_unlock(&player->lock);

  if (pthread_attr_init(&attr) != 0)
    {
      free(pending);
      return;
    }

  (void)pthread_attr_setstacksize(&attr, 4096);
  if (pthread_create(&thread, &attr, rv_event_worker, pending) == 0)
    {
      (void)pthread_detach(thread);
    }
  else
    {
      free(pending);
    }

  pthread_attr_destroy(&attr);
}

#ifdef CONFIG_LIB_HELIX_MP3
struct rv_mp3_decoder
{
  HMP3Decoder decoder;
  struct rv_media_player *player;
  unsigned char input[RV_MP3_INPUT_CAP];
  size_t input_len;
  int phase;
  int sample_rate;
  int channels;
  int16_t decoded[RV_PCM_SAMPLES_CAP];
  int16_t mono[RV_PCM_SAMPLES_CAP / 2];
  int16_t pcm[RV_PCM_SAMPLES_CAP / 2];
};

static int rv_write_pcm(struct rv_mp3_decoder *decoder, const int16_t *samples,
                        int sample_count, int channels, int sample_rate)
{
  int mono_count = 0;
  int output_count = 0;
  int i;

  if (channels < 1 || channels > 2 || sample_rate <= 0 ||
      sample_count <= 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < sample_count / channels && mono_count <
       (int)(sizeof(decoder->mono) / sizeof(decoder->mono[0])); i++)
    {
      int value = samples[i * channels];
      if (channels == 2)
        {
          value = (value + samples[i * channels + 1]) / 2;
        }

      decoder->mono[mono_count++] = (int16_t)value;
    }

  /* A small accumulator is sufficient here because the output contract is
   * fixed at 24 kHz. It avoids a second large resampling buffer. */
  for (i = 0; i < mono_count; i++)
    {
      decoder->phase += RV_OUTPUT_RATE;
      if (decoder->phase >= sample_rate)
        {
          decoder->phase -= sample_rate;
          decoder->pcm[output_count++] =
            (int16_t)((decoder->mono[i] * RV_MUSIC_GAIN_NUM) /
                      RV_MUSIC_GAIN_DEN);
          if (output_count == (int)(sizeof(decoder->pcm) /
                                    sizeof(decoder->pcm[0])))
            {
              int ret = robot_audio_playback_write(
                  (const uint8_t *)decoder->pcm, sizeof(decoder->pcm));
              if (ret < 0)
                {
                  return ret;
                }
              output_count = 0;
            }
        }
    }

  if (output_count > 0)
    {
      int ret = robot_audio_playback_write((const uint8_t *)decoder->pcm,
                                           (size_t)output_count * sizeof(int16_t));
      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}

static int rv_mp3_decode(struct rv_mp3_decoder *decoder,
                         const unsigned char *data, size_t len)
{
  if (len > sizeof(decoder->input) - decoder->input_len)
    {
      return -ENOBUFS;
    }

  memcpy(decoder->input + decoder->input_len, data, len);
  decoder->input_len += len;

  while (decoder->input_len > 0)
    {
      int sync = MP3FindSyncWord(decoder->input, (int)decoder->input_len);
      unsigned char *cursor;
      int left;
      int consumed;
      int ret;
      MP3FrameInfo info;

      if (sync < 0)
        {
          /* Keep enough bytes for a sync word split across HTTP chunks. */
          if (decoder->input_len > 3)
            {
              memmove(decoder->input, decoder->input + decoder->input_len - 3,
                      3);
              decoder->input_len = 3;
            }
          break;
        }

      if (sync > 0)
        {
          memmove(decoder->input, decoder->input + sync,
                  decoder->input_len - (size_t)sync);
          decoder->input_len -= (size_t)sync;
        }

      cursor = decoder->input;
      left = (int)decoder->input_len;
      ret = MP3Decode(decoder->decoder, &cursor, &left, decoder->decoded, 0);
      consumed = (int)(cursor - decoder->input);
      if (consumed > 0 && (size_t)consumed <= decoder->input_len)
        {
          memmove(decoder->input, decoder->input + consumed,
                  decoder->input_len - (size_t)consumed);
          decoder->input_len -= (size_t)consumed;
        }

      if (ret == ERR_MP3_INDATA_UNDERFLOW || ret == ERR_MP3_MAINDATA_UNDERFLOW)
        {
          break;
        }

      if (ret < 0)
        {
          /* Drop one byte and search for the next frame rather than looping
           * forever on an ID3 or malformed chunk. */
          if (decoder->input_len > 0)
            {
              memmove(decoder->input, decoder->input + 1,
                      decoder->input_len - 1);
              decoder->input_len--;
            }
          continue;
        }

      MP3GetLastFrameInfo(decoder->decoder, &info);
      if (rv_write_pcm(decoder, decoder->decoded, info.outputSamps, info.nChans,
                       info.samprate) < 0)
        {
          return -EIO;
        }
    }

  return 0;
}

static int rv_mp3_sink(char **buffer, int offset, int datend, int *buflen,
                       void *arg)
{
  struct rv_mp3_decoder *decoder = arg;
  int ret;

  (void)buflen;
  if (!decoder || !buffer || !*buffer || offset < 0 || datend < offset)
    {
      return -EINVAL;
    }

  if (rv_player_stop_requested(decoder->player))
    {
      syslog(LOG_INFO, "[%s] HTTP MP3 stop observed in sink\n", RV_MEDIA_TAG);
      return -ECANCELED;
    }

  ret = rv_mp3_decode(decoder, (const unsigned char *)*buffer + offset,
                      (size_t)(datend - offset));
  if (ret < 0)
    {
      return ret;
    }

  return rv_player_stop_requested(decoder->player) ? -ECANCELED : 0;
}
#endif

static int rv_normalize_url(const char *url, char *normalized, size_t cap)
{
  const char *prefix = "https://music.163.com/";
  size_t prefix_len = strlen(prefix);

  if (!url || !normalized || cap == 0)
    {
      return -EINVAL;
    }

  if (strncmp(url, prefix, prefix_len) == 0)
    {
      int n = snprintf(normalized, cap, "http://music.163.com/%s",
                       url + prefix_len);
      return n >= 0 && (size_t)n < cap ? 0 : -ENAMETOOLONG;
    }

  if (strncmp(url, "http://", 7) == 0)
    {
      if (strlen(url) >= cap)
        {
          return -ENAMETOOLONG;
        }
      strcpy(normalized, url);
      return 0;
    }

  return -ENOTSUP;
}

static void *rv_url_worker(void *arg)
{
  struct rv_media_player *player = arg;
  int result = 0;

#ifdef CONFIG_LIB_HELIX_MP3
  struct rv_mp3_decoder *decoder;
  char *http_buffer;
  struct webclient_context context;

  decoder = calloc(1, sizeof(*decoder));
  http_buffer = malloc(RV_HTTP_BUFFER_CAP);
  if (!decoder || !http_buffer)
    {
      result = -ENOMEM;
      goto decoder_cleanup;
    }

  decoder->decoder = MP3InitDecoder();
  if (!decoder->decoder)
    {
      result = -ENOMEM;
      goto decoder_cleanup;
    }

  decoder->player = player;

  webclient_set_defaults(&context);
  context.url = player->url;
  context.buffer = http_buffer;
  context.buflen = RV_HTTP_BUFFER_CAP;
  context.sink_callback = rv_mp3_sink;
  context.sink_callback_arg = decoder;
  context.timeout_sec = 30;

  syslog(LOG_INFO, "[%s] HTTP MP3 begin url=%s\n", RV_MEDIA_TAG,
         player->url);
  result = webclient_perform(&context);
  if (rv_player_stop_requested(player))
    {
      /* A sink-side -ECANCELED is an expected user stop, not playback failure. */
      result = 0;
    }
  else if (result == 0 && context.http_status >= 300)
    {
      result = -EIO;
    }

decoder_cleanup:
  if (decoder)
    {
      if (decoder->decoder)
        {
          MP3FreeDecoder(decoder->decoder);
        }
      free(decoder);
    }
  free(http_buffer);
#else
  result = -ENOSYS;
#endif

  bool stopped = rv_player_stop_requested(player);

  pthread_mutex_lock(&player->lock);
  bool playback_open = player->playback_open;
  player->playback_open = false;
  pthread_mutex_unlock(&player->lock);
  if (playback_open)
    {
      if (!stopped)
        {
          robot_audio_playback_finish();
        }
      robot_audio_playback_close();
    }

  pthread_mutex_lock(&player->lock);
  player->worker_done = true;
  pthread_mutex_unlock(&player->lock);

  if (stopped)
    {
      syslog(LOG_INFO, "[%s] HTTP MP3 stopped by request\n", RV_MEDIA_TAG);
    }
  else
    {
      syslog(result == 0 ? LOG_INFO : LOG_ERR,
             "[%s] HTTP MP3 complete rc=%d\n", RV_MEDIA_TAG, result);
      rv_emit_event(player, RV_EVENT_COMPLETED, result);
    }
  return NULL;
}

void *media_player_open(const char *stream)
{
  struct rv_media_player *player = calloc(1, sizeof(*player));

  (void)stream;
  if (!player)
    {
      return NULL;
    }

  if (pthread_mutex_init(&player->lock, NULL) != 0)
    {
      free(player);
      return NULL;
    }

  syslog(LOG_INFO, "[%s] open stream=%s\n", RV_MEDIA_TAG,
         stream ? stream : "unknown");
  return player;
}

static int rv_media_player_stop_internal(struct rv_media_player *player);
int media_player_stop(void *handle);

int media_player_close(void *handle, int pending_stop)
{
  struct rv_media_player *player = handle;
  int ret;

  (void)pending_stop;
  if (!player)
    {
      return -EINVAL;
    }

  /*
   * Serialize close/free with voice-side active-player stop.  The global lock
   * protects the borrowed active handle from becoming a dangling pointer.
   */
  pthread_mutex_lock(&g_rv_active_lock);
  if (g_rv_active_player == player)
    {
      g_rv_active_player = NULL;
    }

  ret = rv_media_player_stop_internal(player);
  pthread_mutex_destroy(&player->lock);
  free(player);
  pthread_mutex_unlock(&g_rv_active_lock);
  return ret;
}

int media_player_set_event_callback(void *handle, void *cookie,
                                    rv_media_event_callback callback)
{
  struct rv_media_player *player = handle;

  if (!player)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&player->lock);
  player->callback_cookie = cookie;
  player->callback = callback;
  pthread_mutex_unlock(&player->lock);
  return 0;
}

int media_player_prepare(void *handle, const char *url, const char *options)
{
  struct rv_media_player *player = handle;
  int ret = 0;

  if (!player)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&player->lock);
  if (player->prepared || player->worker_started)
    {
      pthread_mutex_unlock(&player->lock);
      return -EBUSY;
    }

  player->options = options;
  if (url)
    {
      ret = rv_normalize_url(url, player->url, sizeof(player->url));
      if (ret == 0)
        {
          player->url_mode = true;
        }
    }
  else
    {
      player->url_mode = false;
    }

  if (ret == 0)
    {
      player->prepared = true;
    }
  pthread_mutex_unlock(&player->lock);

  if (ret == 0)
    {
      syslog(LOG_INFO, "[%s] prepared mode=%s\n", RV_MEDIA_TAG,
             player->url_mode ? "url" : "pcm");
      rv_emit_event(player, RV_EVENT_PREPARED, 0);
    }
  else
    {
      syslog(LOG_ERR, "[%s] unsupported URL rc=%d\n", RV_MEDIA_TAG, ret);
    }

  return ret;
}

int media_player_start(void *handle)
{
  struct rv_media_player *player = handle;
  int ret;

  if (!player)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&player->lock);
  if (!player->prepared || player->started)
    {
      pthread_mutex_unlock(&player->lock);
      return -EINVAL;
    }
  player->started = true;
  player->stop_requested = false;
  player->worker_done = false;
  player->playback_open = false;
  pthread_mutex_unlock(&player->lock);

  ret = robot_audio_playback_open();
  if (ret < 0)
    {
      pthread_mutex_lock(&player->lock);
      player->started = false;
      pthread_mutex_unlock(&player->lock);
      return ret;
    }

  pthread_mutex_lock(&player->lock);
  player->playback_open = true;
  if (player->url_mode)
    {
      pthread_attr_t attr;
      if (pthread_attr_init(&attr) != 0)
        {
          player->playback_open = false;
          pthread_mutex_unlock(&player->lock);
          robot_audio_playback_close();
          return -EAGAIN;
        }

      (void)pthread_attr_setstacksize(&attr, 12288);
      ret = pthread_create(&player->worker, &attr, rv_url_worker, player);
      pthread_attr_destroy(&attr);
      if (ret != 0)
        {
          player->playback_open = false;
          pthread_mutex_unlock(&player->lock);
          robot_audio_playback_close();
          return -ret;
        }
      player->worker_started = true;
    }
  pthread_mutex_unlock(&player->lock);

  pthread_mutex_lock(&g_rv_active_lock);
  g_rv_active_player = player;
  pthread_mutex_unlock(&g_rv_active_lock);

  syslog(LOG_INFO, "[%s] started mode=%s\n", RV_MEDIA_TAG,
         player->url_mode ? "url" : "pcm");
  rv_emit_event(player, RV_EVENT_STARTED, 0);
  return 0;
}

static int rv_media_player_stop_internal(struct rv_media_player *player)
{
  pthread_t worker;
  bool join_worker;
  bool playback_open;

  if (!player)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&player->lock);
  player->stop_requested = true;
  worker = player->worker;
  join_worker = player->worker_started;
  player->worker_started = false;
  pthread_mutex_unlock(&player->lock);

  if (join_worker && !pthread_equal(pthread_self(), worker))
    {
      (void)pthread_join(worker, NULL);
    }

  pthread_mutex_lock(&player->lock);
  playback_open = player->playback_open;
  player->playback_open = false;
  player->started = false;
  pthread_mutex_unlock(&player->lock);

  if (playback_open)
    {
      /* User stop should be immediate: do not drain queued music first. */
      robot_audio_playback_close();
    }

  rv_emit_event(player, RV_EVENT_STOPPED, 0);
  return 0;
}

int media_player_stop(void *handle)
{
  struct rv_media_player *player = handle;
  int ret;

  if (!player)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_rv_active_lock);
  if (g_rv_active_player == player)
    {
      g_rv_active_player = NULL;
    }
  ret = rv_media_player_stop_internal(player);
  pthread_mutex_unlock(&g_rv_active_lock);
  return ret;
}

static int rv_write_scaled_pcm16(const void *data, size_t len)
{
  const uint8_t *src = data;
  uint8_t scaled[RV_MUSIC_SCALE_CHUNK];
  size_t offset = 0;

  if (!data || len == 0)
    {
      return -EINVAL;
    }

  while (offset < len)
    {
      size_t chunk = len - offset;
      size_t even;
      size_t i;
      int ret;

      if (chunk > sizeof(scaled))
        {
          chunk = sizeof(scaled);
        }

      if ((chunk & 1u) != 0 && chunk > 1)
        {
          chunk--;
        }

      even = chunk & ~(size_t)1u;

      for (i = 0; i < even; i += 2)
        {
          uint16_t raw = (uint16_t)src[offset + i] |
                         ((uint16_t)src[offset + i + 1] << 8);
          int16_t sample = (int16_t)raw;
          int16_t quieter =
            (int16_t)((sample * RV_MUSIC_GAIN_NUM) / RV_MUSIC_GAIN_DEN);

          scaled[i] = (uint8_t)((uint16_t)quieter & 0xff);
          scaled[i + 1] = (uint8_t)(((uint16_t)quieter >> 8) & 0xff);
        }

      if (chunk > even)
        {
          scaled[even] = src[offset + even];
        }

      ret = robot_audio_playback_write(scaled, chunk);
      if (ret < 0)
        {
          return ret;
        }

      offset += chunk;
    }

  return 0;
}

ssize_t media_player_write_data(void *handle, const void *data, size_t len)
{
  struct rv_media_player *player = handle;

  if (!player || !data || len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&player->lock);
  if (!player->prepared || player->url_mode || !player->started ||
      player->stop_requested)
    {
      pthread_mutex_unlock(&player->lock);
      return -EINVAL;
    }
  pthread_mutex_unlock(&player->lock);

  return rv_write_scaled_pcm16(data, len);
}

/* Contest-local bridge used by robot_stop_music / voice fast path.
 * Holding g_rv_active_lock across media_player_stop() prevents close/free from
 * racing the borrowed active handle. */
int robot_media_stop_active(void)
{
  struct rv_media_player *player;
  int ret = 0;

  /*
   * Hold the active-player lock through the internal stop.  media_player_close()
   * uses the same lock, so the handle cannot be freed underneath this call.
   */
  pthread_mutex_lock(&g_rv_active_lock);
  player = g_rv_active_player;
  if (player != NULL)
    {
      g_rv_active_player = NULL;
      ret = rv_media_player_stop_internal(player);
    }
  pthread_mutex_unlock(&g_rv_active_lock);

  syslog(LOG_INFO, "[%s] voice stop active=%s rc=%d\n",
         RV_MEDIA_TAG, player ? "yes" : "no", ret);
  return ret;
}

int media_player_pause(void *handle)
{
  (void)handle;
  return -ENOTSUP;
}

int media_player_seek(void *handle, unsigned int position)
{
  (void)handle;
  (void)position;
  return -ENOTSUP;
}

int media_player_get_position(void *handle, unsigned int *position)
{
  (void)handle;
  if (position)
    {
      *position = 0;
    }
  return -ENOTSUP;
}

int media_player_get_duration(void *handle, unsigned int *duration)
{
  (void)handle;
  if (duration)
    {
      *duration = 0;
    }
  return -ENOTSUP;
}

int media_policy_set_stream_volume(const char *stream, int volume)
{
  (void)stream;
  (void)volume;
  return 0;
}
