#ifndef ROBOT_VOICE_HTTP_PARSER_H
#define ROBOT_VOICE_HTTP_PARSER_H
#include <stdbool.h>
#include <stddef.h>

/* Entity callback: 0 continues; 1 means a complete application message (e.g.
 * SSE [DONE]) and closes this connection; a negative value aborts.
 * For ASR/LLM streams transport may call (NULL, 0, arg) to poll cancellation
 * before upload and while waiting for network data; no entity bytes arrive. */
typedef int (*rv_http_entity_cb)(const char *, size_t, void *);
enum rv_http_state {
  RV_HTTP_HEADER, RV_HTTP_LENGTH, RV_HTTP_CLOSE, RV_HTTP_CHUNK_SIZE,
  RV_HTTP_CHUNK_DATA, RV_HTTP_CHUNK_CRLF, RV_HTTP_TRAILER, RV_HTTP_DONE
};
struct rv_http_parser {
  enum rv_http_state state;
  rv_http_entity_cb callback;
  void *arg;
  size_t used, remaining, bytes, trailer_bytes;
  unsigned int interim, crlf;
  int status, error;
  bool keep_alive, stopped, extra_bytes;
  const char *framing;
  char header[8192];
  char line[512];
};
void rv_http_parser_init(struct rv_http_parser *, rv_http_entity_cb, void *);
/* Return 1 at the HTTP/application boundary, 0 if more bytes are needed. */
int rv_http_parser_feed(struct rv_http_parser *, const char *, size_t);
int rv_http_parser_eof(struct rv_http_parser *);
#endif
