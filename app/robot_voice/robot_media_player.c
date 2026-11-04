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
#include <time.h>
#include <unistd.h>

#include <netutils/webclient.h>

#include "robot_audio_playback.h"
#include "robot_media_player.h"
#include "robot_task.h"

extern void robot_voice_http_close_idle(void) __attribute__((weak));
extern void vela_tls_pool_cleanup(void) __attribute__((weak));

#ifdef CONFIG_LIB_HELIX_MP3
#  include <mp3dec.h>
#endif

#define RV_MEDIA_TAG                 "RV-MEDIA"
#define RV_MEDIA_URL_CAP             512
#define RV_MP3_INPUT_CAP             (16 * 1024)
#define RV_MP3_QUEUE_CAP             (64 * 1024)
#define RV_MP3_QUEUE_CHUNK           2048
#define RV_HTTP_BUFFER_CAP           8192
/* Bound stop/pause even when recv stalls and no sink callback arrives. */
#define RV_HTTP_TIMEOUT_SEC          1
#define RV_HTTP_MAX_NO_PROGRESS      4
#define RV_HTTP_RESUME_DELAY_MS      500
#define RV_PCM_SAMPLES_CAP           2304
#define RV_OUTPUT_RATE               24000
#define RV_EVENT_PREPARED            1
#define RV_EVENT_STARTED             2
#define RV_EVENT_PAUSED              3
#define RV_EVENT_STOPPED             4
#define RV_EVENT_COMPLETED           6
#define RV_MEDIA_AUTO_PAUSED          1
#define RV_MEDIA_USER_PAUSED          2
#define RV_MEDIA_STAGED               3

/* Music/media software gain: 9/32 amplitude ~= -10.88 dB. */
#define RV_MUSIC_GAIN_NUM            9
#define RV_MUSIC_GAIN_DEN            32
#define RV_MUSIC_SCALE_CHUNK         512

typedef void (*rv_media_event_callback)(void *cookie, int event, int result,
                                        const char *extra);

struct rv_media_player;

static unsigned long long rv_media_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (unsigned long long)ts.tv_sec * 1000ULL +
         (unsigned long long)ts.tv_nsec / 1000000ULL;
}

struct rv_media_player
{
  pthread_mutex_t lock;
  pthread_t worker;
  bool worker_started;
  unsigned char paused; /* running / automatic / user / staged */
  bool stop_requested;
  bool prepared;
  bool started;
  bool url_mode;
  bool playback_open;
  char url[RV_MEDIA_URL_CAP];
  size_t played_bytes; /* replaces unused options pointer; completed PCM only */
  rv_media_event_callback callback;
  void *callback_cookie;
};

/* Track the currently active official Media player so contest voice control
 * can stop online/Media playback without owning tool_media's private handle. */
static pthread_mutex_t g_rv_active_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rv_media_player *g_rv_active_player;

/* A resolved music URL can briefly fail before the first byte is received
 * while the hotspot DNS/socket state is being restored after voice capture
 * stops. These errors are safe to retry with the same byte offset. */
static bool rv_http_retryable(int result)
{
  switch (result)
    {
      case -EBUSY:
      case -ENOTCONN:
      case -ECONNRESET:
      case -EPIPE:
      case -ETIMEDOUT:
      case -EAGAIN:
      case -ENOENT:
      case -EHOSTUNREACH:
      case -ENETUNREACH:
      case -EADDRNOTAVAIL:
        return true;
      default:
        return false;
    }
}

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

/* The upper adapter releases its mutex before commands. Deliver completion
 * inline instead of allocating a detached thread/stack for each event.
 * Receivers only record state/enqueue bounded notifications, never join us. */
static void rv_emit_event(struct rv_media_player *player, int event, int result)
{
  rv_media_event_callback callback;
  void *cookie;
  pthread_mutex_lock(&player->lock);
  callback = player->callback;
  cookie = player->callback_cookie;
  pthread_mutex_unlock(&player->lock);
  if (callback != NULL) callback(cookie, event, result, NULL);
}

#ifdef CONFIG_LIB_HELIX_MP3
struct rv_mp3_decoder
{
  HMP3Decoder decoder;
  struct rv_media_player *player;
  unsigned char input[RV_MP3_INPUT_CAP];
  size_t input_len;
  int phase;
  size_t skip_pcm; /* reuse unused format fields; rebuild Helix on resume */
  size_t http_bytes;
  size_t stream_offset;
  unsigned long long rate_window_ms;
  size_t rate_window_offset;
  unsigned long long sink_block_max_ms;
  int bitrate;
  size_t resume_skip_remaining;
  size_t pcm_bytes;
  unsigned long frame_count;
  unsigned long decoder_errors;
  unsigned long indata_underflows;
  unsigned long maindata_underflows;
  unsigned long bad_frame_info;
  unsigned long sink_calls;
  unsigned long long last_sink_ms;
  unsigned long long last_report_ms;
  unsigned long long max_sink_gap_ms;
  int last_decoder_error;
  int pcm_error;
  int sink_error;
  int16_t decoded[RV_PCM_SAMPLES_CAP];
  int16_t mono[RV_PCM_SAMPLES_CAP / 2];
  int16_t pcm[RV_PCM_SAMPLES_CAP / 2];
  pthread_mutex_t queue_lock;
  pthread_cond_t queue_data;
  pthread_cond_t queue_space;
  pthread_t decode_worker;
  struct webclient_context *http_context;
  bool resume_pending;
  bool resume_rejected;
  bool queue_sync_ready;
  bool decode_worker_started;
  bool queue_stop;
  int queue_error;
  size_t queue_high_water;
  unsigned char queue[RV_MP3_QUEUE_CAP];
  size_t queue_head;
  size_t queue_tail;
  size_t queue_count;
};

static bool rv_mp3_frame_info_valid(const MP3FrameInfo *info)
{
  return info != NULL &&
         info->nChans >= 1 && info->nChans <= 2 &&
         info->samprate >= 8000 && info->samprate <= 48000 &&
         info->outputSamps > 0 &&
         info->outputSamps <= RV_PCM_SAMPLES_CAP &&
         info->outputSamps % info->nChans == 0;
}

/* Replay/discard reconstructs the MP3 bit reservoir and resampler without
 * retaining a second cache during TTS. Only completed PCM is skipped; queued
 * and DMA-pending samples are fetched again after the resource handoff. */
static int rv_write_music_pcm(struct rv_mp3_decoder *decoder,
                              const uint8_t *pcm, size_t bytes)
{
  size_t skip = decoder->skip_pcm < bytes ? decoder->skip_pcm : bytes;
  decoder->skip_pcm -= skip;
  if (rv_player_stop_requested(decoder->player)) return -ECANCELED;
  if (skip == bytes) return 0;
  return robot_audio_playback_write(pcm + skip, bytes - skip);
}

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
              int ret = rv_write_music_pcm(decoder,
                  (const uint8_t *)decoder->pcm, sizeof(decoder->pcm));
              if (ret < 0)
                {
                  if (decoder->pcm_error == 0)
                    {
                      decoder->pcm_error = ret;
                    }
                  return ret;
                }
              decoder->pcm_bytes += sizeof(decoder->pcm);
              output_count = 0;
            }
        }
    }

  if (output_count > 0)
    {
      int ret = rv_write_music_pcm(decoder, (const uint8_t *)decoder->pcm,
                                           (size_t)output_count * sizeof(int16_t));
      if (ret < 0)
        {
          if (decoder->pcm_error == 0)
            {
              decoder->pcm_error = ret;
            }
          return ret;
        }
      decoder->pcm_bytes += (size_t)output_count * sizeof(int16_t);
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
      size_t input_len_before;

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
      input_len_before = decoder->input_len;
      ret = MP3Decode(decoder->decoder, &cursor, &left, decoder->decoded, 0);
      consumed = (int)(cursor - decoder->input);

      /* Helix advances the input pointer past the header and side-info before
       * it discovers that the main data is incomplete.  Those bytes still
       * belong to the current frame and must be retained for the next HTTP
       * chunk.  Dropping them makes the next call scan the middle of a frame,
       * which produces invalid headers, bad frame info, and audible clicks. */
      if (ret == ERR_MP3_INDATA_UNDERFLOW)
        {
          decoder->indata_underflows++;
          decoder->last_decoder_error = ret;
          break;
        }

      if (consumed > 0 && (size_t)consumed <= decoder->input_len)
        {
          memmove(decoder->input, decoder->input + consumed,
                  decoder->input_len - (size_t)consumed);
          decoder->input_len -= (size_t)consumed;
        }

      if (ret == ERR_MP3_MAINDATA_UNDERFLOW)
        {
          /* The frame was copied into Helix's bit reservoir and consumed.
           * It is not valid PCM, but it must not be replayed on retry. */
          decoder->maindata_underflows++;
          decoder->last_decoder_error = ret;
          break;
        }

      if (ret < 0)
        {
          decoder->decoder_errors++;
          decoder->last_decoder_error = ret;
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

      /* A successful decode should consume one frame.  If Helix reports
       * success without advancing, discard one byte and resynchronize so a
       * damaged header cannot spin forever or abort the HTTP sink. */

      if (consumed <= 0 || (size_t)consumed > input_len_before)
        {
          decoder->decoder_errors++;
          decoder->last_decoder_error = -EILSEQ;
          if (decoder->input_len > 0)
            {
              memmove(decoder->input, decoder->input + 1,
                      decoder->input_len - 1);
              decoder->input_len--;
              continue;
            }

          break;
        }

      MP3GetLastFrameInfo(decoder->decoder, &info);
      if (!rv_mp3_frame_info_valid(&info))
        {
          decoder->decoder_errors++;
          decoder->bad_frame_info++;
          decoder->last_decoder_error = -EINVAL;
          if (decoder->bad_frame_info == 1)
            {
              syslog(LOG_ERR,
                     "[%s] invalid MP3 frame info samples=%d channels=%d "
                     "rate=%d consumed=%d\n",
                     RV_MEDIA_TAG, info.outputSamps, info.nChans,
                     info.samprate, consumed);
            }

          /* The corrupt frame has already been consumed.  Skip it and keep
           * receiving the rest of the HTTP body.  Returning -EINVAL here
           * would make webclient_perform abort the entire download. */
          continue;
        }

      __atomic_store_n(&decoder->bitrate, info.bitrate, __ATOMIC_RELAXED);
      ret = rv_write_pcm(decoder, decoder->decoded, info.outputSamps,
                         info.nChans, info.samprate);
      if (ret < 0)
        {
          /* Preserve the playback/I2S error.  Mapping every sink failure to
           * EIO made network truncation and audio-device failures
           * indistinguishable on the board. */
          return ret;
        }
      decoder->frame_count++;
    }

  return 0;
}

static int rv_mp3_queue_write(struct rv_mp3_decoder *decoder,
                              const unsigned char *src, size_t len)
{
  size_t offset = 0;

  while (offset < len)
    {
      size_t amount;
      size_t first;

      pthread_mutex_lock(&decoder->queue_lock);
      while (decoder->queue_count == sizeof(decoder->queue) &&
             decoder->queue_error == 0 && !decoder->queue_stop &&
             !rv_player_stop_requested(decoder->player))
        {
          pthread_cond_wait(&decoder->queue_space, &decoder->queue_lock);
        }

      if (decoder->queue_error < 0)
        {
          int error = decoder->queue_error;
          pthread_mutex_unlock(&decoder->queue_lock);
          return error;
        }

      if (decoder->queue_stop || rv_player_stop_requested(decoder->player))
        {
          pthread_mutex_unlock(&decoder->queue_lock);
          return -ECANCELED;
        }

      amount = sizeof(decoder->queue) - decoder->queue_count;
      if (amount > len - offset) amount = len - offset;
      first = sizeof(decoder->queue) - decoder->queue_head;
      if (first > amount) first = amount;
      memcpy(decoder->queue + decoder->queue_head, src + offset, first);
      if (first < amount)
        memcpy(decoder->queue, src + offset + first, amount - first);
      decoder->queue_head = (decoder->queue_head + amount) %
                            sizeof(decoder->queue);
      decoder->queue_count += amount;
      if (decoder->queue_count > decoder->queue_high_water)
        {
          decoder->queue_high_water = decoder->queue_count;
        }
      offset += amount;
      pthread_cond_signal(&decoder->queue_data);
      pthread_mutex_unlock(&decoder->queue_lock);
    }

  return 0;
}

static void *rv_mp3_decode_worker(void *arg)
{
  struct rv_mp3_decoder *decoder = arg;
  unsigned char chunk[RV_MP3_QUEUE_CHUNK];

  for (;;)
    {
      size_t amount;
      size_t first;
      int ret;

      pthread_mutex_lock(&decoder->queue_lock);
      while (decoder->queue_count == 0 && !decoder->queue_stop)
        pthread_cond_wait(&decoder->queue_data, &decoder->queue_lock);

      if (rv_player_stop_requested(decoder->player) ||
          (decoder->queue_count == 0 && decoder->queue_stop))
        {
          pthread_mutex_unlock(&decoder->queue_lock);
          break;
        }

      amount = decoder->queue_count;
      if (amount > sizeof(chunk)) amount = sizeof(chunk);
      first = sizeof(decoder->queue) - decoder->queue_tail;
      if (first > amount) first = amount;
      memcpy(chunk, decoder->queue + decoder->queue_tail, first);
      if (first < amount)
        memcpy(chunk + first, decoder->queue, amount - first);
      decoder->queue_tail = (decoder->queue_tail + amount) %
                            sizeof(decoder->queue);
      decoder->queue_count -= amount;
      pthread_cond_broadcast(&decoder->queue_space);
      pthread_mutex_unlock(&decoder->queue_lock);

      ret = rv_mp3_decode(decoder, chunk, amount);
      if (ret < 0)
        {
          pthread_mutex_lock(&decoder->queue_lock);
          if (decoder->queue_error == 0) decoder->queue_error = ret;
          decoder->queue_stop = true;
          pthread_cond_broadcast(&decoder->queue_space);
          pthread_cond_broadcast(&decoder->queue_data);
          pthread_mutex_unlock(&decoder->queue_lock);
          break;
        }
    }

  return NULL;
}

static int rv_mp3_sink(char **buffer, int offset, int datend, int *buflen,
                       void *arg)
{
  struct rv_mp3_decoder *decoder = arg;
  unsigned long long now_ms;
  unsigned long long gap_ms = 0;
  bool report = false;
  int ret;

  (void)buflen;
  if (!decoder)
    {
      syslog(LOG_ERR, "[%s] MP3 sink called without decoder\n", RV_MEDIA_TAG);
      return -EINVAL;
    }

  if (!buffer || !*buffer || offset < 0 || datend < offset)
    {
      if (decoder->sink_error == 0)
        {
          decoder->sink_error = -EINVAL;
        }
      syslog(LOG_ERR, "[%s] invalid MP3 sink range offset=%d end=%d\n",
             RV_MEDIA_TAG, offset, datend);
      return -EINVAL;
    }

  if (rv_player_stop_requested(decoder->player))
    {
      syslog(LOG_INFO, "[%s] HTTP MP3 stop observed in sink\n", RV_MEDIA_TAG);
      return -ECANCELED;
    }

  /* A reconnect must start at the exact byte offset requested.  Reject a
   * server that ignores Range before adding any bytes to the live decoder;
   * otherwise the restarted response would duplicate the beginning of the
   * song in the current MP3 stream. */
  if (decoder->resume_pending)
    {
      decoder->resume_pending = false;
      if (decoder->http_context != NULL &&
          decoder->http_context->http_status == 200)
        {
          /* Some music CDNs ignore Range. Re-fetch from byte zero and drop
           * the already queued prefix so playback continues without a
           * repeated intro. */
          decoder->resume_skip_remaining = decoder->stream_offset;
        }
      else if (decoder->http_context != NULL &&
               decoder->http_context->http_status == 206)
        {
          decoder->resume_skip_remaining = 0;
        }
      else if (decoder->http_context == NULL ||
               decoder->http_context->http_status != 206)
        {
          decoder->resume_rejected = true;
          if (decoder->sink_error == 0)
            {
              decoder->sink_error = -EPROTO;
            }

          syslog(LOG_ERR,
                 "[%s] HTTP Range resume rejected status=%ld offset=%zu\n",
                 RV_MEDIA_TAG,
                 decoder->http_context != NULL ?
                   (long)decoder->http_context->http_status : 0L,
                 decoder->stream_offset);
          return -EPROTO;
        }
    }

  now_ms = rv_media_now_ms();
  if (decoder->last_sink_ms != 0 && now_ms >= decoder->last_sink_ms)
    {
      gap_ms = now_ms - decoder->last_sink_ms;
      if (gap_ms > decoder->max_sink_gap_ms)
        {
          decoder->max_sink_gap_ms = gap_ms;
        }
    }
  decoder->last_sink_ms = now_ms;
  decoder->sink_calls++;
  decoder->http_bytes += (size_t)(datend - offset);

  if (decoder->resume_skip_remaining != 0)
    {
      size_t amount = (size_t)(datend - offset);

      if (amount <= decoder->resume_skip_remaining)
        {
          decoder->resume_skip_remaining -= amount;
          return rv_player_stop_requested(decoder->player) ?
                 -ECANCELED : 0;
        }

      offset += (int)decoder->resume_skip_remaining;
      decoder->resume_skip_remaining = 0;
    }

  /* Report stream cadence once per second.  This is intentionally separate
   * from the packet-level [WLAN-TLS] driver trace, so a long HTTP gap can be
   * correlated with Wi-Fi/TLS diagnostics without flooding the console. */
  report = decoder->sink_calls == 1;
  if (now_ms != 0 && decoder->last_report_ms != 0 &&
      now_ms >= decoder->last_report_ms &&
      now_ms - decoder->last_report_ms >= 1000)
    {
      report = true;
    }
  else if (now_ms == 0 && (decoder->sink_calls % 32) == 0)
    {
      report = true;
    }

  if (report)
    {
      size_t queue_depth;
      size_t queue_high_water;

      pthread_mutex_lock(&decoder->queue_lock);
      queue_depth = decoder->queue_count;
      queue_high_water = decoder->queue_high_water;
      pthread_mutex_unlock(&decoder->queue_lock);
      syslog(LOG_INFO,
             "[RV-MUSIC-NET] rx=%zu pcm=%zu frames=%lu callbacks=%lu "
             "last_gap_ms=%llu max_gap_ms=%llu decode_errors=%lu "
             "queue=%zu/%zu queue_high=%zu\n",
             decoder->http_bytes, decoder->pcm_bytes, decoder->frame_count,
             decoder->sink_calls,
             gap_ms, decoder->max_sink_gap_ms, decoder->decoder_errors,
             queue_depth, sizeof(decoder->queue), queue_high_water);
      decoder->last_report_ms = now_ms;
    }

  unsigned long long enqueue_ms = rv_media_now_ms();
  ret = rv_mp3_queue_write(decoder,
                           (const unsigned char *)*buffer + offset,
                           (size_t)(datend - offset));
  if (ret < 0)
    {
      if (decoder->sink_error == 0)
        {
          decoder->sink_error = ret;
          syslog(LOG_ERR, "[%s] MP3 sink failed rc=%d http=%zu frames=%lu "
                 "decode_errors=%lu\n", RV_MEDIA_TAG, ret,
                 decoder->http_bytes, decoder->frame_count,
                 decoder->decoder_errors);
        }

      return ret;
    }

  decoder->stream_offset += (size_t)(datend - offset);
  now_ms = rv_media_now_ms();
  unsigned long long block_ms = now_ms >= enqueue_ms ? now_ms - enqueue_ms : 0;
  if (block_ms > decoder->sink_block_max_ms) decoder->sink_block_max_ms = block_ms;
  if (decoder->rate_window_ms == 0)
    {
      decoder->rate_window_ms = now_ms;
      decoder->rate_window_offset = decoder->stream_offset;
    }
  else if (now_ms - decoder->rate_window_ms >= 5000)
    {
      unsigned long long elapsed = now_ms - decoder->rate_window_ms;
      size_t rate = (decoder->stream_offset - decoder->rate_window_offset) * 1000ULL / elapsed;
      int bitrate = __atomic_load_n(&decoder->bitrate, __ATOMIC_RELAXED);
      unsigned int need = bitrate > 0 ? (unsigned int)bitrate / 8U : 0;
      unsigned long long pct = need ? rate * 100ULL / need : 0;
      syslog(LOG_INFO, "[RV-MUSIC-RATE] window_ms=%llu download_Bps=%zu encoded_need_Bps=%u supply_pct=%llu sink_block_max_ms=%llu note=%s\n",
        elapsed, rate, need, pct, decoder->sink_block_max_ms,
        need && rate < need ? "below-realtime" : "ok-or-unknown");
      decoder->rate_window_ms = now_ms;
      decoder->rate_window_offset = decoder->stream_offset;
      decoder->sink_block_max_ms = 0;
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
  int finish_result = 0;
  size_t http_bytes = 0;
  size_t pcm_bytes = 0;
  unsigned long frame_count = 0;
  unsigned long decoder_errors = 0;
  unsigned long indata_underflows = 0;
  unsigned long maindata_underflows = 0;
  unsigned long bad_frame_info = 0;
  unsigned long sink_calls = 0;
  unsigned long long max_sink_gap_ms = 0;
  unsigned long long request_start_ms = rv_media_now_ms();
  unsigned long long request_elapsed_ms = 0;
  unsigned long long final_sink_age_ms = 0;
  int last_decoder_error = 0;
  int pcm_error = 0;
  int sink_error = 0;
  int webclient_result = 0;
  long http_status = 0;
  unsigned int resume_attempts = 0;
  unsigned int no_progress_retries = 0;

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
  decoder->skip_pcm = player->played_bytes;
  if (pthread_mutex_init(&decoder->queue_lock, NULL) != 0)
    {
      result = -ENOMEM;
      goto decoder_cleanup;
    }
  if (pthread_cond_init(&decoder->queue_data, NULL) != 0)
    {
      pthread_mutex_destroy(&decoder->queue_lock);
      result = -ENOMEM;
      goto decoder_cleanup;
    }
  if (pthread_cond_init(&decoder->queue_space, NULL) != 0)
    {
      pthread_cond_destroy(&decoder->queue_data);
      pthread_mutex_destroy(&decoder->queue_lock);
      result = -ENOMEM;
      goto decoder_cleanup;
    }
  decoder->queue_sync_ready = true;
  {
    pthread_attr_t attr;
    int thread_ret = pthread_attr_init(&attr);
    if (thread_ret == 0)
      {
        (void)pthread_attr_setstacksize(&attr, 12288);
        thread_ret = pthread_create(&decoder->decode_worker, &attr,
                                    rv_mp3_decode_worker, decoder);
        pthread_attr_destroy(&attr);
      }
    if (thread_ret != 0)
      {
        result = -thread_ret;
        goto decoder_cleanup;
      }
    decoder->decode_worker_started = true;
  }

  /* Short receive slices keep this owner joinable. Transient stalls use
   * the existing bounded byte-range retry, without a WLAN callback wait. */
  syslog(LOG_INFO, "[%s] HTTP MP3 begin url=%s\n", RV_MEDIA_TAG,
         player->url);
  syslog(LOG_INFO,
         "[RV-MUSIC-NET] transport=http webclient=1 tls=0 "
         "driver_tls_diag=TCP/443-only\n");

  for (;;)
    {
      char range_header[64];
      const char *request_headers[2];
      unsigned int request_header_count = 1;
      size_t offset_before_request = decoder->stream_offset;

      webclient_set_defaults(&context);
      context.url = player->url;
      context.buffer = http_buffer;
      context.buflen = RV_HTTP_BUFFER_CAP;
      context.sink_callback = rv_mp3_sink;
      context.sink_callback_arg = decoder;
      context.timeout_sec = RV_HTTP_TIMEOUT_SEC;

      /* Keep byte offsets stable if an endpoint negotiates compression. */
      request_headers[0] = "Accept-Encoding: identity";
      if (decoder->stream_offset != 0)
        {
          int n = snprintf(range_header, sizeof(range_header),
                           "Range: bytes=%zu-", decoder->stream_offset);
          if (n < 0 || (size_t)n >= sizeof(range_header))
            {
              webclient_result = -EOVERFLOW;
              break;
            }

          request_headers[1] = request_headers[0];
          request_headers[0] = range_header;
          request_header_count = 2;
          decoder->resume_pending = true;
          decoder->resume_rejected = false;
        }

      context.headers = request_headers;
      context.nheaders = request_header_count;
      decoder->http_context = &context;

      webclient_result = webclient_perform(&context);
      if (webclient_result == 0 &&
          decoder->resume_skip_remaining != 0)
        {
          /* The replay ended before reaching the previous logical offset. */
          webclient_result = -ENODATA;
        }
      if (webclient_result == 0 ||
          rv_player_stop_requested(player) ||
          decoder->resume_rejected ||
          (decoder->sink_error < 0) ||
          !rv_http_retryable(webclient_result))
        {
          break;
        }

      if (decoder->stream_offset > offset_before_request)
        {
          /* A productive connection earns another resume if it drops. */
          no_progress_retries = 0;
        }
      else if (no_progress_retries >= RV_HTTP_MAX_NO_PROGRESS)
        {
          break;
        }
      else
        {
          no_progress_retries++;
        }

      /* Recovery belongs to the existing Wi-Fi owner. This producer must
       * remain joinable during task preemption; do not perform a blocking
       * reconnect or reset the radio used by the Feishu receiver here. */
      if (rv_player_stop_requested(player)) break;
      resume_attempts++;
      syslog(LOG_WARNING,
             "[%s] HTTP MP3 interrupted rc=%d; resume=%u offset=%zu\n",
             RV_MEDIA_TAG, webclient_result, resume_attempts,
             decoder->stream_offset);
      for (unsigned int waited = 0; waited < RV_HTTP_RESUME_DELAY_MS &&
           !rv_player_stop_requested(player); waited += 20)
        usleep(20 * 1000);
    }

  request_elapsed_ms = rv_media_now_ms() - request_start_ms;
  decoder->resume_pending = false;
  decoder->http_context = NULL;
  if (decoder->last_sink_ms != 0)
    {
      unsigned long long now_ms = rv_media_now_ms();
      final_sink_age_ms = now_ms >= decoder->last_sink_ms ?
                          now_ms - decoder->last_sink_ms : 0;
    }
  result = webclient_result;
  http_status = context.http_status;
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
      if (decoder->queue_sync_ready && decoder->decode_worker_started)
        {
          pthread_mutex_lock(&decoder->queue_lock);
          decoder->queue_stop = true;
          pthread_cond_broadcast(&decoder->queue_data);
          pthread_cond_broadcast(&decoder->queue_space);
          pthread_mutex_unlock(&decoder->queue_lock);
          (void)pthread_join(decoder->decode_worker, NULL);
          decoder->decode_worker_started = false;
          if (decoder->queue_error < 0 && result == 0)
            result = decoder->queue_error;
        }
      if (result == 0 && decoder->skip_pcm && !rv_player_stop_requested(player))
        result = -ENODATA; /* Track ended before the saved audible checkpoint. */
      http_bytes = decoder->http_bytes;
      pcm_bytes = decoder->pcm_bytes;
      frame_count = decoder->frame_count;
      decoder_errors = decoder->decoder_errors;
      indata_underflows = decoder->indata_underflows;
      maindata_underflows = decoder->maindata_underflows;
      bad_frame_info = decoder->bad_frame_info;
      sink_calls = decoder->sink_calls;
      max_sink_gap_ms = decoder->max_sink_gap_ms;
      sink_error = decoder->sink_error;
      last_decoder_error = decoder->last_decoder_error;
      pcm_error = decoder->pcm_error;
      if (decoder->decoder)
        {
          MP3FreeDecoder(decoder->decoder);
        }
      if (decoder->queue_sync_ready)
        {
          pthread_cond_destroy(&decoder->queue_space);
          pthread_cond_destroy(&decoder->queue_data);
          pthread_mutex_destroy(&decoder->queue_lock);
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
  pthread_mutex_unlock(&player->lock);
  if (playback_open && !stopped)
    finish_result = robot_audio_playback_finish();
  /* Leave ownership visible while finish drains, so pause can cancel it.
   * Serialize the final free with cancel using the existing player lock. */
  pthread_mutex_lock(&player->lock);
  if (player->playback_open)
    {
      robot_audio_playback_close();
      player->playback_open = false;
    }
  stopped = player->stop_requested;
  pthread_mutex_unlock(&player->lock);

  if (robot_task_release_music_transport != NULL)
    robot_task_release_music_transport();

  if (!stopped && result == 0 && finish_result < 0)
    {
      result = finish_result;
    }

  pthread_mutex_lock(&player->lock);
  if (!stopped) player->started = false;
  pthread_mutex_unlock(&player->lock);

  if (stopped)
    {
      syslog(LOG_INFO, "[%s] HTTP MP3 stopped by request\n", RV_MEDIA_TAG);
    }
  else
    {
      syslog(result == 0 ? LOG_INFO : LOG_ERR,
             "[%s] HTTP MP3 stats http=%zu pcm=%zu frames=%lu status=%ld "
             "decode_errors=%lu last_decode=%d pcm_error=%d finish=%d "
             "indata_underflow=%lu maindata_underflow=%lu "
             "sink_calls=%lu max_gap_ms=%llu request_ms=%llu resumes=%u "
             "last_rx_age_ms=%llu\n",
             RV_MEDIA_TAG, http_bytes, pcm_bytes, frame_count, http_status,
             decoder_errors, last_decoder_error, pcm_error, finish_result,
             indata_underflows, maindata_underflows, sink_calls,
             max_sink_gap_ms, request_elapsed_ms, resume_attempts,
             final_sink_age_ms);
      syslog(result == 0 ? LOG_INFO : LOG_ERR,
             "[%s] HTTP MP3 result webclient=%d sink=%d bad_frame_info=%lu\n",
             RV_MEDIA_TAG, webclient_result, sink_error,
             bad_frame_info);
      syslog(result == 0 ? LOG_INFO : LOG_ERR,
             "[%s] HTTP MP3 complete rc=%d\n", RV_MEDIA_TAG, result);
      rv_emit_event(player, RV_EVENT_COMPLETED, result);
    }
  return NULL;
}

void *robot_legacy_media_open(const char *stream)
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
int robot_legacy_media_stop(void *handle);

int robot_legacy_media_close(void *handle, int pending_stop)
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

int robot_legacy_media_set_event_callback(void *handle, void *cookie,
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

int robot_legacy_media_prepare(void *handle, const char *url, const char *options)
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

  (void)options;
  player->played_bytes = 0;
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

/* All start/stop/pause/close transitions share the existing active lock.
 * The URL worker never takes it, so joining it here cannot invert locks. */
static int rv_media_start_locked(struct rv_media_player *player)
{
  pthread_attr_t attr;
  int ret;
  pthread_mutex_lock(&player->lock);
  unsigned char saved_pause = player->paused;
  bool valid = player->prepared && (!player->started || player->paused);
  bool notify_started = player->paused != RV_MEDIA_AUTO_PAUSED;
  pthread_mutex_unlock(&player->lock);
  if (!valid) return -EINVAL;
  if (g_rv_active_player && g_rv_active_player != player &&
      (robot_legacy_media_is_running(g_rv_active_player) ||
       robot_legacy_media_is_paused(g_rv_active_player))) return -EBUSY;
  if (player->worker_started)
    {
      (void)pthread_join(player->worker, NULL);
      player->worker_started = false;
    }
  ret = robot_task_acquire_music_transport != NULL ?
        robot_task_acquire_music_transport() : -ENOSYS;
  if (ret == -EBUSY)
    {
      /* A music tool may run inside a foreground Agent task. Stage it on
       * the same handle; its end hook/idle tick will start it after TTS. */
      pthread_mutex_lock(&player->lock);
      player->paused = RV_MEDIA_STAGED;
      player->started = true;
      pthread_mutex_unlock(&player->lock);
      g_rv_active_player = player;
      return 0;
    }
  if (ret < 0) return ret;
  if (robot_voice_http_close_idle != NULL) robot_voice_http_close_idle();
  if (vela_tls_pool_cleanup != NULL) vela_tls_pool_cleanup();
  pthread_mutex_lock(&player->lock);
  player->stop_requested = false;
  player->paused = 0;
  ret = robot_audio_playback_open_music();
  if (ret < 0) goto failed;
  player->playback_open = true;
  player->started = true;
  if (player->url_mode)
    {
      ret = pthread_attr_init(&attr);
      if (ret != 0) { ret = -ret; goto close_audio; }
      ret = pthread_attr_setstacksize(&attr, 12288);
      if (ret == 0)
        ret = pthread_create(&player->worker, &attr, rv_url_worker, player);
      pthread_attr_destroy(&attr);
      if (ret != 0) { ret = -ret; goto close_audio; }
      player->worker_started = true;
    }
  g_rv_active_player = player;
  pthread_mutex_unlock(&player->lock);
  if (notify_started) rv_emit_event(player, RV_EVENT_STARTED, 0);
  syslog(LOG_INFO, "[RV-TASK] music running checkpoint_pcm=%zu\n", player->played_bytes);
  return 0;
close_audio:
  player->playback_open = false;
  robot_audio_playback_close();
failed:
  player->started = saved_pause != 0;
  player->paused = saved_pause;
  player->stop_requested = saved_pause != 0;
  pthread_mutex_unlock(&player->lock);
  robot_task_release_music_transport();
  return ret;
}

int robot_legacy_media_start(void *handle)
{
  struct rv_media_player *player = handle;
  int ret;
  if (!player) return -EINVAL;
  pthread_mutex_lock(&g_rv_active_lock);
  ret = rv_media_start_locked(player);
  pthread_mutex_unlock(&g_rv_active_lock);
  return ret;
}

/* Keep allocation/lifetime stable until all music producers have returned.
 * close() alone would free a ring underneath a blocked decode writer. */
static int rv_media_quiesce_locked(struct rv_media_player *player,
                                    unsigned char paused)
{
  uint64_t completed = 0;
  pthread_mutex_lock(&player->lock);
  if (paused && (!player->started || !player->url_mode))
    {
      pthread_mutex_unlock(&player->lock);
      return player->started ? -ENOTSUP : 0;
    }
  if (paused && player->paused)
    {
      if (paused == RV_MEDIA_USER_PAUSED) player->paused = RV_MEDIA_USER_PAUSED;
      pthread_mutex_unlock(&player->lock);
      if (paused == RV_MEDIA_USER_PAUSED) rv_emit_event(player, RV_EVENT_PAUSED, 0);
      return 0;
    }
  player->stop_requested = true;
  player->paused = paused;
  if (player->playback_open)
    completed = robot_audio_playback_cancel();
  if (paused)
    {
      if (completed > SIZE_MAX - player->played_bytes)
        {
          player->paused = RV_MEDIA_USER_PAUSED;
          paused = RV_MEDIA_USER_PAUSED; /* Do not auto-resume an unrepresentable checkpoint. */
        }
      else player->played_bytes += (size_t)completed;
    }
  pthread_mutex_unlock(&player->lock);
  if (player->worker_started)
    {
      (void)pthread_join(player->worker, NULL);
      player->worker_started = false;
    }
  if (player->playback_open)
    {
      player->playback_open = false;
      robot_audio_playback_close();
      if (robot_task_release_music_transport)
        robot_task_release_music_transport();
    }
  if (!paused)
    {
      pthread_mutex_lock(&player->lock);
      player->started = false;
      player->played_bytes = 0;
      pthread_mutex_unlock(&player->lock);
      rv_emit_event(player, RV_EVENT_STOPPED, 0);
    }
  if (paused == RV_MEDIA_USER_PAUSED) rv_emit_event(player, RV_EVENT_PAUSED, 0);
  if (paused)
    syslog(LOG_INFO, "[RV-TASK] music yielded checkpoint_pcm=%zu user_pause=%d\n",
           player->played_bytes, paused == RV_MEDIA_USER_PAUSED);
  return 0;
}

static int rv_media_player_stop_internal(struct rv_media_player *player)
{
  return player ? rv_media_quiesce_locked(player, 0) : -EINVAL;
}

int robot_legacy_media_stop(void *handle)
{
  struct rv_media_player *player = handle;
  int ret;
  if (!player) return -EINVAL;
  pthread_mutex_lock(&g_rv_active_lock);
  ret = rv_media_player_stop_internal(player);
  if (g_rv_active_player == player) g_rv_active_player = NULL;
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

ssize_t robot_legacy_media_write_data(void *handle, const void *data, size_t len)
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
 * Holding g_rv_active_lock across robot_legacy_media_stop() prevents close/free from
 * racing the borrowed active handle. */
int robot_legacy_media_stop_active(void)
{
  struct rv_media_player *player;
  int ret = 0;

  /*
   * Hold the active-player lock through the internal stop.  robot_legacy_media_close()
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

int robot_legacy_media_suspend(void *handle)
{
  struct rv_media_player *player = handle;
  int ret;
  if (!player) return -EINVAL;
  pthread_mutex_lock(&g_rv_active_lock);
  ret = rv_media_quiesce_locked(player, RV_MEDIA_AUTO_PAUSED);
  pthread_mutex_unlock(&g_rv_active_lock);
  return ret;
}

int robot_legacy_media_pause(void *handle)
{
  struct rv_media_player *player = handle;
  int ret;
  if (!player) return -EINVAL;
  pthread_mutex_lock(&g_rv_active_lock);
  ret = rv_media_quiesce_locked(player, RV_MEDIA_USER_PAUSED);
  pthread_mutex_unlock(&g_rv_active_lock);
  return ret;
}

int robot_legacy_media_resume_active(void)
{
  int ret = 0;
  pthread_mutex_lock(&g_rv_active_lock);
  if (g_rv_active_player)
    {
      pthread_mutex_lock(&g_rv_active_player->lock);
      bool resume = g_rv_active_player->started &&
        (g_rv_active_player->paused == RV_MEDIA_AUTO_PAUSED ||
         g_rv_active_player->paused == RV_MEDIA_STAGED);
      pthread_mutex_unlock(&g_rv_active_player->lock);
      if (resume) ret = rv_media_start_locked(g_rv_active_player);
    }
  pthread_mutex_unlock(&g_rv_active_lock);
  return ret;
}

bool robot_legacy_media_start_pending(void *handle)
{
  struct rv_media_player *player = handle;
  if (!player) return false;
  pthread_mutex_lock(&player->lock);
  bool pending = player->started && player->paused == RV_MEDIA_STAGED;
  pthread_mutex_unlock(&player->lock);
  return pending;
}

bool robot_legacy_media_is_running(void *handle)
{
  struct rv_media_player *player = handle;
  bool running;
  if (!player) return false;
  pthread_mutex_lock(&player->lock);
  running = player->started && !player->paused && !player->stop_requested;
  pthread_mutex_unlock(&player->lock);
  return running;
}

bool robot_legacy_media_is_paused(void *handle)
{
  struct rv_media_player *player = handle;
  bool paused;
  if (!player) return false;
  pthread_mutex_lock(&player->lock);
  paused = player->started && player->paused != 0;
  pthread_mutex_unlock(&player->lock);
  return paused;
}

int robot_legacy_media_seek(void *handle, unsigned int position)
{
  (void)handle;
  (void)position;
  return -ENOTSUP;
}

int robot_legacy_media_get_position(void *handle, unsigned int *position)
{
  struct rv_media_player *player = handle;
  uint64_t completed = 0, pending = 0;
  if (!player || !position) return -EINVAL;
  pthread_mutex_lock(&g_rv_active_lock);
  if (player->playback_open)
    (void)robot_audio_playback_progress(&completed, &pending);
  *position = (unsigned int)((player->played_bytes + completed) * 1000ULL /
                             (RV_OUTPUT_RATE * sizeof(int16_t)));
  pthread_mutex_unlock(&g_rv_active_lock);
  return 0;
}

int robot_legacy_media_get_duration(void *handle, unsigned int *duration)
{
  (void)handle;
  if (duration)
    {
      *duration = 0;
    }
  return -ENOTSUP;
}

int robot_legacy_media_policy_set_stream_volume(const char *stream, int volume)
{
  (void)stream;
  (void)volume;
  return 0;
}
