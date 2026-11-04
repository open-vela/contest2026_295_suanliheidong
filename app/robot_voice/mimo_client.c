#include "robot_voice_diag.h"
#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <infra/vela_tls.h>
#include <netutils/cJSON.h>

#include "mimo_client.h"
#include "robot_voice_config.h"
#include "robot_voice_http.h"
#include "robot_voice_weather.h"
#include "robot_voice_clock.h"

/* Host fixtures may stub the legacy POST entry. The target build supplies a
 * strong deadline-aware implementation from robot_voice_http.c. */
__attribute__((weak)) int robot_voice_http_post_json_timeout(
  const char *host, const char *port, const char *path,
  const vela_header_t *headers, const char *body, char *response, size_t cap,
  unsigned int timeout_ms)
{
  (void)timeout_ms;
  return robot_voice_http_post_json(host, port, path, headers, body,
                                    response, cap);
}

int robot_mimo_post_timeout(const char *api_key, const char *body, size_t body_len,
                    char *response, size_t response_cap, size_t *response_len,
                    unsigned int timeout_ms)
{
  char auth[320];
  vela_header_t headers[3];
  int status;
  int auth_len;

  if (!api_key || !body || !response || response_cap < 2)
    {
      return -EINVAL;
    }
  auth_len = snprintf(auth, sizeof(auth), "Bearer %s", api_key);
  if (auth_len < 0 || (size_t)auth_len >= sizeof(auth))
    {
      memset(auth, 0, sizeof(auth));
      return -EOVERFLOW;
    }
  headers[0].name = "Authorization";
  headers[0].value = auth;
  headers[1].name = "Accept";
  headers[1].value = "text/event-stream, application/json";
  headers[2].name = NULL;
  headers[2].value = NULL;
  RV_VERBOSE("[RV-NET] POST %s body=%zu\n", ROBOT_VOICE_PATH, body_len);
  response[0] = '\0';
  status = robot_voice_http_post_json_timeout(ROBOT_VOICE_HOST, ROBOT_VOICE_PORT,
                                ROBOT_VOICE_PATH, headers, body,
                                response, response_cap, timeout_ms);
  memset(auth, 0, sizeof(auth));
  if (response_len != NULL)
    {
      *response_len = strnlen(response, response_cap);
    }
  RV_VERBOSE("[RV-NET] response status=%d bytes=%zu\n", status,
         response_len ? *response_len : 0);
  if (status < 0)
    {
      return status;
    }
  /* Let the monitor back off rather than hammering the account quota. */
  if (status == 429)
    {
      return -EAGAIN;
    }
  if (status == 401 || status == 403) return -EACCES;
  if (strnlen(response, response_cap) >= response_cap)
    {
      return -EOVERFLOW;
    }
  return status >= 200 && status < 300 ? 0 : -EIO;
}

int robot_mimo_post(const char *api_key, const char *body, size_t body_len,
                    char *response, size_t response_cap, size_t *response_len)
{
  return robot_mimo_post_timeout(api_key, body, body_len, response,
                                 response_cap, response_len, 0);
}

int robot_mimo_prepare_llm_body(const char *body, char **out)
{
  cJSON *root, *model, *thinking = NULL, *messages, *policy = NULL;
  cJSON *clock_policy = NULL;
  int clock_index = -1;
  int i;
  bool policy_present = false;
  int ret = -ENOMEM;
  const char *name;
  if (out == NULL || body == NULL) return -EINVAL;
  *out = NULL;
  /* Our voice bodies put model first. Avoid parsing megabytes of base64 for
   * ASR/TTS requests, which must pass through without any LLM options. */
  if (strncmp(body, "{\"model\":\"" ROBOT_VOICE_ASR_MODEL "\"",
              strlen("{\"model\":\"" ROBOT_VOICE_ASR_MODEL "\"")) == 0 ||
      strncmp(body, "{\"model\":\"" ROBOT_VOICE_TTS_MODEL "\"",
              strlen("{\"model\":\"" ROBOT_VOICE_TTS_MODEL "\"")) == 0)
    return 0;
  root = cJSON_Parse(body);
  if (root == NULL) return -EPROTO;
  model = cJSON_GetObjectItem(root, "model");
  if (!cJSON_IsString(model) || model->valuestring == NULL) goto skip;
  name = model->valuestring;
  /* Never apply chat options to ASR, TTS, unrelated models or services. */
  if (strncmp(name, "mimo-v2.", 8) != 0 || strstr(name, "asr") ||
      strstr(name, "tts")) goto skip;
  cJSON_DeleteItemFromObject(root, "stream");
  if (!cJSON_AddBoolToObject(root, "stream", true)) goto out;
  thinking = cJSON_CreateObject();
  if (thinking == NULL ||
      !cJSON_AddStringToObject(thinking, "type", "disabled")) goto out;
  cJSON_DeleteItemFromObject(root, "thinking");
  if (!cJSON_AddItemToObject(root, "thinking", thinking)) goto out;
  thinking = NULL;
  cJSON_DeleteItemFromObject(root, "max_tokens");
  cJSON_DeleteItemFromObject(root, "max_completion_tokens");
  if (!cJSON_AddNumberToObject(root, "max_completion_tokens",
                              ROBOT_VOICE_LLM_MAX_TOKENS)) goto out;
  messages = cJSON_GetObjectItem(root, "messages");
  if (cJSON_IsArray(messages))
    {
      for (i = 0; i < cJSON_GetArraySize(messages); i++)
        {
          cJSON *item = cJSON_GetArrayItem(messages, i);
          cJSON *content = cJSON_GetObjectItem(item, "content");
          cJSON *role = cJSON_GetObjectItem(item, "role");
          if (!cJSON_IsString(role) || !role->valuestring ||
              strcmp(role->valuestring, "system")) continue;
          if (cJSON_IsString(content) && content->valuestring &&
              strncmp(content->valuestring, "语音回复要求：", strlen("语音回复要求：")) == 0)
            policy_present = true;
          if (cJSON_IsString(content) && content->valuestring &&
              strncmp(content->valuestring, "设备时间状态：", strlen("设备时间状态：")) == 0)
            clock_index = i;
        }
    }
  if (cJSON_IsArray(messages) && !policy_present)
    {
      policy = cJSON_CreateObject();
      if (!policy || !cJSON_AddStringToObject(policy, "role", "system") ||
          !cJSON_AddStringToObject(policy, "content",
            "语音回复要求：默认用中文简短回答，先给结论，控制在一到两句话、60个汉字以内。"
            "使用适合口语播报的纯文本，不要Markdown列表、标题、星号或表格，不堆砌长名单，避免反问和复述。"
            "用户明确要求详细内容、多个问题或必要信息时完整回答，不受此长度建议限制。"
            "保留用户整句中的所有要求。日常闲聊和简单建议直接回答；"
            "只在任务确实需要时读取相应技能，不逐个读取所有技能。"
            "需要实时天气等信息时使用可用的查询工具；"
            "没有实时结果时如实说明，不编造。当前时间以已同步的设备时钟为准，"
            "没有可信时间来源时不能猜测或沿用对话中旧的时间。"
            "需要执行动作时遵守原有工具和安全规则。"))
        goto out;
      if (!cJSON_InsertItemInArray(messages, 0, policy)) goto out;
      policy = NULL;
      if (clock_index >= 0) clock_index++;
    }
  if (cJSON_IsArray(messages)) {
    char clock_text[320];
    robot_voice_clock_context(clock_text, sizeof(clock_text));
    clock_policy = cJSON_CreateObject();
    if (!clock_policy || !cJSON_AddStringToObject(clock_policy, "role", "system") ||
        !cJSON_AddStringToObject(clock_policy, "content", clock_text)) goto out;
    if (clock_index >= 0) {
      if (!cJSON_ReplaceItemInArray(messages, clock_index, clock_policy)) goto out;
    } else if (!cJSON_InsertItemInArray(messages, 0, clock_policy)) goto out;
    clock_policy = NULL;
  }
  *out = cJSON_PrintUnformatted(root);
  if (*out == NULL) goto out;
  RV_VERBOSE("[RV-LLM-POLICY] model=%s thinking=disabled max_completion_tokens=%u bytes=%zu\n",
         name, ROBOT_VOICE_LLM_MAX_TOKENS, strlen(*out));
  ret = 0;
  goto out;
skip:
  ret = 0;
out:
  cJSON_Delete(policy);
  cJSON_Delete(clock_policy);
  cJSON_Delete(thinking);
  cJSON_Delete(root);
  return ret;
}

int robot_mimo_post_stream_timeout(const char *api_key, const char *body,
                          size_t body_len, robot_mimo_chunk_cb_t cb, void *arg,
                          unsigned int timeout_ms)
{
  int status;
  if (!api_key || !body || !cb || strlen(body) != body_len) return -EINVAL;
  status = robot_mimo_agent_stream_post_timeout(api_key, body, cb, arg,
                                                timeout_ms);
  if (status < 0) return status;
  if (status == 429) return -EAGAIN;
  if (status == 401 || status == 403) return -EACCES;
  return status >= 200 && status < 300 ? 0 : -EIO;
}

int robot_mimo_post_stream(const char *api_key, const char *body, size_t body_len,
                          robot_mimo_chunk_cb_t cb, void *arg)
{
  return robot_mimo_post_stream_timeout(api_key, body, body_len, cb, arg, 0);
}


/* Strong application-owned symbols keep ordinary builds link-complete.
 * The actual official interface has no entity callback; use the fixed contest
 * mbedTLS/HTTP framing implementation instead of an inferred callback ABI. */
int robot_mimo_agent_policy_available(void)
{
#ifdef ROBOT_VOICE_AGENT_INTERPOSE
  return 1;
#else
  return 0;
#endif
}

int robot_mimo_agent_stream_available(void)
{
  return 1;
}

int robot_mimo_agent_stream_post(const char *api_key, const char *body,
                                robot_mimo_chunk_cb_t cb, void *arg)
{
  return robot_mimo_agent_stream_post_timeout(api_key, body, cb, arg, 0);
}

int robot_mimo_agent_stream_post_timeout(const char *api_key, const char *body,
                                robot_mimo_chunk_cb_t cb, void *arg,
                                unsigned int timeout_ms)
{
  char auth[320];
  vela_header_t headers[3];
  int status;
  int auth_len;
  if (!api_key || !body || !cb) return -EINVAL;
  auth_len = snprintf(auth, sizeof(auth), "Bearer %s", api_key);
  if (auth_len < 0 || (size_t)auth_len >= sizeof(auth))
    {
      memset(auth, 0, sizeof(auth));
      return -EOVERFLOW;
    }
  headers[0].name = "Authorization";
  headers[0].value = auth;
  headers[1].name = "Accept";
  headers[1].value = "text/event-stream, application/json";
  headers[2].name = NULL;
  headers[2].value = NULL;
  status = robot_voice_http_request(ROBOT_VOICE_HOST, ROBOT_VOICE_PORT,
      "POST", ROBOT_VOICE_PATH, headers, body, strlen(body), NULL, 0,
      cb, arg,
      strncmp(body, "{\"model\":\"" ROBOT_VOICE_ASR_MODEL "\"",
              strlen("{\"model\":\"" ROBOT_VOICE_ASR_MODEL "\"")) == 0 ?
        (timeout_ms ? timeout_ms : ROBOT_VOICE_HTTP_ASR_MS) :
        (timeout_ms ? timeout_ms : ROBOT_VOICE_HTTP_TTS_MS));
  memset(auth, 0, sizeof(auth));
  return status;
}

void robot_mimo_agent_adapter_anchor(void)
{
  extern void robot_voice_skill_guard_link_anchor(void);
  robot_voice_http_link_anchor();
  robot_voice_weather_link_anchor();
  robot_voice_skill_guard_link_anchor();
  RV_VERBOSE("[RV-ADAPTER] bundled=yes TTS=SSE Agent_policy=%s\n",
         robot_mimo_agent_policy_available() ? "disabled-thinking" : "unhooked");
  if (!robot_mimo_agent_policy_available())
    RV_VERBOSE("[RV-ADAPTER] use updated build_with_hal_backport.sh to enable the Agent HTTP hook\n");
}
