#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <infra/vela_tls.h>

#include "mimo_client.h"
#include "robot_voice_config.h"

int robot_mimo_post(const char *api_key, const char *body, size_t body_len,
                    char *response, size_t response_cap, size_t *response_len)
{
  char auth[320];
  vela_header_t headers[2];
  int status;

  if (!api_key || !body || !response || response_cap < 2)
    {
      return -EINVAL;
    }
  snprintf(auth, sizeof(auth), "Bearer %s", api_key);
  headers[0].name = "Authorization";
  headers[0].value = auth;
  headers[1].name = NULL;
  headers[1].value = NULL;
  printf("[RV-NET] POST %s body=%zu\n", ROBOT_VOICE_PATH, body_len);
  response[0] = '\0';
  status = vela_https_post_json(ROBOT_VOICE_HOST, ROBOT_VOICE_PORT,
                                ROBOT_VOICE_PATH, headers, body,
                                response, response_cap);
  memset(auth, 0, sizeof(auth));
  if (response_len != NULL)
    {
      *response_len = strnlen(response, response_cap);
    }
  printf("[RV-NET] response status=%d bytes=%zu\n", status,
         response_len ? *response_len : 0);
  if (status < 0)
    {
      return status;
    }
  return status >= 200 && status < 300 ? 0 : -EIO;
}
