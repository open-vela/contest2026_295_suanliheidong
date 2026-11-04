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

#include "core/agent_trace.h"

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>

static const char* TAG = "trace";

static uint64_t trace_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000ULL
        + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint64_t trace_elapsed_ms(uint64_t start, uint64_t end)
{
    return start != 0 && end != 0 && end >= start ? end - start : 0;
}

static const char* status_str(int status)
{
    switch (status) {
    case AGENT_TRACE_RUNNING:
        return "running";
    case AGENT_TRACE_OK:
        return "ok";
    case AGENT_TRACE_FAIL:
        return "fail";
    case AGENT_TRACE_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

void agent_trace_begin(agent_trace_t* t, const char* chat_id,
    const char* channel)
{
    uint64_t mono_ms;

    memset(t, 0, sizeof(*t));
    mono_ms = trace_monotonic_ms();
    t->start_mono_ms = mono_ms;

    /* Generate run_id: lower 32 bits of time + rand for uniqueness */
    snprintf(t->run_id, sizeof(t->run_id), "%08x%08x",
        (unsigned)(mono_ms & 0xFFFFFFFF),
        (unsigned)(rand() & 0xFFFFFFFF));

    if (chat_id) {
        strncpy(t->chat_id, chat_id, sizeof(t->chat_id) - 1);
    }
    if (channel) {
        strncpy(t->channel, channel, sizeof(t->channel) - 1);
    }

    t->status = AGENT_TRACE_RUNNING;
    t->backend_idx = -1;

    syslog(LOG_INFO,
        "[%s:%s] BEGIN chat=%s chan=%s",
        TAG, t->run_id, t->chat_id, t->channel);
}

void agent_trace_step(agent_trace_t* t, int iteration,
    const char* tool_name, uint64_t latency_ms,
    int llm_ok)
{
    t->iteration = iteration;
    t->total_latency_ms += latency_ms;
    if (tool_name && tool_name[0]) {
        t->total_tool_calls++;
    }

    syslog(LOG_INFO,
        "[%s:%s] iter=%d tool=%s latency=%" PRIu64 "ms llm=%s backend=%d",
        TAG, t->run_id, iteration,
        (tool_name && tool_name[0]) ? tool_name : "(none)",
        latency_ms,
        llm_ok ? "ok" : "fail",
        t->backend_idx);
}

void agent_trace_end(agent_trace_t* t, int status)
{
    uint64_t end_mono_ms = trace_monotonic_ms();
    t->status = status;

    uint64_t elapsed = trace_elapsed_ms(t->start_mono_ms, end_mono_ms);

    syslog(LOG_INFO,
        "[%s:%s] END status=%s iters=%d tools=%d "
        "llm_ms=%" PRIu64 " elapsed=%" PRIu64 "ms backend=%d",
        TAG, t->run_id, status_str(status),
        t->iteration + 1, t->total_tool_calls,
        t->total_latency_ms, elapsed,
        t->backend_idx);
}
