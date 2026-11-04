#include <nuttx/config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mimo_llm_stream.h"
#include "robot_voice_config.h"
#include "robot_voice_diag.h"
#include "robot_voice_http.h"
#include "robot_voice_retry.h"
#include "robot_voice_wifi.h"

__attribute__((weak)) void *robot_voice_llm_stream_begin(const char *body)
{ (void)body; return NULL; }
__attribute__((weak)) void robot_voice_llm_stream_delta(void *arg,
  const char *text, size_t len, bool tools)
{ (void)arg; (void)text; (void)len; (void)tools; }
__attribute__((weak)) void robot_voice_llm_stream_end(void *arg, int status)
{ (void)arg; (void)status; }
__attribute__((weak)) void robot_voice_llm_stream_hold(void *arg)
{ (void)arg; }
__attribute__((weak)) void robot_voice_llm_stream_retry_reset(void *arg)
{ (void)arg; }
__attribute__((weak)) void robot_voice_llm_stream_commit(void *arg)
{ (void)arg; }
__attribute__((weak)) bool robot_voice_llm_stream_cancelled(void *arg)
{ (void)arg; return false; }

__attribute__((weak)) unsigned long robot_voice_llm_request_token(const char *body)
{ (void)body; return 0; }
__attribute__((weak)) bool robot_voice_llm_request_current(unsigned long token)
{ (void)token; return true; }
__attribute__((weak)) void robot_voice_llm_reply_started(unsigned long token)
{ (void)token; }

static unsigned long long llm_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int append(struct rv_llm_sse_s *s, cJSON *object,
                  const char *key, const cJSON *part)
{
  cJSON *old = cJSON_GetObjectItem(object, key);
  size_t before = cJSON_IsString(old) ? strlen(old->valuestring) : 0;
  size_t n;
  char *joined;
  if (!part) return 0;
  if (!cJSON_IsString(part) || !part->valuestring) return -EPROTO;
  n = strlen(part->valuestring);
  if (n > s->limit - s->string_bytes) return -EOVERFLOW;
  joined = malloc(before + n + 1);
  if (!joined) return -ENOMEM;
  if (before) memcpy(joined, old->valuestring, before);
  memcpy(joined + before, part->valuestring, n + 1);
  cJSON_DeleteItemFromObject(object, key);
  old = cJSON_AddStringToObject(object, key, joined);
  free(joined);
  if (!old) return -ENOMEM;
  s->string_bytes += n;
  return 0;
}

static int copy_field(cJSON *to, const cJSON *from, const char *name)
{
  cJSON *item = cJSON_GetObjectItem(from, name), *copy;
  if (!item) return 0;
  copy = cJSON_Duplicate(item, true);
  if (!copy) return -ENOMEM;
  cJSON_DeleteItemFromObject(to, name);
  if (!cJSON_AddItemToObject(to, name, copy))
    { cJSON_Delete(copy); return -ENOMEM; }
  return 0;
}

static bool finish_reason_is_tool(const cJSON *item)
{
  if (!cJSON_IsString(item) || !item->valuestring)
    return false;

  /* OpenAI uses tool_calls. Some compatible gateways use the singular
   * spelling or retain the older function_call spelling. */
  return !strcmp(item->valuestring, "tool_calls") ||
         !strcmp(item->valuestring, "tool_call") ||
         !strcmp(item->valuestring, "function_call");
}

static bool finish_reason_known(const cJSON *item)
{
  if (!cJSON_IsString(item) || !item->valuestring)
    return false;

  return !strcmp(item->valuestring, "stop") ||
         !strcmp(item->valuestring, "length") ||
         finish_reason_is_tool(item) ||
         !strcmp(item->valuestring, "end_turn") ||
         !strcmp(item->valuestring, "content_filter") ||
         !strcmp(item->valuestring, "stop_sequence") ||
         !strcmp(item->valuestring, "eos") ||
         !strcmp(item->valuestring, "max_tokens");
}

static int tools_delta(struct rv_llm_sse_s *s, cJSON *parts)
{
  cJSON *tools = cJSON_GetObjectItem(s->message, "tool_calls");
  if (!cJSON_IsArray(parts)) return -EPROTO;
  if (!tools)
    {
      tools = cJSON_CreateArray();
      if (!tools) return -ENOMEM;
      if (!cJSON_AddItemToObject(s->message, "tool_calls", tools))
        { cJSON_Delete(tools); return -ENOMEM; }
    }
  for (int i = 0; i < cJSON_GetArraySize(parts); i++)
    {
      cJSON *p = cJSON_GetArrayItem(parts, i);
      cJSON *index = cJSON_GetObjectItem(p, "index");
      cJSON *call, *function, *f = cJSON_GetObjectItem(p, "function");
      int index_value = i;
      int ret;
      /* A few OpenAI-compatible gateways omit index when an event contains
       * one tool delta. Keep the event usable while retaining strict bounds
       * for an explicitly supplied index. */
      if (index && (!cJSON_IsNumber(index) ||
                    index->valuedouble != index->valueint ||
                    index->valueint < 0 || index->valueint >= 16))
        return -EPROTO;
      if (index) index_value = index->valueint;
      if (index_value >= 16) return -EPROTO;
      while (cJSON_GetArraySize(tools) <= index_value)
        {
          call = cJSON_CreateObject();
          if (!call) return -ENOMEM;
          if (!cJSON_AddItemToArray(tools, call))
            { cJSON_Delete(call); return -ENOMEM; }
        }
      call = cJSON_GetArrayItem(tools, index_value);
      ret = append(s, call, "id", cJSON_GetObjectItem(p, "id"));
      if (ret < 0) return ret;
      ret = append(s, call, "type", cJSON_GetObjectItem(p, "type"));
      if (ret < 0) return ret;
      if (!f) continue;
      if (!cJSON_IsObject(f)) return -EPROTO;
      function = cJSON_GetObjectItem(call, "function");
      if (!function)
        {
          function = cJSON_CreateObject();
          if (!function) return -ENOMEM;
          if (!cJSON_AddItemToObject(call, "function", function))
            { cJSON_Delete(function); return -ENOMEM; }
        }
      ret = append(s, function, "name", cJSON_GetObjectItem(f, "name"));
      if (ret < 0) return ret;
      ret = append(s, function, "arguments", cJSON_GetObjectItem(f, "arguments"));
      if (ret < 0) return ret;
    }
  return 0;
}

static int event(struct rv_llm_sse_s *s)
{
  char *line = s->event, *out = s->event;
  cJSON *root = NULL, *choices, *first, *delta, *text, *calls, *finish;
  cJSON *legacy_call;
  bool found = false;
  int ret = 0;
  while (*line)
    {
      char *end = strchr(line, '\n');
      size_t n = end ? (size_t)(end - line) : strlen(line);
      if (n && line[n - 1] == '\r') n--;
      if (n >= 5 && !memcmp(line, "data:", 5))
        {
          size_t skip = n > 5 && line[5] == ' ' ? 6 : 5;
          if (found) *out++ = '\n';
          memmove(out, line + skip, n - skip);
          out += n - skip;
          found = true;
        }
      if (!end) break;
      line = end + 1;
    }
  *out = 0;
  if (!found || out == s->event) return 0;
  if (s->done) return -EPROTO;
  if (!strcmp(s->event, "[DONE]"))
    {
      if (!s->finished) return -EPROTO;
      s->done = true;
      return 0;
    }
  root = cJSON_Parse(s->event);
  if (!cJSON_IsObject(root)) { ret = -EPROTO; goto out; }
  if (cJSON_GetObjectItem(root, "error")) { ret = -EIO; goto out; }
  if ((ret = copy_field(s->root, root, "id")) < 0 ||
      (ret = copy_field(s->root, root, "model")) < 0 ||
      (ret = copy_field(s->root, root, "created")) < 0 ||
      (ret = copy_field(s->root, root, "usage")) < 0) goto out;
  choices = cJSON_GetObjectItem(root, "choices");
  if (!choices)
    {
      /* A few gateways omit choices on the trailing usage event. It is
       * metadata after a valid terminal choice and does not carry text. */
      if (s->finished && cJSON_IsObject(cJSON_GetObjectItem(root, "usage")))
        goto out;
      ret = -EPROTO;
      goto out;
    }
  if (!cJSON_IsArray(choices)) { ret = -EPROTO; goto out; }
  if (cJSON_GetArraySize(choices) == 0)
    {
      /* Usage/metadata-only events are valid both before [DONE] and after
       * the terminal finish chunk. Keep waiting for [DONE]. */
      if (s->finished || cJSON_IsObject(cJSON_GetObjectItem(root, "usage")))
        goto out;
      ret = -EPROTO;
      goto out;
    }
  for (int i = 0; i < cJSON_GetArraySize(choices); i++)
    {
      first = cJSON_GetArrayItem(choices, i);
      if (!cJSON_IsObject(first)) { ret = -EPROTO; goto out; }
      cJSON *idx = cJSON_GetObjectItem(first, "index");
      if (idx && (!cJSON_IsNumber(idx) || idx->valuedouble != 0))
        { ret = -EPROTO; goto out; }
      delta = cJSON_GetObjectItem(first, "delta");
      if (!delta)
        {
          /* Some OpenAI-compatible gateways return a complete message in a
           * streamed choice. Treat it as the delta for this event. */
          delta = cJSON_GetObjectItem(first, "message");
        }
      finish = cJSON_GetObjectItem(first, "finish_reason");
      /* Some compatible OpenAI endpoints use delta=null on the terminal
       * chunk. It carries no content; the finish_reason still completes the
       * choice and must be accepted. */
      if (!delta || cJSON_IsNull(delta))
        {
          if (!finish || cJSON_IsNull(finish))
            {
              /* A gateway may send an empty choice after the terminal
               * choice and before the usage/[DONE] trailer. */
              if (s->finished) continue;
              ret = -EPROTO;
              goto out;
            }
          delta = NULL;
        }
      else if (!cJSON_IsObject(delta))
        { ret = -EPROTO; goto out; }
      calls = delta ? cJSON_GetObjectItem(delta, "tool_calls") : NULL;
      legacy_call = delta ? cJSON_GetObjectItem(delta, "function_call") : NULL;
      if (s->finished)
        {
          cJSON *trailing_content = delta ?
            cJSON_GetObjectItem(delta, "content") : NULL;

          /* Once a terminal choice was accepted, only empty metadata
           * choices are allowed. Do not silently append a second answer. */
          if ((trailing_content && !cJSON_IsNull(trailing_content)) ||
              (calls && !cJSON_IsNull(calls) &&
               cJSON_GetArraySize(calls) > 0) ||
              (legacy_call && !cJSON_IsNull(legacy_call)))
            {
              ret = -EPROTO;
              goto out;
            }
          continue;
        }
      /* Stop queued speech before accepting content in the same event. */
      if (calls && !cJSON_IsNull(calls) && !cJSON_IsArray(calls))
        { ret = -EPROTO; goto out; }
      if (calls && cJSON_GetArraySize(calls) > 0)
        {
          if (!s->first_delta_ms) s->first_delta_ms = llm_now_ms();
          s->tools = true;
          if (s->delta) s->delta(s->arg, NULL, 0, true);
          ret = tools_delta(s, calls);
          if (ret < 0) goto out;
        }
      else if (legacy_call && !cJSON_IsNull(legacy_call))
        {
          cJSON *compat = NULL;
          cJSON *call = NULL;
          cJSON *copy = NULL;

          if (!cJSON_IsObject(legacy_call))
            { ret = -EPROTO; goto out; }
          compat = cJSON_CreateArray();
          call = cJSON_CreateObject();
          copy = cJSON_Duplicate(legacy_call, true);
          if (!compat || !call || !copy ||
              !cJSON_AddNumberToObject(call, "index", 0) ||
              !cJSON_AddStringToObject(call, "id", "legacy-0") ||
              !cJSON_AddStringToObject(call, "type", "function") ||
              !cJSON_AddItemToObject(call, "function", copy))
            {
              cJSON_Delete(copy);
              cJSON_Delete(call);
              cJSON_Delete(compat);
              ret = -ENOMEM;
              goto out;
            }
          copy = NULL;
          if (!cJSON_AddItemToArray(compat, call))
            {
              cJSON_Delete(call);
              cJSON_Delete(compat);
              ret = -ENOMEM;
              goto out;
            }
          if (!s->first_delta_ms) s->first_delta_ms = llm_now_ms();
          s->tools = true;
          if (s->delta) s->delta(s->arg, NULL, 0, true);
          ret = tools_delta(s, compat);
          cJSON_Delete(compat);
          if (ret < 0) goto out;
        }
      text = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
      if (text && !cJSON_IsNull(text))
        {
          if (!cJSON_IsString(text)) { ret = -EPROTO; goto out; }
          size_t n = strlen(text->valuestring);
          if (n >= ROBOT_VOICE_TEXT_CAP - s->text_bytes)
            { ret = -EOVERFLOW; goto out; }
          ret = append(s, s->message, "content", text);
          if (ret < 0) goto out;
          if (n && !s->first_delta_ms) s->first_delta_ms = llm_now_ms();
          s->text_bytes += n;
          if (n && s->delta) s->delta(s->arg, text->valuestring, n, s->tools);
        }
      text = delta ? cJSON_GetObjectItem(delta, "reasoning_content") : NULL;
      if (text && !cJSON_IsNull(text))
        { ret = append(s, s->message, "reasoning_content", text); if (ret < 0) goto out; }
      if (finish && !cJSON_IsNull(finish))
        {
          if (!finish_reason_known(finish))
            { ret = -EPROTO; goto out; }
          ret = copy_field(s->choice, first, "finish_reason");
          if (ret < 0) goto out;
          s->finished = true;
        }
    }
out:
  cJSON_Delete(root);
  return ret;
}

int rv_llm_sse_init(struct rv_llm_sse_s *s, size_t limit,
                    void (*cb)(void *, const char *, size_t, bool), void *arg)
{
  cJSON *choices;
  memset(s, 0, sizeof(*s));
  s->limit = limit;
  s->delta = cb; s->arg = arg;
  s->root = cJSON_Parse("{\"object\":\"chat.completion\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"\"}}]}");
  if (!s->root) return -ENOMEM;
  choices = cJSON_GetObjectItem(s->root, "choices");
  s->choice = cJSON_GetArrayItem(choices, 0);
  s->message = cJSON_GetObjectItem(s->choice, "message");
  return 0;
}

int rv_llm_sse_feed(const char *data, size_t len, void *arg)
{
  struct rv_llm_sse_s *s = arg;
  if (!s || (!data && len)) return -EINVAL;
  if (s->error) return s->error;
  for (size_t i = 0; i < len; i++)
    {
      if (!data[i]) return s->error = -EPROTO;
      if (s->used + 2 > s->capacity)
        {
          size_t cap = s->capacity ? s->capacity * 2 : 1024;
          char *p;
          if (cap > ROBOT_VOICE_LLM_SSE_MAX + 1) cap = ROBOT_VOICE_LLM_SSE_MAX + 1;
          if (cap <= s->used + 1) return s->error = -EOVERFLOW;
          p = realloc(s->event, cap);
          if (!p) return s->error = -ENOMEM;
          s->event = p; s->capacity = cap;
        }
      s->event[s->used++] = data[i]; s->event[s->used] = 0;
      if ((s->used >= 2 && !memcmp(s->event + s->used - 2, "\n\n", 2)) ||
          (s->used >= 3 && !memcmp(s->event + s->used - 3, "\n\r\n", 3)) ||
          (s->used >= 4 && !memcmp(s->event + s->used - 4, "\r\n\r\n", 4)))
        {
          int ret;
          s->last_event_bytes = s->used;
          ret = event(s);
          s->used = 0;
          if (ret < 0) return s->error = ret;
        }
    }
  return 0;
}

int rv_llm_sse_validate(struct rv_llm_sse_s *s)
{
  cJSON *tools = cJSON_GetObjectItem(s->message, "tool_calls");
  if (s->error) return s->error;
  if (!s->done || !s->finished || s->used) return -EPROTO;
  if (!s->text_bytes && !s->tools) return -ENODATA;
  cJSON *reason = cJSON_GetObjectItem(s->choice, "finish_reason");
  if (s->tools != finish_reason_is_tool(reason)) return -EPROTO;
  if (s->tools)
    {
      if (!cJSON_GetArraySize(tools)) return -EPROTO;
      for (int i = 0; i < cJSON_GetArraySize(tools); i++)
        {
          cJSON *t = cJSON_GetArrayItem(tools, i);
          cJSON *id = cJSON_GetObjectItem(t, "id");
          cJSON *type = cJSON_GetObjectItem(t, "type");
          cJSON *f = cJSON_GetObjectItem(t, "function");
          cJSON *name = cJSON_GetObjectItem(f, "name");
          cJSON *args = cJSON_GetObjectItem(f, "arguments");
          if (!cJSON_IsString(id) || !id->valuestring[0] ||
              !cJSON_IsString(type) || strcmp(type->valuestring, "function") ||
              !cJSON_IsString(name) || !name->valuestring[0] ||
              !cJSON_IsString(args)) return -EPROTO;
        }
    }
  return 0;
}

int rv_llm_sse_finish(struct rv_llm_sse_s *s, char *out, size_t cap)
{
  char *json;
  int ret;
  if (!out || !cap) return -EINVAL;
  out[0] = 0;
  ret = rv_llm_sse_validate(s);
  if (ret < 0) return ret;
  json = cJSON_PrintUnformatted(s->root);
  if (!json) return -ENOMEM;
  if (strlen(json) >= cap) { free(json); return -EOVERFLOW; }
  memcpy(out, json, strlen(json) + 1); free(json);
  return 0;
}

void rv_llm_sse_destroy(struct rv_llm_sse_s *s)
{ free(s->event); cJSON_Delete(s->root); memset(s, 0, sizeof(*s)); }

static int http_feed(const char *p, size_t n, void *arg)
{
  struct rv_llm_sse_s *s = arg;
  if (!robot_voice_llm_request_current(s->request_token)) return -ECANCELED;
  int ret = rv_llm_sse_feed(p, n, s);
  if (ret < 0)
    RV_EVENT("[RV-LLM-PARSE] SSE response rejected rc=%d event_bytes=%zu\n",
             ret, s->last_event_bytes);
  return ret < 0 ? ret : (s->done ? 1 : 0);
}

struct rv_llm_retry_s
{
  const char *host, *port, *path, *body;
  const vela_header_t *headers;
  char *response;
  size_t cap;
  void *voice;
  unsigned long token;
  size_t text_bytes;
  bool tools;
  bool parse_failed;
  unsigned long long first_delta_ms;
  int http_status;
};

static void rv_llm_voice_delta(void *arg, const char *text, size_t n, bool tools)
{
  struct rv_llm_retry_s *ctx = arg;
  if (!robot_voice_llm_request_current(ctx->token)) return;
  if (tools || (text && n)) robot_voice_llm_reply_started(ctx->token);
  robot_voice_llm_stream_delta(ctx->voice, text, n, tools);
}

static int rv_llm_retry_attempt(void *arg, unsigned int timeout_ms)
{
  struct rv_llm_retry_s *ctx = arg;
  struct rv_llm_sse_s s;
  int ret;
  ctx->response[0] = 0;
  ctx->parse_failed = false;
  robot_voice_llm_stream_retry_reset(ctx->voice);
  ret = rv_llm_sse_init(&s, ctx->cap - 1,
                        rv_llm_voice_delta, ctx);
  if (ret < 0) return ret;
  s.request_token = ctx->token;
  ret = robot_voice_http_request(ctx->host, ctx->port, "POST", ctx->path,
      ctx->headers, ctx->body, strlen(ctx->body), NULL, 0, http_feed, &s,
      timeout_ms);
  ctx->parse_failed = s.error < 0;
  ctx->http_status = ret;
  /* Preserve the amount of data already handed to TTS/tool handling even
   * when the terminal SSE event is malformed.  Retrying such a partial
   * response could duplicate side effects. */
  ctx->text_bytes = s.text_bytes;
  ctx->tools = s.tools;
  if (ret >= 200 && ret < 300)
    {
      ret = rv_llm_sse_finish(&s, ctx->response, ctx->cap);
      if (ret == 0)
        {
          ctx->text_bytes = s.text_bytes;
          ctx->tools = s.tools;
          if (!ctx->first_delta_ms) ctx->first_delta_ms = s.first_delta_ms;
          robot_voice_llm_stream_commit(ctx->voice);
        }
    }
  else if (ret == 401 || ret == 403)
    ret = -EACCES;
  else if (ret == 429)
    ret = -EAGAIN;
  else if (ret >= 500)
    ret = -EIO;
  else if (ret >= 300)
    ret = -EPROTO;
  /* Feed normal Flash deltas immediately. Freeze only on a failed attempt
   * while a retry builds a replacement for its unsubmitted suffix. */
  if (!ctx->first_delta_ms) ctx->first_delta_ms = s.first_delta_ms;
  if (ret < 0) robot_voice_llm_stream_hold(ctx->voice);
  rv_llm_sse_destroy(&s);
  return ret;
}

static bool rv_llm_retry_cancelled(void *arg)
{
  struct rv_llm_retry_s *ctx = arg;
  return !robot_voice_llm_request_current(ctx->token);
}

int mimo_llm_stream_request(const char *host, const char *port, const char *path,
  const vela_header_t *headers, const char *body, char *response, size_t cap)
{
  return mimo_llm_stream_request_timeout(host, port, path, headers, body,
      response, cap, ROBOT_VOICE_HTTP_LLM_MS);
}

int mimo_llm_stream_request_timeout(const char *host, const char *port,
  const char *path, const vela_header_t *headers, const char *body,
  char *response, size_t cap, unsigned int timeout_ms)
{
  struct rv_llm_retry_s ctx = {
    .host = host, .port = port, .path = path, .body = body,
    .headers = headers, .response = response, .cap = cap,
  };
  int ret;
  unsigned int retries = 0;
  unsigned long long started = llm_now_ms();
  if (!host || !port || !path || !body || !response || cap < 2 || !timeout_ms)
    return -EINVAL;
  response[0] = 0;
  ctx.token = robot_voice_llm_request_token(body);
  ctx.voice = robot_voice_llm_stream_begin(body);
  RV_EVENT("[Flash] 开始流式请求\n");
  RV_VERBOSE("[RV-LLM] stream enabled model=%s\n", ROBOT_VOICE_LLM_MODEL);
  ret = rv_llm_retry_attempt(&ctx, timeout_ms);
  if (ret < 0 && !ctx.parse_failed)
    {
      int last = ret;
      ret = robot_voice_retry_after_failure(rv_llm_retry_attempt, &ctx,
        ret, rv_llm_retry_cancelled, &ctx, &retries);
      if (retries)
        RV_EVENT("[RV-RETRY] Flash retry=%u/%u rc=%d\n", retries,
                 ROBOT_VOICE_RETRY_MAX, ret ? ret : last);
    }
  else if (ret < 0 && ctx.parse_failed)
    {
      int net_ret;
      int last = ret;

      /* SSE tool deltas are only accumulated in the parser.  The Agent does
       * not execute them until this request returns a validated response, so
       * replaying an incomplete response cannot duplicate a tool side effect.
       * Text deltas keep the existing hold/commit path in the voice owner. */
      net_ret = robot_voice_wifi_ensure_connected(0);
      if (net_ret < 0)
        {
          RV_EVENT("[RV-RETRY] SSE parse error network recovery failed "
                   "rc=%d parse_rc=%d text_bytes=%zu tools=%u\n", net_ret,
                   ret, ctx.text_bytes, ctx.tools);
          ret = net_ret;
        }
      else
        {
          ret = robot_voice_retry_after_failure(rv_llm_retry_attempt,
            &ctx, ret, rv_llm_retry_cancelled, &ctx, &retries);
          RV_EVENT("[RV-RETRY] Flash retry after SSE parse "
                   "retries=%u/%u rc=%d text_bytes=%zu tools=%u\n", retries,
                   ROBOT_VOICE_RETRY_MAX, ret ? ret : last,
                   ctx.text_bytes, ctx.tools);
        }
    }
  RV_VERBOSE("[RV-LLM] generation complete text_bytes=%zu tools=%u rc=%d mono_ms=%llu total_ms=%llu\n",
         ctx.text_bytes, ctx.tools, ret, llm_now_ms(), llm_now_ms() - started);
  RV_EVENT("[耗时][Flash] 首个有效增量=%lldms 流式结束=%lldms rc=%d\n",
           rv_elapsed_ms(started, ctx.first_delta_ms),
           rv_elapsed_ms(started, llm_now_ms()), ret);
  if (ret == 0)
    {
      cJSON *root = cJSON_Parse(response);
      cJSON *choices = cJSON_GetObjectItem(root, "choices");
      cJSON *first = cJSON_GetArrayItem(choices, 0);
      cJSON *message = first ? cJSON_GetObjectItem(first, "message") : NULL;
      cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
      if (cJSON_IsString(content) && content->valuestring && content->valuestring[0])
        RV_DIALOGUE("[Flash回复] %s\n", content->valuestring);
      else if (!ctx.tools)
        RV_EVENT("[Flash] 返回空回复\n");
      if (ctx.tools) RV_EVENT("[Flash] 请求工具调用，等待工具结果\n");
      cJSON_Delete(root);
      ret = ctx.http_status;
    }
  else
    RV_EVENT("[Flash] 请求失败 rc=%d\n", ret);
  robot_voice_llm_stream_end(ctx.voice, ret);
  return ret;
}
