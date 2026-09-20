#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include <errno.h>
#include <mbedtls/base64.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netutils/cJSON.h>
#include "mimo_asr.h"
#include "mimo_client.h"
#include "robot_voice_config.h"

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
  printf("[RV-ASR] %s cpu=%d sp=%08lx stack_free=%zu body=%p body_len=%zu "
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

static int make_asr_body(const uint8_t *pcm, size_t pcm_len, char **body_out,
                         size_t *body_len_out)
{
  uint8_t *wav = NULL;
  unsigned char *b64 = NULL;
  char *data_uri = NULL;
  char *body = NULL;
  cJSON *root = NULL;
  cJSON *messages = NULL;
  cJSON *message = NULL;
  cJSON *content = NULL;
  cJSON *item = NULL;
  cJSON *audio = NULL;
  size_t wav_len;
  size_t b64_len = 0;
  size_t b64_cap;
  int ret;

  if (pcm == NULL || body_out == NULL || body_len_out == NULL ||
      pcm_len == 0 || pcm_len > ROBOT_VOICE_MAX_BODY - 44)
    {
      return -EINVAL;
    }

  wav_len = pcm_len + 44;
  b64_cap = ((wav_len + 2) / 3) * 4 + 1;
  wav = malloc(wav_len);
  b64 = malloc(b64_cap);
  if (wav == NULL || b64 == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  memcpy(wav, "RIFF", 4); put32(wav + 4, pcm_len + 36);
  memcpy(wav + 8, "WAVEfmt ", 8); put32(wav + 16, 16);
  put16(wav + 20, 1); put16(wav + 22, 1); put32(wav + 24, 16000);
  put32(wav + 28, 32000); put16(wav + 32, 2); put16(wav + 34, 16);
  memcpy(wav + 36, "data", 4); put32(wav + 40, pcm_len);
  memcpy(wav + 44, pcm, pcm_len);
  ret = mbedtls_base64_encode(b64, b64_cap, &b64_len, wav, wav_len);
  free(wav);
  wav = NULL;
  if (ret != 0)
    {
      ret = -EIO;
      goto out;
    }

  b64[b64_len] = '\0';
  data_uri = malloc(b64_len + 24);
  if (data_uri == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  snprintf(data_uri, b64_len + 24, "data:audio/wav;base64,%s", b64);
  free(b64);
  b64 = NULL;

  root = cJSON_CreateObject();
  messages = cJSON_CreateArray();
  message = cJSON_CreateObject();
  content = cJSON_CreateArray();
  item = cJSON_CreateObject();
  audio = cJSON_CreateObject();
  if (!root || !messages || !message || !content || !item || !audio)
    {
      ret = -ENOMEM;
      goto out;
    }

  cJSON_AddStringToObject(root, "model", ROBOT_VOICE_ASR_MODEL);
  cJSON_AddStringToObject(message, "role", "user");
  cJSON_AddStringToObject(item, "type", "input_audio");
  cJSON_AddStringToObject(audio, "data", data_uri);
  cJSON_AddItemToObject(item, "input_audio", audio); audio = NULL;
  cJSON_AddItemToArray(content, item); item = NULL;
  cJSON_AddItemToObject(message, "content", content); content = NULL;
  cJSON_AddItemToArray(messages, message); message = NULL;
  cJSON_AddItemToObject(root, "messages", messages); messages = NULL;
  cJSON_AddItemToObject(root, "asr_options", cJSON_CreateObject());
  cJSON_AddStringToObject(cJSON_GetObjectItem(root, "asr_options"),
                          "language", "zh");

  body = cJSON_PrintUnformatted(root);
  if (body == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  *body_out = body;
  *body_len_out = strlen(body);
  body = NULL;
  ret = 0;

out:
  cJSON_Delete(root);
  cJSON_Delete(messages);
  cJSON_Delete(message);
  cJSON_Delete(content);
  cJSON_Delete(item);
  cJSON_Delete(audio);
  free(wav);
  free(b64);
  free(data_uri);
  free(body);
  return ret;
}

int mimo_asr_recognize(const char *api_key, const uint8_t *pcm, size_t pcm_len,
                       char *text, size_t text_cap)
{
  char *body = NULL;
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

  printf("[RV-ASR] request begin\n");
  if (text != NULL && text_cap > 0)
    {
      text[0] = '\0';
    }
  if (!api_key || !pcm || !text || text_cap == 0 || pcm_len == 0 ||
      pcm_len > ROBOT_VOICE_MAX_BODY - 44)
    {
      return -EINVAL;
    }

  ret = make_asr_body(pcm, pcm_len, &body, &body_len);
  if (ret < 0)
    {
      goto out;
    }

  ret = rv_guard_alloc(&body_buf, body_len + 1);
  if (ret < 0)
    {
      goto out;
    }
  memcpy(body_buf.data, body, body_len + 1);
  free(body);
  body = NULL;

  ret = rv_guard_alloc(&response_buf, ROBOT_VOICE_HTTP_RESPONSE);
  if (ret < 0)
    {
      goto out;
    }

  printf("[RV-GUARD] body %s\n", rv_guard_ok(&body_buf) ? "PASS" : "FAIL");
  printf("[RV-GUARD] response %s\n",
         rv_guard_ok(&response_buf) ? "PASS" : "FAIL");
  if (!rv_guard_ok(&body_buf) || !rv_guard_ok(&response_buf))
    {
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
  ret = robot_mimo_post(api_key, (char *)body_buf.data, body_len,
                        (char *)response_buf.data, response_buf.cap,
                        &response_len);
  rv_asr_diag("after_transport", body_buf.data, body_len, body_buf.cap,
              response_buf.data, response_len, response_buf.cap);
  printf("[RV-GUARD] body %s\n", rv_guard_ok(&body_buf) ? "PASS" : "FAIL");
  printf("[RV-GUARD] response %s\n",
         rv_guard_ok(&response_buf) ? "PASS" : "FAIL");
  if (!rv_guard_ok(&body_buf) || !rv_guard_ok(&response_buf))
    {
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
  strncpy(text, answer->valuestring, text_cap - 1); text[text_cap - 1] = '\0';
  printf("[RV-ASR] pcm=%zu response=%zu result=%zu\n", pcm_len, response_len,
         strlen(text));
  ret = 0;
out:
  cJSON_Delete(root);
  free(body);
  rv_guard_free(&body_buf);
  rv_guard_free(&response_buf);
  printf("[RV-ASR] request done rc=%d\n", ret);
  return ret;
}
