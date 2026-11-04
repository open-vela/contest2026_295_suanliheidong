/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tools/tool_media.h"
#include "core/message_bus.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"

#include <math.h>
#include <errno.h>
#include <semaphore.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "cJSON.h"

#include <media_player.h>
#include <media_policy.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TAG "AGENT_MUSIC"

/* The contest voice controller owns Wi-Fi recovery. Keep these weak so the
 * official Agent remains usable on boards that do not link robot_voice. */
extern int robot_voice_wifi_ensure_connected(unsigned int timeout_ms)
    __attribute__((weak));
extern int robot_voice_wifi_reconnect_and_ensure(unsigned int timeout_ms)
    __attribute__((weak));

/* Optional contest runtime: owns the single network/audio task scope. */
extern int robot_task_begin(void) __attribute__((weak));
extern void robot_task_end(void) __attribute__((weak));
extern void robot_task_end_without_resume(void) __attribute__((weak));
extern bool robot_music_media_is_active(void) __attribute__((weak));
extern bool robot_music_media_start_pending(void *) __attribute__((weak));

#define MEDIA_URL_MAX 512
#define MEDIA_OPTIONS_MAX 256
#define MEDIA_VOL_MIN_IDX 0
#define MEDIA_VOL_MAX_IDX 15
#define MEDIA_VOL_USER_MIN 0
#define MEDIA_VOL_USER_MAX 100

/* Timeouts for sem_timedwait (milliseconds) */

#define TIMEOUT_PREPARE_MS 10000
#define TIMEOUT_START_MS 3000
#define TIMEOUT_PAUSE_MS 3000
#define TIMEOUT_STOP_MS 3000
#define TIMEOUT_SEEK_MS 3000

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef enum {
    MEDIA_STATE_IDLE = 0,
    MEDIA_STATE_PREPARED,
    MEDIA_STATE_PLAYING,
    MEDIA_STATE_PAUSED,
    MEDIA_STATE_COMPLETED,
    MEDIA_STATE_ERROR,
} media_state_e;

typedef struct
{
    pthread_mutex_t lock;
    media_state_e state;
    void* handle;
    uint32_t session_id;
    char url[MEDIA_URL_MAX];
    int volume; /* user-facing 0..100 */
    bool exiting; /* module teardown flag */
    bool stopping; /* stop/close is in progress; reject stale resume */
    /* Reuse the former is_live_stream byte for live/task policy bits. */
    unsigned char policy_flags;
#define MEDIA_IS_LIVE 1u
#define MEDIA_TASK_HELD 2u
#define MEDIA_TASK_RESUME 4u

    /* Event confirmation via semaphore */

    sem_t sem; /* posted by event callback */
    int last_event; /* MEDIA_EVENT_* from callback */
    int last_result; /* result code from callback */
} media_ctx_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static media_ctx_t g_media;
static bool g_media_initialized = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void media_ctx_init_once(void)
{
    if (g_media_initialized) {
        return;
    }

    memset(&g_media, 0, sizeof(g_media));
    pthread_mutex_init(&g_media.lock, NULL);
    sem_init(&g_media.sem, 0, 0);
    g_media.state = MEDIA_STATE_IDLE;
    g_media.handle = NULL;
    g_media.session_id = 0;
    g_media.volume = 50;
    g_media.exiting = false;
    g_media.stopping = false;
    g_media.last_event = 0;
    g_media.last_result = 0;
    g_media_initialized = true;
}

static const char* state_str(media_state_e s)
{
    switch (s) {
    case MEDIA_STATE_IDLE:
        return "IDLE";
    case MEDIA_STATE_PREPARED:
        return "PREPARED";
    case MEDIA_STATE_PLAYING:
        return "PLAYING";
    case MEDIA_STATE_PAUSED:
        return "PAUSED";
    case MEDIA_STATE_COMPLETED:
        return "COMPLETED";
    case MEDIA_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

/**
 * Sanitize URL for logging — strip query string to avoid token leaks.
 */

static void log_url(const char* url, char* buf, size_t bufsz)
{
    const char* q = strchr(url, '?');
    if (q) {
        size_t len = (size_t)(q - url);
        if (len >= bufsz) {
            len = bufsz - 1;
        }

        memcpy(buf, url, len);
        buf[len] = '\0';
    } else {
        snprintf(buf, bufsz, "%s", url);
    }
}

/**
 * Force-close current session (stop + close). Caller must hold lock.
 * Does NOT wait for events — used for error recovery and cleanup.
 */

static void force_close_locked(void)
{
    void *handle = g_media.handle;
    bool stopped = g_media.state == MEDIA_STATE_COMPLETED ||
                   g_media.state == MEDIA_STATE_ERROR;
    g_media.stopping = true;
    g_media.handle = NULL;
    g_media.session_id++; /* Invalidate callbacks before dropping the lock. */
    g_media.state = MEDIA_STATE_IDLE;
    g_media.policy_flags &= (unsigned char)~MEDIA_TASK_RESUME;
    g_media.url[0] = '\0';
    pthread_mutex_unlock(&g_media.lock);
    if (handle) {
        if (!stopped) media_player_stop(handle);
        media_player_close(handle, 0);
    }
    pthread_mutex_lock(&g_media.lock);
    g_media.stopping = false;
}

/**
 * Map user volume (0..100) to hardware index.
 */

static int volume_to_index(int vol)
{
    if (vol <= MEDIA_VOL_USER_MIN) {
        return MEDIA_VOL_MIN_IDX;
    }

    if (vol >= MEDIA_VOL_USER_MAX) {
        return MEDIA_VOL_MAX_IDX;
    }

    int idx = MEDIA_VOL_MIN_IDX + (int)round((double)vol * (MEDIA_VOL_MAX_IDX - MEDIA_VOL_MIN_IDX) / 100.0);

    if (idx < MEDIA_VOL_MIN_IDX) {
        idx = MEDIA_VOL_MIN_IDX;
    }

    if (idx > MEDIA_VOL_MAX_IDX) {
        idx = MEDIA_VOL_MAX_IDX;
    }

    return idx;
}

/**
 * Push a notification to the agent loop so the LLM can inform the user
 * about asynchronous playback events (EOF, errors).
 * Must be called WITHOUT g_media.lock held (push may block briefly).
 */

static void notify_agent(const char* fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    agent_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, "system", sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "media", sizeof(msg.chat_id) - 1);
    msg.flags = AGENT_MSG_FLAG_INTERNAL_MUSIC | AGENT_MSG_FLAG_NO_TTS;
    msg.content = strdup(buf);
    if (msg.content) {
        if (message_bus_push_inbound(&msg) != OK) {
            syslog(LOG_WARNING, "[%s] Failed to push media notification\n", TAG);
            free(msg.content);
        }
    }
}

/**
 * Media event callback — called from media framework thread.
 * Records event + result, then posts semaphore for waiting execute.
 * Also handles autonomous events (COMPLETED / async errors).
 */

static void media_event_cb(void* cookie, int event, int result,
    const char* extra)
{
    uint32_t cb_session = (uint32_t)(uintptr_t)cookie;

    pthread_mutex_lock(&g_media.lock);

    /* Discard stale session events */

    if (cb_session != g_media.session_id) {
        syslog(LOG_DEBUG,
            "[%s] Stale event %d for session %" PRIu32
            " (current %" PRIu32 "), discarded\n",
            TAG, event, cb_session, g_media.session_id);
        pthread_mutex_unlock(&g_media.lock);
        return;
    }

    if (g_media.exiting) {
        pthread_mutex_unlock(&g_media.lock);
        return;
    }

    /* Discard events after force_close (e.g. STOPPED from cleanup) */

    if (g_media.state == MEDIA_STATE_IDLE && g_media.handle == NULL) {
        syslog(LOG_DEBUG,
            "[%s] Event %d after cleanup, discarded\n",
            TAG, event);
        pthread_mutex_unlock(&g_media.lock);
        return;
    }

    syslog(LOG_INFO, "[%s] Event %d result %d extra=%s\n",
        TAG, event, result, extra ? extra : "");

    /* Ignore NOP events — audio_output fires these continuously
     * during playback.  If we let them through they poison
     * last_event and wake sem, breaking wait_event() for real
     * commands (stop / pause / seek / switch). */

    if (event == MEDIA_EVENT_NOP) {
        pthread_mutex_unlock(&g_media.lock);
        return;
    }

    /* Handle autonomous COMPLETED (EOF or error-complete).  The media
     * server has already drained and marked the graph EOF at this point.
     * Calling media_player_stop() here would unlink the same EOF links a
     * second time and trips FFmpeg's status_in assertion.  Keep the handle
     * until the next explicit stop/cut-song/cleanup path, which can close it
     * through the completed-state-safe server path. */

    if (event == MEDIA_EVENT_COMPLETED) {
        uint32_t sid = g_media.session_id;

        g_media.last_event = event;
        g_media.last_result = result;
        g_media.state = result >= 0 ? MEDIA_STATE_COMPLETED : MEDIA_STATE_ERROR;
        pthread_mutex_unlock(&g_media.lock);
        sem_post(&g_media.sem);

        if (result >= 0) {
            syslog(LOG_INFO, "[%s] EOF session %" PRIu32
                " (handle retained until explicit close)\n", TAG, sid);
            notify_agent("[music] Playback finished (session %" PRIu32 ")."
                         " The track has ended normally.", sid);
        } else {
            syslog(LOG_ERR,
                "[%s] COMPLETED with error result=%d session %" PRIu32 "\n",
                TAG, result, sid);
            if (result == -EBUSY) {
                notify_agent("[music] Playback could not open the network "
                             "stream (session %" PRIu32 ", error=%d). No "
                             "audio bytes reached the MP3 decoder; this was "
                             "a temporary network resource-busy condition, "
                             "not an unsupported audio format.", sid, result);
            } else {
                notify_agent("[music] Playback failed (session %" PRIu32
                             ", error=%d). Please inform the user that"
                             " the music could not be played successfully.",
                    sid, result);
            }
        }

        return;
    }

    /* Record event + result for the waiting execute function */

    g_media.last_event = event;
    g_media.last_result = result;

    /* Update state based on confirmed events */

    if (result >= 0) {
        switch (event) {
        case MEDIA_EVENT_PREPARED:
            g_media.state = MEDIA_STATE_PREPARED;
            break;

        case MEDIA_EVENT_STARTED:
            g_media.state = MEDIA_STATE_PLAYING;
            break;

        case MEDIA_EVENT_PAUSED:
            g_media.state = MEDIA_STATE_PAUSED;
            break;

        case MEDIA_EVENT_STOPPED:
            /* stop event — state will be set to IDLE by force_close */
            break;

        default:
            break;
        }
    } else {
        /* Async error */

        syslog(LOG_ERR,
            "[%s] Async error event=%d result=%d extra=%s\n",
            TAG, event, result, extra ? extra : "");
        g_media.state = MEDIA_STATE_ERROR;
    }

    pthread_mutex_unlock(&g_media.lock);

    /* Wake up the waiting execute function */

    sem_post(&g_media.sem);
}

/**
 * Wait for a specific media event with timeout.
 * Caller must hold g_media.lock. Lock is released during wait
 * to allow the callback to run, then re-acquired.
 *
 * Returns 0 on success (expected event with result >= 0).
 * Returns -1 on timeout, wrong event, or error result.
 */

static void arm_event_locked(void)
{
    while (sem_trywait(&g_media.sem) == 0) { }
    g_media.last_event = -1;
    g_media.last_result = 0;
}

static int wait_event(int expected_event, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    for (;;) {
        if (g_media.last_result < 0 ||
            g_media.state == MEDIA_STATE_COMPLETED ||
            g_media.state == MEDIA_STATE_ERROR) return -1;
        if (g_media.last_event == expected_event) return 0;
        pthread_mutex_unlock(&g_media.lock);
        int rc;
        do { rc = sem_timedwait(&g_media.sem, &ts); } while (rc && errno == EINTR);
        pthread_mutex_lock(&g_media.lock);
        if (rc) return -1;
    }
}

/* Backends may call the callback inline. Never send a command with our
 * callback's mutex held, and never erase confirmation after sending it. */
static int player_start_locked(void)
{
    void *handle = g_media.handle;
    arm_event_locked();
    pthread_mutex_unlock(&g_media.lock);
    int rc = media_player_start(handle);
    pthread_mutex_lock(&g_media.lock);
    return rc;
}

static int player_pause_locked(void)
{
    void *handle = g_media.handle;
    arm_event_locked();
    pthread_mutex_unlock(&g_media.lock);
    int rc = media_player_pause(handle);
    pthread_mutex_lock(&g_media.lock);
    return rc;
}

static int player_seek_locked(unsigned int pos)
{
    void *handle = g_media.handle;
    arm_event_locked();
    pthread_mutex_unlock(&g_media.lock);
    int rc = media_player_seek(handle, pos);
    pthread_mutex_lock(&g_media.lock);
    return rc;
}

/* Preserve the existing playback checkpoint in the backend. On unsupported
 * pause or timeout, close the session rather than let TLS race the stream. */
static int task_suspend_locked(bool resume)
{
    if (!g_media.handle || (g_media.state != MEDIA_STATE_PLAYING &&
                           g_media.state != MEDIA_STATE_PREPARED)) return OK;
    int rc = player_pause_locked();
    if (rc < 0 || wait_event(MEDIA_EVENT_PAUSED, TIMEOUT_PAUSE_MS) != 0) {
        if (g_media.state == MEDIA_STATE_COMPLETED) return OK;
        syslog(LOG_ERR, "[%s] Task pause failed; closing music session\n", TAG);
        force_close_locked();
        return ERROR;
    }
    if (resume) g_media.policy_flags |= MEDIA_TASK_RESUME;
    return OK;
}

int tool_media_music_busy(void)
{
    if (robot_music_media_is_active) return robot_music_media_is_active();
    if (!g_media_initialized) return 0;
    pthread_mutex_lock(&g_media.lock);
    int busy = g_media.stopping || g_media.state == MEDIA_STATE_PLAYING ||
               g_media.state == MEDIA_STATE_PREPARED;
    pthread_mutex_unlock(&g_media.lock);
    return busy;
}

int tool_media_task_begin(void)
{
    if (robot_task_begin && robot_task_end) return robot_task_begin();
    media_ctx_init_once();
    pthread_mutex_lock(&g_media.lock);
    g_media.policy_flags = (g_media.policy_flags & MEDIA_IS_LIVE) | MEDIA_TASK_HELD;
    if (g_media.state == MEDIA_STATE_COMPLETED || g_media.state == MEDIA_STATE_ERROR)
        force_close_locked();
    int rc = task_suspend_locked(g_media.state == MEDIA_STATE_PLAYING);
    pthread_mutex_unlock(&g_media.lock);
    return rc;
}

void tool_media_task_end(int allow_resume)
{
    if (robot_task_begin && robot_task_end) {
        if (!allow_resume && robot_task_end_without_resume)
            robot_task_end_without_resume();
        else
            robot_task_end();
        return;
    }
    if (!g_media_initialized) return;
    pthread_mutex_lock(&g_media.lock);
    bool resume = allow_resume && (g_media.policy_flags & MEDIA_TASK_RESUME) &&
                  g_media.state == MEDIA_STATE_PAUSED && !g_media.exiting;
    g_media.policy_flags &= MEDIA_IS_LIVE;
    if (resume) {
        int rc = player_start_locked();
        if (rc < 0 || wait_event(MEDIA_EVENT_STARTED, TIMEOUT_START_MS) != 0) {
            syslog(LOG_ERR, "[%s] Task resume failed; closing music session\n", TAG);
            force_close_locked();
        }
    }
    pthread_mutex_unlock(&g_media.lock);
}

/**
 * JSON error response helper.
 */

static int respond_error(char* output, size_t output_size,
    const char* code, const char* message)
{
    snprintf(output, output_size,
        "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        code, message);
    return ERROR;
}

/**
 * JSON ok response with state.
 */

static int respond_ok_state(char* output, size_t output_size,
    const char* extra)
{
    if (extra) {
        snprintf(output, output_size,
            "{\"ok\":true,\"state\":\"%s\",%s}",
            state_str(g_media.state), extra);
    } else {
        snprintf(output, output_size,
            "{\"ok\":true,\"state\":\"%s\"}",
            state_str(g_media.state));
    }

    return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * music_play — open + prepare (wait PREPARED) + (seek) + start (wait STARTED).
 */

int tool_music_play_execute(const char* input_json,
    char* output, size_t output_size)
{
    media_ctx_init_once();

    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        return respond_error(output, output_size,
            "EINVAL_ARG", "Invalid JSON input");
    }

    cJSON* j_url = cJSON_GetObjectItem(root, "url");
    if (!j_url || !cJSON_IsString(j_url) || !j_url->valuestring[0]) {
        cJSON_Delete(root);
        return respond_error(output, output_size,
            "EINVAL_ARG", "Missing or empty 'url'");
    }

    cJSON* j_title = cJSON_GetObjectItem(root, "title");
    cJSON* j_artist = cJSON_GetObjectItem(root, "artist");

    /* Build media options string for prepare().
     * For raw PCM files, format info is required because there is no
     * container header for avformat to auto-detect. */

    char options_buf[MEDIA_OPTIONS_MAX] = { 0 };
    const char* options = NULL;

    cJSON* j_opts = cJSON_GetObjectItem(root, "options");
    if (j_opts && cJSON_IsString(j_opts) && j_opts->valuestring[0]) {
        snprintf(options_buf, sizeof(options_buf), "%s",
            j_opts->valuestring);
        options = options_buf;
    } else {
        cJSON* j_fmt = cJSON_GetObjectItem(root, "format");
        cJSON* j_rate = cJSON_GetObjectItem(root, "sample_rate");
        cJSON* j_ch = cJSON_GetObjectItem(root, "channels");

        /* Treat empty strings and zero values as "not provided" so
         * that bogus LLM args like format="" sample_rate=0 channels=0
         * fall through to auto-detection instead of producing an
         * invalid options string. */

        bool has_fmt = j_fmt && cJSON_IsString(j_fmt)
            && j_fmt->valuestring[0];
        bool has_rate = j_rate && cJSON_IsNumber(j_rate)
            && (int)j_rate->valuedouble > 0;
        bool has_ch = j_ch && cJSON_IsNumber(j_ch)
            && (int)j_ch->valuedouble > 0;

        if (has_fmt || has_rate || has_ch) {
            const char* fmt = has_fmt ? j_fmt->valuestring : "s16le";
            int rate = has_rate ? (int)j_rate->valuedouble : 16000;
            int ch = has_ch ? (int)j_ch->valuedouble : 1;
            const char* layout = (ch == 2) ? "stereo" : "mono";

            snprintf(options_buf, sizeof(options_buf),
                "format=%s:sample_rate=%d:ch_layout=%s",
                fmt, rate, layout);
            options = options_buf;
        }
    }

    /* Auto-detect: if URL ends with .pcm and no options given,
     * provide a sensible default (s16le, 16kHz, mono). */

    if (!options) {
        const char* ext = strrchr(j_url->valuestring, '.');
        if (ext && strcasecmp(ext, ".pcm") == 0) {
            snprintf(options_buf, sizeof(options_buf),
                "format=s16le:sample_rate=16000:ch_layout=mono");
            options = options_buf;
            syslog(LOG_INFO,
                "[%s] Auto-detected .pcm, using default options: %s\n",
                TAG, options);
        }
    }

    bool autostart = true;
    cJSON* j_auto = cJSON_GetObjectItem(root, "autostart");
    if (j_auto && cJSON_IsBool(j_auto)) {
        autostart = cJSON_IsTrue(j_auto);
    }

    int start_pos = 0;
    cJSON* j_pos = cJSON_GetObjectItem(root, "start_position_ms");
    if (j_pos && cJSON_IsNumber(j_pos)) {
        start_pos = (int)j_pos->valuedouble;
        if (start_pos < 0) {
            start_pos = 0;
        }
    }

    const char* url = j_url->valuestring;

    pthread_mutex_lock(&g_media.lock);

    if (g_media.exiting) {
        pthread_mutex_unlock(&g_media.lock);
        cJSON_Delete(root);
        return respond_error(output, output_size,
            "EINTERNAL", "Module is shutting down");
    }

    /* Cut-song: force close old session if not idle */

    if (g_media.state != MEDIA_STATE_IDLE) {
        syslog(LOG_INFO, "[%s] Cut-song: closing old session %" PRIu32 "\n",
            TAG, g_media.session_id);
        force_close_locked();
    }

    /* New session — drain stale sem posts from previous session */

    while (sem_trywait(&g_media.sem) == 0)
        ;

    g_media.session_id++;
    uint32_t sid = g_media.session_id;

    char safe_url[128];
    log_url(url, safe_url, sizeof(safe_url));
    syslog(LOG_INFO, "[%s] Play session %" PRIu32 " url=%s options=%s\n",
        TAG, sid, safe_url, options ? options : "(auto)");

    /* Open */

    g_media.handle = media_player_open(MEDIA_STREAM_MUSIC);
    if (!g_media.handle) {
        syslog(LOG_ERR, "[%s] media_player_open failed\n", TAG);
        pthread_mutex_unlock(&g_media.lock);
        cJSON_Delete(root);
        return respond_error(output, output_size,
            "EIO_MEDIA", "media_player_open failed");
    }

    if ((j_title && cJSON_IsString(j_title)) ||
        (j_artist && cJSON_IsString(j_artist))) {
        (void)media_player_set_track_info(
            g_media.handle,
            j_title && cJSON_IsString(j_title) ? j_title->valuestring : "",
            j_artist && cJSON_IsString(j_artist) ? j_artist->valuestring : "");
    }

    /* Set event callback with session_id as cookie */

    pthread_mutex_unlock(&g_media.lock);
    int cb_ret = media_player_set_event_callback(g_media.handle,
        (void*)(uintptr_t)sid, media_event_cb);
    pthread_mutex_lock(&g_media.lock);
    if (cb_ret < 0) {
        syslog(LOG_ERR, "[%s] set_event_callback failed: %d\n", TAG, cb_ret);
        force_close_locked();
        pthread_mutex_unlock(&g_media.lock);
        cJSON_Delete(root);
        return respond_error(output, output_size,
            "EIO_MEDIA", "set_event_callback failed");
    }

    /* Prepare + wait for MEDIA_EVENT_PREPARED */

    arm_event_locked();
    pthread_mutex_unlock(&g_media.lock);
    int ret = media_player_prepare(g_media.handle, url, options);
    pthread_mutex_lock(&g_media.lock);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] media_player_prepare send failed: %d\n",
            TAG, ret);
        force_close_locked();
        pthread_mutex_unlock(&g_media.lock);
        cJSON_Delete(root);
        return respond_error(output, output_size,
            "EIO_MEDIA", "media_player_prepare failed");
    }

    if (wait_event(MEDIA_EVENT_PREPARED, TIMEOUT_PREPARE_MS) != 0) {
        force_close_locked();
        pthread_mutex_unlock(&g_media.lock);
        cJSON_Delete(root);
        return respond_error(output, output_size, "EIO_MEDIA", "Music prepare not confirmed");
    }

    /* State is now PREPARED (set by event callback or forced above) */

    snprintf(g_media.url, sizeof(g_media.url), "%s", url);

    /* Detect live stream for duration_supported semantics */

    if (strncasecmp(url, "rtsp://", 7) == 0 || strncasecmp(url, "rtmp://", 7) == 0)
        g_media.policy_flags |= MEDIA_IS_LIVE;
    else
        g_media.policy_flags &= (unsigned char)~MEDIA_IS_LIVE;

    /* Pre-seek if requested */

    if (start_pos > 0) {
        ret = player_seek_locked((unsigned int)start_pos);
        if (ret >= 0) {
            if (wait_event(MEDIA_EVENT_SEEKED, TIMEOUT_SEEK_MS) != 0) {
                syslog(LOG_WARNING,
                    "[%s] Pre-seek wait failed, continuing\n", TAG);
            } else {
                syslog(LOG_INFO, "[%s] Pre-seek to %d ms OK\n",
                    TAG, start_pos);
            }
        } else {
            syslog(LOG_WARNING,
                "[%s] Pre-seek send failed (%d), will try after start\n",
                TAG, ret);
        }
    }

    /* Start if autostart + wait for MEDIA_EVENT_STARTED */

    if (autostart) {
        ret = player_start_locked();
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] media_player_start send failed: %d\n",
                TAG, ret);
            force_close_locked();
            pthread_mutex_unlock(&g_media.lock);
            cJSON_Delete(root);
            return respond_error(output, output_size,
                "EIO_MEDIA", "media_player_start failed");
        }

        bool staged = robot_music_media_start_pending &&
                      robot_music_media_start_pending(g_media.handle);
        if (staged) {
            g_media.state = MEDIA_STATE_PREPARED;
            syslog(LOG_INFO, "[%s] Music staged until task/reply completes\n", TAG);
        } else if (wait_event(MEDIA_EVENT_STARTED, TIMEOUT_START_MS) != 0) {
            force_close_locked();
            pthread_mutex_unlock(&g_media.lock);
            cJSON_Delete(root);
            return respond_error(output, output_size, "EIO_MEDIA", "Music start not confirmed");
        }

        /* State is now PLAYING (set by event callback or forced above) */

        /* Fallback seek after start if pre-seek was not done */

        if (start_pos > 0) {
            ret = player_seek_locked((unsigned int)start_pos);
            if (ret >= 0) {
                /* Best-effort, don't fail the whole play for this */

                wait_event(MEDIA_EVENT_SEEKED, TIMEOUT_SEEK_MS);
            }
        }
    }

    if (g_media.policy_flags & MEDIA_TASK_HELD) {
        if (task_suspend_locked(autostart) != OK) {
            pthread_mutex_unlock(&g_media.lock);
            cJSON_Delete(root);
            return respond_error(output, output_size, "EIO_MEDIA", "Music could not yield resources");
        }
    }

    /* Build response */

    char extra[256];
    snprintf(extra, sizeof(extra),
        "\"session_id\":%" PRIu32 ",\"url\":\"%s\","
        "\"start_position_ms\":%d,\"resume_pending\":%s",
        sid, url, start_pos,
        ((g_media.policy_flags & MEDIA_TASK_RESUME) ||
         (robot_music_media_start_pending && robot_music_media_start_pending(g_media.handle)))
         ? "true" : "false");

    int rc = respond_ok_state(output, output_size, extra);

    pthread_mutex_unlock(&g_media.lock);
    cJSON_Delete(root);
    return rc;
}

/**
 * music_pause — pause + wait MEDIA_EVENT_PAUSED.
 */

int tool_music_pause_execute(const char* input_json,
    char* output, size_t output_size)
{
    (void)input_json;
    media_ctx_init_once();

    pthread_mutex_lock(&g_media.lock);
    g_media.policy_flags &= (unsigned char)~MEDIA_TASK_RESUME;

    if (g_media.state == MEDIA_STATE_PAUSED) {
        int rc = respond_ok_state(output, output_size, "\"noop\":true");
        pthread_mutex_unlock(&g_media.lock);
        return rc;
    }

    if (g_media.state != MEDIA_STATE_PLAYING &&
        !(g_media.state == MEDIA_STATE_PREPARED && robot_music_media_start_pending &&
          robot_music_media_start_pending(g_media.handle))) {
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EINVAL_STATE",
            "Cannot pause: not playing or staged");
    }

    int ret = player_pause_locked();
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] media_player_pause send failed: %d\n",
            TAG, ret);
        force_close_locked();
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EIO_MEDIA", "media_player_pause failed");
    }

    if (wait_event(MEDIA_EVENT_PAUSED, TIMEOUT_PAUSE_MS) != 0) {
        syslog(LOG_ERR, "[%s] Pause not confirmed\n", TAG);
        force_close_locked();
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EIO_MEDIA",
            "media_player_pause timed out");
    }

    syslog(LOG_INFO, "[%s] Paused session %" PRIu32 "\n",
        TAG, g_media.session_id);

    int rc = respond_ok_state(output, output_size, NULL);
    pthread_mutex_unlock(&g_media.lock);
    return rc;
}

/**
 * music_resume — start + wait MEDIA_EVENT_STARTED.
 */

int tool_music_resume_execute(const char* input_json,
    char* output, size_t output_size)
{
    (void)input_json;
    media_ctx_init_once();

    pthread_mutex_lock(&g_media.lock);

    if (g_media.stopping) {
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EBUSY_MEDIA", "Cannot resume while music is stopping");
    }

    if (g_media.state == MEDIA_STATE_PLAYING) {
        int rc = respond_ok_state(output, output_size, "\"noop\":true");
        pthread_mutex_unlock(&g_media.lock);
        return rc;
    }

    if (g_media.state != MEDIA_STATE_PAUSED && g_media.state != MEDIA_STATE_PREPARED) {
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EINVAL_STATE",
            "Cannot resume: not paused or prepared");
    }

    if (g_media.policy_flags & MEDIA_TASK_HELD) {
        g_media.policy_flags |= MEDIA_TASK_RESUME;
        int rc = respond_ok_state(output, output_size, "\"resume_pending\":true");
        pthread_mutex_unlock(&g_media.lock);
        return rc;
    }

    int ret = player_start_locked();
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] media_player_start (resume) send failed: %d\n",
            TAG, ret);
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EIO_MEDIA", "media_player_start failed");
    }

    if (robot_music_media_start_pending && robot_music_media_start_pending(g_media.handle)) {
        g_media.state = MEDIA_STATE_PREPARED;
        int rc = respond_ok_state(output, output_size, "\"resume_pending\":true");
        pthread_mutex_unlock(&g_media.lock);
        return rc;
    }

    if (wait_event(MEDIA_EVENT_STARTED, TIMEOUT_START_MS) != 0) {
        syslog(LOG_ERR, "[%s] Resume/start not confirmed\n", TAG);
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EIO_MEDIA",
            "media_player_start timed out");
    }

    syslog(LOG_INFO, "[%s] Resumed session %" PRIu32 "\n",
        TAG, g_media.session_id);

    int rc = respond_ok_state(output, output_size, NULL);
    pthread_mutex_unlock(&g_media.lock);
    return rc;
}

/**
 * music_stop — stop + wait MEDIA_EVENT_STOPPED, then close.
 */

int tool_music_stop_execute(const char* input_json,
    char* output, size_t output_size)
{
    (void)input_json;
    media_ctx_init_once();

    pthread_mutex_lock(&g_media.lock);
    g_media.policy_flags &= (unsigned char)~MEDIA_TASK_RESUME;

    if (g_media.state == MEDIA_STATE_IDLE) {
        int rc = respond_ok_state(output, output_size, "\"noop\":true");
        pthread_mutex_unlock(&g_media.lock);
        return rc;
    }

    syslog(LOG_INFO, "[%s] Stop session %" PRIu32 "\n",
        TAG, g_media.session_id);

    /* Set the gate before sending STOP.  Media callbacks can arrive on a
     * separate thread and must not make the session look resumable while the
     * close sequence is draining I2S and releasing its handle. */
    g_media.stopping = true;

    /* Always force close to ensure clean state */

    force_close_locked();

    int rc = respond_ok_state(output, output_size, NULL);
    pthread_mutex_unlock(&g_media.lock);
    return rc;
}

/**
 * music_seek — seek + wait MEDIA_EVENT_SEEKED.
 */

int tool_music_seek_execute(const char* input_json,
    char* output, size_t output_size)
{
    media_ctx_init_once();

    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        return respond_error(output, output_size,
            "EINVAL_ARG", "Invalid JSON input");
    }

    cJSON* j_pos = cJSON_GetObjectItem(root, "position_ms");
    if (!j_pos || !cJSON_IsNumber(j_pos)) {
        cJSON_Delete(root);
        return respond_error(output, output_size,
            "EINVAL_ARG", "Missing 'position_ms'");
    }

    int pos = (int)j_pos->valuedouble;
    if (pos < 0) {
        pos = 0;
    }

    cJSON_Delete(root);

    pthread_mutex_lock(&g_media.lock);

    if (g_media.state != MEDIA_STATE_PREPARED && g_media.state != MEDIA_STATE_PLAYING && g_media.state != MEDIA_STATE_PAUSED) {
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EINVAL_STATE",
            "Cannot seek in current state");
    }

    int ret = player_seek_locked((unsigned int)pos);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] media_player_seek send failed: %d\n",
            TAG, ret);
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EIO_MEDIA", "media_player_seek failed");
    }

    if (wait_event(MEDIA_EVENT_SEEKED, TIMEOUT_SEEK_MS) != 0) {
        syslog(LOG_ERR, "[%s] Seek not confirmed\n", TAG);
        pthread_mutex_unlock(&g_media.lock);
        return respond_error(output, output_size,
            "EIO_MEDIA",
            "media_player_seek timed out");
    }

    syslog(LOG_INFO, "[%s] Seek to %d ms, state=%s\n",
        TAG, pos, state_str(g_media.state));

    char extra[64];
    snprintf(extra, sizeof(extra), "\"position_ms\":%d", pos);

    int rc = respond_ok_state(output, output_size, extra);
    pthread_mutex_unlock(&g_media.lock);
    return rc;
}

/**
 * music_set_volume — maps 0..100 to hardware index via policy API.
 * Policy API is truly synchronous, no event wait needed.
 */

int tool_music_set_volume_execute(const char* input_json,
    char* output, size_t output_size)
{
    media_ctx_init_once();

    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        return respond_error(output, output_size,
            "EINVAL_ARG", "Invalid JSON input");
    }

    cJSON* j_vol = cJSON_GetObjectItem(root, "volume");
    if (!j_vol || !cJSON_IsNumber(j_vol)) {
        cJSON_Delete(root);
        return respond_error(output, output_size,
            "EINVAL_ARG", "Missing 'volume'");
    }

    int vol = (int)j_vol->valuedouble;
    cJSON_Delete(root);

    if (vol < MEDIA_VOL_USER_MIN) {
        syslog(LOG_WARNING, "[%s] Volume %d clamped to %d\n",
            TAG, vol, MEDIA_VOL_USER_MIN);
        vol = MEDIA_VOL_USER_MIN;
    }

    if (vol > MEDIA_VOL_USER_MAX) {
        syslog(LOG_WARNING, "[%s] Volume %d clamped to %d\n",
            TAG, vol, MEDIA_VOL_USER_MAX);
        vol = MEDIA_VOL_USER_MAX;
    }

    int idx = volume_to_index(vol);

    int ret = media_policy_set_stream_volume(MEDIA_STREAM_MUSIC, idx);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] media_policy_set_stream_volume failed: %d\n",
            TAG, ret);
        return respond_error(output, output_size,
            "EIO_MEDIA",
            "media_policy_set_stream_volume failed");
    }

    pthread_mutex_lock(&g_media.lock);
    g_media.volume = vol;
    pthread_mutex_unlock(&g_media.lock);

    syslog(LOG_INFO, "[%s] Volume set to %d (index %d)\n", TAG, vol, idx);

    char extra[64];
    snprintf(extra, sizeof(extra),
        "\"volume\":%d,\"mapped_index\":%d", vol, idx);

    snprintf(output, output_size, "{\"ok\":true,%s}", extra);
    return OK;
}

/**
 * music_status — read-only query, no event wait needed.
 */

int tool_music_status_execute(const char* input_json,
    char* output, size_t output_size)
{
    (void)input_json;
    media_ctx_init_once();

    pthread_mutex_lock(&g_media.lock);

    media_state_e st = g_media.state;
    uint32_t sid = g_media.session_id;
    int vol = g_media.volume;

    unsigned int position_ms = 0;
    unsigned int duration_ms = 0;
    bool duration_supported = !(g_media.policy_flags & MEDIA_IS_LIVE);
    bool duration_ok = false;
    bool position_ok = false;

    void *handle = g_media.handle;
    pthread_mutex_unlock(&g_media.lock);
    if (handle && st != MEDIA_STATE_IDLE && st != MEDIA_STATE_ERROR) {
        position_ok = media_player_get_position(handle, &position_ms) == 0;
        duration_ok = media_player_get_duration(handle, &duration_ms) == 0;
    }
    pthread_mutex_lock(&g_media.lock);
    if (handle != g_media.handle) position_ok = duration_ok = false;

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "state", state_str(st));
    cJSON_AddBoolToObject(resp, "resume_pending", (g_media.policy_flags & MEDIA_TASK_RESUME) != 0 ||
        (robot_music_media_start_pending && robot_music_media_start_pending(g_media.handle)));

    if (st != MEDIA_STATE_IDLE) {
        cJSON_AddStringToObject(resp, "url", g_media.url);
        cJSON_AddNumberToObject(resp, "session_id", sid);
    }

    if (position_ok) {
        cJSON_AddNumberToObject(resp, "position_ms", position_ms);
    }

    if (duration_ok) {
        cJSON_AddNumberToObject(resp, "duration_ms", duration_ms);
    } else {
        cJSON_AddNumberToObject(resp, "duration_ms", -1);
        cJSON_AddBoolToObject(resp, "duration_supported", duration_supported);
        if (st != MEDIA_STATE_IDLE) {
            cJSON_AddStringToObject(resp, "duration_error", "query_failed");
        }
    }

    cJSON_AddNumberToObject(resp, "volume", vol);

    pthread_mutex_unlock(&g_media.lock);

    char* json = cJSON_PrintUnformatted(resp);
    if (json) {
        snprintf(output, output_size, "%s", json);
        free(json);
    } else {
        snprintf(output, output_size, "{\"ok\":true,\"state\":\"%s\"}",
            state_str(st));
    }

    cJSON_Delete(resp);
    return OK;
}

/**
 * Module cleanup — called on program exit.
 */

void tool_media_cleanup(void)
{
    if (!g_media_initialized) {
        return;
    }

    syslog(LOG_INFO, "[%s] Cleanup\n", TAG);

    pthread_mutex_lock(&g_media.lock);
    g_media.exiting = true;
    force_close_locked();
    pthread_mutex_unlock(&g_media.lock);

    sem_destroy(&g_media.sem);
    pthread_mutex_destroy(&g_media.lock);
    g_media_initialized = false;
}

/****************************************************************************
 * Music Search (Netease Cloud Music API)
 ****************************************************************************/

#define SEARCH_RESP_SIZE 16384
#define SEARCH_MAX_RESULTS 5
#define MUSIC_SEARCH_ATTEMPTS 3
#define MUSIC_SEARCH_NET_TIMEOUT_MS 5000

static void music_search_prepare_retry(unsigned int attempt,
    bool force_reconnect)
{
    /* The voice HTTP pool and vela_tls pool are separate. Retire both idle
     * pools before retrying so a request after voice_stop cannot inherit a
     * socket invalidated by the previous Wi-Fi/TLS session. */
    if (attempt > 0) {
        vela_tls_pool_cleanup();
    }

    if (robot_voice_wifi_ensure_connected != NULL) {
        int ret = robot_voice_wifi_ensure_connected(
            MUSIC_SEARCH_NET_TIMEOUT_MS);
        if (ret < 0) {
            syslog(LOG_WARNING,
                "[%s] search network gate attempt=%u rc=%d\n",
                TAG, attempt + 1, ret);
        }
    }

    /* A failed handshake can leave DHCP up while the WLAN/TLS path is stale.
     * Force one saved-credential reconnect only after the first failed try. */
    if (force_reconnect && robot_voice_wifi_reconnect_and_ensure != NULL) {
        int ret = robot_voice_wifi_reconnect_and_ensure(
            MUSIC_SEARCH_NET_TIMEOUT_MS);
        if (ret < 0) {
            syslog(LOG_WARNING,
                "[%s] search forced network recovery rc=%d\n", TAG, ret);
        }
    }
}

static void url_encode(const char* src, char* dst, size_t dstsz)
{
    size_t pos = 0;

    while (*src && pos + 3 < dstsz) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            dst[pos++] = c;
        } else {
            pos += snprintf(dst + pos, dstsz - pos, "%%%02X", c);
        }

        src++;
    }

    dst[pos] = '\0';
}

int tool_music_search_execute(const char* input_json,
    char* output, size_t output_size)
{
    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
        return -1;
    }

    const char* keyword = cJSON_GetStringValue(cJSON_GetObjectItem(root, "keyword"));
    if (!keyword || !keyword[0]) {
        keyword = cJSON_GetStringValue(cJSON_GetObjectItem(root, "query"));
    }
    if (!keyword || !keyword[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing 'keyword'");
        return -1;
    }

    char encoded[256];
    url_encode(keyword, encoded, sizeof(encoded));

    char path[512];
    snprintf(path, sizeof(path),
        "/api/search/get/web?s=%s&type=1&offset=0&total=true&limit=%d",
        encoded, SEARCH_MAX_RESULTS);

    char* resp = malloc(SEARCH_RESP_SIZE);
    if (!resp) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: OOM");
        return -1;
    }

    static const vela_header_t search_headers[] = {
        { "Accept", "application/json" },
        { "Accept-Encoding", "identity" },
        { "Referer", "https://music.163.com/" },
        /* Search responses are small and one-shot. Do not return a possibly
         * truncated body to the shared keep-alive pool. */
        { "Connection", "close" },
        { NULL, NULL },
    };
    int status = VELA_TLS_ERR_READ;
    size_t body_len = 0;
    cJSON* j = NULL;
    bool force_reconnect = false;

    for (unsigned int attempt = 0; attempt < MUSIC_SEARCH_ATTEMPTS;
         attempt++) {
        if (attempt > 0) {
            music_search_prepare_retry(attempt, force_reconnect);
            force_reconnect = false;
            usleep(250 * 1000);
        } else if (robot_voice_wifi_ensure_connected != NULL) {
            music_search_prepare_retry(0, false);
        }

        memset(resp, 0, SEARCH_RESP_SIZE);
        body_len = 0;
        status = vela_https_request("music.163.com", "443", "GET", path,
            search_headers, NULL, 0, resp, SEARCH_RESP_SIZE, &body_len);

        if (status != 200) {
            if (status < 0 && attempt + 1 < MUSIC_SEARCH_ATTEMPTS) {
                force_reconnect = attempt == 0;
                syslog(LOG_WARNING,
                    "[%s] search request attempt=%u failed rc=%d; retrying\n",
                    TAG, attempt + 1, status);
                continue;
            }
            break;
        }

        if (body_len == 0 || resp[0] == '\0') {
            syslog(LOG_WARNING,
                "[%s] search HTTP 200 with empty body attempt=%u\n",
                TAG, attempt + 1);
        } else {
            j = cJSON_Parse(resp);
            if (j != NULL) {
                break;
            }
            syslog(LOG_WARNING,
                "[%s] search JSON parse failed attempt=%u body=%zu; retrying\n",
                TAG, attempt + 1, body_len);
        }

        if (attempt + 1 < MUSIC_SEARCH_ATTEMPTS) {
            /* The response may have been truncated or the pooled stream may
             * contain a partial body. The next attempt must use a new TLS
             * connection instead of the same stream. */
            vela_tls_pool_cleanup();
            continue;
        }
    }
    cJSON_Delete(root);

    if (status != 200) {
        snprintf(output, output_size, "Error: HTTP %d", status);
        free(resp);
        return -1;
    }

    free(resp);
    if (!j) {
        snprintf(output, output_size,
            "Error: bad JSON from server (body=%zu)", body_len);
        return -1;
    }

    cJSON* songs = cJSON_GetObjectItem(
        cJSON_GetObjectItem(j, "result"), "songs");
    if (!songs || !cJSON_IsArray(songs) || cJSON_GetArraySize(songs) == 0) {
        cJSON_Delete(j);
        snprintf(output, output_size, "No results for this keyword");
        return 0;
    }

    int count = cJSON_GetArraySize(songs);
    if (count > SEARCH_MAX_RESULTS) {
        count = SEARCH_MAX_RESULTS;
    }

    /* Use cJSON to build result so song names/artists are properly escaped */
    cJSON* result = cJSON_CreateObject();
    cJSON* songs_arr = cJSON_CreateArray();
    if (!result || !songs_arr) {
        cJSON_Delete(result);
        cJSON_Delete(songs_arr);
        cJSON_Delete(j);
        snprintf(output, output_size, "Error: OOM");
        return -ENOMEM;
    }

    cJSON_AddItemToObject(result, "songs", songs_arr);

    for (int i = 0; i < count; i++) {
        cJSON* song = cJSON_GetArrayItem(songs, i);
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(song, "name"));
        /* NetEase song IDs exceed INT_MAX.  Truncating them produces the
         * sentinel-looking 2147483647 URL and an unusable media stream. */
        long long id = (long long)cJSON_GetNumberValue(
            cJSON_GetObjectItem(song, "id"));

        cJSON* artists = cJSON_GetObjectItem(song, "artists");
        const char* artist = "Unknown";
        if (artists && cJSON_GetArraySize(artists) > 0) {
            const char* a = cJSON_GetStringValue(
                cJSON_GetObjectItem(cJSON_GetArrayItem(artists, 0), "name"));
            if (a) {
                artist = a;
            }
        }

        cJSON* song_obj = cJSON_CreateObject();
        if (!song_obj) {
            cJSON_Delete(result);
            cJSON_Delete(j);
            snprintf(output, output_size, "Error: OOM");
            return -ENOMEM;
        }

        char url[128];
        snprintf(url, sizeof(url),
            "https://music.163.com/song/media/outer/url?id=%lld.mp3", id);
        if (!cJSON_AddStringToObject(song_obj, "name", name ? name : "") ||
            !cJSON_AddStringToObject(song_obj, "artist", artist) ||
            !cJSON_AddStringToObject(song_obj, "url", url)) {
            cJSON_Delete(song_obj);
            cJSON_Delete(result);
            cJSON_Delete(j);
            snprintf(output, output_size, "Error: OOM");
            return -ENOMEM;
        }

        cJSON_AddItemToArray(songs_arr, song_obj);
    }

    cJSON_AddStringToObject(result, "action",
        "call music_play with one of the URLs above");

    char* result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    cJSON_Delete(j);

    if (!result_str) {
        snprintf(output, output_size, "Error: OOM building result");
        return -ENOMEM;
    }

    strlcpy(output, result_str, output_size);
    free(result_str);

    syslog(LOG_INFO, "[%s] Found %d songs for search\n", TAG, count);
    return 0;
}
