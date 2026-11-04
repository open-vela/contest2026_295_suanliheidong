/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/* MiMo ASR/TTS backend using the validated OpenAI-compatible API format. */

#include "voice/mimo_voice.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_config.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include <arch/irq.h>
#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char *TAG = "mimo_voice";

#define MIMO_ASR_BACKEND_NAME "mimo-v2.5-asr"
#define MIMO_TTS_BACKEND_NAME "mimo-v2.5-tts"
#define MIMO_VOICE_GUARD 0xa5c35a3cU

/*
 * MiMo TTS returns a Base64-encoded WAV inside JSON.  Do not buffer that
 * entire JSON document: long replies easily exceed 1 MiB.  The streaming
 * path below consumes HTTP body fragments, finds message.audio.data,
 * Base64-decodes incrementally, parses the WAV container incrementally, and
 * forwards PCM16 chunks to voice_channel while the response is still arriving.
 */
#define MIMO_TTS_B64_QUANTUM 4
#define MIMO_TTS_DECODE_BUF_BYTES 3072


struct mimo_guard_buf {
    unsigned char *raw;
    unsigned char *data;
    size_t cap;
};

static size_t mimo_stack_free(void)
{
    struct stackinfo_s info;
    uintptr_t sp;
    uintptr_t low;

    if (nxsched_get_stackinfo(0, &info) < 0) {
        return 0;
    }

    sp = (uintptr_t)up_getsp();
    low = (uintptr_t)info.stack_alloc_ptr;
    return sp > low ? sp - low : 0;
}

static int mimo_guard_alloc(struct mimo_guard_buf *buf, size_t cap)
{
    size_t total;

    if (!buf || cap == 0 || cap > SIZE_MAX - 8) {
        return -EINVAL;
    }

    total = cap + 8;
    buf->raw = calloc(1, total);
    if (!buf->raw) {
        return -ENOMEM;
    }

    *(uint32_t *)buf->raw = MIMO_VOICE_GUARD;
    *(uint32_t *)(buf->raw + 4 + cap) = MIMO_VOICE_GUARD;
    buf->data = buf->raw + 4;
    buf->cap = cap;
    return 0;
}

static bool mimo_guard_ok(const struct mimo_guard_buf *buf)
{
    return buf && buf->raw && buf->data && buf->cap > 0
        && *(const uint32_t *)buf->raw == MIMO_VOICE_GUARD
        && *(const uint32_t *)(buf->data + buf->cap) == MIMO_VOICE_GUARD;
}

static void mimo_guard_free(struct mimo_guard_buf *buf)
{
    if (buf) {
        free(buf->raw);
        memset(buf, 0, sizeof(*buf));
    }
}

static void mimo_voice_diag(const char *phase, const char *task,
    const void *body, size_t body_len, size_t body_cap,
    const void *response, size_t response_len, size_t response_cap)
{
    char task_name[32] = { 0 };

    if (!task) {
        if (pthread_getname_np(pthread_self(), task_name,
                sizeof(task_name)) != 0 || task_name[0] == '\0') {
            strncpy(task_name, "unknown", sizeof(task_name) - 1);
        }
        task = task_name;
    }

    syslog(LOG_INFO,
        "[VOICE-DIAG] %s task=%s cpu=%d sp=%08lx stack_free=%zu body=%p body_len=%zu body_cap=%zu response=%p response_len=%zu response_cap=%zu\n",
        phase, task, sched_getcpu(), (unsigned long)up_getsp(),
        mimo_stack_free(), body, body_len, body_cap, response, response_len,
        response_cap);
}

static void mimo_voice_guard_diag(const char *which, bool pass)
{
    syslog(LOG_INFO, "[VOICE-GUARD] %s %s\n", which,
        pass ? "PASS" : "FAIL");
}

static int mimo_asr_done(int ret)
{
    syslog(LOG_INFO, "[VOICE-DIAG] asr_done rc=%d\n", ret);
    return ret;
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void write_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int get_api_key(char *out, size_t cap)
{
    /* Voice-specific key must win; build_with_hal_backport.sh verifies this. */
    if (claw_config_get(AGENT_CFG_KEY_MIMO_API_KEY, out, cap) == OK
        && out[0] != '\0') {
        return 0;
    }

    if (claw_config_get(AGENT_CFG_KEY_API_KEY, out, cap) == OK
        && out[0] != '\0') {
        return 0;
    }

    out[0] = '\0';
    return -ENOENT;
}

static void get_endpoint(char *host, size_t host_cap,
                         char *path, size_t path_cap)
{
    strncpy(host, AGENT_MIMO_VOICE_HOST, host_cap - 1);
    strncpy(path, AGENT_MIMO_VOICE_PATH, path_cap - 1);

    /* ASR/TTS are served by the token-plan endpoint.  Do not inherit the
     * general LLM host: older MiMo presets used api.xiaomimimo.com. */

    host[host_cap - 1] = '\0';
    path[path_cap - 1] = '\0';
}

static int post_mimo(const char *body, char *response, size_t response_cap)
{
    char key[256] = { 0 };
    char host[128] = { 0 };
    char path[128] = { 0 };
    char auth[320];
    vela_header_t headers[2];
    int status;

    if (get_api_key(key, sizeof(key)) < 0) {
        syslog(LOG_ERR, "[%s] No API key; use set_llm or set_mimo_key\n", TAG);
        return -EACCES;
    }

    get_endpoint(host, sizeof(host), path, sizeof(path));
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    headers[0].name = "Authorization";
    headers[0].value = auth;
    headers[1].name = NULL;
    headers[1].value = NULL;

    status = vela_https_post_json(host, AGENT_MIMO_VOICE_PORT, path,
        headers, body, response, response_cap);
    memset(key, 0, sizeof(key));

    if (status < 200 || status >= 300) {
        syslog(LOG_ERR, "[%s] HTTPS request status=%d\n", TAG, status);
        return status > 0 ? -EIO : status;
    }

    return 0;
}


typedef vela_https_body_cb_t mimo_https_body_cb_t;

static int post_mimo_stream(const char *body, mimo_https_body_cb_t body_cb,
                            void *user_data)
{
    char key[256] = { 0 };
    char host[128] = { 0 };
    char path[128] = { 0 };
    char auth[320];
    vela_header_t headers[2];
    int status;

    if (!body || !body_cb) {
        return -EINVAL;
    }

    if (get_api_key(key, sizeof(key)) < 0) {
        syslog(LOG_ERR, "[%s] No API key; use set_llm or set_mimo_key\n", TAG);
        return -EACCES;
    }

    get_endpoint(host, sizeof(host), path, sizeof(path));
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    headers[0].name = "Authorization";
    headers[0].value = auth;
    headers[1].name = NULL;
    headers[1].value = NULL;

    status = vela_https_post_json_stream(host, AGENT_MIMO_VOICE_PORT, path,
        headers, body, body_cb, user_data);
    memset(key, 0, sizeof(key));

    if (status < 200 || status >= 300) {
        syslog(LOG_ERR, "[%s] HTTPS streaming request status=%d\n", TAG, status);
        return status > 0 ? -EIO : status;
    }

    return 0;
}

static int make_wav(const unsigned char *pcm, size_t pcm_len,
                    unsigned char **wav_out, size_t *wav_len)
{
    unsigned char *wav;

    if (!pcm || !wav_out || !wav_len || pcm_len > UINT32_MAX - 44) {
        return -EINVAL;
    }

    wav = malloc(pcm_len + 44);
    if (!wav) {
        return -ENOMEM;
    }

    memset(wav, 0, pcm_len + 44);
    memcpy(wav, "RIFF", 4);
    write_le32(wav + 4, (uint32_t)(pcm_len + 36));
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_le32(wav + 16, 16);
    write_le16(wav + 20, 1);
    write_le16(wav + 22, AGENT_VOICE_CHANNELS);
    write_le32(wav + 24, AGENT_VOICE_SAMPLE_RATE);
    write_le32(wav + 28, AGENT_VOICE_SAMPLE_RATE
        * AGENT_VOICE_CHANNELS * (AGENT_VOICE_BITS / 8));
    write_le16(wav + 32, AGENT_VOICE_CHANNELS * (AGENT_VOICE_BITS / 8));
    write_le16(wav + 34, AGENT_VOICE_BITS);
    memcpy(wav + 36, "data", 4);
    write_le32(wav + 40, (uint32_t)pcm_len);
    memcpy(wav + 44, pcm, pcm_len);

    *wav_out = wav;
    *wav_len = pcm_len + 44;
    return 0;
}

static int json_string(cJSON *root, const char *field,
                       char *out, size_t cap)
{
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *first;
    cJSON *message;
    cJSON *item;

    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) < 1) {
        return -EPROTO;
    }
    first = cJSON_GetArrayItem(choices, 0);
    message = cJSON_GetObjectItemCaseSensitive(first, "message");
    item = message ? cJSON_GetObjectItemCaseSensitive(message, field) : NULL;
    if (!cJSON_IsString(item) || !item->valuestring) {
        return -EPROTO;
    }

    strncpy(out, item->valuestring, cap - 1);
    out[cap - 1] = '\0';
    return 0;
}

static int make_asr_body(const unsigned char *pcm, size_t pcm_len,
                         char **body_out, size_t *body_len_out)
{
    unsigned char *wav = NULL;
    size_t wav_len = 0;
    size_t b64_cap;
    size_t b64_len = 0;
    char *b64 = NULL;
    char *data_uri = NULL;
    cJSON *root = NULL;
    cJSON *messages;
    cJSON *message;
    cJSON *content;
    cJSON *audio_item;
    cJSON *audio;
    char *body;
    int ret;

    ret = make_wav(pcm, pcm_len, &wav, &wav_len);
    if (ret < 0) {
        return ret;
    }

    b64_cap = ((wav_len + 2) / 3) * 4 + 1;
    b64 = malloc(b64_cap);
    if (!b64) {
        free(wav);
        return -ENOMEM;
    }

    /* The ESP mbedTLS implementation requires destination capacity for the
     * encoded data plus its terminating NUL. */
    ret = mbedtls_base64_encode((unsigned char *)b64, b64_cap,
        &b64_len, wav, wav_len);
    free(wav);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] WAV base64 encode failed: %d\n", TAG, ret);
        free(b64);
        return -EIO;
    }
    b64[b64_len] = '\0';

    data_uri = malloc(b64_len + 24);
    if (!data_uri) {
        free(b64);
        return -ENOMEM;
    }
    snprintf(data_uri, b64_len + 24, "data:audio/wav;base64,%s", b64);
    free(b64);

    root = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    message = cJSON_CreateObject();
    content = cJSON_CreateArray();
    audio_item = cJSON_CreateObject();
    audio = cJSON_CreateObject();
    if (!root || !messages || !message || !content || !audio_item || !audio) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(message);
        cJSON_Delete(content);
        cJSON_Delete(audio_item);
        cJSON_Delete(audio);
        free(data_uri);
        return -ENOMEM;
    }

    cJSON_AddStringToObject(root, "model", AGENT_MIMO_ASR_MODEL);
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(audio_item, "type", "input_audio");
    cJSON_AddStringToObject(audio, "data", data_uri);
    cJSON_AddItemToObject(audio_item, "input_audio", audio);
    cJSON_AddItemToArray(content, audio_item);
    cJSON_AddItemToObject(message, "content", content);
    cJSON_AddItemToArray(messages, message);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddItemToObject(root, "asr_options", cJSON_CreateObject());
    cJSON_AddStringToObject(cJSON_GetObjectItem(root, "asr_options"),
        "language", "zh");

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(data_uri);
    if (!body) {
        return -ENOMEM;
    }

    *body_out = body;
    *body_len_out = strlen(body);
    return 0;
}

static int mimo_asr_init(void)
{
    char key[256] = { 0 };
    int ret = get_api_key(key, sizeof(key));
    memset(key, 0, sizeof(key));
    if (ret < 0) {
        syslog(LOG_WARNING, "[%s] ASR API key is not configured\n", TAG);
    }
    return 0;
}

static int mimo_asr_recognize(const unsigned char *pcm_data,
                              size_t pcm_len,
                              char *text_out,
                              size_t text_cap)
{
    char *body = NULL;
    struct mimo_guard_buf body_buf = { 0 };
    size_t body_len = 0;
    struct mimo_guard_buf response_buf = { 0 };
    cJSON *root = NULL;
    int ret;

    syslog(LOG_INFO, "[VOICE-DIAG] asr_begin\n");
    if (!pcm_data || pcm_len == 0 || !text_out || text_cap == 0) {
        return mimo_asr_done(-EINVAL);
    }
    if (pcm_len > AGENT_MIMO_MAX_AUDIO_BYTES) {
        return mimo_asr_done(-EFBIG);
    }

    ret = make_asr_body(pcm_data, pcm_len, &body, &body_len);
    if (ret < 0) {
        return mimo_asr_done(ret);
    }
    if (body_len == SIZE_MAX) {
        free(body);
        return mimo_asr_done(-EOVERFLOW);
    }

    ret = mimo_guard_alloc(&body_buf, body_len + 1);
    if (ret < 0) {
        free(body);
        return mimo_asr_done(ret);
    }
    memcpy(body_buf.data, body, body_len + 1);
    free(body);
    body = (char *)body_buf.data;

    ret = mimo_guard_alloc(&response_buf, AGENT_MIMO_HTTP_RESP_BYTES);
    if (ret < 0) {
        mimo_guard_free(&body_buf);
        return mimo_asr_done(ret);
    }

    mimo_voice_guard_diag("body", mimo_guard_ok(&body_buf));
    mimo_voice_guard_diag("response", mimo_guard_ok(&response_buf));
    if (!mimo_guard_ok(&body_buf) || !mimo_guard_ok(&response_buf)) {
        return mimo_asr_done(-EFAULT);
    }
    syslog(LOG_INFO, "[VOICE-DIAG] body_ready len=%zu cap=%zu\n",
        body_len, body_buf.cap);
    mimo_voice_diag("body_ready", NULL, body, body_len, body_buf.cap,
        response_buf.data, 0, response_buf.cap);
    mimo_voice_guard_diag("body", mimo_guard_ok(&body_buf));
    mimo_voice_guard_diag("response", mimo_guard_ok(&response_buf));
    if (!mimo_guard_ok(&body_buf) || !mimo_guard_ok(&response_buf)) {
        return mimo_asr_done(-EFAULT);
    }
    mimo_voice_diag("before_transport", NULL, body, body_len,
        body_buf.cap, response_buf.data, 0, response_buf.cap);
    syslog(LOG_INFO, "[VOICE-DIAG] transport_enter body=%p body_len=%zu response=%p response_cap=%zu\n",
        body, body_len, response_buf.data, response_buf.cap);

    ret = post_mimo(body, (char *)response_buf.data, response_buf.cap);
    size_t response_len = ret >= 0
        ? strnlen((char *)response_buf.data, response_buf.cap) : 0;
    syslog(LOG_INFO, "[VOICE-DIAG] transport_return rc=%d response_len=%zu\n",
        ret, response_len);
    mimo_voice_diag("after_transport", NULL, body, body_len, body_buf.cap,
        response_buf.data, response_len, response_buf.cap);
    mimo_voice_guard_diag("body", mimo_guard_ok(&body_buf));
    mimo_voice_guard_diag("response", mimo_guard_ok(&response_buf));
    if (!mimo_guard_ok(&body_buf) || !mimo_guard_ok(&response_buf)) {
        return mimo_asr_done(-EFAULT);
    }
    mimo_guard_free(&body_buf);
    if (ret < 0) {
        mimo_guard_free(&response_buf);
        return mimo_asr_done(ret);
    }
    if (response_len == response_buf.cap) {
        mimo_guard_free(&response_buf);
        return mimo_asr_done(-EOVERFLOW);
    }

    root = cJSON_Parse((char *)response_buf.data);
    mimo_voice_guard_diag("response", mimo_guard_ok(&response_buf));
    if (!mimo_guard_ok(&response_buf)) {
        return mimo_asr_done(-EFAULT);
    }
    mimo_guard_free(&response_buf);
    if (!root) {
        return mimo_asr_done(-EPROTO);
    }

    ret = json_string(root, "content", text_out, text_cap);
    cJSON_Delete(root);
    if (ret == 0) {
        syslog(LOG_INFO, "[%s] ASR recognized %zu bytes\n", TAG, pcm_len);
    }
    return mimo_asr_done(ret);
}

static int make_tts_body(const char *text, char **body_out)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    char *body;

    if (!root || !messages || !message || !audio) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(message);
        cJSON_Delete(audio);
        return -ENOMEM;
    }

    cJSON_AddStringToObject(root, "model", AGENT_MIMO_TTS_MODEL);
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content", text);
    cJSON_AddItemToArray(messages, message);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddStringToObject(audio, "format", "wav");
    cJSON_AddStringToObject(audio, "voice", AGENT_MIMO_TTS_VOICE);
    cJSON_AddItemToObject(root, "audio", audio);

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return -ENOMEM;
    }
    *body_out = body;
    return 0;
}

/* ── TTS streaming JSON -> Base64 -> WAV -> PCM pipeline ───────────── */

typedef void (*mimo_tts_pcm_cb_t)(const unsigned char *pcm_data,
                                  size_t pcm_len, int is_last,
                                  void *user_data);

enum mimo_wav_state {
    MIMO_WAV_RIFF = 0,
    MIMO_WAV_CHUNK_HEADER,
    MIMO_WAV_CHUNK_DATA,
    MIMO_WAV_PAD,
    MIMO_WAV_DONE,
};

struct mimo_wav_stream {
    enum mimo_wav_state state;
    unsigned char small[16];
    size_t small_used;
    char chunk_id[4];
    uint32_t chunk_len;
    uint32_t chunk_remaining;
    uint16_t audio_format;
    uint16_t channels;
    uint16_t bits;
    uint32_t sample_rate;
    bool fmt_seen;
    bool data_seen;
    bool pcm_pending_valid;
    unsigned char pcm_pending;
    size_t pcm_total;
    mimo_tts_pcm_cb_t pcm_cb;
    void *pcm_user;
};

enum mimo_json_stream_state {
    MIMO_JSON_FIND_AUDIO = 0,
    MIMO_JSON_AFTER_AUDIO,
    MIMO_JSON_FIND_DATA,
    MIMO_JSON_AFTER_DATA,
    MIMO_JSON_IN_B64,
    MIMO_JSON_DONE,
};

struct mimo_tts_stream {
    enum mimo_json_stream_state json_state;
    size_t audio_match;
    size_t data_match;
    unsigned char b64_q[MIMO_TTS_B64_QUANTUM];
    size_t b64_q_len;
    unsigned char decoded_buf[MIMO_TTS_DECODE_BUF_BYTES];
    size_t decoded_len;
    bool b64_padded;
    bool audio_found;
    bool data_found;
    bool first_pcm_logged;
    size_t http_bytes;
    size_t b64_chars;
    int error;
    struct mimo_wav_stream wav;
};

static int mimo_wav_emit(struct mimo_wav_stream *wav,
                         const unsigned char *data, size_t len)
{
    unsigned char pair[2];
    size_t even_len;

    if (!wav || (!data && len > 0) || !wav->pcm_cb) {
        return -EINVAL;
    }

    if (len == 0) {
        return 0;
    }

    if (wav->pcm_pending_valid) {
        pair[0] = wav->pcm_pending;
        pair[1] = data[0];
        wav->pcm_cb(pair, sizeof(pair), 0, wav->pcm_user);
        wav->pcm_total += sizeof(pair);
        wav->pcm_pending_valid = false;
        data++;
        len--;
    }

    even_len = len & ~(size_t)1;
    if (even_len > 0) {
        wav->pcm_cb(data, even_len, 0, wav->pcm_user);
        wav->pcm_total += even_len;
        data += even_len;
        len -= even_len;
    }

    if (len == 1) {
        wav->pcm_pending = data[0];
        wav->pcm_pending_valid = true;
    }

    return 0;
}

static int mimo_wav_validate_fmt(struct mimo_wav_stream *wav)
{
    if (!wav || !wav->fmt_seen) {
        return -EPROTO;
    }

    if (wav->audio_format != 1 || wav->channels != 1 || wav->bits != 16
        || wav->sample_rate == 0) {
        syslog(LOG_ERR,
            "[%s] TTS WAV unsupported fmt=%u ch=%u rate=%lu bits=%u\n",
            TAG, (unsigned int)wav->audio_format,
            (unsigned int)wav->channels, (unsigned long)wav->sample_rate,
            (unsigned int)wav->bits);
        return -ENOTSUP;
    }

    if (wav->sample_rate != AGENT_MIMO_TTS_SAMPLE_RATE) {
        syslog(LOG_ERR, "[%s] TTS sample rate %lu unsupported\n",
            TAG, (unsigned long)wav->sample_rate);
        return -ENOTSUP;
    }

    return 0;
}

static int mimo_wav_feed(struct mimo_wav_stream *wav,
                         const unsigned char *data, size_t len)
{
    while (len > 0) {
        if (wav->state == MIMO_WAV_RIFF) {
            size_t need = 12 - wav->small_used;
            size_t take = len < need ? len : need;
            memcpy(wav->small + wav->small_used, data, take);
            wav->small_used += take;
            data += take;
            len -= take;
            if (wav->small_used == 12) {
                if (memcmp(wav->small, "RIFF", 4) != 0
                    || memcmp(wav->small + 8, "WAVE", 4) != 0) {
                    return -EPROTO;
                }
                wav->small_used = 0;
                wav->state = MIMO_WAV_CHUNK_HEADER;
            }
            continue;
        }

        if (wav->state == MIMO_WAV_CHUNK_HEADER) {
            size_t need = 8 - wav->small_used;
            size_t take = len < need ? len : need;
            memcpy(wav->small + wav->small_used, data, take);
            wav->small_used += take;
            data += take;
            len -= take;
            if (wav->small_used == 8) {
                memcpy(wav->chunk_id, wav->small, 4);
                wav->chunk_len = read_le32(wav->small + 4);
                wav->chunk_remaining = wav->chunk_len;
                wav->small_used = 0;

                if (memcmp(wav->chunk_id, "data", 4) == 0) {
                    int ret = mimo_wav_validate_fmt(wav);
                    if (ret < 0) {
                        return ret;
                    }
                    wav->data_seen = true;
                    syslog(LOG_INFO,
                        "[%s] TTS WAV data begin bytes=%lu rate=%lu\n",
                        TAG, (unsigned long)wav->chunk_len,
                        (unsigned long)wav->sample_rate);
                }
                wav->state = MIMO_WAV_CHUNK_DATA;
            }
            continue;
        }

        if (wav->state == MIMO_WAV_CHUNK_DATA) {
            size_t take = len;
            if (take > wav->chunk_remaining) {
                take = wav->chunk_remaining;
            }

            if (memcmp(wav->chunk_id, "fmt ", 4) == 0) {
                size_t need = sizeof(wav->small) - wav->small_used;
                size_t keep = take < need ? take : need;
                if (keep > 0) {
                    memcpy(wav->small + wav->small_used, data, keep);
                    wav->small_used += keep;
                }
            } else if (memcmp(wav->chunk_id, "data", 4) == 0) {
                int ret = mimo_wav_emit(wav, data, take);
                if (ret < 0) {
                    return ret;
                }
            }

            data += take;
            len -= take;
            wav->chunk_remaining -= (uint32_t)take;

            if (wav->chunk_remaining == 0) {
                if (memcmp(wav->chunk_id, "fmt ", 4) == 0) {
                    if (wav->chunk_len < 16 || wav->small_used < 16) {
                        return -EPROTO;
                    }
                    wav->audio_format = read_le16(wav->small);
                    wav->channels = read_le16(wav->small + 2);
                    wav->sample_rate = read_le32(wav->small + 4);
                    wav->bits = read_le16(wav->small + 14);
                    wav->fmt_seen = true;
                    wav->small_used = 0;
                }

                if (wav->chunk_len & 1U) {
                    wav->state = MIMO_WAV_PAD;
                } else {
                    wav->state = MIMO_WAV_CHUNK_HEADER;
                }
            }
            continue;
        }

        if (wav->state == MIMO_WAV_PAD) {
            data++;
            len--;
            wav->state = MIMO_WAV_CHUNK_HEADER;
            continue;
        }

        if (wav->state == MIMO_WAV_DONE) {
            return 0;
        }
    }

    return 0;
}

static int mimo_wav_finish(struct mimo_wav_stream *wav)
{
    if (!wav || !wav->data_seen || wav->pcm_total == 0) {
        return -EPROTO;
    }
    if (wav->pcm_pending_valid) {
        return -EPROTO;
    }
    return 0;
}

static int mimo_b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    if (c == '=') {
        return -2;
    }
    return -1;
}

static int mimo_tts_flush_decoded(struct mimo_tts_stream *st)
{
    int ret;

    if (!st || st->decoded_len == 0) {
        return 0;
    }

    ret = mimo_wav_feed(&st->wav, st->decoded_buf, st->decoded_len);
    st->decoded_len = 0;
    return ret;
}

static int mimo_tts_queue_decoded(struct mimo_tts_stream *st,
                                  const unsigned char *data, size_t len)
{
    int ret;

    if (len > sizeof(st->decoded_buf)) {
        return -EOVERFLOW;
    }
    if (st->decoded_len + len > sizeof(st->decoded_buf)) {
        ret = mimo_tts_flush_decoded(st);
        if (ret < 0) {
            return ret;
        }
    }

    memcpy(st->decoded_buf + st->decoded_len, data, len);
    st->decoded_len += len;
    if (st->decoded_len == sizeof(st->decoded_buf)) {
        return mimo_tts_flush_decoded(st);
    }
    return 0;
}

static int mimo_b64_decode_quantum(struct mimo_tts_stream *st)
{
    int a = mimo_b64_value(st->b64_q[0]);
    int b = mimo_b64_value(st->b64_q[1]);
    int c = mimo_b64_value(st->b64_q[2]);
    int d = mimo_b64_value(st->b64_q[3]);
    unsigned char out[3];
    size_t out_len = 3;

    if (a < 0 || b < 0 || c == -1 || d == -1) {
        return -EPROTO;
    }
    if (c == -2 && d != -2) {
        return -EPROTO;
    }

    out[0] = (unsigned char)((a << 2) | (b >> 4));
    if (c == -2) {
        out_len = 1;
        st->b64_padded = true;
    } else {
        out[1] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
        if (d == -2) {
            out_len = 2;
            st->b64_padded = true;
        } else {
            out[2] = (unsigned char)(((c & 0x03) << 6) | d);
        }
    }

    st->b64_q_len = 0;
    return mimo_tts_queue_decoded(st, out, out_len);
}

static int mimo_b64_feed_char(struct mimo_tts_stream *st, unsigned char c)
{
    int v;

    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
        return 0;
    }
    if (st->b64_padded) {
        return -EPROTO;
    }

    v = mimo_b64_value(c);
    if (v == -1) {
        return -EPROTO;
    }

    st->b64_q[st->b64_q_len++] = c;
    st->b64_chars++;
    if (st->b64_q_len == MIMO_TTS_B64_QUANTUM) {
        return mimo_b64_decode_quantum(st);
    }
    return 0;
}

static int mimo_b64_finish(struct mimo_tts_stream *st)
{
    unsigned char out[2];
    int a;
    int b;
    int c;

    if (st->b64_q_len == 0) {
        return mimo_tts_flush_decoded(st);
    }
    if (st->b64_q_len == 1) {
        return -EPROTO;
    }

    a = mimo_b64_value(st->b64_q[0]);
    b = mimo_b64_value(st->b64_q[1]);
    if (a < 0 || b < 0) {
        return -EPROTO;
    }
    out[0] = (unsigned char)((a << 2) | (b >> 4));

    if (st->b64_q_len == 2) {
        st->b64_q_len = 0;
        {
            int ret = mimo_tts_queue_decoded(st, out, 1);
            if (ret < 0) {
                return ret;
            }
            return mimo_tts_flush_decoded(st);
        }
    }

    c = mimo_b64_value(st->b64_q[2]);
    if (c < 0) {
        return -EPROTO;
    }
    out[1] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
    st->b64_q_len = 0;
    {
        int ret = mimo_tts_queue_decoded(st, out, 2);
        if (ret < 0) {
            return ret;
        }
        return mimo_tts_flush_decoded(st);
    }
}

static void mimo_match_key(size_t *match, const char *key,
                           unsigned char c)
{
    size_t key_len = strlen(key);

    if (c == (unsigned char)key[*match]) {
        (*match)++;
        if (*match > key_len) {
            *match = 0;
        }
    } else {
        *match = c == (unsigned char)key[0] ? 1 : 0;
    }
}

static int mimo_tts_json_feed(const unsigned char *data, size_t len,
                              void *user_data)
{
    static const char audio_key[] = "\"audio\"";
    static const char data_key[] = "\"data\"";
    struct mimo_tts_stream *st = (struct mimo_tts_stream *)user_data;
    size_t i;

    if (!st || (!data && len > 0)) {
        return -EINVAL;
    }
    if (st->error < 0) {
        return st->error;
    }

    st->http_bytes += len;

    for (i = 0; i < len; i++) {
        unsigned char c = data[i];
        int ret;

        switch (st->json_state) {
        case MIMO_JSON_FIND_AUDIO:
            mimo_match_key(&st->audio_match, audio_key, c);
            if (st->audio_match == strlen(audio_key)) {
                st->audio_found = true;
                st->audio_match = 0;
                st->json_state = MIMO_JSON_AFTER_AUDIO;
            }
            break;

        case MIMO_JSON_AFTER_AUDIO:
            if (c == ':' || c == '{' || c == ' ' || c == '\r'
                || c == '\n' || c == '\t') {
                if (c == '{') {
                    st->json_state = MIMO_JSON_FIND_DATA;
                }
            } else {
                st->json_state = MIMO_JSON_FIND_AUDIO;
            }
            break;

        case MIMO_JSON_FIND_DATA:
            mimo_match_key(&st->data_match, data_key, c);
            if (st->data_match == strlen(data_key)) {
                st->data_match = 0;
                st->json_state = MIMO_JSON_AFTER_DATA;
            } else if (c == '}') {
                st->json_state = MIMO_JSON_FIND_AUDIO;
            }
            break;

        case MIMO_JSON_AFTER_DATA:
            if (c == ':' || c == ' ' || c == '\r' || c == '\n' || c == '\t') {
                break;
            }
            if (c == '"') {
                st->data_found = true;
                st->json_state = MIMO_JSON_IN_B64;
                syslog(LOG_INFO, "[%s] TTS audio base64 stream begin\n", TAG);
                break;
            }
            st->json_state = MIMO_JSON_FIND_DATA;
            break;

        case MIMO_JSON_IN_B64:
            if (c == '"') {
                ret = mimo_b64_finish(st);
                if (ret < 0) {
                    st->error = ret;
                    return ret;
                }
                st->json_state = MIMO_JSON_DONE;
                break;
            }
            ret = mimo_b64_feed_char(st, c);
            if (ret < 0) {
                st->error = ret;
                return ret;
            }
            break;

        case MIMO_JSON_DONE:
            break;
        }
    }

    return 0;
}

static int mimo_tts_stream_run(const char *text, mimo_tts_pcm_cb_t pcm_cb,
                               void *pcm_user)
{
    char *body = NULL;
    struct mimo_tts_stream st;
    int ret;

    if (!text || text[0] == '\0' || !pcm_cb) {
        return -EINVAL;
    }

    memset(&st, 0, sizeof(st));
    st.json_state = MIMO_JSON_FIND_AUDIO;
    st.wav.state = MIMO_WAV_RIFF;
    st.wav.pcm_cb = pcm_cb;
    st.wav.pcm_user = pcm_user;

    ret = make_tts_body(text, &body);
    if (ret < 0) {
        return ret;
    }

    syslog(LOG_INFO, "[%s] TTS streaming request body=%zu\n",
        TAG, strlen(body));
    ret = post_mimo_stream(body, mimo_tts_json_feed, &st);
    free(body);
    if (ret < 0) {
        return ret;
    }
    if (st.error < 0) {
        return st.error;
    }
    if (!st.audio_found || !st.data_found || st.json_state != MIMO_JSON_DONE) {
        syslog(LOG_ERR,
            "[%s] TTS stream JSON incomplete http=%zu audio=%d data=%d state=%d\n",
            TAG, st.http_bytes, st.audio_found ? 1 : 0,
            st.data_found ? 1 : 0, (int)st.json_state);
        return -EPROTO;
    }

    ret = mimo_wav_finish(&st.wav);
    if (ret < 0) {
        syslog(LOG_ERR,
            "[%s] TTS WAV stream incomplete ret=%d pcm=%zu b64=%zu\n",
            TAG, ret, st.wav.pcm_total, st.b64_chars);
        return ret;
    }

    syslog(LOG_INFO,
        "[%s] TTS stream done http=%zu b64=%zu pcm=%zu\n",
        TAG, st.http_bytes, st.b64_chars, st.wav.pcm_total);
    pcm_cb(NULL, 0, 1, pcm_user);
    return 0;
}

struct mimo_pcm_collect {
    unsigned char *out;
    size_t cap;
    size_t len;
    int error;
};

static void mimo_pcm_collect_cb(const unsigned char *pcm_data,
                                size_t pcm_len, int is_last,
                                void *user_data)
{
    struct mimo_pcm_collect *sink = (struct mimo_pcm_collect *)user_data;

    (void)is_last;
    if (!sink || sink->error < 0 || !pcm_data || pcm_len == 0) {
        return;
    }
    if (pcm_len > sink->cap - sink->len) {
        sink->error = -ENOSPC;
        return;
    }

    memcpy(sink->out + sink->len, pcm_data, pcm_len);
    sink->len += pcm_len;
}

static int mimo_tts_init(void)
{
    return mimo_asr_init();
}

static int mimo_tts_synthesize(const char *text,
                               unsigned char *pcm_out,
                               size_t pcm_cap,
                               size_t *pcm_len)
{
    struct mimo_pcm_collect sink;
    int ret;

    if (!text || text[0] == '\0' || !pcm_out || !pcm_len) {
        return -EINVAL;
    }

    memset(&sink, 0, sizeof(sink));
    sink.out = pcm_out;
    sink.cap = pcm_cap;

    ret = mimo_tts_stream_run(text, mimo_pcm_collect_cb, &sink);
    if (ret < 0) {
        return ret;
    }
    if (sink.error < 0) {
        syslog(LOG_ERR,
            "[%s] TTS PCM output too small pcm_cap=%zu collected=%zu\n",
            TAG, pcm_cap, sink.len);
        return sink.error;
    }

    *pcm_len = sink.len;
    syslog(LOG_INFO, "[%s] TTS PCM bytes=%zu\n", TAG, sink.len);
    return 0;
}

static int mimo_tts_stream_synthesize(const char *text,
    void (*callback)(const unsigned char *pcm_data, size_t pcm_len,
                     int is_last, void *user_data),
    void *user_data)
{
    if (!callback) {
        return -EINVAL;
    }
    return mimo_tts_stream_run(text, callback, user_data);
}

static const voice_asr_ops_t s_mimo_asr_ops = {
    .name = MIMO_ASR_BACKEND_NAME,
    .init = mimo_asr_init,
    .recognize = mimo_asr_recognize,
    .stream_open = NULL,
    .stream_send = NULL,
    .stream_finish = NULL,
    .stream_abort = NULL,
    .deinit = NULL,
};

static const voice_tts_ops_t s_mimo_tts_ops = {
    .name = MIMO_TTS_BACKEND_NAME,
    .init = mimo_tts_init,
    .synthesize = mimo_tts_synthesize,
    .stream_synthesize = mimo_tts_stream_synthesize,
    .deinit = NULL,
};

int mimo_asr_register(void)
{
    return voice_asr_register(&s_mimo_asr_ops);
}

int mimo_tts_register(void)
{
    return voice_tts_register(&s_mimo_tts_ops);
}
