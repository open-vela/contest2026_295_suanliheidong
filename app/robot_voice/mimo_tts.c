#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netutils/cJSON.h>

#include "mimo_client.h"
#include "mimo_tts.h"
#include "robot_voice_config.h"

#define RV_TTS_WORKER_STACK (32 * 1024)

static uint16_t rv_le16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rv_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rv_pcm_fnv1a32(const uint8_t *data, size_t len)
{
  uint32_t h = 2166136261u;
  size_t i;

  for (i = 0; i < len; i++)
    {
      h ^= data[i];
      h *= 16777619u;
    }

  return h;
}

static int rv_b64_value(unsigned char c)
{
  if (c >= 'A' && c <= 'Z')
    {
      return c - 'A';
    }

  if (c >= 'a' && c <= 'z')
    {
      return c - 'a' + 26;
    }

  if (c >= '0' && c <= '9')
    {
      return c - '0' + 52;
    }

  if (c == '+')
    {
      return 62;
    }

  if (c == '/')
    {
      return 63;
    }

  return -1;
}

/* Decode MiMo's complete audio.data Base64 string in place.
 *
 * Forward in-place decode is safe because every four input bytes become at
 * most three output bytes, so the write cursor never overtakes unread input.
 * This intentionally removes streaming-decoder state from long TTS responses.
 */

static int rv_base64_decode_inplace(char *data, size_t text_len,
                                    size_t *decoded_len)
{
  uint8_t *out = (uint8_t *)data;
  size_t r = 0;
  size_t w = 0;
  bool padded = false;

  if (data == NULL || decoded_len == NULL || text_len == 0 ||
      (text_len & 3) != 0)
    {
      return -EPROTO;
    }

  while (r < text_len)
    {
      unsigned char c0 = (unsigned char)data[r + 0];
      unsigned char c1 = (unsigned char)data[r + 1];
      unsigned char c2 = (unsigned char)data[r + 2];
      unsigned char c3 = (unsigned char)data[r + 3];
      int v0;
      int v1;
      int v2;
      int v3;

      if (padded)
        {
          return -EPROTO;
        }

      v0 = rv_b64_value(c0);
      v1 = rv_b64_value(c1);
      if (v0 < 0 || v1 < 0)
        {
          return -EPROTO;
        }

      out[w++] = (uint8_t)((v0 << 2) | (v1 >> 4));

      if (c2 == '=')
        {
          if (c3 != '=' || r + 4 != text_len)
            {
              return -EPROTO;
            }

          padded = true;
        }
      else
        {
          v2 = rv_b64_value(c2);
          if (v2 < 0)
            {
              return -EPROTO;
            }

          out[w++] = (uint8_t)(((v1 & 15) << 4) | (v2 >> 2));

          if (c3 == '=')
            {
              if (r + 4 != text_len)
                {
                  return -EPROTO;
                }

              padded = true;
            }
          else
            {
              v3 = rv_b64_value(c3);
              if (v3 < 0)
                {
                  return -EPROTO;
                }

              out[w++] = (uint8_t)(((v2 & 3) << 6) | v3);
            }
        }

      r += 4;
    }

  *decoded_len = w;
  return 0;
}

/* Parse one complete decoded RIFF/WAVE image before emitting the first PCM
 * byte. Only PCM16LE / 24 kHz / mono is accepted. Malformed or truncated WAV
 * data fails before partial or corrupted PCM reaches the speaker.
 */

static int rv_emit_wav_pcm(uint8_t *wav, size_t wav_len,
                           mimo_tts_pcm_cb_t cb, void *arg,
                           size_t *pcm_total)
{
  size_t pos = 12;
  uint8_t *pcm = NULL;
  size_t pcm_len = 0;
  bool fmt_seen = false;
  uint16_t format = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t rate = 0;

  if (wav == NULL || cb == NULL || pcm_total == NULL || wav_len < 12 ||
      memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
    {
      return -EPROTO;
    }

  while (pos + 8 <= wav_len)
    {
      uint8_t *chunk = wav + pos;
      uint32_t chunk_len = rv_le32(chunk + 4);
      size_t data_pos = pos + 8;
      size_t next;

      if ((size_t)chunk_len > wav_len - data_pos)
        {
          return -EPROTO;
        }

      if (memcmp(chunk, "fmt ", 4) == 0)
        {
          if (chunk_len < 16)
            {
              return -EPROTO;
            }

          format = rv_le16(wav + data_pos + 0);
          channels = rv_le16(wav + data_pos + 2);
          rate = rv_le32(wav + data_pos + 4);
          bits = rv_le16(wav + data_pos + 14);
          fmt_seen = true;
        }
      else if (memcmp(chunk, "data", 4) == 0)
        {
          pcm = wav + data_pos;
          pcm_len = chunk_len;
          break;
        }

      next = data_pos + (size_t)chunk_len + (chunk_len & 1u);
      if (next < data_pos || next > wav_len)
        {
          return -EPROTO;
        }

      pos = next;
    }

  if (!fmt_seen || pcm == NULL || format != 1 || channels != 1 ||
      rate != 24000 || bits != 16 || pcm_len == 0 || (pcm_len & 1) != 0)
    {
      printf("[RV-TTS] WAV reject fmt=%u rate=%lu ch=%u bits=%u data=%zu\n",
             format, (unsigned long)rate, channels, bits, pcm_len);
      return -EPROTO;
    }

  printf("[RV-TTS] WAV fmt pcm_s16le %luHz %uch %ubit\n",
         (unsigned long)rate, channels, bits);
  printf("[RV-TTS] WAV data begin bytes=%zu rate=%lu parser=whole-wav "
         "pcm_fnv=%08lx\n",
         pcm_len, (unsigned long)rate,
         (unsigned long)rv_pcm_fnv1a32(pcm, pcm_len));

  /* Preserve one continuous producer write.
   *
   * The playback layer already owns the proven 1024-byte source-block
   * splitting, ring-space waits and 8-slot cadence.  Splitting again here
   * changed producer scheduling and correlated with the "every syllable
   * twice" regression.  Hand the exact WAV data payload to playback once and
   * let robot_audio_playback_write() pace it.
   */
  {
    int ret = cb(pcm, pcm_len, arg);

    if (ret < 0)
      {
        return ret;
      }
  }

  *pcm_total = pcm_len;
  return 0;
}


#ifndef ROBOT_VOICE_TTS_VOICE
#  define ROBOT_VOICE_TTS_VOICE "mimo_default"
#endif

struct tts_sink_s
{
  mimo_tts_pcm_cb_t cb;
  void *arg;
};

struct tts_worker_s
{
  char *api_key;
  char *text;
  mimo_tts_pcm_cb_t cb;
  void *arg;
  int result;
};

static int tts_sink(const uint8_t *pcm, size_t len, void *arg)
{
  struct tts_sink_s *sink = arg;

  if (sink == NULL || sink->cb == NULL)
    {
      return -EINVAL;
    }

  return sink->cb(pcm, len, sink->arg);
}

/* The TTS response can be well over one megabyte because WAV PCM is base64
 * encoded inside choices[0].message.audio.data.  Parsing that response with
 * cJSON duplicates the very large base64 string and creates unnecessary heap
 * pressure.  Locate only the audio.data JSON string in-place instead.
 *
 * MiMo audio.data is base64, so the value itself cannot contain an unescaped
 * quote.  We still handle normal JSON whitespace around ':' for robustness.
 */

static char *find_audio_data(char *json, size_t len, char **value_end)
{
  static const char audio_key[] = "\"audio\"";
  static const char data_key[] = "\"data\"";
  char *begin = json;
  char *limit = json + len;
  char *audio;
  char *data;
  char *p;

  if (json == NULL || value_end == NULL)
    {
      return NULL;
    }

  *value_end = NULL;

  audio = strstr(begin, audio_key);
  if (audio == NULL || audio >= limit)
    {
      return NULL;
    }

  data = strstr(audio + sizeof(audio_key) - 1, data_key);
  if (data == NULL || data >= limit)
    {
      return NULL;
    }

  p = data + sizeof(data_key) - 1;
  while (p < limit && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    {
      p++;
    }

  if (p >= limit || *p != ':')
    {
      return NULL;
    }

  p++;
  while (p < limit && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    {
      p++;
    }

  if (p >= limit || *p != '"')
    {
      return NULL;
    }

  begin = ++p;
  while (p < limit)
    {
      if (*p == '"')
        {
          *value_end = p;
          return begin;
        }

      /* Base64 itself never needs JSON escaping.  Treat an escape as a
       * malformed audio payload rather than accidentally decoding it.
       */

      if (*p == '\\')
        {
          return NULL;
        }

      p++;
    }

  return NULL;
}

static int mimo_tts_run(const char *api_key, const char *text,
                        mimo_tts_pcm_cb_t cb, void *arg)
{
  cJSON *root = NULL;
  cJSON *messages = NULL;
  cJSON *message = NULL;
  cJSON *audio = NULL;
  char *body = NULL;
  char *response = NULL;
  char *audio_data = NULL;
  char *audio_end = NULL;
  size_t body_len;
  size_t response_len = 0;
  struct tts_sink_s sink;
  int ret = -ENOMEM;
  char saved;

  root = cJSON_CreateObject();
  messages = cJSON_CreateArray();
  message = cJSON_CreateObject();
  audio = cJSON_CreateObject();

  if (root == NULL || messages == NULL || message == NULL || audio == NULL)
    {
      goto out;
    }

  if (!cJSON_AddStringToObject(root, "model", ROBOT_VOICE_TTS_MODEL) ||
      !cJSON_AddStringToObject(message, "role", "assistant") ||
      !cJSON_AddStringToObject(message, "content", text) ||
      !cJSON_AddStringToObject(audio, "format", "wav") ||
      !cJSON_AddStringToObject(audio, "voice", ROBOT_VOICE_TTS_VOICE))
    {
      goto out;
    }

  cJSON_AddItemToArray(messages, message);
  message = NULL;
  cJSON_AddItemToObject(root, "messages", messages);
  messages = NULL;
  cJSON_AddItemToObject(root, "audio", audio);
  audio = NULL;

  body = cJSON_PrintUnformatted(root);
  if (body == NULL)
    {
      goto out;
    }

  body_len = strlen(body);

  /* +1 is intentional: robot_mimo_post() receives exactly the advertised
   * response capacity, while we retain one private byte for a NUL terminator.
   */

  response = malloc((size_t)ROBOT_VOICE_TTS_RESPONSE + 1);
  if (response == NULL)
    {
      goto out;
    }

  response[0] = '\0';
  response[ROBOT_VOICE_TTS_RESPONSE] = '\0';

  printf("[RV-TTS] request begin body=%zu response_cap=%u voice=%s\n",
         body_len, (unsigned int)ROBOT_VOICE_TTS_RESPONSE,
         ROBOT_VOICE_TTS_VOICE);

  ret = robot_mimo_post(api_key, body, body_len, response,
                        ROBOT_VOICE_TTS_RESPONSE, &response_len);
  if (ret < 0)
    {
      printf("[RV-TTS] transport failed rc=%d\n", ret);
      goto out;
    }

  if (response_len >= ROBOT_VOICE_TTS_RESPONSE)
    {
      printf("[RV-TTS] response too large len=%zu cap=%u\n",
             response_len, (unsigned int)ROBOT_VOICE_TTS_RESPONSE);
      ret = -ENOSPC;
      goto out;
    }

  response[response_len] = '\0';
  printf("[RV-TTS] transport done response=%zu\n", response_len);

  audio_data = find_audio_data(response, response_len, &audio_end);
  if (audio_data == NULL || audio_end == NULL || audio_end <= audio_data)
    {
      printf("[RV-TTS] audio.data not found\n");
      ret = -EPROTO;
      goto out;
    }

  /* Temporarily terminate the base64 substring in-place.  This avoids both a
   * multi-megabyte cJSON string allocation and another base64 copy.
   */

  saved = *audio_end;
  *audio_end = '\0';
  sink.cb = cb;
  sink.arg = arg;

  {
    size_t b64_len = (size_t)(audio_end - audio_data);
    size_t wav_len = 0;
    size_t pcm_len = 0;

    ret = rv_base64_decode_inplace(audio_data, b64_len, &wav_len);
    if (ret == 0)
      {
        printf("[RV-TTS] base64 whole decode b64=%zu wav=%zu\n",
               b64_len, wav_len);

        ret = rv_emit_wav_pcm((uint8_t *)audio_data, wav_len,
                              tts_sink, &sink, &pcm_len);
      }

    printf("[RV-TTS] decode done b64=%zu wav=%zu pcm=%zu result=%d "
           "parser=whole-wav\n",
           b64_len, wav_len, pcm_len, ret);
  }

  *audio_end = saved;

out:
  free(response);
  free(body);
  cJSON_Delete(audio);
  cJSON_Delete(message);
  cJSON_Delete(messages);
  cJSON_Delete(root);
  return ret;
}

static void *mimo_tts_worker(void *arg)
{
  struct tts_worker_s *work = arg;

  printf("[RV-TTS] worker start stack=%u\n",
         (unsigned int)RV_TTS_WORKER_STACK);
  work->result = mimo_tts_run(work->api_key, work->text,
                              work->cb, work->arg);
  printf("[RV-TTS] worker done rc=%d\n", work->result);
  return NULL;
}

int mimo_tts_speak_stream(const char *api_key, const char *text,
                          mimo_tts_pcm_cb_t cb, void *arg)
{
  struct tts_worker_s work;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (api_key == NULL || api_key[0] == '\0' || text == NULL ||
      text[0] == '\0' || cb == NULL)
    {
      return -EINVAL;
    }

  memset(&work, 0, sizeof(work));
  work.api_key = strdup(api_key);
  work.text = strdup(text);
  work.cb = cb;
  work.arg = arg;
  work.result = -EIO;

  if (work.api_key == NULL || work.text == NULL)
    {
      free(work.api_key);
      free(work.text);
      return -ENOMEM;
    }

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      free(work.api_key);
      free(work.text);
      return -ret;
    }

  ret = pthread_attr_setstacksize(&attr, RV_TTS_WORKER_STACK);
  if (ret == 0)
    {
      ret = pthread_create(&thread, &attr, mimo_tts_worker, &work);
    }

  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      free(work.api_key);
      free(work.text);
      return -ret;
    }

  /* Keep voice_channel_speak() synchronous.  The ai_agent outbound dispatcher
   * owns its message text and may free it immediately after speak returns.
   * The worker uses private key/text copies and is joined before those copies
   * are released, avoiding both stack exhaustion and use-after-free.
   */

  ret = pthread_join(thread, NULL);
  if (ret != 0)
    {
      work.result = -ret;
    }

  free(work.api_key);
  free(work.text);
  return work.result;
}
