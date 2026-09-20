#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netutils/cJSON.h>
#include "mimo_client.h"
#include "mimo_llm.h"
#include "robot_voice_config.h"

int mimo_llm_chat(const char *api_key, const char *prompt, char *reply,
                  size_t reply_cap)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *messages = cJSON_CreateArray();
  cJSON *message = cJSON_CreateObject();
  char *body = NULL;
  char *response = NULL;
  size_t response_len = 0;
  cJSON *choices; cJSON *first; cJSON *msg; cJSON *answer;
  int ret;

  if (!api_key || !prompt || !reply || reply_cap == 0 || !root || !messages ||
      !message)
    { cJSON_Delete(root); cJSON_Delete(messages); cJSON_Delete(message); return -EINVAL; }
  cJSON_AddStringToObject(root, "model", ROBOT_VOICE_LLM_MODEL);
  cJSON_AddStringToObject(message, "role", "user");
  cJSON_AddStringToObject(message, "content", prompt);
  cJSON_AddItemToArray(messages, message); message = NULL;
  cJSON_AddItemToObject(root, "messages", messages); messages = NULL;
  body = cJSON_PrintUnformatted(root); cJSON_Delete(root); root = NULL;
  response = malloc(ROBOT_VOICE_HTTP_RESPONSE);
  if (!body || !response) { free(body); free(response); return -ENOMEM; }
  printf("[RV-LLM] request begin bytes=%zu\n", strlen(body));
  ret = robot_mimo_post(api_key, body, strlen(body), response,
                        ROBOT_VOICE_HTTP_RESPONSE, &response_len);
  free(body);
  if (ret < 0) { free(response); return ret; }
  root = cJSON_Parse(response); free(response);
  if (!root) return -EPROTO;
  choices = cJSON_GetObjectItem(root, "choices"); first = cJSON_GetArrayItem(choices, 0);
  msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
  answer = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
  if (!cJSON_IsString(answer) || !answer->valuestring) { cJSON_Delete(root); return -EPROTO; }
  strncpy(reply, answer->valuestring, reply_cap - 1); reply[reply_cap - 1] = '\0';
  printf("[RV-LLM] response bytes=%zu reply=%zu\n", response_len, strlen(reply));
  cJSON_Delete(root); return 0;
}
