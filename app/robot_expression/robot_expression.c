/*
 * Contest-local priority arbiter and timeout worker.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "robot_expression.h"
#include "robot_oled.h"

#define ROBOT_EXPRESSION_MAX_MS     10000
#define ROBOT_EXPRESSION_STACK      8192
#define ROBOT_EXPRESSION_POLL_US    (200 * 1000)

struct robot_expression_slot_s
{
  bool active;
  enum robot_expression_e expression;
  uint64_t expires_ms;
  uint32_t generation;
};

static const unsigned int g_source_priority[ROBOT_EXPRESSION_SOURCE_COUNT] =
{
  [ROBOT_EXPRESSION_SOURCE_SYSTEM] = 100,
  [ROBOT_EXPRESSION_SOURCE_AGENT] = 70,
  [ROBOT_EXPRESSION_SOURCE_REACTION] = 60,
  [ROBOT_EXPRESSION_SOURCE_VOICE] = 50,
  [ROBOT_EXPRESSION_SOURCE_BEHAVIOR] = 40,
  [ROBOT_EXPRESSION_SOURCE_MUSIC] = 30,
};

static const char *const g_source_names[ROBOT_EXPRESSION_SOURCE_COUNT] =
{
  [ROBOT_EXPRESSION_SOURCE_SYSTEM] = "system",
  [ROBOT_EXPRESSION_SOURCE_AGENT] = "agent",
  [ROBOT_EXPRESSION_SOURCE_REACTION] = "reaction",
  [ROBOT_EXPRESSION_SOURCE_VOICE] = "voice",
  [ROBOT_EXPRESSION_SOURCE_BEHAVIOR] = "behavior",
  [ROBOT_EXPRESSION_SOURCE_MUSIC] = "music",
};

static const char *const g_expression_names[ROBOT_EXPRESSION_COUNT] =
{
  [ROBOT_EXPRESSION_IDLE] = "idle",
  [ROBOT_EXPRESSION_LISTENING] = "listening",
  [ROBOT_EXPRESSION_THINKING] = "thinking",
  [ROBOT_EXPRESSION_HAPPY] = "happy",
  [ROBOT_EXPRESSION_SPEAKING] = "speaking",
  [ROBOT_EXPRESSION_ERROR] = "error",
  [ROBOT_EXPRESSION_EXCITED] = "excited",
  [ROBOT_EXPRESSION_WINK] = "wink",
  [ROBOT_EXPRESSION_LOVE] = "love",
  [ROBOT_EXPRESSION_SURPRISED] = "surprised",
  [ROBOT_EXPRESSION_COOL] = "cool",
  [ROBOT_EXPRESSION_PLAYFUL] = "playful",
  [ROBOT_EXPRESSION_SLEEPY] = "sleepy",
  [ROBOT_EXPRESSION_SINGING] = "singing",
  [ROBOT_EXPRESSION_CURIOUS] = "curious",
  [ROBOT_EXPRESSION_PROUD] = "proud",
};

static struct robot_expression_slot_s g_slots[ROBOT_EXPRESSION_SOURCE_COUNT];
static pthread_mutex_t g_expression_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_expression_thread;
static bool g_expression_initialized;
static bool g_expression_thread_valid;
static bool g_expression_stop;
static bool g_display_valid;
static enum robot_expression_e g_effective = ROBOT_EXPRESSION_IDLE;

static uint64_t robot_expression_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000u +
         (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *robot_expression_source_name(
    enum robot_expression_source_e source)
{
  if (source < 0 || source >= ROBOT_EXPRESSION_SOURCE_COUNT)
    {
      return "unknown";
    }

  return g_source_names[source];
}

const char *robot_expression_name(enum robot_expression_e expression)
{
  if (expression < 0 || expression >= ROBOT_EXPRESSION_COUNT)
    {
      return "unknown";
    }

  return g_expression_names[expression];
}

static int robot_expression_submit_display(
    enum robot_expression_e expression)
{
  int ret = robot_oled_submit(expression);

  pthread_mutex_lock(&g_expression_lock);
  g_display_valid = ret == 0;
  pthread_mutex_unlock(&g_expression_lock);

  if (ret < 0)
    {
      printf("[RV-EXPR] display submit failed expression=%s rc=%d\n",
             robot_expression_name(expression), ret);
      return ret;
    }

  printf("[RV-EXPR] display request=%s\n",
         robot_expression_name(expression));
  return 0;
}

static enum robot_expression_e
robot_expression_recompute_locked(uint64_t now_ms, bool *changed)
{
  unsigned int best_priority = 0;
  enum robot_expression_e next = ROBOT_EXPRESSION_IDLE;
  unsigned int source;

  for (source = 0; source < ROBOT_EXPRESSION_SOURCE_COUNT; source++)
    {
      struct robot_expression_slot_s *slot = &g_slots[source];

      if (!slot->active)
        {
          continue;
        }

      if (slot->expires_ms != 0 && slot->expires_ms <= now_ms)
        {
          printf("[RV-EXPR] expire source=%s expression=%s generation=%u\n",
                 robot_expression_source_name(source),
                 robot_expression_name(slot->expression),
                 slot->generation);
          slot->active = false;
          slot->generation++;
          continue;
        }

      if (g_source_priority[source] >= best_priority)
        {
          best_priority = g_source_priority[source];
          next = slot->expression;
        }
    }

  *changed = next != g_effective;
  if (*changed)
    {
      g_effective = next;
    }

  return next;
}

static void *robot_expression_worker(void *arg)
{
  (void)arg;

  for (;;)
    {
      bool changed = false;
      bool display_valid;
      bool need_submit;
      enum robot_expression_e expression;

      /*
       * Expression expiry does not need a 50 ms scheduler tick.  A slower
       * poll substantially reduces SMP scheduling churn while Wi-Fi/audio
       * services are active, while still keeping short expressions visually
       * responsive (worst-case expiry delay is about 200 ms).
       */
      usleep(ROBOT_EXPRESSION_POLL_US);

      pthread_mutex_lock(&g_expression_lock);
      if (g_expression_stop)
        {
          pthread_mutex_unlock(&g_expression_lock);
          break;
        }

      expression =
          robot_expression_recompute_locked(robot_expression_now_ms(),
                                            &changed);
      display_valid = g_display_valid;
      pthread_mutex_unlock(&g_expression_lock);

      /*
       * If a previous submission failed, retry only after the OLED worker
       * reports itself available.  This avoids a 50 ms error-log loop when
       * the panel is genuinely unavailable.
       */

      need_submit =
          changed || (!display_valid && robot_oled_is_available());

      if (need_submit)
        {
          (void)robot_expression_submit_display(expression);

          if (changed)
            {
              printf("[RV-EXPR] effective=%s\n",
                     robot_expression_name(expression));
            }
        }
    }

  return NULL;
}

int robot_expression_init(void)
{
  pthread_attr_t attr;
  int ret;
  int display_ret;

  pthread_mutex_lock(&g_expression_lock);
  if (g_expression_initialized)
    {
      pthread_mutex_unlock(&g_expression_lock);
      return 0;
    }

  memset(g_slots, 0, sizeof(g_slots));
  g_effective = ROBOT_EXPRESSION_IDLE;
  g_display_valid = false;
  g_expression_stop = false;
  g_expression_initialized = true;
  pthread_mutex_unlock(&g_expression_lock);

  printf("[RV-EXPR] init begin\n");

  ret = robot_oled_init();
  if (ret < 0)
    {
      printf("[RV-EXPR] OLED worker unavailable rc=%d\n", ret);
    }

  /*
   * Always submit the first idle frame.  Do not let software state
   * "effective == idle" suppress the very first physical display update.
   * robot_oled_submit() is allowed to queue this before the OLED worker has
   * finished board initialization.
   */

  display_ret = robot_expression_submit_display(ROBOT_EXPRESSION_IDLE);
  if (display_ret < 0)
    {
      printf("[RV-EXPR] initial idle submit failed rc=%d\n", display_ret);
    }

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&attr, ROBOT_EXPRESSION_STACK);
      if (ret == 0)
        {
          ret = pthread_create(&g_expression_thread, &attr,
                               robot_expression_worker, NULL);
        }

      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      pthread_mutex_lock(&g_expression_lock);
      g_expression_thread_valid = false;
      g_expression_initialized = false;
      pthread_mutex_unlock(&g_expression_lock);
      printf("[RV-EXPR] timeout worker create failed rc=%d\n", ret);
      return -ret;
    }

  pthread_mutex_lock(&g_expression_lock);
  g_expression_thread_valid = true;
  pthread_mutex_unlock(&g_expression_lock);

  printf("[RV-EXPR] init success stack=%u poll_ms=%u\n",
         (unsigned int)ROBOT_EXPRESSION_STACK,
         (unsigned int)(ROBOT_EXPRESSION_POLL_US / 1000));
  return 0;
}

int robot_expression_deinit(void)
{
  bool join_needed;

  pthread_mutex_lock(&g_expression_lock);
  join_needed = g_expression_thread_valid;
  g_expression_stop = true;
  pthread_mutex_unlock(&g_expression_lock);

  if (join_needed)
    {
      pthread_join(g_expression_thread, NULL);
    }

  (void)robot_oled_stop();

  pthread_mutex_lock(&g_expression_lock);
  g_expression_thread_valid = false;
  g_expression_initialized = false;
  g_display_valid = false;
  pthread_mutex_unlock(&g_expression_lock);
  return 0;
}

/*
 * duration_ms semantics:
 *   0  -> persistent until this source is explicitly changed or cleared
 *   >0 -> temporary request that expires after duration_ms
 *
 * Agent Tool calls intentionally use 0 so the selected facial expression
 * remains visible until the next requested expression.
 */
int robot_expression_set(enum robot_expression_source_e source,
                         enum robot_expression_e expression,
                         unsigned int duration_ms)
{
  bool changed = false;
  bool need_submit;
  enum robot_expression_e requested = expression;
  uint64_t now_ms;
  int ret;

  if (source < 0 || source >= ROBOT_EXPRESSION_SOURCE_COUNT ||
      expression < 0 || expression >= ROBOT_EXPRESSION_COUNT)
    {
      return -EINVAL;
    }

  if (duration_ms > ROBOT_EXPRESSION_MAX_MS)
    {
      return -ERANGE;
    }

  if (robot_expression_init() < 0)
    {
      return -EIO;
    }

  now_ms = robot_expression_now_ms();

  pthread_mutex_lock(&g_expression_lock);
  g_slots[source].active = true;
  g_slots[source].expression = requested;
  g_slots[source].generation++;
  g_slots[source].expires_ms =
      duration_ms == 0 ? 0 : now_ms + duration_ms;

  (void)robot_expression_recompute_locked(now_ms, &changed);
  expression = g_effective;
  need_submit = changed || !g_display_valid;
  pthread_mutex_unlock(&g_expression_lock);

  if (need_submit)
    {
      ret = robot_expression_submit_display(expression);
      if (ret < 0)
        {
          return ret;
        }
    }

  printf("[RV-EXPR] set source=%s requested=%s effective=%s duration=%u\n",
         robot_expression_source_name(source),
         robot_expression_name(requested),
         robot_expression_name(expression),
         duration_ms);

  return 0;
}

int robot_expression_clear(enum robot_expression_source_e source)
{
  bool changed = false;
  bool need_submit;
  enum robot_expression_e expression;
  int ret;

  if (source < 0 || source >= ROBOT_EXPRESSION_SOURCE_COUNT)
    {
      return -EINVAL;
    }

  if (robot_expression_init() < 0)
    {
      return -EIO;
    }

  pthread_mutex_lock(&g_expression_lock);
  g_slots[source].active = false;
  g_slots[source].generation++;

  expression =
      robot_expression_recompute_locked(robot_expression_now_ms(),
                                        &changed);
  need_submit = changed || !g_display_valid;
  pthread_mutex_unlock(&g_expression_lock);

  if (need_submit)
    {
      ret = robot_expression_submit_display(expression);
      if (ret < 0)
        {
          return ret;
        }
    }

  printf("[RV-EXPR] clear source=%s effective=%s\n",
         robot_expression_source_name(source),
         robot_expression_name(expression));
  return 0;
}

bool robot_expression_is_available(void)
{
  return robot_oled_is_available();
}
