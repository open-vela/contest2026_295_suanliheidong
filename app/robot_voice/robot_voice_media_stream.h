#ifndef ROBOT_VOICE_MEDIA_STREAM_H
#define ROBOT_VOICE_MEDIA_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Contest-local adapter for Media's raw PCM buffer mode.  The official
 * packages/ai_agent sources remain untouched. */
struct robot_voice_media_stream_s
{
  void *handle;
  bool prepared;
  bool started;
  bool failed;
  size_t bytes_written;
  unsigned int write_calls;
  unsigned int retry_count;
  unsigned long long open_ms;
  unsigned long long start_ms;
  unsigned long long first_write_ms;
  unsigned long long last_write_ms;
};

int robot_voice_media_stream_open(struct robot_voice_media_stream_s *stream);
int robot_voice_media_stream_start(struct robot_voice_media_stream_s *stream);
int robot_voice_media_stream_write(struct robot_voice_media_stream_s *stream,
                                   const uint8_t *pcm, size_t len);
int robot_voice_media_stream_finish(struct robot_voice_media_stream_s *stream);
void robot_voice_media_stream_abort(struct robot_voice_media_stream_s *stream);
void robot_voice_media_stream_close(struct robot_voice_media_stream_s *stream);

#ifdef __cplusplus
}
#endif

#endif /* ROBOT_VOICE_MEDIA_STREAM_H */
