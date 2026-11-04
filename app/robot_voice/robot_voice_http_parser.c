#include "robot_voice_http_parser.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RV_HTTP_ENTITY_MAX (32u * 1024u * 1024u)

static int fail(struct rv_http_parser *p, int error)
{
  p->error = error;
  p->keep_alive = false;
  return error;
}

void rv_http_parser_init(struct rv_http_parser *p, rv_http_entity_cb cb, void *arg)
{
  memset(p, 0, sizeof(*p));
  p->state = RV_HTTP_HEADER;
  p->callback = cb;
  p->arg = arg;
  p->framing = "headers";
}

static char *trim(char *s)
{
  char *end;
  while (*s == ' ' || *s == '\t') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
  return s;
}

static bool token(const char *value, const char *word)
{
  size_t n = strlen(word);
  while (*value) {
    const char *end;
    while (*value == ' ' || *value == '\t' || *value == ',') value++;
    end = strchr(value, ',');
    if (!end) end = value + strlen(value);
    const char *last = end;
    while (last > value && (last[-1] == ' ' || last[-1] == '\t')) last--;
    if ((size_t)(last - value) == n && !strncasecmp(value, word, n)) return true;
    value = end;
  }
  return false;
}

static int parse_headers(struct rv_http_parser *p)
{
  char *line = p->header, *end = strstr(line, "\r\n");
  bool chunked = false, have_length = false, have_te = false;
  size_t length = 0;
  int version = 0;
  if (!end) return fail(p, -EPROTO);
  *end = 0;
  if (sscanf(line, "HTTP/1.%d %d", &version, &p->status) != 2 ||
      (version != 0 && version != 1) || p->status < 100 || p->status > 599)
    return fail(p, -EPROTO);
  p->keep_alive = version == 1;
  line = end + 2;
  while (*line) {
    char *colon, *value;
    end = strstr(line, "\r\n");
    if (!end) return fail(p, -EPROTO);
    *end = 0;
    if (!*line) break;
    colon = strchr(line, ':');
    if (!colon || colon == line || *line == ' ' || *line == '\t')
      return fail(p, -EPROTO);
    *colon = 0;
    value = trim(colon + 1);
    if (!strcasecmp(line, "Content-Length")) {
      if (have_length || !*value) return fail(p, -EPROTO);
      for (char *v = value; *v; v++) {
        if (!isdigit((unsigned char)*v) || length > (RV_HTTP_ENTITY_MAX - (*v - '0')) / 10)
          return fail(p, -EOVERFLOW);
        length = length * 10 + (*v - '0');
      }
      have_length = true;
    } else if (!strcasecmp(line, "Transfer-Encoding")) {
      if (have_te || strcasecmp(value, "chunked")) return fail(p, -ENOTSUP);
      have_te = chunked = true;
    } else if (!strcasecmp(line, "Connection")) {
      if (token(value, "close")) p->keep_alive = false;
      else if (token(value, "keep-alive")) p->keep_alive = true;
    } else if (!strcasecmp(line, "Content-Encoding") && strcasecmp(value, "identity")) {
      return fail(p, -ENOTSUP);
    }
    line = end + 2;
  }
  if (have_length && chunked) return fail(p, -EPROTO);
  p->used = 0;
  if (p->status < 200) {
    if (p->status == 101 || ++p->interim > 8) return fail(p, -EPROTO);
    p->state = RV_HTTP_HEADER;
    return 0;
  }
  if (p->status == 204 || p->status == 304 || (have_length && length == 0)) {
    p->framing = "empty";
    p->state = RV_HTTP_DONE;
  } else if (chunked) {
    p->framing = "chunked";
    p->state = RV_HTTP_CHUNK_SIZE;
  } else if (have_length) {
    p->framing = "length";
    p->remaining = length;
    p->state = RV_HTTP_LENGTH;
  } else {
    p->framing = "close";
    p->keep_alive = false;
    p->state = RV_HTTP_CLOSE;
  }
  return p->state == RV_HTTP_DONE;
}

static int emit(struct rv_http_parser *p, const char *data, size_t len)
{
  int ret;
  if (len > RV_HTTP_ENTITY_MAX - p->bytes) return fail(p, -EOVERFLOW);
  p->bytes += len;
  ret = p->callback ? p->callback(data, len, p->arg) : 0;
  if (ret < 0) return fail(p, ret);
  if (ret > 0) {
    p->stopped = true;
    p->keep_alive = false;
    p->state = RV_HTTP_DONE;
    return 1;
  }
  return 0;
}

static int parse_chunk_line(struct rv_http_parser *p)
{
  const char *s = p->line;
  size_t n = 0;
  bool digit = false;
  while (*s && *s != ';') {
    int v;
    unsigned char c = (unsigned char)*s++;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return fail(p, -EPROTO);
    if (n > (RV_HTTP_ENTITY_MAX - (size_t)v) / 16) return fail(p, -EOVERFLOW);
    n = n * 16 + v;
    digit = true;
  }
  if (!digit) return fail(p, -EPROTO);
  for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s == 127)
    return fail(p, -EPROTO);
  p->remaining = n;
  p->state = n ? RV_HTTP_CHUNK_DATA : RV_HTTP_TRAILER;
  return 0;
}

int rv_http_parser_feed(struct rv_http_parser *p, const char *data, size_t len)
{
  if (p->error) return p->error;
  if (!data && len) return fail(p, -EINVAL);
  while (len) {
    int ret;
    if (p->state == RV_HTTP_DONE) {
      p->extra_bytes = true;
      p->keep_alive = false;
      return 1;
    }
    if (p->state == RV_HTTP_HEADER) {
      if (p->used + 1 >= sizeof(p->header)) return fail(p, -EOVERFLOW);
      if (!*data) return fail(p, -EPROTO);
      p->header[p->used++] = *data++;
      len--;
      p->header[p->used] = 0;
      if (p->used >= 4 && !memcmp(p->header + p->used - 4, "\r\n\r\n", 4)) {
        ret = parse_headers(p);
        if (ret < 0) return ret;
      }
    } else if (p->state == RV_HTTP_LENGTH || p->state == RV_HTTP_CHUNK_DATA ||
               p->state == RV_HTTP_CLOSE) {
      size_t n = len;
      enum rv_http_state state = p->state;
      if (state != RV_HTTP_CLOSE && n > p->remaining) n = p->remaining;
      ret = emit(p, data, n);
      if (ret < 0) return ret;
      data += n;
      len -= n;
      if (ret > 0) continue;
      if (state != RV_HTTP_CLOSE) {
        p->remaining -= n;
        if (p->remaining == 0) {
          p->state = state == RV_HTTP_LENGTH ? RV_HTTP_DONE : RV_HTTP_CHUNK_CRLF;
          p->crlf = 0;
        }
      }
    } else if (p->state == RV_HTTP_CHUNK_CRLF) {
      if (*data++ != (p->crlf == 0 ? '\r' : '\n')) return fail(p, -EPROTO);
      len--;
      if (++p->crlf == 2) { p->state = RV_HTTP_CHUNK_SIZE; p->used = 0; }
    } else {
      if (p->used + 1 >= sizeof(p->line)) return fail(p, -EOVERFLOW);
      if (!*data) return fail(p, -EPROTO);
      if (p->state == RV_HTTP_TRAILER && ++p->trailer_bytes > sizeof(p->header))
        return fail(p, -EOVERFLOW);
      p->line[p->used++] = *data++;
      len--;
      if (p->used >= 2 && p->line[p->used - 2] == '\r' && p->line[p->used - 1] == '\n') {
        p->line[p->used - 2] = 0;
        if (p->state == RV_HTTP_CHUNK_SIZE) {
          ret = parse_chunk_line(p);
          if (ret < 0) return ret;
        } else if (!p->line[0]) p->state = RV_HTTP_DONE;
        else if (!strchr(p->line, ':')) return fail(p, -EPROTO);
        p->used = 0;
      }
    }
  }
  return p->state == RV_HTTP_DONE;
}

int rv_http_parser_eof(struct rv_http_parser *p)
{
  if (p->error) return p->error;
  p->keep_alive = false;
  if (p->state == RV_HTTP_CLOSE) p->state = RV_HTTP_DONE;
  return p->state == RV_HTTP_DONE ? 1 : fail(p, -EPROTO);
}
