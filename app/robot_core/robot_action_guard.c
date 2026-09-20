/*
 * Contest-local cache guard for physical side-effect Tools.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <llm/llm_cache.h>

#include "robot_action_guard.h"

#define ROBOT_ACTION_CACHE_GUARD_WINDOW_MS (30ULL * 1000ULL)
struct robot_action_guard_state_s
{
  pthread_mutex_t lock;
  bool initialized;
  uint64_t guard_until_ms;
  uint32_t generation;
};

static struct robot_action_guard_state_s g_guard =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
};

static uint64_t robot_action_guard_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *robot_action_type_name(enum robot_action_type_e type)
{
  switch (type)
    {
      case ROBOT_ACTION_EXPRESSION:
        return "expression";
      case ROBOT_ACTION_MOTION:
        return "motion";
      case ROBOT_ACTION_POSTURE:
        return "posture";
      case ROBOT_ACTION_BEHAVIOR:
        return "behavior";
      case ROBOT_ACTION_AUDIO:
        return "audio";
      case ROBOT_ACTION_SYSTEM:
        return "system";
      default:
        return "unknown";
    }
}

int robot_action_guard_init(void)
{
  pthread_mutex_lock(&g_guard.lock);
  if (g_guard.initialized)
    {
      pthread_mutex_unlock(&g_guard.lock);
      return 0;
    }

  g_guard.initialized = true;
  pthread_mutex_unlock(&g_guard.lock);
  /*
   * Cache invalidation is deliberately synchronous.  A periodic worker that
   * frees cache entries while the Agent builds cJSON/LLM requests can race
   * the shared heap and turn a harmless Tool call into a free-list panic.
   * begin/end still invalidate at both physical-action boundaries.
   */
  printf("[ROBOT-ACTION] initialized synchronous cache guard\n");
  return 0;
}

static uint32_t robot_action_guard_extend(enum robot_action_type_e type,
                                          const char *tool_name,
                                          bool begin)
{
  uint64_t now;
  uint64_t requested_until;
  uint32_t generation;
  bool extended;

  now = robot_action_guard_now_ms();
  requested_until = now + ROBOT_ACTION_CACHE_GUARD_WINDOW_MS;

  pthread_mutex_lock(&g_guard.lock);
  extended = requested_until > g_guard.guard_until_ms;
  if (extended)
    {
      g_guard.guard_until_ms = requested_until;
    }
  if (begin)
    {
      g_guard.generation++;
    }
  generation = g_guard.generation;
  pthread_mutex_unlock(&g_guard.lock);

  printf("[ROBOT-ACTION] %s type=%s tool=%s gen=%lu\n",
         begin ? "begin" : "end", robot_action_type_name(type),
         tool_name != NULL ? tool_name : "unknown",
         (unsigned long)generation);
  if (extended)
    {
      printf("[ROBOT-ACTION] guard window %s\n",
             begin ? "started" : "extended");
    }

  return generation;
}

void robot_action_guard_begin(enum robot_action_type_e type,
                              const char *tool_name)
{
  int ret;

  ret = robot_action_guard_init();
  if (ret < 0)
    {
      /* A cache guard failure must never block the physical action. */
      printf("[ROBOT-ACTION] begin guard unavailable; fail-open\n");
      return;
    }

  (void)robot_action_guard_extend(type, tool_name, true);
  llm_cache_invalidate();
}

void robot_action_guard_end(enum robot_action_type_e type,
                            const char *tool_name,
                            int action_result)
{
  int ret;

  ret = robot_action_guard_init();
  if (ret < 0)
    {
      printf("[ROBOT-ACTION] end type=%s tool=%s rc=%d fail-open\n",
             robot_action_type_name(type),
             tool_name != NULL ? tool_name : "unknown", action_result);
      return;
    }

  (void)robot_action_guard_extend(type, tool_name, false);
  printf("[ROBOT-ACTION] action-result rc=%d\n", action_result);
  llm_cache_invalidate();
}
