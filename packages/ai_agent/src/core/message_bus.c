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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "core/message_bus.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "tools/tool_media.h"
#include <strings.h>

static const char *TAG = "bus";

typedef struct {
    agent_msg_t items[AGENT_BUS_QUEUE_LEN];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} msg_queue_t;

static msg_queue_t g_inbound_queue;
static msg_queue_t g_outbound_queue;
/* Reuses the old initialization byte; no new synchronization object. */
#define BUS_INITIALIZED 1u
#define BUS_REPLY_PENDING 2u
static unsigned char g_bus_state;
static bool g_bus_shutdown;

static unsigned char bus_state(void)
{ return __atomic_load_n(&g_bus_state, __ATOMIC_ACQUIRE); }
static bool bus_shutting_down(void)
{ return __atomic_load_n(&g_bus_shutdown, __ATOMIC_ACQUIRE); }

static void deadline_after(struct timespec *ts, uint32_t ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

uint32_t message_bus_music_control_flags(const char *text)
{
    if (!text) return 0;
    while (*text && (unsigned char)*text <= ' ') text++;
    const char *metadata = strstr(text, "\n[mentioned_users: ");
    size_t len = metadata ? (size_t)(metadata - text) : strlen(text);
    while (len && ((unsigned char)text[len - 1] <= ' ' ||
                  text[len - 1] == '!' || text[len - 1] == '.')) len--;
    /* Chinese sentence punctuation, without a temporary text buffer. */
    while (len >= 3 && (!memcmp(text + len - 3, "。", 3) ||
                        !memcmp(text + len - 3, "！", 3))) len -= 3;
#define COMMAND(s) (len == sizeof(s) - 1 && !strncasecmp(text, s, len))
    if (COMMAND("暂停音乐") || COMMAND("暂停播放") ||
        COMMAND("暂停") || COMMAND("pause music")) return AGENT_MSG_FLAG_MUSIC_PAUSE;
    if (COMMAND("停止音乐") || COMMAND("停止播放") ||
        COMMAND("停止") || COMMAND("stop music")) return AGENT_MSG_FLAG_MUSIC_STOP;
#undef COMMAND
    return 0;
}

static void msg_queue_init(msg_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void message_bus_msg_free(agent_msg_t *msg)
{
    if (!msg) return;
    free(msg->content);
    free(msg->image_b64);
    msg->content = NULL;
    msg->image_b64 = NULL;
}

static void msg_queue_destroy(msg_queue_t *q)
{
    for (int i = 0; i < q->count; i++)
        message_bus_msg_free(&q->items[(q->head + i) % AGENT_BUS_QUEUE_LEN]);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->lock);
}

static int msg_queue_push(msg_queue_t *q, const agent_msg_t *msg,
                          uint32_t timeout_ms)
{
    struct timespec ts;
    bool inbound = q == &g_inbound_queue;
    bool urgent = (msg->flags & (AGENT_MSG_FLAG_MUSIC_PAUSE |
                                AGENT_MSG_FLAG_MUSIC_STOP)) != 0;
    /* Reserve an existing slot for controls; never allocate overflow nodes. */
    int limit = AGENT_BUS_QUEUE_LEN - (inbound && !urgent ? 1 : 0);
    if (timeout_ms) deadline_after(&ts, timeout_ms);
    pthread_mutex_lock(&q->lock);
    while (q->count >= limit && !bus_shutting_down()) {
        if (!timeout_ms || pthread_cond_timedwait(&q->not_full, &q->lock, &ts)) {
            pthread_mutex_unlock(&q->lock);
            syslog(LOG_WARNING, "[%s] Queue full; producer retains message\n", TAG);
            return ERROR;
        }
    }
    if (bus_shutting_down()) {
        pthread_mutex_unlock(&q->lock);
        return ERROR;
    }
    q->items[q->tail] = *msg;
    q->tail = (q->tail + 1) % AGENT_BUS_QUEUE_LEN;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return OK;
}

static int msg_queue_pop(msg_queue_t *q, agent_msg_t *msg, uint32_t timeout_ms)
{
    struct timespec deadline, poll;
    bool inbound = q == &g_inbound_queue;
    if (timeout_ms != UINT32_MAX) deadline_after(&deadline, timeout_ms);
    for (;;) {
        /* Never take the media lock while holding a bus lock: media callbacks
         * can publish messages. Busy is rechecked at each scheduling point. */
        bool busy = inbound && tool_media_music_busy();
        pthread_mutex_lock(&q->lock);
        if (bus_shutting_down()) {
            pthread_mutex_unlock(&q->lock);
            return ERROR;
        }
        int selected = -1;
        for (int i = 0; i < q->count; i++) {
            const agent_msg_t *m = &q->items[(q->head + i) % AGENT_BUS_QUEUE_LEN];
            if (busy && (m->flags & AGENT_MSG_FLAG_DEFER_MUSIC) &&
                !(m->flags & (AGENT_MSG_FLAG_MUSIC_PAUSE | AGENT_MSG_FLAG_MUSIC_STOP))) continue;
            if (selected < 0) selected = i;
            if (inbound && (m->flags & (AGENT_MSG_FLAG_MUSIC_PAUSE |
                                       AGENT_MSG_FLAG_MUSIC_STOP))) {
                selected = i;
                break;
            }
            if (!inbound) break;
        }
        if (selected >= 0) {
            *msg = q->items[(q->head + selected) % AGENT_BUS_QUEUE_LEN];
            for (int i = selected; i + 1 < q->count; i++)
                q->items[(q->head + i) % AGENT_BUS_QUEUE_LEN] =
                    q->items[(q->head + i + 1) % AGENT_BUS_QUEUE_LEN];
            q->tail = (q->tail + AGENT_BUS_QUEUE_LEN - 1) % AGENT_BUS_QUEUE_LEN;
            q->count--;
            pthread_cond_broadcast(&q->not_full);
            pthread_mutex_unlock(&q->lock);
            return OK;
        }
        if (!timeout_ms) {
            pthread_mutex_unlock(&q->lock);
            return ERROR;
        }
        if (timeout_ms != UINT32_MAX) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                pthread_mutex_unlock(&q->lock);
                return ERROR;
            }
        }
        /* EOF need not create another event/message just to release deferred
         * work. Poll using the existing condition variable, no timer task. */
        deadline_after(&poll, 250);
        if (timeout_ms != UINT32_MAX &&
            (poll.tv_sec > deadline.tv_sec ||
             (poll.tv_sec == deadline.tv_sec && poll.tv_nsec > deadline.tv_nsec)))
            poll = deadline;
        pthread_cond_timedwait(&q->not_empty, &q->lock, &poll);
        pthread_mutex_unlock(&q->lock);
    }
}

int message_bus_init(void)
{
    if (bus_state() & BUS_INITIALIZED) return OK;
    msg_queue_init(&g_inbound_queue);
    msg_queue_init(&g_outbound_queue);
    __atomic_store_n(&g_bus_shutdown, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_bus_state, BUS_INITIALIZED, __ATOMIC_RELEASE);
    return OK;
}

void message_bus_destroy(void)
{
    if (!(bus_state() & BUS_INITIALIZED)) return;
    msg_queue_destroy(&g_inbound_queue);
    msg_queue_destroy(&g_outbound_queue);
    __atomic_store_n(&g_bus_state, 0, __ATOMIC_RELEASE);
}

void message_bus_wakeup(void)
{
    __atomic_store_n(&g_bus_shutdown, true, __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_inbound_queue.lock);
    pthread_cond_broadcast(&g_inbound_queue.not_empty);
    pthread_cond_broadcast(&g_inbound_queue.not_full);
    pthread_mutex_unlock(&g_inbound_queue.lock);
    pthread_mutex_lock(&g_outbound_queue.lock);
    pthread_cond_broadcast(&g_outbound_queue.not_empty);
    pthread_cond_broadcast(&g_outbound_queue.not_full);
    pthread_mutex_unlock(&g_outbound_queue.lock);
}

int message_bus_push_inbound(const agent_msg_t *msg)
{
    if (!msg) return ERROR;
    agent_msg_t routed = *msg;
    if (!(routed.flags & (AGENT_MSG_FLAG_INTERNAL_MUSIC |
                         AGENT_MSG_FLAG_NOTIFICATION | AGENT_MSG_FLAG_TOOL_JOB |
                         AGENT_MSG_FLAG_FEISHU_IMAGE)))
        routed.flags |= message_bus_music_control_flags(msg->content);
    return msg_queue_push(&g_inbound_queue, &routed,
        strcmp(msg->channel, AGENT_CHAN_FEISHU) == 0 ||
        (msg->flags & (AGENT_MSG_FLAG_INTERNAL_MUSIC | AGENT_MSG_FLAG_TOOL_JOB |
                       AGENT_MSG_FLAG_DEFER_MUSIC)) ? 0 : AGENT_BUS_PUSH_TIMEOUT_MS);
}

int message_bus_pop_inbound(agent_msg_t *msg, uint32_t timeout_ms)
{
    return msg ? msg_queue_pop(&g_inbound_queue, msg, timeout_ms) : ERROR;
}

int message_bus_push_outbound(const agent_msg_t *msg)
{
    if (!msg) return ERROR;
    if (!(msg->flags & (AGENT_MSG_FLAG_TASK_REPLY | AGENT_MSG_FLAG_TASK_STATUS))) {
        /* Background notifications also pass through the task scheduler,
         * instead of starting independent TTS while music owns I2S. */
        agent_msg_t routed = *msg;
        routed.flags |= AGENT_MSG_FLAG_NOTIFICATION | AGENT_MSG_FLAG_DEFER_MUSIC;
        return message_bus_push_inbound(&routed);
    }
    return msg_queue_push(&g_outbound_queue, msg, AGENT_BUS_PUSH_TIMEOUT_MS);
}

int message_bus_pop_outbound(agent_msg_t *msg, uint32_t timeout_ms)
{
    return msg ? msg_queue_pop(&g_outbound_queue, msg, timeout_ms) : ERROR;
}

int message_bus_reply_and_wait(agent_msg_t *msg)
{
    msg->flags |= AGENT_MSG_FLAG_TASK_REPLY;
    pthread_mutex_lock(&g_outbound_queue.lock);
    __atomic_fetch_or(&g_bus_state, BUS_REPLY_PENDING, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_outbound_queue.lock);
    if (message_bus_push_outbound(msg) != OK) {
        message_bus_reply_complete(msg);
        return ERROR;
    }
    pthread_mutex_lock(&g_outbound_queue.lock);
    while ((bus_state() & BUS_REPLY_PENDING) && !bus_shutting_down())
        pthread_cond_wait(&g_outbound_queue.not_full, &g_outbound_queue.lock);
    pthread_mutex_unlock(&g_outbound_queue.lock);
    /* Once enqueued the caller must not free the message, even on shutdown. */
    return OK;
}

void message_bus_reply_complete(const agent_msg_t *msg)
{
    if (!(msg->flags & AGENT_MSG_FLAG_TASK_REPLY)) return;
    pthread_mutex_lock(&g_outbound_queue.lock);
    __atomic_fetch_and(&g_bus_state, (unsigned char)~BUS_REPLY_PENDING, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_outbound_queue.not_full);
    pthread_mutex_unlock(&g_outbound_queue.lock);
}

int message_bus_music_control_pending(void)
{
    int pending = 0;
    pthread_mutex_lock(&g_inbound_queue.lock);
    for (int i = 0; i < g_inbound_queue.count; i++) {
        if (g_inbound_queue.items[(g_inbound_queue.head + i) % AGENT_BUS_QUEUE_LEN].flags &
            (AGENT_MSG_FLAG_MUSIC_PAUSE | AGENT_MSG_FLAG_MUSIC_STOP)) {
            pending = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_inbound_queue.lock);
    return pending;
}

/* Called by the shared runtime without media/voice/transport locks held.
 * Skip a resume between queued foreground tasks. Internal music policy events
 * are local/non-speaking and can run while the song owns network and I2S. */
bool agent_scheduler_can_resume_music(void)
{
    if (bus_shutting_down()) return false;
    /* Standalone contest/CLI calls have no Agent queue to drain. */
    if (!(bus_state() & BUS_INITIALIZED)) return true;
    bool ready = true;
    pthread_mutex_lock(&g_inbound_queue.lock);
    for (int i = 0; ready && i < g_inbound_queue.count; i++) {
        uint32_t flags = g_inbound_queue.items[
            (g_inbound_queue.head + i) % AGENT_BUS_QUEUE_LEN].flags;
        if (!(flags & (AGENT_MSG_FLAG_DEFER_MUSIC | AGENT_MSG_FLAG_INTERNAL_MUSIC)) ||
            (flags & (AGENT_MSG_FLAG_MUSIC_PAUSE | AGENT_MSG_FLAG_MUSIC_STOP))) ready = false;
    }
    pthread_mutex_unlock(&g_inbound_queue.lock);
    pthread_mutex_lock(&g_outbound_queue.lock);
    if (bus_state() & BUS_REPLY_PENDING) ready = false;
    pthread_mutex_unlock(&g_outbound_queue.lock);
    return ready && !bus_shutting_down();
}
