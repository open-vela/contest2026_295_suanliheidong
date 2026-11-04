#ifndef ROBOT_VOICE_HTTP_H
#define ROBOT_VOICE_HTTP_H
#include <stddef.h>
#include <stdbool.h>
#include <infra/vela_tls.h>
#include "robot_voice_http_parser.h"

/* Independent contest HTTP entity reader over mbedTLS. Official source and
 * Agent/Skill/Tool dispatch remain unchanged. timeout_ms is a total budget
 * including connect, handshake, upload and response. POSIX DNS resolution
 * cannot be preempted here and may exceed this budget on a broken resolver. */
int robot_voice_http_request(const char *host, const char *port,
  const char *method, const char *path, const vela_header_t *headers,
  const char *body, size_t body_len, char *response, size_t capacity,
  rv_http_entity_cb callback, void *arg, unsigned int timeout_ms);
int robot_voice_http_post_json(const char *, const char *, const char *,
  const vela_header_t *, const char *, char *, size_t);
int robot_voice_http_post_json_timeout(const char *, const char *, const char *,
  const vela_header_t *, const char *, char *, size_t, unsigned int);
void robot_voice_http_link_anchor(void);
/* Refcounted conversation resource lease, separate from socket ownership. */
int robot_voice_http_session_begin(void);
void robot_voice_http_session_end(void);
/* Close idle fds only from the task group that owns them. */
void robot_voice_http_close_idle(void);
/* Recovery owns the network only while no HTTP request is active. New
 * requests wait inside their normal deadline until recovery_end(). */
bool robot_voice_http_network_recovery_begin(void);
void robot_voice_http_network_recovery_end(void);
#endif
