#ifndef __APPS_ROBOT_VOICE_MIMO_CLIENT_H
#define __APPS_ROBOT_VOICE_MIMO_CLIENT_H
#include <stddef.h>

typedef struct
{
  const char *name;
  const char *value;
} robot_mimo_header_t;

typedef int (*robot_mimo_chunk_cb_t)(const char *data, size_t len, void *arg);
int robot_mimo_post(const char *api_key, const char *body, size_t body_len,
                    char *response, size_t response_cap, size_t *response_len);
int robot_mimo_post_timeout(const char *api_key, const char *body, size_t body_len,
                    char *response, size_t response_cap, size_t *response_len,
                    unsigned int timeout_ms);
int robot_mimo_post_stream(const char *api_key, const char *body, size_t body_len,
                          robot_mimo_chunk_cb_t cb, void *arg);
int robot_mimo_post_stream_timeout(const char *api_key, const char *body,
                          size_t body_len, robot_mimo_chunk_cb_t cb,
                          void *arg, unsigned int timeout_ms);

/* Called by the contest HTTP adapter. The caller owns *out.
 * Non-MiMo/chat requests are passed through with *out == NULL. */
int robot_mimo_prepare_llm_body(const char *body, char **out);
void robot_mimo_agent_adapter_anchor(void);
int robot_mimo_agent_stream_available(void);
int robot_mimo_agent_policy_available(void);
int robot_mimo_agent_stream_post(const char *api_key, const char *body,
                                robot_mimo_chunk_cb_t cb, void *arg);
int robot_mimo_agent_stream_post_timeout(const char *api_key, const char *body,
                                robot_mimo_chunk_cb_t cb, void *arg,
                                unsigned int timeout_ms);
#endif
