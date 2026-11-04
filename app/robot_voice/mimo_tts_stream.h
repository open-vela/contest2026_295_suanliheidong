#ifndef ROBOT_MIMO_TTS_STREAM_H
#define ROBOT_MIMO_TTS_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mimo_tts.h"

/* Internal, portable SSE parser. HTTP delivers decoded entity bytes, never
 * chunked-transfer framing. The PCM sink must consume borrowed data inline. */
struct rv_tts_sse_s
{
  char *event;
  size_t used;
  size_t capacity;
  size_t pcm_bytes;
  unsigned int events, audio_events, repeated_audio_events;
  uint32_t pcm_hash, previous_audio_hash;
  size_t previous_audio_bytes;
  unsigned long long first_audio_ms, last_audio_ms, max_audio_gap_ms;
  unsigned long long sink_ms, max_sink_ms;
  unsigned char carry;
  bool has_carry;
  bool done;
  int error;
  mimo_tts_pcm_cb_t sink;
  void *arg;
};
void rv_tts_sse_init(struct rv_tts_sse_s *s, mimo_tts_pcm_cb_t sink, void *arg);
int rv_tts_sse_feed(const char *data, size_t len, void *arg);
int rv_tts_sse_finish(struct rv_tts_sse_s *s);
void rv_tts_sse_destroy(struct rv_tts_sse_s *s);
int mimo_tts_stream_run(const char *key, const char *text,
                       mimo_tts_pcm_cb_t cb, void *arg);
int mimo_tts_stream_run_timeout(const char *key, const char *text,
                       mimo_tts_pcm_cb_t cb, void *arg,
                       unsigned int timeout_ms);
#endif
