#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netutils/cJSON.h>
#include "mimo_asr.h"
#include "mimo_client.h"
#include "mimo_llm_stream.h"
#include "robot_voice_config.h"
#include "robot_voice_retry.h"
#include "robot_voice_diag.h"

#define RV_ASR_GUARD 0x52564153u

struct rv_guard_buf_s
{
  unsigned char *raw;
  unsigned char *data;
  size_t cap;
};

static int rv_guard_alloc(struct rv_guard_buf_s *buf, size_t cap)
{
  size_t total;

  if (buf == NULL || cap == 0 || cap > SIZE_MAX - 8)
    {
      return -EINVAL;
    }

  total = cap + 8;
  buf->raw = calloc(1, total);
  if (buf->raw == NULL)
    {
      return -ENOMEM;
    }

  memcpy(buf->raw, &(uint32_t){RV_ASR_GUARD}, sizeof(uint32_t));
  memcpy(buf->raw + 4 + cap, &(uint32_t){RV_ASR_GUARD}, sizeof(uint32_t));
  buf->data = buf->raw + 4;
  buf->cap = cap;
  return 0;
}

static bool rv_guard_ok(const struct rv_guard_buf_s *buf)
{
  uint32_t head = 0;
  uint32_t tail = 0;

  if (buf == NULL || buf->raw == NULL || buf->data == NULL || buf->cap == 0)
    {
      return false;
    }

  memcpy(&head, buf->raw, sizeof(head));
  memcpy(&tail, buf->data + buf->cap, sizeof(tail));
  return head == RV_ASR_GUARD && tail == RV_ASR_GUARD;
}

static void rv_guard_free(struct rv_guard_buf_s *buf)
{
  if (buf != NULL)
    {
      free(buf->raw);
      memset(buf, 0, sizeof(*buf));
    }
}

static size_t rv_stack_free(void)
{
  struct stackinfo_s info;
  uintptr_t sp;
  uintptr_t low;

  if (nxsched_get_stackinfo(0, &info) < 0)
    {
      return 0;
    }

  sp = (uintptr_t)up_getsp();
  low = (uintptr_t)info.stack_alloc_ptr;
  return sp > low ? sp - low : 0;
}

static void rv_asr_diag(const char *phase, const void *body, size_t body_len,
                        size_t body_cap, const void *response,
                        size_t response_len, size_t response_cap)
{
  RV_VERBOSE("[RV-ASR] %s cpu=%d sp=%08lx stack_free=%zu body=%p body_len=%zu "
         "body_cap=%zu response=%p response_len=%zu response_cap=%zu\n",
         phase, sched_getcpu(), (unsigned long)up_getsp(), rv_stack_free(),
         body, body_len, body_cap, response, response_len, response_cap);
}

static void put16(uint8_t *p, unsigned int v)
{
  p[0] = v;
  p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

/* Build the JSON/base64 payload directly in its guarded send buffer. The
 * original path copied WAV -> base64 -> URI -> cJSON -> JSON -> guard buffer,
 * multiplying peak heap use when two long ASR requests overlap. */
static int make_asr_body(const uint8_t *pcm, size_t pcm_len,
                         struct rv_guard_buf_s *out, size_t *body_len_out)
{
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char prefix[] =
    "{\"model\":\"" ROBOT_VOICE_ASR_MODEL "\",\"messages\":[{\"role\":\"user\","
    "\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
    "\"data:audio/wav;base64,";
  static const char suffix[] =
    "\"}}]}],\"asr_options\":{\"language\":\"" ROBOT_VOICE_ASR_LANGUAGE "\"},"
#if ROBOT_VOICE_ASR_STREAM
    "\"stream\":true}";
#else
    "\"stream\":false}";
#endif
  uint8_t header[44];
  size_t wav_len, b64_len, body_len, i, w;
  int ret;

  if (!pcm || !out || !body_len_out || pcm_len == 0 || (pcm_len & 1) ||
      pcm_len > ROBOT_VOICE_MAX_BODY - 44)
    return -EINVAL;
  wav_len = pcm_len + sizeof(header);
  b64_len = ((wav_len + 2) / 3) * 4;
  body_len = sizeof(prefix) - 1 + b64_len + sizeof(suffix) - 1;
  if (body_len > ROBOT_VOICE_MAX_BODY) return -EOVERFLOW;
  ret = rv_guard_alloc(out, body_len + 1);
  if (ret < 0) return ret;

  memcpy(header, "RIFF", 4); put32(header + 4, pcm_len + 36);
  memcpy(header + 8, "WAVEfmt ", 8); put32(header + 16, 16);
  put16(header + 20, 1); put16(header + 22, 1);
  put32(header + 24, ROBOT_VOICE_CAPTURE_RATE);
  put32(header + 28, ROBOT_VOICE_CAPTURE_RATE * 2);
  put16(header + 32, 2); put16(header + 34, 16);
  memcpy(header + 36, "data", 4); put32(header + 40, pcm_len);
  memcpy(out->data, prefix, sizeof(prefix) - 1);
  w = sizeof(prefix) - 1;
  for (i = 0; i < wav_len; i += 3)
    {
      uint8_t v[3] = {0, 0, 0};
      size_t j, n = wav_len - i;
      if (n > 3) n = 3;
      for (j = 0; j < n; j++)
        v[j] = i + j < sizeof(header) ? header[i + j] : pcm[i + j - sizeof(header)];
      out->data[w++] = alphabet[v[0] >> 2];
      out->data[w++] = alphabet[((v[0] & 3) << 4) | (v[1] >> 4)];
      out->data[w++] = n > 1 ? alphabet[((v[1] & 15) << 2) | (v[2] >> 6)] : '=';
      out->data[w++] = n > 2 ? alphabet[v[2] & 63] : '=';
    }
  memcpy(out->data + w, suffix, sizeof(suffix));
  *body_len_out = body_len;
  return 0;
}

#if ROBOT_VOICE_ASR_STREAM
struct rv_asr_stream_s
{
  struct rv_llm_sse_s parser;
  mimo_asr_partial_cb_t partial;
  void *arg;
  size_t wire_bytes;
  int callback_error;
};

static void rv_asr_delta(void *arg, const char *p, size_t n, bool tools)
{
  struct rv_asr_stream_s *s = arg;
  (void)p;
  if (tools) { s->callback_error = -EPROTO; return; }
  if (n && s->partial && !s->callback_error) {
    cJSON *text = cJSON_GetObjectItem(s->parser.message, "content");
    s->callback_error = s->partial(text->valuestring, s->arg);
    if (s->callback_error > 0) s->callback_error = -EINVAL;
  }
}

static int rv_asr_feed(const char *p, size_t n, void *arg)
{
  struct rv_asr_stream_s *s = arg;
  if (s->partial && s->partial(NULL, s->arg) != 0) return -ECANCELED;
  s->wire_bytes += n;
  int ret = rv_llm_sse_feed(p, n, &s->parser);
  if (ret < 0) return ret;
  if (s->callback_error) return s->callback_error;
  return s->parser.done ? 1 : 0;
}

static int rv_asr_stream_run(const char *key, const char *body, size_t len,
  char *text, size_t cap, mimo_asr_partial_cb_t partial, void *arg,
  size_t *wire_bytes, unsigned int timeout_ms)
{
  struct rv_asr_stream_s s = {0};
  cJSON *answer, *finish;
  int ret;
  s.partial = partial; s.arg = arg;
  ret = rv_llm_sse_init(&s.parser, ROBOT_VOICE_TEXT_CAP - 1, rv_asr_delta, &s);
  if (ret < 0) return ret;
  RV_VERBOSE("[RV-ASR] SSE enabled; audio snapshot uploaded before recognition\n");
  ret = robot_mimo_post_stream_timeout(key, body, len, rv_asr_feed, &s,
                                       timeout_ms);
  if (ret == 0) ret = rv_llm_sse_validate(&s.parser);
  /* A valid empty transcript is silence, never a partial command. */
  if (ret == -ENODATA && s.parser.done && s.parser.finished && !s.parser.tools) ret = 0;
  finish = cJSON_GetObjectItem(s.parser.choice, "finish_reason");
  if (ret == 0 && (!cJSON_IsString(finish) || strcmp(finish->valuestring, "stop")))
    ret = -EOVERFLOW; /* length/content_filter cannot execute a truncated command */
  answer = cJSON_GetObjectItem(s.parser.message, "content");
  if (ret == 0 && (!cJSON_IsString(answer) || strlen(answer->valuestring) >= cap)) ret = -EOVERFLOW;
  if (ret == 0) memcpy(text, answer->valuestring, strlen(answer->valuestring) + 1);
  *wire_bytes = s.wire_bytes;
  rv_llm_sse_destroy(&s.parser);
  return ret;
}
#endif

static int mimo_asr_recognize_once(const char *api_key, const uint8_t *pcm, size_t pcm_len,
                       char *text, size_t text_cap,
                       mimo_asr_partial_cb_t partial, void *partial_arg,
                       unsigned int timeout_ms)
{
  struct rv_guard_buf_s body_buf = { 0 };
  struct rv_guard_buf_s response_buf = { 0 };
  size_t response_len = 0;
  size_t body_len = 0;
  cJSON *root = NULL;
  cJSON *choices = NULL;
  cJSON *first = NULL;
  cJSON *msg = NULL;
  cJSON *answer = NULL;
  int ret;

  (void)partial; (void)partial_arg;
  unsigned long long start_ms = rv_diag_now_ms();
  unsigned long long body_ready_ms = 0, transport_done_ms = 0;
  RV_VERBOSE("[RV-ASR] request begin\n");
  if (text != NULL && text_cap > 0)
    {
      text[0] = '\0';
    }
  if (!api_key || !pcm || !text || text_cap == 0 || !timeout_ms || pcm_len == 0 ||
      pcm_len > ROBOT_VOICE_MAX_BODY - 44)
    {
      return -EINVAL;
    }

  ret = make_asr_body(pcm, pcm_len, &body_buf, &body_len);
  if (ret < 0)
    {
      goto out;
    }

#if ROBOT_VOICE_ASR_STREAM
  if (rv_stack_free() < 8192) { ret = -EOVERFLOW; goto out; }
  body_ready_ms = rv_diag_now_ms();
  ret = rv_asr_stream_run(api_key, (char *)body_buf.data, body_len,
                         text, text_cap, partial, partial_arg, &response_len,
                         timeout_ms);
  transport_done_ms = rv_diag_now_ms();
  if (!rv_guard_ok(&body_buf)) { text[0] = 0; ret = -EFAULT; }
  goto out;
#endif

  ret = rv_guard_alloc(&response_buf, ROBOT_VOICE_HTTP_RESPONSE);
  if (ret < 0)
    {
      goto out;
    }

  RV_VERBOSE("[RV-GUARD] body %s\n", rv_guard_ok(&body_buf) ? "PASS" : "FAIL");
  RV_VERBOSE("[RV-GUARD] response %s\n",
         rv_guard_ok(&response_buf) ? "PASS" : "FAIL");
  if (!rv_guard_ok(&body_buf) || !rv_guard_ok(&response_buf))
    {
      printf("[RV-GUARD] ASR buffer corruption body_ok=%u response_ok=%u\n",
             (unsigned int)rv_guard_ok(&body_buf), (unsigned int)rv_guard_ok(&response_buf));
      ret = -EFAULT;
      goto out;
    }

  rv_asr_diag("before_transport", body_buf.data, body_len, body_buf.cap,
              response_buf.data, 0, response_buf.cap);
  if (rv_stack_free() < 8192)
    {
      printf("[RV-ASR] refusing transport: low stack free=%zu\n",
             rv_stack_free());
      ret = -EOVERFLOW;
      goto out;
    }
  body_ready_ms = rv_diag_now_ms();
  ret = robot_mimo_post_timeout(api_key, (char *)body_buf.data, body_len,
                        (char *)response_buf.data, response_buf.cap,
                        &response_len, timeout_ms);
  transport_done_ms = rv_diag_now_ms();
  rv_asr_diag("after_transport", body_buf.data, body_len, body_buf.cap,
              response_buf.data, response_len, response_buf.cap);
  RV_VERBOSE("[RV-GUARD] body %s\n", rv_guard_ok(&body_buf) ? "PASS" : "FAIL");
  RV_VERBOSE("[RV-GUARD] response %s\n",
         rv_guard_ok(&response_buf) ? "PASS" : "FAIL");
  if (!rv_guard_ok(&body_buf) || !rv_guard_ok(&response_buf))
    {
      printf("[RV-GUARD] ASR buffer corruption body_ok=%u response_ok=%u\n",
             (unsigned int)rv_guard_ok(&body_buf), (unsigned int)rv_guard_ok(&response_buf));
      ret = -EFAULT;
      goto out;
    }
  if (ret < 0)
    {
      goto out;
    }
  if (response_len == response_buf.cap)
    {
      ret = -EOVERFLOW;
      goto out;
    }

  root = cJSON_Parse((char *)response_buf.data);
  if (!root) { ret = -EPROTO; goto out; }
  choices = cJSON_GetObjectItem(root, "choices");
  first = cJSON_GetArrayItem(choices, 0);
  msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
  answer = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
  if (!cJSON_IsString(answer) || !answer->valuestring)
    { ret = -EPROTO; goto out; }
  if (strlen(answer->valuestring) >= text_cap)
    { ret = -EOVERFLOW; goto out; }
  memcpy(text, answer->valuestring, strlen(answer->valuestring) + 1);

  ret = 0;
out:
  cJSON_Delete(root);
  rv_guard_free(&body_buf);
  rv_guard_free(&response_buf);
  unsigned long long end_ms = rv_diag_now_ms();
  RV_VERBOSE("[RV-ASR] done pcm=%zu response=%zu result=%zu encode_ms=%llu "
         "network_ms=%llu parse_ms=%llu total_ms=%llu rc=%d\n",
         pcm_len, response_len, ret == 0 ? strlen(text) : 0,
         body_ready_ms ? body_ready_ms - start_ms : 0,
         transport_done_ms ? transport_done_ms - body_ready_ms : 0,
         transport_done_ms ? end_ms - transport_done_ms : 0,
         end_ms - start_ms, ret);
  return ret;
}

struct rv_asr_retry_s
{
  const char *key;
  const uint8_t *pcm;
  size_t pcm_len;
  char *text;
  size_t cap;
  mimo_asr_partial_cb_t partial;
  void *partial_arg;
};

static int rv_asr_retry_attempt(void *arg, unsigned int timeout_ms)
{
  struct rv_asr_retry_s *retry = arg;
  return mimo_asr_recognize_once(retry->key, retry->pcm,
    retry->pcm_len, retry->text, retry->cap, retry->partial,
    retry->partial_arg, timeout_ms);
}

static bool rv_asr_retry_cancelled(void *arg)
{
  struct rv_asr_retry_s *retry = arg;
  return retry->partial && retry->partial(NULL, retry->partial_arg) != 0;
}

int mimo_asr_recognize_progress(const char *api_key, const uint8_t *pcm,
                       size_t pcm_len, char *text, size_t text_cap,
                       mimo_asr_partial_cb_t partial, void *partial_arg)
{
  struct rv_asr_retry_s retry = {
    .key = api_key, .pcm = pcm, .pcm_len = pcm_len, .text = text,
    .cap = text_cap, .partial = partial, .partial_arg = partial_arg,
  };
  unsigned int retries = 0;
  int ret = rv_asr_retry_attempt(&retry, ROBOT_VOICE_HTTP_ASR_MS);
  if (ret < 0)
    {
      int last = ret;
      ret = robot_voice_retry_after_failure(rv_asr_retry_attempt, &retry,
        ret, rv_asr_retry_cancelled, &retry, &retries);
      if (retries)
        RV_EVENT("[RV-RETRY] ASR retry=%u/%u rc=%d\n", retries,
                 ROBOT_VOICE_RETRY_MAX, ret ? ret : last);
    }
  return ret;
}

int mimo_asr_recognize(const char *key, const uint8_t *pcm, size_t bytes,
                       char *text, size_t cap)
{
  return mimo_asr_recognize_progress(key, pcm, bytes, text, cap, NULL, NULL);
}
