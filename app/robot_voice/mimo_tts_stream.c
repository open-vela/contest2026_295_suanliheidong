#include <nuttx/config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netutils/cJSON.h>
#include "mimo_client.h"
#include "mimo_tts_stream.h"
#include "robot_voice_config.h"
#include "robot_voice_diag.h"
#ifndef ROBOT_VOICE_TTS_VOICE
#  define ROBOT_VOICE_TTS_VOICE "mimo_default"
#endif

static unsigned long long stream_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int b64_value(unsigned char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int decode_pcm(char *p, size_t *bytes)
{
  size_t n = strlen(p), r, w = 0;
  if (n == 0 || (n & 3)) return -EPROTO;
  for (r = 0; r < n; r += 4)
    {
      int a = b64_value(p[r]), b = b64_value(p[r + 1]);
      int c = p[r + 2] == '=' ? 0 : b64_value(p[r + 2]);
      int d = p[r + 3] == '=' ? 0 : b64_value(p[r + 3]);
      bool pad2 = p[r + 2] == '=', pad1 = p[r + 3] == '=';
      if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad1) ||
          ((pad1 || pad2) && r + 4 != n) ||
          (pad2 && (b & 15)) || (pad1 && !pad2 && (c & 3)))
        return -EPROTO;
      p[w++] = (char)((a << 2) | (b >> 4));
      if (!pad2) p[w++] = (char)((b << 4) | (c >> 2));
      if (!pad1) p[w++] = (char)((c << 6) | d);
    }
  *bytes = w;
  return 0;
}

static int pcm_emit(struct rv_tts_sse_s *s, uint8_t *pcm, size_t n)
{
  int ret;
  size_t even;
  unsigned long long sink_at = stream_now_ms();
  if (s->pcm_bytes == 0 && !s->has_carry)
    RV_VERBOSE("[RV-TTS] first streamed audio mono_ms=%llu decoded=%zu\n",
           stream_now_ms(), n);
  if (s->has_carry && n != 0)
    {
      uint8_t pair[2] = {s->carry, pcm[0]};
      ret = s->sink(pair, sizeof(pair), s->arg);
      if (ret < 0) return ret;
      s->pcm_hash = rv_pcm_hash(s->pcm_hash, pair, sizeof(pair));
      s->pcm_bytes += 2;
      s->has_carry = false;
      pcm++;
      n--;
    }
  even = n & ~(size_t)1;
  if (even != 0)
    {
      ret = s->sink(pcm, even, s->arg);
      if (ret < 0) return ret;
      s->pcm_hash = rv_pcm_hash(s->pcm_hash, pcm, even);
      s->pcm_bytes += even;
    }
  if (n & 1)
    {
      s->carry = pcm[even];
      s->has_carry = true;
    }
  unsigned long long elapsed = stream_now_ms() - sink_at;
  s->sink_ms += elapsed;
  if (elapsed > s->max_sink_ms) s->max_sink_ms = elapsed;
  return 0;
}

static int sse_event(struct rv_tts_sse_s *s)
{
  char *line = s->event, *out = s->event;
  cJSON *root, *choices, *first, *delta, *audio, *data;
  size_t bytes;
  int ret = 0;
  bool found = false;
  /* SSE permits comments and multiple data lines. Compact data in place. */
  while (*line)
    {
      char *end = strchr(line, '\n');
      size_t n = end ? (size_t)(end - line) : strlen(line);
      if (n && line[n - 1] == '\r') n--;
      if (n >= 5 && memcmp(line, "data:", 5) == 0)
        {
          char *value = line + 5;
          size_t skip = n > 5 && *value == ' ' ? 6 : 5;
          if (found) *out++ = '\n';
          memmove(out, line + skip, n - skip);
          out += n - skip;
          found = true;
        }
      if (!end) break;
      line = end + 1;
    }
  *out = '\0';
  if (!found || out == s->event) return 0;
  if (strcmp(s->event, "[DONE]") == 0)
    {
      s->done = true;
      return 0;
    }
  if (s->done) return -EPROTO;
  root = cJSON_Parse(s->event);
  if (!root) return -EPROTO;
  if (cJSON_GetObjectItem(root, "error"))
    {
      cJSON_Delete(root);
      return -EIO;
    }
  choices = cJSON_GetObjectItem(root, "choices");
  first = cJSON_GetArrayItem(choices, 0);
  delta = first ? cJSON_GetObjectItem(first, "delta") : NULL;
  audio = delta ? cJSON_GetObjectItem(delta, "audio") : NULL;
  data = cJSON_IsString(audio) ? audio :
         (audio ? cJSON_GetObjectItem(audio, "data") : NULL);
  if (data)
    {
      if (!cJSON_IsString(data) || !data->valuestring)
        ret = -EPROTO;
      else if (data->valuestring[0])
        {
          ret = decode_pcm(data->valuestring, &bytes);
          if (ret == 0)
            {
              unsigned long long now = stream_now_ms();
              uint32_t hash = rv_pcm_hash(RV_PCM_HASH_INIT,
                  (uint8_t *)data->valuestring, bytes);
              if (!s->audio_events) s->first_audio_ms = now;
              else
                {
                  unsigned long long gap = now - s->last_audio_ms;
                  if (gap > s->max_audio_gap_ms) s->max_audio_gap_ms = gap;
                  if (bytes == s->previous_audio_bytes &&
                      hash == s->previous_audio_hash) s->repeated_audio_events++;
                }
              s->last_audio_ms = now;
              s->previous_audio_hash = hash;
              s->previous_audio_bytes = bytes;
              s->audio_events++;
              /* Equal PCM chunks can be valid. Diagnose, never deduplicate
               * by content or lose legitimate source audio. */
              ret = pcm_emit(s, (uint8_t *)data->valuestring, bytes);
            }
        }
    }
  s->events++;
  cJSON_Delete(root);
  return ret;
}

void rv_tts_sse_init(struct rv_tts_sse_s *s, mimo_tts_pcm_cb_t sink, void *arg)
{
  memset(s, 0, sizeof(*s));
  s->pcm_hash = RV_PCM_HASH_INIT;
  s->sink = sink;
  s->arg = arg;
}

int rv_tts_sse_feed(const char *data, size_t len, void *arg)
{
  struct rv_tts_sse_s *s = arg;
  size_t i;
  if (!s || !s->sink || (!data && len)) return -EINVAL;
  if (s->error) return s->error;
  for (i = 0; i < len; i++)
    {
      if (s->used + 2 > s->capacity)
        {
          size_t cap = s->capacity ? s->capacity * 2 : 2048;
          char *p;
          if (cap > ROBOT_VOICE_TTS_SSE_MAX + 1)
            cap = ROBOT_VOICE_TTS_SSE_MAX + 1;
          if (cap <= s->used + 1)
            return s->error = -EOVERFLOW;
          p = realloc(s->event, cap);
          if (!p) return s->error = -ENOMEM;
          s->event = p;
          s->capacity = cap;
        }
      s->event[s->used++] = data[i];
      s->event[s->used] = '\0';
      if ((s->used >= 2 &&
           memcmp(s->event + s->used - 2, "\n\n", 2) == 0) ||
          (s->used >= 3 &&
           memcmp(s->event + s->used - 3, "\n\r\n", 3) == 0) ||
          (s->used >= 4 &&
           memcmp(s->event + s->used - 4, "\r\n\r\n", 4) == 0))
        {
          int ret = sse_event(s);
          s->used = 0;
          if (ret < 0) return s->error = ret;
        }
    }
  return 0;
}

int rv_tts_sse_finish(struct rv_tts_sse_s *s)
{
  if (s->error) return s->error;
  if (!s->done || s->has_carry) return -EPROTO;
  return s->pcm_bytes ? 0 : -ENODATA;
}

void rv_tts_sse_destroy(struct rv_tts_sse_s *s)
{
  free(s->event);
  s->event = NULL;
}

static int rv_tts_http_feed(const char *data, size_t len, void *arg)
{
  struct rv_tts_sse_s *s = arg;
  int ret = rv_tts_sse_feed(data, len, arg);
  /* A validated SSE [DONE] is enough to stop reading an unclosed HTTP
   * stream. The transport closes this socket rather than reusing it. */
  return ret < 0 ? ret : (s->done ? 1 : 0);
}

int mimo_tts_stream_run_timeout(const char *key, const char *text,
                       mimo_tts_pcm_cb_t cb, void *arg,
                       unsigned int timeout_ms)
{
  cJSON *root = NULL, *messages = NULL, *message = NULL, *audio = NULL;
  char *body = NULL;
  struct rv_tts_sse_s s;
  unsigned long long start = stream_now_ms();
  int ret = -ENOMEM;
  rv_tts_sse_init(&s, cb, arg);
  root = cJSON_CreateObject();
  messages = cJSON_CreateArray();
  message = cJSON_CreateObject();
  audio = cJSON_CreateObject();
  if (!root || !messages || !message || !audio) goto out;
  if (!cJSON_AddStringToObject(root, "model", ROBOT_VOICE_TTS_MODEL) ||
      !cJSON_AddBoolToObject(root, "stream", true) ||
      !cJSON_AddStringToObject(message, "role", "assistant") ||
      !cJSON_AddStringToObject(message, "content", text) ||
      !cJSON_AddStringToObject(audio, "format", "pcm16") ||
      !cJSON_AddStringToObject(audio, "voice", ROBOT_VOICE_TTS_VOICE)) goto out;
  cJSON_AddItemToArray(messages, message); message = NULL;
  cJSON_AddItemToObject(root, "messages", messages); messages = NULL;
  cJSON_AddItemToObject(root, "audio", audio); audio = NULL;
  body = cJSON_PrintUnformatted(root);
  if (!body) goto out;
  RV_VERBOSE("[RV-TTS] real SSE stream request bytes=%zu voice=%s\n",
         strlen(body), ROBOT_VOICE_TTS_VOICE);
  ret = robot_mimo_post_stream_timeout(key, body, strlen(body),
    rv_tts_http_feed, &s, timeout_ms);
  if (ret == 0) ret = rv_tts_sse_finish(&s);
  RV_EVENT("[耗时][TTS网络] 首PCM=%lldms 流结束=%lldms 最大音频块间隔=%llums 回调等待=%llums PCM=%zuB rc=%d\n",
    rv_elapsed_ms(start, s.first_audio_ms), rv_elapsed_ms(start, stream_now_ms()),
    s.max_audio_gap_ms, s.sink_ms, s.pcm_bytes, ret);
  RV_VERBOSE("[RV-TTS] stream done events=%u audio_events=%u pcm=%zu hash=%08lx "
         "first_audio_ms=%llu audio_gap_max_ms=%llu repeated_audio_events=%u "
         "sink_ms=%llu sink_max_ms=%llu network_ms=%llu rc=%d\n",
         s.events, s.audio_events, s.pcm_bytes, (unsigned long)s.pcm_hash,
         s.first_audio_ms >= start ? s.first_audio_ms - start : 0,
         s.max_audio_gap_ms, s.repeated_audio_events, s.sink_ms, s.max_sink_ms,
         stream_now_ms() - start, ret);
out:
  free(body);
  cJSON_Delete(audio); cJSON_Delete(message);
  cJSON_Delete(messages); cJSON_Delete(root);
  rv_tts_sse_destroy(&s);
  return ret;
}

int mimo_tts_stream_run(const char *key, const char *text,
                       mimo_tts_pcm_cb_t cb, void *arg)
{
  return mimo_tts_stream_run_timeout(key, text, cb, arg, 0);
}
