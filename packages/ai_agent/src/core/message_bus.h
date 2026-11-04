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

#pragma once
/**
 * message_bus.h — inter-task message bus
 *
 * with POSIX mutex + condition variable + ring buffer.
 */

#include "agent_compat.h"
#include <stdint.h>

/* Per-message delivery policy.  Zero keeps the default channel behavior. */
#define AGENT_MSG_FLAG_NO_TTS (1u << 0)
/* Contest-local music companion events are already handled by the media
 * runtime.  The Agent must not run keyword fast paths or LLM tools on their
 * embedded Skill text. */
#define AGENT_MSG_FLAG_INTERNAL_MUSIC (1u << 1)
/* Scheduling policy, carried in the existing flags word. */
#define AGENT_MSG_FLAG_DEFER_MUSIC (1u << 2)
#define AGENT_MSG_FLAG_MUSIC_PAUSE (1u << 3)
#define AGENT_MSG_FLAG_MUSIC_STOP (1u << 4)
#define AGENT_MSG_FLAG_TASK_REPLY (1u << 5)
#define AGENT_MSG_FLAG_NOTIFICATION (1u << 6)
#define AGENT_MSG_FLAG_TOOL_JOB (1u << 7)
#define AGENT_MSG_FLAG_FEISHU_IMAGE (1u << 8)
/* Non-speaking progress update within the current held task. */
#define AGENT_MSG_FLAG_TASK_STATUS (1u << 9)

/** A single message travelling on the bus. */
typedef struct {
    char  channel[16];   /**< "feishu", "websocket", "cli", "system" */
    char  chat_id[64];   /**< chat_id (Feishu IDs are ~36 chars) */
    char *content;       /**< Heap-allocated text; receiver must free. */
    char *image_b64;     /**< Optional base64-encoded image; receiver must free. NULL if none. */
    uint32_t flags;      /**< AGENT_MSG_FLAG_* delivery policy bits. */
} agent_msg_t;

/** Free heap members (content, image_b64) of a message.
 *  Safe to call on a zeroed or already-freed message. */
void message_bus_msg_free(agent_msg_t *msg);

/** Initialise inbound and outbound queues. */
int message_bus_init(void);

/** Destroy queues and release synchronisation resources. */
void message_bus_destroy(void);

/** Wake all threads blocked on pop (used during shutdown). */
void message_bus_wakeup(void);

/** Push a message to the inbound queue (towards the agent loop).
 *  On success the bus takes ownership of heap members.
 *  On failure (queue full / timeout) the caller still owns them. */
int message_bus_push_inbound(const agent_msg_t *msg);

/** Block until an inbound message is available (or timeout expires).
 *  Caller must free msg->content / msg->image_b64 when done. */
int message_bus_pop_inbound(agent_msg_t *msg, uint32_t timeout_ms);

/** Push a message to the outbound queue (towards channels).
 *  On success the bus takes ownership of heap members.
 *  On failure the caller still owns them. */
int message_bus_push_outbound(const agent_msg_t *msg);

/** Block until an outbound message is available (or timeout expires). */
int message_bus_pop_outbound(agent_msg_t *msg, uint32_t timeout_ms);

/* Exact control commands only; never infer destructive actions from substrings. */
uint32_t message_bus_music_control_flags(const char *text);
/* Agent-only reply: ownership transfers on OK, including shutdown after enqueue.
 * Waits for channel delivery AND synchronous TTS, using existing queue condvars. */
int message_bus_reply_and_wait(agent_msg_t *msg);
void message_bus_reply_complete(const agent_msg_t *msg);

int message_bus_music_control_pending(void);

bool agent_scheduler_can_resume_music(void);
