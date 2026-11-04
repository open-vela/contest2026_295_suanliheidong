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

#include <inttypes.h>

#include "core/agent_loop.h"
#include "ui/agent_oled.h"
#include "core/agent_mem.h"
#include "core/agent_trace.h"
#include "core/context_builder.h"
#include "core/message_bus.h"
#include "core/session_mgr.h"
#include "llm/llm_cache.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "tools/skill_loader.h"
#include "tools/tool_guard.h"
#include "tools/tool_registry.h"
#include "tools/tool_media.h"
#ifdef CONFIG_AI_AGENT_FEISHU
#include "channels/feishu_bot.h"
#endif
#include "agent_compat.h"
#include "agent_config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"

static const char* TAG = "agent";

/* The contest voice module owns this optional synchronous notice path.  Keep
 * the Agent package usable when the voice module is not linked. */
extern int robot_voice_speak_notice(const char *text) __attribute__((weak));
extern int voice_channel_init(void) __attribute__((weak));
extern int robot_proactive_apply_music_skill(const char *) __attribute__((weak));

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define TOOL_OUTPUT_SIZE_MIN (2 * 1024)

/* ── Forward declarations ──────────────────────────────────── */

static char* handle_slash_note(const agent_msg_t* msg);
static char* handle_slash_remind(const agent_msg_t* msg);
static bool llm_call_timed_out(uint64_t latency_ms);

static bool llm_watchdog_expired(uint64_t latency_ms, int err)
{
    /* Preserve HTTP/transport failures; watchdog only successful calls. */
    return err == OK && llm_call_timed_out(latency_ms);
}

/* ── Timeout message constants ─────────────────────────────── */

#define LLM_TIMEOUT_MSG \
    "请求超时，LLM 响应时间过长。请稍后重试，或尝试简化你的问题。"
#define LLM_TIMEOUT_TASK_COMPLETE_MSG \
    "任务已完成，但生成确认消息超时。"

/* ── Clock-safe elapsed time calculation ───────────────────── */

static inline uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000ULL
        + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static inline uint64_t monotonic_elapsed_ms(uint64_t start, uint64_t end)
{
    return start != 0 && end != 0 && end >= start ? end - start : 0;
}

/* Build OpenAI-format assistant message with tool_calls at the top level */
static void add_assistant_message(cJSON* messages, const llm_response_t* resp)
{
    cJSON* asst_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(asst_msg, "role", "assistant");

    if (resp->text && resp->text_len > 0) {
        cJSON_AddStringToObject(asst_msg, "content", resp->text);
    } else {
        cJSON_AddNullToObject(asst_msg, "content");
    }

    /* Kimi thinking mode: echo back reasoning_content or the API returns 400 */
    if (resp->reasoning_content && resp->reasoning_content[0]) {
        cJSON_AddStringToObject(asst_msg, "reasoning_content",
            resp->reasoning_content);
    }

    if (resp->call_count > 0) {
        cJSON* tool_calls = cJSON_CreateArray();
        for (int i = 0; i < resp->call_count; i++) {
            const llm_tool_call_t* call = &resp->calls[i];
            cJSON* tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", call->id);
            cJSON_AddStringToObject(tc, "type", "function");

            cJSON* func_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(func_obj, "name", call->name);
            cJSON_AddStringToObject(func_obj, "arguments",
                call->input ? call->input : "{}");
            cJSON_AddItemToObject(tc, "function", func_obj);
            cJSON_AddItemToArray(tool_calls, tc);
        }
        cJSON_AddItemToObject(asst_msg, "tool_calls", tool_calls);
    }

    cJSON_AddItemToArray(messages, asst_msg);
}

/* Auto-inject channel/chat_id into cron_add input JSON. */
static char* inject_cron_context(const char* tool_name,
    const char* input_json, const char* channel, const char* chat_id)
{
    if (strcmp(tool_name, "cron_add") != 0) {
        return NULL;
    }
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) {
        return NULL;
    }

    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        return NULL;
    }

    cJSON_DeleteItemFromObject(root, "channel");
    cJSON_DeleteItemFromObject(root, "chat_id");
    cJSON_AddStringToObject(root, "channel", channel);
    cJSON_AddStringToObject(root, "chat_id", chat_id);

    char* patched = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return patched;
}

/* One scheduler executes tools in declared order. Reuse tool_output instead
 * of allocating per-tool outputs and temporary worker stacks. */
static void add_tool_result_messages(cJSON* messages,
    const llm_response_t* resp, char* tool_output,
    size_t tool_output_size, const char* msg_channel,
    const char* msg_chat_id)
{
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        bool duplicate = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(resp->calls[j].id, call->id) == 0) duplicate = true;
        }
        if (duplicate) continue;
        tool_output[0] = '\0';
        char *patched = inject_cron_context(call->name, call->input,
                                           msg_channel, msg_chat_id);
        agent_tool_exec_streamed(call->name, patched ? patched : call->input,
                                 tool_output, tool_output_size);
        free(patched);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "role", "tool");
        cJSON_AddStringToObject(result, "tool_call_id", call->id);
        cJSON_AddStringToObject(result, "content", tool_output);
        cJSON_AddItemToArray(messages, result);
    }
}

/* ── Fast path counters ─────────────────────────────────── */

static int s_fast_path_count = 0;
static int s_llm_path_count = 0;

/* ── Natural language fast path (keyword → direct tool call) ── */

static bool contains_any(const char* text, const char** keywords)
{
    for (int i = 0; keywords[i]; i++) {
        if (strcasestr(text, keywords[i])) {
            return true;
        }
    }
    return false;
}

static bool is_external_channel_action(const char* text)
{
    static const char* channel_keywords[] = { "飞书", "feishu", NULL };
    static const char* action_keywords[] = {
        "发消息", "发到群", "发到", "发给", "发至", "告诉大家",
        "发送", "send", "message", NULL
    };

    return contains_any(text, channel_keywords) &&
        contains_any(text, action_keywords);
}

static bool is_external_time_action(const char* text)
{
    static const char* time_keywords[] = {
        "这个时间", "当前时间", "现在时间", "现在是几点", "几点钟",
        "几点", "current time", "what time", NULL
    };

    return is_external_channel_action(text) &&
        contains_any(text, time_keywords);
}

static char* handle_external_time_action(void)
{
    char time_text[256] = { 0 };
    int time_rc = tool_registry_execute("get_current_time", "{}",
        time_text, sizeof(time_text));
    if (time_rc != OK || !time_text[0] || strncmp(time_text, "Error:", 6) == 0) {
        return strdup("获取当前时间失败，未发送到飞书。");
    }

    cJSON* send_obj = cJSON_CreateObject();
    if (!send_obj) {
        return strdup("已获取时间，但准备飞书消息失败。");
    }
    cJSON_AddStringToObject(send_obj, "text", time_text);
    char* send_json = cJSON_PrintUnformatted(send_obj);
    cJSON_Delete(send_obj);
    if (!send_json) {
        return strdup("已获取时间，但准备飞书消息失败。");
    }

    char send_result[512] = { 0 };
    int send_rc = tool_registry_execute("feishu_send_text", send_json,
        send_result, sizeof(send_result));
    free(send_json);

    if (send_rc != OK) {
        syslog(LOG_WARNING,
            "[%s] direct Feishu time send failed: %.160s\n",
            TAG, send_result);
        char* reply = calloc(1, 768);
        if (reply) {
            snprintf(reply, 768, "当前时间是%s，但发送到飞书失败：%s",
                time_text, send_result[0] ? send_result : "未知错误");
        }
        return reply;
    }

    syslog(LOG_INFO, "[%s] direct Feishu time message sent\n", TAG);
    char* reply = calloc(1, 384);
    if (reply) {
        snprintf(reply, 384, "当前时间是%s，已发送到飞书群。", time_text);
    }
    return reply;
}

/* Execute unambiguous Feishu side effects without making them depend on an
 * otherwise unrelated LLM/TLS round trip.  General Feishu conversation still
 * follows the normal ReAct path below. */
static const char* feishu_motion_direction(const char* text)
{
    bool has_motion = strstr(text, "走") != NULL
        || strstr(text, "前进") != NULL
        || strstr(text, "后退") != NULL
        || strstr(text, "左转") != NULL
        || strstr(text, "右转") != NULL
        || strstr(text, "移动") != NULL
        || strstr(text, "一步") != NULL
        || strcasestr(text, "move") != NULL
        || strcasestr(text, "walk") != NULL
        || strcasestr(text, "step") != NULL;

    if (!has_motion) {
        return NULL;
    }

    if (strstr(text, "向前") != NULL || strstr(text, "往前") != NULL
        || strstr(text, "前进") != NULL || strcasestr(text, "forward") != NULL) {
        return "forward";
    }
    if (strstr(text, "向后") != NULL || strstr(text, "往后") != NULL
        || strstr(text, "后退") != NULL || strcasestr(text, "backward") != NULL) {
        return "backward";
    }
    if (strstr(text, "向左") != NULL || strstr(text, "往左") != NULL
        || strstr(text, "左转") != NULL || strcasestr(text, "left") != NULL) {
        return "left";
    }
    if (strstr(text, "向右") != NULL || strstr(text, "往右") != NULL
        || strstr(text, "右转") != NULL || strcasestr(text, "right") != NULL) {
        return "right";
    }

    return NULL;
}

static unsigned int feishu_music_stop_seconds(const char* text)
{
    if (strstr(text, "十秒") != NULL) {
        return 10;
    }

    for (const char* p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            unsigned int value = 0;
            const char* q = p;

            while (*q >= '0' && *q <= '9') {
                value = value * 10U + (unsigned int)(*q - '0');
                if (value > 3600U) {
                    value = 3600U;
                }
                q++;
            }

            if ((*q == 's' || *q == 'S' || strstr(q, "秒") == q)
                && value > 0) {
                return value;
            }
            p = q;
        }
    }

    return 0;
}

static void feishu_music_keyword(const char* text, char* keyword,
    size_t keyword_size)
{
    const char* p = strstr(text, "播放");
    const char* end;
    size_t length;

    if (p != NULL) {
        p += strlen("播放");
    } else {
        p = strcasestr(text, "play ");
        if (p != NULL) {
            p += 5;
        }
    }

    while (p != NULL && *p == ' ') {
        p++;
    }

    if (p == NULL || *p == '\0' || strncmp(p, "一首", strlen("一首")) == 0
        || strncmp(p, "音乐", strlen("音乐")) == 0
        || strncmp(p, "歌曲", strlen("歌曲")) == 0
        || strncmp(p, "歌曲", strlen("歌曲")) == 0) {
        snprintf(keyword, keyword_size, "音乐");
        return;
    }

    end = p + strlen(p);
    const char* hit = strstr(p, "，");
    if (hit != NULL && hit < end) end = hit;
    hit = strstr(p, "。");
    if (hit != NULL && hit < end) end = hit;
    hit = strpbrk(p, ",.;");
    if (hit != NULL && hit < end) end = hit;
    hit = strstr(p, "；");
    if (hit != NULL && hit < end) end = hit;
    hit = strstr(p, "然后");
    if (hit != NULL && hit < end) end = hit;
    hit = strstr(p, "并在");
    if (hit != NULL && hit < end) end = hit;
    hit = strstr(p, "之后");
    if (hit != NULL && hit < end) end = hit;
    hit = strstr(p, "后关闭");
    if (hit != NULL && hit < end) end = hit;
    hit = strstr(p, "后停止");
    if (hit != NULL && hit < end) end = hit;

    length = (size_t)(end - p);
    if (length >= keyword_size) {
        length = keyword_size - 1;
    }
    memcpy(keyword, p, length);
    keyword[length] = '\0';

    while (length > 0 && keyword[length - 1] == ' ') {
        keyword[--length] = '\0';
    }
    if (length == 0 || strcmp(keyword, "歌") == 0) {
        snprintf(keyword, keyword_size, "音乐");
    }
}

static void motion_speak_notice(const char* text, const char* phase)
{
    int rc;

    if (robot_voice_speak_notice == NULL) {
        return;
    }

    rc = robot_voice_speak_notice(text);
    if (rc < 0) {
        syslog(LOG_WARNING,
            "[%s] motion %s notice skipped rc=%d\n",
            TAG, phase, rc);
    }
}

/* Explicit physical movement requests should not depend on an LLM deciding
 * to emit a tool call.  Voice and Feishu both use this bounded path so a
 * phrase such as "向前一步走" always reaches the same guarded motion tool. */
static char* handle_motion_fast_path(const char* text)
{
    const char* direction = feishu_motion_direction(text);
    char result[512] = { 0 };
    char input[128];
    char notice[96];
    const char* motion_name;
    int rc;

    if (direction == NULL) {
        return NULL;
    }

    motion_name = strcmp(direction, "forward") == 0 ? "向前"
        : strcmp(direction, "backward") == 0 ? "向后"
        : strcmp(direction, "left") == 0 ? "向左" : "向右";
    snprintf(notice, sizeof(notice), "正在执行%s一步", motion_name);
    motion_speak_notice(notice, "start");

    snprintf(input, sizeof(input),
        "{\"direction\":\"%s\",\"steps\":1}", direction);
    rc = tool_registry_execute("robot_move", input, result, sizeof(result));
    if (rc != OK && strstr(result, "unknown tool 'robot_move'") != NULL &&
        voice_channel_init != NULL) {
        /* The contest voice application owns this provider.  Its initializer
         * is idempotent, so cover a first-message registration race. */
        int init_rc = voice_channel_init();
        if (init_rc == OK) {
            result[0] = '\0';
            rc = tool_registry_execute("robot_move", input, result,
                sizeof(result));
        }
    }

    s_fast_path_count++;
    syslog(LOG_INFO,
        "[%s] motion fast path: direction=%s rc=%d\n", TAG, direction, rc);

    if (rc == OK && strstr(result, "\"ok\":true") != NULL) {
        motion_speak_notice("动作执行完毕", "completion");
        return strdup("动作执行完毕");
    }

    if (strstr(result, "network unavailable") != NULL) {
        motion_speak_notice("网络异常，请稍后再试", "failure");
    } else {
        motion_speak_notice("动作执行失败", "failure");
    }

    char* reply = calloc(1, 512);
    if (reply != NULL) {
        if (strstr(result, "network unavailable") != NULL) {
            snprintf(reply, 512, "网络异常，请稍后再试");
        } else {
            snprintf(reply, 512, "执行移动失败：%s",
                result[0] != '\0' ? result : "机器人动作工具不可用");
        }
    }
    return reply;
}

static char* handle_feishu_action_fast_path(agent_msg_t* msg)
{
    if (msg == NULL || strcmp(msg->channel, AGENT_CHAN_FEISHU) != 0
        || msg->content == NULL || msg->content[0] == '\0') {
        return NULL;
    }

    if (feishu_motion_direction(msg->content) != NULL) {
        return handle_motion_fast_path(msg->content);
    }

    if (contains_any(msg->content,
            (const char*[]) { "播放", "放一首", "play ", NULL })) {
        char keyword[128];
        char search_input[256];
        char search_result[4096] = { 0 };
        cJSON* search_json = NULL;
        cJSON* songs = NULL;
        cJSON* first = NULL;
        cJSON* url = NULL;
        char* reply = NULL;
        int rc;

        feishu_music_keyword(msg->content, keyword, sizeof(keyword));
        snprintf(search_input, sizeof(search_input),
            "{\"keyword\":\"%s\"}", keyword);
        rc = tool_registry_execute("music_search", search_input,
            search_result, sizeof(search_result));
        if (rc != OK) {
            reply = calloc(1, 512);
            if (reply != NULL) {
                snprintf(reply, 512, "搜索音乐失败：%s",
                    search_result[0] != '\0' ? search_result : "网络请求失败");
            }
            return reply;
        }

        search_json = cJSON_Parse(search_result);
        songs = search_json ? cJSON_GetObjectItem(search_json, "songs") : NULL;
        first = songs ? cJSON_GetArrayItem(songs, 0) : NULL;
        url = first ? cJSON_GetObjectItem(first, "url") : NULL;
        if (url == NULL || !cJSON_IsString(url) || url->valuestring[0] == '\0') {
            cJSON_Delete(search_json);
            return strdup("没有找到可播放的音乐。");
        }

        const char* title = cJSON_GetStringValue(
            cJSON_GetObjectItem(first, "name"));
        const char* artist = cJSON_GetStringValue(
            cJSON_GetObjectItem(first, "artist"));
        char play_input[768];
        char play_result[512] = { 0 };
        cJSON* play_json = cJSON_CreateObject();
        char* play_text = NULL;

        if (play_json == NULL) {
            cJSON_Delete(search_json);
            return strdup("准备音乐播放失败。");
        }
        cJSON_AddStringToObject(play_json, "url", url->valuestring);
        if (title != NULL) {
            cJSON_AddStringToObject(play_json, "title", title);
        }
        if (artist != NULL) {
            cJSON_AddStringToObject(play_json, "artist", artist);
        }
        play_text = cJSON_PrintUnformatted(play_json);
        snprintf(play_input, sizeof(play_input), "%s",
            play_text != NULL ? play_text : "{}");
        free(play_text);
        cJSON_Delete(play_json);

        rc = tool_registry_execute("music_play", play_input, play_result,
            sizeof(play_result));
        if (rc != OK || strstr(play_result, "\"ok\":false") != NULL) {
            reply = calloc(1, 640);
            if (reply != NULL) {
                snprintf(reply, 640, "找到了 %s - %s，但播放失败：%s",
                    title ? title : "未知歌曲", artist ? artist : "未知歌手",
                    play_result[0] != '\0' ? play_result : "播放工具不可用");
            }
            cJSON_Delete(search_json);
            return reply;
        }

        /* Music is the audible response to this command. Speaking the
         * acknowledgement locally can hold the task scope longer than a
         * short requested play window, so keep the Feishu reply text-only. */
        msg->flags |= AGENT_MSG_FLAG_NO_TTS;

        unsigned int stop_seconds = feishu_music_stop_seconds(msg->content);
        bool stop_scheduled = false;
        if (stop_seconds > 0
            && (strstr(msg->content, "关闭") != NULL
                || strstr(msg->content, "停止") != NULL)) {
            char cron_input[768];
            char cron_result[512] = { 0 };
            snprintf(cron_input, sizeof(cron_input),
                "{\"name\":\"feishu_music_stop\","
                "\"schedule_type\":\"at\",\"at_epoch\":%lld,"
                "\"message\":\"music stop\",\"action\":\"music_stop\","
                "\"action_args\":\"{}\",\"notify\":false,"
                "\"delete_after_run\":true}",
                (long long)(time(NULL) + stop_seconds));
            stop_scheduled = tool_registry_execute("cron_add", cron_input,
                cron_result, sizeof(cron_result)) == OK;
            if (!stop_scheduled) {
                syslog(LOG_WARNING,
                    "[%s] Feishu music stop schedule failed: %s\n", TAG,
                    cron_result);
            }
        }

        reply = calloc(1, 768);
        if (reply != NULL) {
            if (stop_seconds > 0 && stop_scheduled) {
                snprintf(reply, 768, "正在播放：%s - %s，已设置 %u 秒后停止。",
                    title ? title : "未知歌曲", artist ? artist : "未知歌手",
                    stop_seconds);
            } else if (stop_seconds > 0) {
                snprintf(reply, 768, "正在播放：%s - %s，但定时停止设置失败。",
                    title ? title : "未知歌曲", artist ? artist : "未知歌手");
            } else {
                snprintf(reply, 768, "正在播放：%s - %s。",
                    title ? title : "未知歌曲", artist ? artist : "未知歌手");
            }
        }
        cJSON_Delete(search_json);
        s_fast_path_count++;
        syslog(LOG_INFO,
            "[%s] Feishu action fast path: music_play keyword=%s stop=%u\n",
            TAG, keyword, stop_seconds);
        return reply;
    }

    return NULL;
}

/* Intent table: each entry maps keywords → tool + args + output size */

typedef struct {
    const char** keywords;
    const char* tool_name;
    const char* tool_args;
    size_t output_size;
} nl_intent_t;

static const char* kw_time[]       = { "几点了", "什么时间", "what time", "几点钟", NULL };
static const char* kw_battery[]    = { "电量多少", "电池电量", "battery", NULL };
static const char* kw_heartrate[]  = { "心率多少", "heart rate", "心率是多", NULL };
static const char* kw_steps[]      = { "走了多少步", "今天走了多少", "steps", "步数", NULL };
static const char* kw_pause[]      = { "暂停音乐", "暂停播放", "pause music", NULL };
static const char* kw_stop[]       = { "停止播放", "停止音乐", "stop music", NULL };
static const char* kw_resume[]     = { "继续播放", "resume", NULL };

static const nl_intent_t s_intents[] = {
    { kw_time,      "get_current_time", "{}",  256  },
    { kw_battery,   "get_battery",      "{}",  512  },
    { kw_heartrate, "get_heartrate",    "{}",  256  },
    { kw_steps,     "get_steps",        "{}",  256  },
    { kw_pause,     "music_pause",      "{}",  256  },
    { kw_stop,      "music_stop",       "{}",  256  },
    { kw_resume,    "music_resume",     "{}",  256  },
    { NULL,         NULL,               NULL,  0    },
};

static char* handle_nl_fast_path(const char* text)
{
    /* Compound channel actions must reach the LLM tool planner. */
    if (is_external_channel_action(text)) {
        if (is_external_time_action(text)) {
            char* reply = handle_external_time_action();
            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: feishu_send_text(time) (fast=%d llm=%d)\n",
                    TAG, s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }

        syslog(LOG_INFO, "[%s] NL fast path bypass: Feishu channel action\n",
            TAG);
        return NULL;
    }

    /* Table-driven: scan intents, first match wins */
    for (int i = 0; s_intents[i].keywords; i++) {
        if (contains_any(text, s_intents[i].keywords)) {
            char* reply = calloc(1, s_intents[i].output_size);
            if (reply) {
                tool_registry_execute(s_intents[i].tool_name,
                    s_intents[i].tool_args, reply,
                    s_intents[i].output_size);
            }
            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: %s (fast=%d llm=%d)\n",
                    TAG, s_intents[i].tool_name,
                    s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }

    /* Skill list (special: no tool call) */
    static const char* kw_skill[] = {
        "技能列表", "有什么技能", "list skill", NULL };
    if (contains_any(text, kw_skill)) {
        char buf[2048];
        size_t n = skill_loader_build_summary(buf, sizeof(buf));
        s_fast_path_count++;
        return n > 0 ? strdup(buf) : strdup("暂无已加载的技能。");
    }

    /* Weather (special: needs city extraction) */
    static const char* kw_weather[] = {
        "天气怎么样", "weather", "天气如何", NULL };
    if (contains_any(text, kw_weather)) {
        char safe_city[64];
        strncpy(safe_city, "Beijing", sizeof(safe_city) - 1);
        safe_city[sizeof(safe_city) - 1] = '\0';

        const char* p = strstr(text, "天气");
        if (p && p > text && (size_t)(p - text) < sizeof(safe_city) - 1) {
            int ci = 0;
            for (const char* s = text; s < p && ci < 62; s++) {
                if (*s != '"' && *s != '\\' && *s != ' ')
                    safe_city[ci++] = *s;
            }
            if (ci > 0) safe_city[ci] = '\0';
        }

        char input[256];
        snprintf(input, sizeof(input),
            "{\"location\":\"%s\"}", safe_city);
        char* reply = calloc(1, 4096);
        if (reply)
            tool_registry_execute("get_weather", input, reply, 4096);
        if (reply) {
            s_fast_path_count++;
            syslog(LOG_INFO,
                "[%s] NL fast path: get_weather (fast=%d llm=%d)\n",
                TAG, s_fast_path_count, s_llm_path_count);
        }
        return reply;
    }

    /* Music play (special: needs keyword extraction) */
    static const char* kw_play[] = { "播放", "play ", "放一首", NULL };
    if (contains_any(text, kw_play)) {
        const char* kw = NULL;
        const char* p = strstr(text, "播放");
        if (p) { p += strlen("播放"); while (*p == ' ') p++; if (*p) kw = p; }
        if (!kw) {
            p = strcasestr(text, "play ");
            if (p) { p += 5; while (*p == ' ') p++; if (*p) kw = p; }
        }
        if (kw) {
            int klen = 0;
            while (kw[klen] && kw[klen] != '"' && kw[klen] != '\\' && klen < 100)
                klen++;
            char input[256];
            snprintf(input, sizeof(input),
                "{\"keyword\":\"%.*s\"}", klen, kw);
            char search_result[4096];
            memset(search_result, 0, sizeof(search_result));
            tool_registry_execute("music_search", input, search_result, sizeof(search_result));

            /* Auto-play first result if search succeeded */
            char* reply = NULL;
            cJSON* sr = cJSON_Parse(search_result);
            cJSON* songs = sr ? cJSON_GetObjectItem(sr, "songs") : NULL;
            cJSON* first = songs ? cJSON_GetArrayItem(songs, 0) : NULL;
            cJSON* url = first ? cJSON_GetObjectItem(first, "url") : NULL;
            if (url && cJSON_IsString(url) && url->valuestring[0]) {
                char play_input[512];
                cJSON* play_obj = cJSON_CreateObject();
                char* play_json = NULL;
                cJSON* play_name = first ? cJSON_GetObjectItem(first, "name") : NULL;
                cJSON* play_artist = first ? cJSON_GetObjectItem(first, "artist") : NULL;
                if (play_obj) {
                    cJSON_AddStringToObject(play_obj, "url", url->valuestring);
                    if (play_name && cJSON_IsString(play_name))
                        cJSON_AddStringToObject(play_obj, "title", play_name->valuestring);
                    if (play_artist && cJSON_IsString(play_artist))
                        cJSON_AddStringToObject(play_obj, "artist", play_artist->valuestring);
                    play_json = cJSON_PrintUnformatted(play_obj);
                }
                snprintf(play_input, sizeof(play_input), "%s",
                    play_json ? play_json : "{}");
                free(play_json);
                cJSON_Delete(play_obj);
                char play_result[256];
                memset(play_result, 0, sizeof(play_result));
                tool_registry_execute("music_play", play_input, play_result, sizeof(play_result));

                /* Build reply with song info */
                cJSON* name = cJSON_GetObjectItem(first, "name");
                cJSON* artist = cJSON_GetObjectItem(first, "artist");
                const char* sname = (name && cJSON_IsString(name)) ? name->valuestring : "未知";
                const char* sartist = (artist && cJSON_IsString(artist)) ? artist->valuestring : "未知";

                /* Check if play actually succeeded */
                bool play_ok = (strstr(play_result, "error") == NULL && play_result[0] != '\0');
                reply = calloc(1, 512);
                if (reply) {
                    if (play_ok) {
                        snprintf(reply, 512, "正在播放: %s - %s", sname, sartist);
                    } else {
                        snprintf(reply, 512, "找到了 %s - %s，但播放失败: %s",
                            sname, sartist, play_result);
                    }
                }
            } else {
                reply = calloc(1, 4096);
                if (reply)
                    strncpy(reply, search_result, 4095);
            }
            cJSON_Delete(sr);

            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: music_search (fast=%d llm=%d)\n",
                    TAG, s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }

    return NULL;
}

/* ── Handle slash commands + NL fast path (bypass LLM) ─── */

static char* handle_slash_command(agent_msg_t* msg)
{
    if (!msg->content) {
        return NULL;
    }

    /* Phase 1: slash commands (highest priority) */
    if (msg->content[0] != '/') {
        return NULL; /* NL fast path handled separately after injection check */
    }

    char* reply = NULL;

    if (strncmp(msg->content, "/help", 5) == 0) {
        reply = strdup(
            "AI Agent 快捷命令：\n\n"
            "/help           显示本帮助\n"
            "/time           当前时间\n"
            "/weather 城市   实时天气\n"
            "/news [关键词]  最新新闻\n"
            "/memory         查看长期记忆\n"
            "/note 内容      记一条笔记\n"
            "/remind 秒数 内容  设提醒\n"
            "/skill          列出技能\n"
            "/translate 文本 翻译\n"
            "/daily          每日简报\n\n"
            "也可以直接用自然语言对话，我会自动调用工具。");
    } else if (strncmp(msg->content, "/time", 5) == 0) {
        reply = calloc(1, 256);
        if (reply) {
            tool_registry_execute("get_current_time", "{}", reply, 256);
        }
    } else if (strncmp(msg->content, "/weather", 8) == 0) {
        const char* arg = msg->content + 8;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "Beijing";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"location\":\"%s\"}", arg);
        reply = calloc(1, 4096);
        if (reply) {
            tool_registry_execute("get_weather", input, reply, 4096);
        }
    } else if (strncmp(msg->content, "/news", 5) == 0) {
        const char* arg = msg->content + 5;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "today";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"query\":\"%s\",\"top_headlines\":true}", arg);
        reply = calloc(1, 8192);
        if (reply) {
            tool_registry_execute("news_search", input, reply, 8192);
        }
    } else if (strncmp(msg->content, "/memory", 7) == 0) {
        char mem_path[256];
        snprintf(mem_path, sizeof(mem_path),
            "{\"path\":\"%s/memory/MEMORY.md\"}", AGENT_DATA_DIR);
        reply = calloc(1, 4096);
        if (reply) {
            int r = tool_registry_execute(
                "read_file", mem_path, reply, 4096);
            if (r != OK || !reply[0]) {
                snprintf(reply, 4096, "暂无长期记忆。");
            }
        }
    } else if (strncmp(msg->content, "/note ", 6) == 0) {
        reply = handle_slash_note(msg);
    } else if (strncmp(msg->content, "/remind ", 8) == 0) {
        reply = handle_slash_remind(msg);
    } else if (strncmp(msg->content, "/skill", 6) == 0) {
        char skills_buf[2048];
        size_t slen = skill_loader_build_summary(
            skills_buf, sizeof(skills_buf));
        reply = (slen > 0)
            ? strdup(skills_buf)
            : strdup("暂无已加载的技能。");
    } else if (strncmp(msg->content, "/translate ", 11) == 0) {
        /* Rewrite content to pass through LLM */
        char* nc = malloc(strlen(msg->content) + 64);
        if (nc) {
            snprintf(nc, strlen(msg->content) + 64,
                "请翻译以下内容（中英互译）：%s", msg->content + 11);
            free(msg->content);
            msg->content = nc;
        }
    } else if (strncmp(msg->content, "/daily", 6) == 0) {
        char* nc = strdup(
            "请给我一份今日简报，包括：当前时间、今日天气、"
            "最新新闻摘要。");
        if (nc) {
            free(msg->content);
            msg->content = nc;
        }
    }

    if (reply) {
        s_fast_path_count++;
    }
    return reply;
}

/* /note sub-handler */
static char* handle_slash_note(const agent_msg_t* msg)
{
    const char* note = msg->content + 6;
    time_t now = time(NULL);
    struct tm tm_now;
    time_t local_epoch = now + 8 * 3600;

    gmtime_r(&local_epoch, &tm_now);

    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);

    char path[256];
    snprintf(path, sizeof(path), "%s/memory/daily/%s.md",
        AGENT_DATA_DIR, date_str);

    char input[4096];
    snprintf(input, sizeof(input),
        "{\"path\":\"%s\",\"content\":\"%s %s\\n- %s %s\\n\"}",
        path, "# ", date_str, time_str, note);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("write_file", input, reply, 512);
        snprintf(reply, 512, "已记录：%s", note);
    }
    return reply;
}

/* /remind sub-handler */
static char* handle_slash_remind(const agent_msg_t* msg)
{
    int secs = 0;
    char remind_msg[512];

    remind_msg[0] = '\0';
    if (sscanf(msg->content + 8, "%d %511[^\n]",
            &secs, remind_msg)
        < 1) {
        return NULL;
    }
    if (!remind_msg[0]) {
        strncpy(remind_msg, "提醒时间到", sizeof(remind_msg) - 1);
    }

    char input[1024];
    snprintf(input, sizeof(input),
        "{\"name\":\"remind\",\"schedule_type\":\"at\","
        "\"at_epoch\":%lld,\"message\":\"%s\","
        "\"channel\":\"%s\",\"chat_id\":\"%s\"}",
        (long long)(time(NULL) + secs), remind_msg,
        msg->channel, msg->chat_id);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("cron_add", input, reply, 512);
        snprintf(reply, 512, "好的，%d 秒后提醒你：%s",
            secs, remind_msg);
    }
    return reply;
}

/* ── Extracted: handle vision message ──────────────────────── */

static char* handle_vision_message(agent_msg_t* msg)
{
    if (!msg->image_b64 || !msg->image_b64[0]) {
        return NULL;
    }

    syslog(LOG_INFO,
        "[%s] Vision message detected, calling llm_chat_vision\n", TAG);

    size_t vis_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);
    char* vision_resp = calloc(1, vis_size);

    if (!vision_resp) {
        free(msg->image_b64);
        msg->image_b64 = NULL;
        return NULL;
    }

    const char* prompt = (msg->content && msg->content[0])
        ? msg->content
        : AGENT_VISION_DEFAULT_PROMPT;
    int err = llm_chat_vision(
        prompt, msg->image_b64, NULL, vision_resp, vis_size);

    free(msg->image_b64);
    msg->image_b64 = NULL;

    if (err == OK && vision_resp[0]) {
        return vision_resp;
    }

    free(vision_resp);
    return NULL;
}

/* ── Extracted: strip leaked tool-call XML markup ─────────── */

static char* strip_tool_call_markup(char* text)
{
    if (!text) {
        return NULL;
    }

    int dirty = 0;
    if (strstr(text, "<tool_call>") || strstr(text, ":tool_call>")) {
        dirty = 1;
    }
    if (!dirty) {
        return text;
    }

    syslog(LOG_WARNING,
        "[%s] Force-finish text contains tool-call markup, stripping\n",
        TAG);

    size_t len = strlen(text);
    char* clean = calloc(1, len + 1);
    if (!clean) {
        return text;
    }

    const char* r = text;
    char* w = clean;

    while (*r) {
        const char* ts = NULL;
        const char* te = NULL;

        const char* plain = strstr(r, "<tool_call>");
        const char* ns = strstr(r, ":tool_call>");
        const char* ns_open = NULL;

        if (ns) {
            ns_open = ns;
            while (ns_open > r && *(ns_open - 1) != '<') {
                ns_open--;
            }
            if (ns_open > r) {
                ns_open--;
            } else {
                ns_open = NULL;
            }
        }

        if (plain && (!ns_open || plain <= ns_open)) {
            ts = plain;
            te = strstr(ts + 11, "</tool_call>");
            if (te) {
                te += 12;
            }
        } else if (ns_open) {
            ts = ns_open;
            size_t plen = (size_t)(ns - (ts + 1));
            if (plen > 0 && plen < 32) {
                char ctag[64];
                snprintf(ctag, sizeof(ctag), "</%.*s:tool_call>",
                    (int)plen, ts + 1);
                te = strstr(ts, ctag);
                if (te) {
                    te += strlen(ctag);
                }
            }
        }

        if (ts && te) {
            size_t prefix = (size_t)(ts - r);
            memcpy(w, r, prefix);
            w += prefix;
            r = te;
        } else {
            size_t rest = strlen(r);
            memcpy(w, r, rest);
            w += rest;
            break;
        }
    }
    *w = '\0';

    free(text);
    if (clean[0] == '\0' || strspn(clean, " \t\r\n") == strlen(clean)) {
        free(clean);
        return strdup(
            "抱歉，这个任务比较复杂，"
            "我已经收集了一些信息但未能完成全部步骤。"
            "请尝试拆分成更小的问题再问我。");
    }
    return clean;
}

/* ── Extracted: force finish when iteration limit reached ─── */

static char* force_finish_reply(const char* system_prompt,
    cJSON* messages)
{
    syslog(LOG_WARNING,
        "[%s] Tool iteration limit (%d) reached, forcing finish\n",
        TAG, AGENT_AI_AGENT_MAX_TOOL_ITER);

    cJSON* hint = cJSON_CreateObject();
    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "You have used all available tool iterations. "
        "Do NOT call any more tools. Summarize what you have "
        "learned so far and reply to the user in plain text now.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t resp;
    char* result = NULL;
    uint64_t t0 = monotonic_ms();
    int err = llm_chat_tools(system_prompt, messages, NULL, &resp);
    uint64_t t1 = monotonic_ms();
    uint64_t ms = monotonic_elapsed_ms(t0, t1);
    bool timed_out = llm_watchdog_expired(ms, err);

    if (err == OK && !timed_out && resp.text && resp.text_len > 0) {
        result = strdup(resp.text);
    }
    llm_response_free(&resp);

    if (!result && timed_out) {
        result = strdup(LLM_TIMEOUT_MSG);
    }

    return strip_tool_call_markup(result);
}

/* ── Extracted: dispatch response to outbound bus ─────────── */

static void dispatch_response(const agent_msg_t* msg,
    char* final_text)
{
    if (final_text && final_text[0]) {
        session_append(msg->chat_id, "user", msg->content);
        session_append(msg->chat_id, "assistant", final_text);

        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.flags = msg->flags;
        out.content = final_text;
        agent_oled_enqueue(final_text);
        if (message_bus_reply_and_wait(&out) != OK) {
            free(final_text);
        }
    } else {
        free(final_text);
        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.flags = msg->flags;
        out.content = strdup("Sorry, I encountered an error.");
        if (out.content) {
            if (message_bus_reply_and_wait(&out) != OK) {
                free(out.content);
            }
        }
    }
}

/* ── ReAct tool-calling loop ──────────────────────────────── */

static const char* s_working_phrases[] = {
    "正在思考中...",
    "稍等，处理中...",
    "让我查一下...",
    "正在分析...",
    "马上好...",
};
#define WORKING_PHRASE_COUNT \
    ((int)(sizeof(s_working_phrases) / sizeof(s_working_phrases[0])))

/* Send a "working" status message on the first iteration.
 * Skip for feishu (has its own typing indicator) and voice
 * (TTS synthesis of a status phrase wastes time and memory). */
static void send_working_status(const agent_msg_t* msg, int iteration)
{
    if (iteration != 0) {
        return;
    }
    if (strcmp(msg->channel, AGENT_CHAN_FEISHU) == 0
        || strcmp(msg->channel, AGENT_CHAN_VOICE) == 0
#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
        || strcmp(msg->channel, AGENT_CHAN_QUICKAPP) == 0
#endif
        || strcmp(msg->channel, AGENT_CHAN_WEIXIN) == 0) {
        return;
    }

    agent_msg_t status = { 0 };
    status.flags = AGENT_MSG_FLAG_TASK_STATUS | AGENT_MSG_FLAG_NO_TTS;

    strncpy(status.channel, msg->channel, sizeof(status.channel) - 1);
    strncpy(status.chat_id, msg->chat_id, sizeof(status.chat_id) - 1);
    status.content = strdup(
        s_working_phrases[(unsigned)rand() % WORKING_PHRASE_COUNT]);
    if (status.content) {
        if (message_bus_push_outbound(&status) != OK) {
            free(status.content);
        }
    }
}

/* Check for duplicate tool calls. Returns true if loop should break. */
static bool check_tool_dup(const llm_response_t* resp,
    char* prev_sig, int* dup_count,
    char* prev_name, int* name_repeat)
{
    if (resp->call_count != 1) {
        prev_sig[0] = '\0';
        *dup_count = 0;
        prev_name[0] = '\0';
        *name_repeat = 0;
        return false;
    }

    /* Exact match (name + args) */
    char cur_sig[512];

    snprintf(cur_sig, sizeof(cur_sig), "%s|%.400s",
        resp->calls[0].name,
        resp->calls[0].input ? resp->calls[0].input : "");

    if (strcmp(cur_sig, prev_sig) == 0) {
        (*dup_count)++;
        if (*dup_count >= 2) {
            syslog(LOG_WARNING,
                "[%s] Duplicate tool call detected (%s), breaking\n",
                TAG, resp->calls[0].name);
            return true;
        }
    } else {
        *dup_count = 0;
    }
    strncpy(prev_sig, cur_sig, 511);
    prev_sig[511] = '\0';

    /* Name-only repeat detection */
    if (strcmp(resp->calls[0].name, prev_name) == 0) {
        (*name_repeat)++;
        if (*name_repeat >= AGENT_TOOL_NAME_REPEAT_MAX) {
            syslog(LOG_WARNING,
                "[%s] Tool name repeat limit (%s called %d times)\n",
                TAG, resp->calls[0].name, *name_repeat + 1);
            return true;
        }
    } else {
        *name_repeat = 0;
    }
    strncpy(prev_name, resp->calls[0].name, 63);
    prev_name[63] = '\0';

    return false;
}

/* Check if an LLM call exceeded the watchdog timeout.
 * Returns true when latency_ms exceeds AGENT_LLM_TIMEOUT_SEC. */
static bool llm_call_timed_out(uint64_t latency_ms)
{
    return latency_ms > (uint64_t)AGENT_LLM_TIMEOUT_SEC * 1000ULL;
}

/* Handle TASK_COMPLETE: inject hint and do one final LLM call.
 * If the LLM call times out, *out_timed_out is set to true. */
static char* handle_task_complete(const char* sys_prompt, cJSON* messages,
    bool* out_timed_out)
{
    cJSON* hint = cJSON_CreateObject();

    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "The task has been completed successfully. "
        "Do NOT call any more tools. "
        "Reply to the user now confirming what was done.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t final_resp;
    char* result = NULL;
    uint64_t t0 = monotonic_ms();
    int err = llm_chat_tools(sys_prompt, messages, NULL, &final_resp);
    uint64_t t1 = monotonic_ms();
    uint64_t ms = monotonic_elapsed_ms(t0, t1);
    bool timed_out = llm_watchdog_expired(ms, err);

    if (err == OK && !timed_out
        && final_resp.text && final_resp.text_len > 0) {
        result = strdup(final_resp.text);
    }
    llm_response_free(&final_resp);

    if (!result && timed_out) {
        if (out_timed_out) {
            *out_timed_out = true;
        }
        return strdup(LLM_TIMEOUT_TASK_COMPLETE_MSG);
    }
    return result;
}

/* Run the ReAct tool-calling loop. Returns final_text (caller owns). */
static char* run_react_loop(const char* sys_prompt, cJSON* messages,
    const char* tools_json, char* tool_output, size_t tool_size,
    const agent_msg_t* msg)
{
    char prev_sig[512];
    int dup_count;
    char prev_name[64];
    int name_repeat;
    int iteration;
    char* final_text = NULL;

    prev_sig[0] = '\0';
    dup_count = 0;
    prev_name[0] = '\0';
    name_repeat = 0;
    int last_total_tokens = 0;
    bool watchdog_fired = false;
    bool cacheable = false;

    /* Router: select and apply best backend before first LLM call.
     * Estimate complexity from the last user message. */
    llm_complexity_t complexity = LLM_COMPLEXITY_SIMPLE;
    if (msg->content) {
        complexity = llm_router_estimate_complexity(
            msg->content, strlen(msg->content));
    }
    int router_idx = llm_router_select(complexity);
    if (router_idx >= 0) {
        llm_router_apply(router_idx);
    }

    /* Structured trace: begin */
    agent_trace_t trace;
    agent_trace_begin(&trace, msg->chat_id, msg->channel);
    trace.backend_idx = router_idx;

    /* Cache check: for simple queries, try cache before LLM call */
    if (msg->content && complexity == LLM_COMPLEXITY_SIMPLE) {
        char* cached = llm_cache_get(msg->content, strlen(msg->content));
        if (cached) {
            syslog(LOG_INFO,
                "[%s] Cache hit (successful answer), skipping LLM call\n",
                TAG);
            final_text = cached;
            agent_trace_step(&trace, 0, NULL, 0, 1);
            agent_trace_end(&trace, AGENT_TRACE_OK);
            goto send_reply;
        }
    }

    for (iteration = 0; iteration < AGENT_AI_AGENT_MAX_TOOL_ITER;
         iteration++) {
        send_working_status(msg, iteration);

        llm_response_t resp;
        uint64_t tv_start = monotonic_ms();
        int err = llm_chat_tools(sys_prompt, messages, tools_json, &resp);
        uint64_t tv_end = monotonic_ms();
        uint64_t latency_ms = monotonic_elapsed_ms(tv_start, tv_end);

        /* Router failover: on LLM call failure, try next backend */
        if (err != OK && router_idx >= 0) {
            syslog(LOG_WARNING,
                "[%s] LLM call failed on backend %d, trying failover\n",
                TAG, router_idx);
            llm_router_report_failure(router_idx);

            int next_idx = llm_router_select(complexity);
            if (next_idx >= 0 && next_idx != router_idx) {
                llm_router_apply(next_idx);
                router_idx = next_idx;
                trace.backend_idx = next_idx;
                llm_response_free(&resp);
                tv_start = monotonic_ms();
                err = llm_chat_tools(sys_prompt, messages,
                    tools_json, &resp);
                tv_end = monotonic_ms();
                latency_ms = monotonic_elapsed_ms(tv_start, tv_end);
            }
        }

        if (err != OK) {
            /* Distinguish timeout-induced failure from other errors */
            if (llm_watchdog_expired(latency_ms, err)) {
                syslog(LOG_WARNING,
                    "[%s] LLM watchdog: call failed after %" PRIu64 " ms "
                    "(limit %ds)\n",
                    TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
                agent_trace_step(&trace, iteration, NULL,
                    latency_ms, 0);
                llm_response_free(&resp);
                final_text = strdup(LLM_TIMEOUT_MSG);
                watchdog_fired = true;
                break;
            }
            syslog(LOG_ERR, "[%s] LLM call failed (iter %d)\n",
                TAG, iteration);
            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);
            break;
        }

        /* Watchdog: if the LLM call took longer than the configured
         * timeout, treat it as a timeout even if it returned OK.
         * The socket SO_RCVTIMEO may have fired and caused a partial
         * or error response that llm_chat_tools mapped to ERROR above,
         * but if the call barely completed, we still flag it. */
        if (llm_watchdog_expired(latency_ms, err)) {
            syslog(LOG_WARNING,
                "[%s] LLM watchdog: call took %" PRIu64 " ms (limit %ds), "
                "treating as timeout\n",
                TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);
            final_text = strdup(LLM_TIMEOUT_MSG);
            watchdog_fired = true;
            break;
        }

        /* Report success to router */
        if (router_idx >= 0) {
            llm_router_report_success(router_idx);
            llm_router_report_latency(router_idx,
                latency_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)latency_ms);
            if (resp.total_tokens > 0) {
                llm_router_report_tokens(router_idx,
                    resp.prompt_tokens, resp.completion_tokens);
                last_total_tokens = resp.total_tokens;
            }
        }

        syslog(LOG_INFO,
            "[%s] LLM resp: text=%zu, tool_use=%d, calls=%d\n",
            TAG, resp.text_len, resp.tool_use, resp.call_count);

        if (!resp.tool_use) {
            /* Cascade routing: if AUTO profile selected a cheap backend
             * for a SIMPLE query but the response looks inadequate,
             * retry once with PREMIUM tier. */
            bool cascade_retry = false;
            if (llm_router_get_profile() == LLM_ROUTE_AUTO
                && complexity == LLM_COMPLEXITY_SIMPLE
                && router_idx >= 0
                && resp.text != NULL) {
                bool low_quality = (strstr(resp.text, "I don't know") != NULL
                    || strstr(resp.text, "I cannot") != NULL
                    || strstr(resp.text, "无法") != NULL);
                if (low_quality) {
                    cascade_retry = true;
                }
            }

            if (cascade_retry) {
                syslog(LOG_INFO,
                    "[%s] Cascade: cheap response inadequate, "
                    "retrying with PREMIUM\n",
                    TAG);

                /* Save the cheap response as fallback before freeing */
                char* fallback_text = NULL;
                if (resp.text && resp.text_len > 0) {
                    fallback_text = strdup(resp.text);
                }
                llm_response_free(&resp);

                /* Select a PREMIUM backend without changing profile */
                int prem_idx = llm_router_select_with_profile(
                    LLM_COMPLEXITY_MEDIUM, LLM_ROUTE_PREMIUM);

                if (prem_idx >= 0 && prem_idx != router_idx) {
                    llm_router_apply(prem_idx);
                    router_idx = prem_idx;
                    trace.backend_idx = prem_idx;

                    tv_start = monotonic_ms();
                    err = llm_chat_tools(sys_prompt, messages,
                        tools_json, &resp);
                    tv_end = monotonic_ms();
                    latency_ms = monotonic_elapsed_ms(tv_start, tv_end);

                    /* Watchdog check on cascade retry */
                    if (llm_watchdog_expired(latency_ms, err)) {
                        syslog(LOG_WARNING,
                            "[%s] LLM watchdog: cascade retry "
                            "took %" PRIu64 " ms\n",
                            TAG, latency_ms);
                        free(fallback_text);
                        fallback_text = NULL;
                        final_text = strdup(LLM_TIMEOUT_MSG);
                        watchdog_fired = true;
                        agent_trace_step(&trace, iteration, NULL,
                            latency_ms, 0);
                        llm_response_free(&resp);
                        break;
                    }

                    if (err == OK && resp.text && resp.text_len > 0) {
                        final_text = strdup(resp.text);
                        cacheable = final_text != NULL;
                        free(fallback_text);
                        fallback_text = NULL;
                    } else {
                        /* Premium failed — fall back to cheap response */
                        final_text = fallback_text;
                        cacheable = final_text != NULL;
                        fallback_text = NULL;
                    }
                    agent_trace_step(&trace, iteration, NULL,
                        latency_ms, err == OK);
                    llm_response_free(&resp);
                    break;
                }
                /* No premium backend available — use cheap response */
                final_text = fallback_text;
                cacheable = final_text != NULL;
                fallback_text = NULL;
                agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
                break;
            }

            if (resp.text && resp.text_len > 0 && !final_text) {
                final_text = strdup(resp.text);
                cacheable = final_text != NULL;
            }
            agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
            llm_response_free(&resp);
            break;
        }

        syslog(LOG_INFO, "[%s] Tool iter %d: %d calls\n",
            TAG, iteration + 1, resp.call_count);

        /* Trace: log tool step */
        agent_trace_step(&trace, iteration,
            resp.call_count > 0 ? resp.calls[0].name : NULL,
            latency_ms, 1);

        /* Duplicate detection — break if stuck in a loop */
        bool should_break = check_tool_dup(
            &resp, prev_sig, &dup_count, prev_name, &name_repeat);

        if (should_break) {
            add_assistant_message(messages, &resp);
            add_tool_result_messages(messages, &resp, tool_output,
                tool_size, msg->channel, msg->chat_id);
            llm_response_free(&resp);
            break;
        }

        add_assistant_message(messages, &resp);
        add_tool_result_messages(messages, &resp, tool_output,
            tool_size, msg->channel, msg->chat_id);

        /* Local tool shortcut: if the single tool in this round is a
         * local file op, skip the next LLM round and use the tool
         * output directly as the reply. Saves ~2s.
         * Multi-call rounds still need the LLM to aggregate all tool
         * results; tool_output contains only the last tool's result. */
        if (resp.call_count == 1) {
            bool all_local = true;
            for (int i = 0; i < resp.call_count; i++) {
                if (strcmp(resp.calls[i].name, "read_file") != 0
                    && strcmp(resp.calls[i].name, "write_file") != 0
                    && strcmp(resp.calls[i].name, "edit_file") != 0
                    && strcmp(resp.calls[i].name, "list_dir") != 0) {
                    all_local = false;
                    break;
                }
            }
            if (all_local && tool_output[0]) {
                /* Don't shortcut if the tool read a skill file
                 * from AGENT_SKILLS_DIR — the LLM needs to process
                 * the skill content and generate a proper user-facing
                 * response instead of dumping raw markdown to the user. */
                bool is_skill_read = false;
                if (strcmp(resp.calls[0].name, "read_file") == 0
                    && resp.calls[0].input != NULL) {
                    cJSON *input_obj = cJSON_Parse(resp.calls[0].input);
                    if (input_obj) {
                        cJSON *path_obj = cJSON_GetObjectItem(input_obj, "path");
                        if (path_obj && cJSON_IsString(path_obj)
                            && strncmp(path_obj->valuestring,
                                AGENT_SKILLS_DIR,
                                strlen(AGENT_SKILLS_DIR)) == 0) {
                            is_skill_read = true;
                        }
                        cJSON_Delete(input_obj);
                    }
                }

                if (!is_skill_read) {
                    syslog(LOG_INFO,
                        "[%s] Local tool shortcut: skip LLM round\n",
                        TAG);
                    final_text = strdup(tool_output);
                    llm_response_free(&resp);
                    break;
                }

                syslog(LOG_INFO,
                    "[%s] Skill file read — no shortcut, LLM will process\n",
                    TAG);
            }
        }

        /* TASK_COMPLETE detection */
        if (resp.call_count == 1 && tool_output[0]
            && strstr(tool_output, "TASK_COMPLETE")) {
            syslog(LOG_INFO,
                "[%s] Tool %s returned TASK_COMPLETE\n",
                TAG, resp.calls[0].name);
            llm_response_free(&resp);
            bool task_timed_out = false;
            final_text = handle_task_complete(sys_prompt, messages,
                &task_timed_out);
            if (task_timed_out) {
                watchdog_fired = true;
            } else {
                cacheable = final_text != NULL;
            }
            break;
        }

        llm_response_free(&resp);
    }

    /* Iteration limit reached — force a summary reply */
    if (!final_text && iteration >= AGENT_AI_AGENT_MAX_TOOL_ITER) {
        final_text = force_finish_reply(sys_prompt, messages);
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (watchdog_fired) {
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (final_text) {
        agent_trace_end(&trace, AGENT_TRACE_OK);
    } else {
        agent_trace_end(&trace, AGENT_TRACE_FAIL);
    }

send_reply:
    /* Cache store: save simple query responses for future reuse */
    if (cacheable && !watchdog_fired && final_text && msg->content
        && complexity == LLM_COMPLEXITY_SIMPLE) {
        llm_cache_put(msg->content, strlen(msg->content), final_text);
        if (last_total_tokens > 0) {
            llm_cache_put_tokens(msg->content, strlen(msg->content),
                last_total_tokens);
        }
    }

    return final_text;
}

/* ── Build messages array from session history + current msg ─ */

static cJSON* build_messages(const char* chat_id, const char* content,
    char* history_json, size_t hist_size)
{
    session_get_history_json(chat_id, history_json, hist_size,
        AGENT_AI_AGENT_MAX_HISTORY);

    cJSON* messages = cJSON_Parse(history_json);

    if (!messages) {
        messages = cJSON_CreateArray();
    }

    cJSON* user_msg = cJSON_CreateObject();

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", content);
    cJSON_AddItemToArray(messages, user_msg);
    return messages;
}

/* ── Inject session context into system prompt ───────────── */

static void inject_session_context(char* sys_prompt, size_t size,
    const char* channel, const char* chat_id)
{
    size_t len = strlen(sys_prompt);

    snprintf(sys_prompt + len, size - len,
        "\n## Current Session\n"
        "channel: %s\n"
        "chat_id: %s\n"
        "When using feishu_send_mention or feishu_chat_members, "
        "use the chat_id above unless the user specifies a "
        "different one.\n"
        "When sending a plain message to a Feishu group, use "
        "feishu_send_text. From voice, omit chat_id to use the most "
        "recently active Feishu group; provide chat_id if no group is known.\n"
        "If the user message ends with "
        "[mentioned_users: name=open_id], "
        "use those open_ids when you need to @mention or remind "
        "those users. Do NOT include the [mentioned_users: ...] "
        "block in your reply text.\n",
        channel, chat_id);
}

/* ── Main agent loop task ─────────────────────────────────── */

static void* agent_loop_task(void* arg)
{
    (void)arg;
    agent_mem_status_t mem_st;

    agent_mem_get_status(&mem_st);
    syslog(LOG_INFO, "[%s] Agent loop started, free heap: %zu\n",
        TAG, mem_st.free_heap);

    size_t ctx_size = agent_mem_safe_size(
        AGENT_CONTEXT_BUF_SIZE, 4 * 1024);
    size_t hist_size = agent_mem_safe_size(
        AGENT_LLM_STREAM_BUF_SIZE, 8 * 1024);
    size_t tool_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);

    char* sys_prompt = calloc(1, ctx_size);
    char* history_json = calloc(1, hist_size);
    char* tool_output = calloc(1, tool_size);

    if (!sys_prompt || !history_json || !tool_output) {
        syslog(LOG_ERR, "[%s] Failed to allocate agent buffers\n", TAG);
        free(sys_prompt);
        free(history_json);
        free(tool_output);
        return NULL;
    }

    syslog(LOG_INFO, "[%s] Buffers: ctx=%zu hist=%zu tool=%zu\n",
        TAG, ctx_size, hist_size, tool_size);

    char* tools_json = tool_registry_get_tools_json();

    syslog(LOG_INFO, "[%s] Tools JSON loaded: %d bytes\n",
        TAG, tools_json ? (int)strlen(tools_json) : 0);

    while (!agent_shutdown_requested()) {
        agent_msg_t msg;
        int err = message_bus_pop_inbound(&msg, UINT32_MAX);

        if (err != OK) {
            continue;
        }

        if (agent_shutdown_requested()) {
            free(msg.content);
            free(msg.image_b64);
            break;
        }

        syslog(LOG_INFO, "[%s] Processing message from %s:%s\n",
            TAG, msg.channel, msg.chat_id);

        /* Confirmed music events execute the local companion Skill on the
         * Agent scheduler. Do not enter a foreground scope: that would pause
         * the very song whose tail policy is being armed. Never parse raw
         * Skill prose with keyword paths or invoke LLM/TTS for this event. */
        if ((msg.flags & AGENT_MSG_FLAG_INTERNAL_MUSIC) != 0) {
            if (robot_proactive_apply_music_skill &&
                strcmp(msg.chat_id, "robot_music_companion") == 0) {
                int rc = robot_proactive_apply_music_skill(msg.content);
                syslog(rc == 0 || rc == -ESTALE ? LOG_INFO : LOG_WARNING,
                    "[%s] Local music Skill dispatched rc=%d\n", TAG, rc);
            }
            message_bus_msg_free(&msg);
            continue;
        }

        /* Hold resources through the synchronous outbound acknowledgement.
         * A failed pause closes music; report the handoff failure without LLM. */
        int task_rc = tool_media_task_begin();
        char *reply = NULL;
        if (msg.flags & (AGENT_MSG_FLAG_MUSIC_PAUSE |
                         AGENT_MSG_FLAG_MUSIC_STOP))
            msg.flags |= AGENT_MSG_FLAG_NO_TTS;
        if (task_rc != OK) {
            msg.flags |= AGENT_MSG_FLAG_NO_TTS;
            reply = strdup("当前资源交接未完成，本次指令未执行，请稍后重试。");
            if (!reply) goto task_done;
        }

#ifdef CONFIG_AI_AGENT_FEISHU
        if (!reply && (msg.flags & AGENT_MSG_FLAG_FEISHU_IMAGE)) {
            if (feishu_prepare_agent_image(&msg) != OK) {
                reply = strdup("图片获取失败，请稍后重试。");
                if (!reply) goto task_done;
            }
        }
#endif
        if (!reply && (msg.flags & AGENT_MSG_FLAG_TOOL_JOB)) {
            cJSON *job = cJSON_Parse(msg.content);
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(job, "tool"));
            const char *args = cJSON_GetStringValue(cJSON_GetObjectItem(job, "args"));
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(job, "text"));
            bool notify = cJSON_IsTrue(cJSON_GetObjectItem(job, "notify"));
            int rc = ERROR;
            if (name && args) {
                tool_output[0] = '\0';
                rc = tool_registry_execute(name, args, tool_output, tool_size);
                if (rc != OK) syslog(LOG_WARNING, "[%s] Scheduled tool %s failed: %.128s\n", TAG, name, tool_output);
            }
            if (notify && (rc == OK || strcmp(msg.channel, AGENT_CHAN_VOICE)))
                reply = strdup(rc == OK && text ? text : "定时任务执行失败。");
            cJSON_Delete(job);
            if (!reply) goto task_done;
        }
        if (!reply && (msg.flags & AGENT_MSG_FLAG_NOTIFICATION)) {
            reply = msg.content;
            msg.content = NULL;
            if (!reply) goto task_done;
        }
        /* Exact controls run before memory checks or model parsing. */
        if (!reply && (msg.flags & (AGENT_MSG_FLAG_MUSIC_PAUSE | AGENT_MSG_FLAG_MUSIC_STOP))) {
            reply = calloc(1, 256);
            if (!reply) goto task_done;
            tool_registry_execute((msg.flags & AGENT_MSG_FLAG_MUSIC_STOP)
                ? "music_stop" : "music_pause", "{}", reply, 256);
        }
        if (reply) goto send_reply;

        /* Check memory pressure */
        agent_mem_get_status(&mem_st);
        if (mem_st.free_heap < AGENT_MEM_RESERVE_BYTES) {
            syslog(LOG_WARNING,
                "[%s] Low memory: %zu bytes free, skipping\n",
                TAG, mem_st.free_heap);
            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id,
                sizeof(out.chat_id) - 1);
            out.flags = msg.flags;
            out.content = strdup("系统内存不足，请稍后再试。");
            if (out.content) {
                if (message_bus_reply_and_wait(&out) != OK) {
                    free(out.content);
                }
            }
            goto task_done;
        }

        /* Slash commands — fast path, bypass LLM */
        reply = handle_slash_command(&msg);

        /* Prompt injection check — before LLM */
        if (!reply && tool_guard_check_injection(msg.content)) {
            syslog(LOG_WARNING,
                "[%s] Prompt injection blocked from %s:%s\n",
                TAG, msg.channel, msg.chat_id);
            reply = strdup("I can't do that.");
            if (!reply) {
                /* Fail-closed: skip this message entirely */
                goto task_done;
            }
        }

        /* Explicit Feishu side effects must not depend on an LLM network
         * round trip.  Other Feishu messages continue through the normal
         * fast paths and ReAct planner. */
        if (!reply && strcmp(msg.channel, AGENT_CHAN_FEISHU) == 0) {
            reply = handle_feishu_action_fast_path(&msg);
        }

        /* A direct voice command such as "向前一步走" is an explicit
         * physical side effect.  Execute it before the LLM path so a textual
         * answer cannot silently replace the robot_move tool call. */
        if (!reply && strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
            reply = handle_motion_fast_path(msg.content);
        }

        /* NL fast path — after injection check, before LLM */
        if (!reply) {
            reply = handle_nl_fast_path(msg.content);
        }

send_reply:
        if (reply) {
            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id,
                sizeof(out.chat_id) - 1);
            out.flags = msg.flags;
            out.content = reply;
            if (message_bus_reply_and_wait(&out) != OK) {
                free(reply);
            }
            goto task_done;
        }

        /* Hot-reload skills if directory changed */
        if (skill_loader_check_changed()) {
            syslog(LOG_INFO, "[%s] Skills changed, refreshing\n", TAG);
            skill_loader_refresh();
        }

        context_build_system_prompt(sys_prompt, ctx_size);
        inject_session_context(sys_prompt, ctx_size,
            msg.channel, msg.chat_id);

        /* Refresh tools JSON — Node tools are dynamic */
        free(tools_json);
        tools_json = tool_registry_get_tools_json();

        cJSON* messages = build_messages(msg.chat_id, msg.content,
            history_json, hist_size);

        char* final_text = NULL;

        /* Vision path */
        final_text = handle_vision_message(&msg);
        if (final_text) {
            cJSON_Delete(messages);
        } else {
            /* ReAct loop */
            s_llm_path_count++;
            final_text = run_react_loop(sys_prompt, messages,
                tools_json, tool_output, tool_size, &msg);
            cJSON_Delete(messages);
        }

        dispatch_response(&msg, final_text);

task_done:
        if (task_rc == OK)
            tool_media_task_end(!agent_shutdown_requested());
        message_bus_msg_free(&msg);
    }

    /* Cleanup (unreachable in normal operation) */
    free(tools_json);
    free(sys_prompt);
    free(history_json);
    free(tool_output);
    return NULL;
}

/* ── Public interface ─────────────────────────────────────── */

int agent_loop_init(void)
{
    syslog(LOG_INFO, "[%s] Agent loop initialized\n", TAG);
    return OK;
}

int agent_loop_start(void)
{
    int ret = agent_task_create(agent_loop_task, "agent_loop",
        AGENT_AI_AGENT_STACK, NULL, AGENT_AI_AGENT_PRIO);

    if (ret != OK) {
        syslog(LOG_ERR,
            "[%s] Failed to create agent_loop task\n", TAG);
    }
    return ret;
}
