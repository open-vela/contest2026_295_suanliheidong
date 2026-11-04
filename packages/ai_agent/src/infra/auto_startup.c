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

#include "infra/auto_startup.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "channels/feishu_bot.h"
#include "infra/network_manager.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "voice/voice_asr.h"
#include "voice/voice_channel.h"
#include "voice/voice_tts.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char *TAG = "auto_boot";
static volatile bool g_network_released;
static volatile bool g_network_ready;

static void retry_delay(void)
{
    sleep(2);
}

static int wait_wifi(void)
{
    unsigned int attempt = 0;

    for (;;) {
        int ret;

        attempt++;
        syslog(LOG_INFO, "[%s] step=set_wifi attempt=%u\n", TAG, attempt);
        ret = network_wifi_connect(NULL,
            CONFIG_AI_AGENT_AUTOSTART_WIFI_SSID,
            CONFIG_AI_AGENT_AUTOSTART_WIFI_PASSWORD);
        if (ret == OK && network_wait_connected(5000) == OK) {
            syslog(LOG_INFO, "[%s] step=set_wifi ready ip=%s\n", TAG,
                network_get_ip());
            return OK;
        }

        syslog(LOG_WARNING, "[%s] step=set_wifi failed rc=%d; retrying\n",
            TAG, ret);
        retry_delay();
    }
}

static int parse_endpoint(const char *endpoint, char *host, size_t host_cap,
    char *path, size_t path_cap)
{
    const char *begin;
    const char *slash;
    size_t host_len;

    if (!endpoint || !host || host_cap == 0 || !path || path_cap == 0) {
        return -EINVAL;
    }

    if (strncmp(endpoint, "https://", 8) == 0) {
        begin = endpoint + 8;
    } else if (strncmp(endpoint, "http://", 7) == 0) {
        begin = endpoint + 7;
    } else {
        return -EINVAL;
    }

    slash = strchr(begin, '/');
    host_len = slash ? (size_t)(slash - begin) : strlen(begin);
    if (host_len == 0 || host_len >= host_cap) {
        return -EINVAL;
    }

    memcpy(host, begin, host_len);
    host[host_len] = '\0';

    if (!slash || slash[1] == '\0' || strcmp(slash, "/v1") == 0 ||
        strcmp(slash, "/v1/") == 0) {
        snprintf(path, path_cap, "/v1/chat/completions");
    } else {
        if (strlen(slash) >= path_cap) {
            return -EINVAL;
        }
        snprintf(path, path_cap, "%s", slash);
    }

    return OK;
}

static int apply_llm(void)
{
    char host[128];
    char path[256];
    llm_backend_t backend;
    llm_backend_t applied;
    int ret;

    ret = parse_endpoint(CONFIG_AI_AGENT_AUTOSTART_LLM_ENDPOINT, host,
        sizeof(host), path, sizeof(path));
    if (ret < 0) {
        return ret;
    }

    memset(&backend, 0, sizeof(backend));
    snprintf(backend.host, sizeof(backend.host), "%s", host);
    snprintf(backend.path, sizeof(backend.path), "%s", path);
    snprintf(backend.port, sizeof(backend.port), "443");
    snprintf(backend.model, sizeof(backend.model), "%s",
        CONFIG_AI_AGENT_AUTOSTART_LLM_MODEL);
    snprintf(backend.api_key, sizeof(backend.api_key), "%s",
        CONFIG_AI_AGENT_AUTOSTART_LLM_API_KEY);
    backend.priority = 0;
    backend.cost_tier = 1;
    backend.enabled = true;

    ret = llm_router_set_backend(0, &backend);
    if (ret == OK) {
        ret = llm_router_apply(0);
    }
    if (ret != OK || llm_router_get_backend(0, &applied) != OK ||
        strcmp(applied.host, backend.host) != 0 ||
        strcmp(applied.model, backend.model) != 0 ||
        strcmp(applied.path, backend.path) != 0 ||
        strcmp(applied.api_key, backend.api_key) != 0) {
        return ret == OK ? -EIO : ret;
    }

    return OK;
}

static int wait_llm(void)
{
    unsigned int attempt = 0;

    for (;;) {
        int ret;

        attempt++;
        syslog(LOG_INFO, "[%s] step=set_llm attempt=%u\n", TAG, attempt);
        ret = apply_llm();
        if (ret == OK) {
            syslog(LOG_INFO, "[%s] step=set_llm ready\n", TAG);
            return OK;
        }

        syslog(LOG_WARNING, "[%s] step=set_llm failed rc=%d; retrying\n",
            TAG, ret);
        retry_delay();
    }
}

static int wait_voice_backend(const char *step, const char *name,
    int (*set_backend)(const char *), const char *(*get_backend)(void))
{
    unsigned int attempt = 0;

    for (;;) {
        int ret;

        attempt++;
        syslog(LOG_INFO, "[%s] step=%s attempt=%u\n", TAG, step, attempt);
        ret = set_backend(name);
        if (ret == OK && get_backend() && strcmp(get_backend(), name) == 0) {
            syslog(LOG_INFO, "[%s] step=%s ready\n", TAG, step);
            return OK;
        }

        syslog(LOG_WARNING, "[%s] step=%s failed rc=%d; retrying\n", TAG,
            step, ret);
        retry_delay();
    }
}

static int wait_feishu(void)
{
#ifdef CONFIG_AI_AGENT_FEISHU
    unsigned int attempt = 0;

    for (;;) {
        int ret;

        attempt++;
        syslog(LOG_INFO, "[%s] step=set_feishu_app attempt=%u\n", TAG,
            attempt);
        ret = feishu_set_app(CONFIG_AI_AGENT_AUTOSTART_FEISHU_APP_ID,
            CONFIG_AI_AGENT_AUTOSTART_FEISHU_APP_SECRET);
        if (ret == OK && feishu_get_app_id() &&
            strcmp(feishu_get_app_id(), CONFIG_AI_AGENT_AUTOSTART_FEISHU_APP_ID) == 0) {
            syslog(LOG_INFO, "[%s] step=set_feishu_app ready\n", TAG);
            return OK;
        }

        syslog(LOG_WARNING,
            "[%s] step=set_feishu_app failed rc=%d; retrying\n", TAG, ret);
        retry_delay();
    }
#else
    syslog(LOG_ERR, "[%s] step=set_feishu_app unavailable: CONFIG_AI_AGENT_FEISHU is disabled\n",
        TAG);
    return -ENOSYS;
#endif
}

static int wait_voice_start(void)
{
    unsigned int attempt = 0;

    for (;;) {
        int ret;

        attempt++;
        syslog(LOG_INFO, "[%s] step=voice_start attempt=%u\n", TAG,
            attempt);
        ret = voice_channel_start();
        if (ret == OK) {
            syslog(LOG_INFO, "[%s] step=voice_start ready\n", TAG);
            return OK;
        }

        syslog(LOG_WARNING, "[%s] step=voice_start failed rc=%d; retrying\n",
            TAG, ret);
        retry_delay();
    }
}

int agent_autostartup_run(void)
{
    int ret;

    ret = wait_wifi();
    if (ret != OK) {
        return ret;
    }

    ret = wait_llm();
    if (ret != OK) {
        return ret;
    }

    ret = wait_voice_backend("set_voice_asr",
        CONFIG_AI_AGENT_AUTOSTART_ASR_BACKEND,
        voice_asr_set_backend, voice_asr_get_backend);
    if (ret != OK) {
        return ret;
    }

    ret = wait_voice_backend("set_voice_tts",
        CONFIG_AI_AGENT_AUTOSTART_TTS_BACKEND,
        voice_tts_set_backend, voice_tts_get_backend);
    if (ret != OK) {
        return ret;
    }

    ret = wait_feishu();
    if (ret != OK) {
        return ret;
    }

    /* Let the network watcher start Feishu/Agent services only now. */
    agent_autostartup_release_network();
    /* Do not start voice capture while the watcher is still doing the first
     * time sync and launching network-facing services. */
    agent_autostartup_wait_network_ready();

    return wait_voice_start();
}

void agent_autostartup_release_network(void)
{
    g_network_released = true;
}

void agent_autostartup_wait_network_release(void)
{
    while (!g_network_released) {
        usleep(100000);
    }
}

void agent_autostartup_mark_network_ready(void)
{
    g_network_ready = true;
}

void agent_autostartup_wait_network_ready(void)
{
    while (!g_network_ready) {
        usleep(100000);
    }
}
