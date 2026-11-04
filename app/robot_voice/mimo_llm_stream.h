#ifndef ROBOT_MIMO_LLM_STREAM_H
#define ROBOT_MIMO_LLM_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <infra/vela_tls.h>
#include <netutils/cJSON.h>

/* HTTP supplies entity bytes. No TLS record/SSE event boundary is assumed. */
struct rv_llm_sse_s
{
  char *event;
  size_t used, capacity, string_bytes, limit, text_bytes;
  size_t last_event_bytes;
  bool done, finished, tools;
  int error;
  unsigned long long first_delta_ms;
  cJSON *root, *choice, *message;
  void (*delta)(void *, const char *, size_t, bool);
  void *arg;
  unsigned long request_token;
};
int rv_llm_sse_init(struct rv_llm_sse_s *, size_t,
                    void (*)(void *, const char *, size_t, bool), void *);
int rv_llm_sse_feed(const char *, size_t, void *);
int rv_llm_sse_validate(struct rv_llm_sse_s *);
int rv_llm_sse_finish(struct rv_llm_sse_s *, char *, size_t);
void rv_llm_sse_destroy(struct rv_llm_sse_s *);
int mimo_llm_stream_request(const char *, const char *, const char *,
                           const vela_header_t *, const char *, char *, size_t);
int mimo_llm_stream_request_timeout(const char *, const char *, const char *,
                           const vela_header_t *, const char *, char *, size_t,
                           unsigned int timeout_ms);

unsigned long robot_voice_llm_request_token(const char *);
bool robot_voice_llm_request_current(unsigned long);
void robot_voice_llm_reply_started(unsigned long);

/* Optional voice owner. NULL means collect a complete Agent response only. */
void *robot_voice_llm_stream_begin(const char *);
void robot_voice_llm_stream_delta(void *, const char *, size_t, bool);
void robot_voice_llm_stream_end(void *, int);
void robot_voice_llm_stream_hold(void *);
void robot_voice_llm_stream_retry_reset(void *);
void robot_voice_llm_stream_commit(void *);
bool robot_voice_llm_stream_cancelled(void *);
#endif
