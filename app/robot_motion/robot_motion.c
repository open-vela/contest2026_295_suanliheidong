/*
 * Contest-local POWER_SAFE gait and single-request motion worker.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "robot_motion.h"
#include "robotctl_servo.h"

extern void robot_world_state_set_motion_active(bool) __attribute__((weak));
extern bool robot_music_media_is_active(void) __attribute__((weak));
extern bool robot_music_media_motion_allowed(uint32_t) __attribute__((weak));
extern int robot_music_media_claim_tail(uint32_t, uint32_t) __attribute__((weak));
extern int robot_music_media_claim_periodic_tail(uint32_t, uint32_t) __attribute__((weak));

static bool robot_motion_music_active(void)
{ return robot_music_media_is_active && robot_music_media_is_active(); }

#define ROBOT_MOTION_PHASE_COUNT 8
#define ROBOT_MOTION_HOME_SETTLE_MS 200
#define ROBOT_MOTION_WORKER_STACK 8192

/* Increase limb movement time by 25% to reduce acceleration and peak load.
 * Tail motion uses a separate timing profile below. */
#define ROBOT_MOTION_LIMB_TIME_NUM 5
#define ROBOT_MOTION_LIMB_TIME_DEN 4
/* Keep the home profile at the same conservative cadence as gait motion.
 * The previous fixed home profile used 30 ms; retaining that floor avoids a
 * fast reset when the requested gait period maps to a 25 ms interval. */
#define ROBOT_MOTION_LIMB_MIN_INTERVAL_MS 30

#define ROBOT_MOTION_PHASE(rf, rr, lr, lf, first, second) \
  { {rf, rr, lr, lf, 85}, {first, second} }

struct robot_motion_phase_s
{
  int target[ROBOTCTL_SERVO_COUNT];
  enum robotctl_servo_e order[2];
};

struct robot_motion_gait_s
{
  const char *name;
  struct robot_motion_phase_s phase[ROBOT_MOTION_PHASE_COUNT];
};

enum robot_motion_phase_power_e
{
  ROBOT_MOTION_PHASE_LIGHT = 0,
  ROBOT_MOTION_PHASE_MEDIUM,
  ROBOT_MOTION_PHASE_HEAVY,
};

struct robot_motion_context_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t worker;
  bool initializing;
  bool worker_started;
  bool request_pending;
  enum robot_motion_state_e state;
  struct robot_motion_request_s request;
  uint32_t next_request_id;
  uint32_t completed_request_id;
  bool cancel_requested;
  int last_result;
  int music_tail_direction;
};

static struct robot_motion_context_s g_motion =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER,
  .state = ROBOT_MOTION_STATE_IDLE,
  .music_tail_direction = -1,
};

static uint64_t robot_motion_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool robot_motion_cancel_check(void *arg)
{
  bool cancel_requested;

  (void)arg;
  pthread_mutex_lock(&g_motion.lock);
  cancel_requested = g_motion.cancel_requested;
  enum robot_motion_source_e source = g_motion.request.source;
  enum robot_motion_request_type_e type = g_motion.request.type;
  uint32_t session = g_motion.request.music_session_id;
  pthread_mutex_unlock(&g_motion.lock);
  if (source == ROBOT_MOTION_SOURCE_MUSIC)
    return cancel_requested || !robot_music_media_motion_allowed ||
           !robot_music_media_motion_allowed(session);
  return cancel_requested ||
         (type == ROBOT_MOTION_REQUEST_GAIT && robot_motion_music_active());
}

static const enum robotctl_servo_e g_home_order[ROBOTCTL_SERVO_COUNT] =
{
  ROBOTCTL_LF, ROBOTCTL_RF, ROBOTCTL_LR, ROBOTCTL_RR, ROBOTCTL_TAIL
};

static const struct robot_motion_gait_s g_forward =
{
  "forward",
  {
    ROBOT_MOTION_PHASE(135, 90, 45, 90, ROBOTCTL_LR, ROBOTCTL_RF),
    ROBOT_MOTION_PHASE(135, 45, 45, 135, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 45, 90, 135, ROBOTCTL_LR, ROBOTCTL_RF),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 135, 90, 45, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(45, 135, 135, 45, ROBOTCTL_LR, ROBOTCTL_RF),
    ROBOT_MOTION_PHASE(45, 90, 135, 90, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_LR, ROBOTCTL_RF)
  }
};

static const struct robot_motion_gait_s g_backward =
{
  "backward",
  {
    ROBOT_MOTION_PHASE(45, 90, 135, 90, ROBOTCTL_LR, ROBOTCTL_RF),
    ROBOT_MOTION_PHASE(45, 135, 135, 45, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 135, 90, 45, ROBOTCTL_LR, ROBOTCTL_RF),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 45, 90, 135, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(135, 45, 45, 135, ROBOTCTL_LR, ROBOTCTL_RF),
    ROBOT_MOTION_PHASE(135, 90, 45, 90, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_LR, ROBOTCTL_RF)
  }
};

static const struct robot_motion_gait_s g_left =
{
  "left",
  {
    ROBOT_MOTION_PHASE(135, 90, 135, 90, ROBOTCTL_RF, ROBOTCTL_LR),
    ROBOT_MOTION_PHASE(135, 45, 135, 45, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 45, 90, 45, ROBOTCTL_RF, ROBOTCTL_LR),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 135, 90, 135, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(45, 135, 45, 135, ROBOTCTL_RF, ROBOTCTL_LR),
    ROBOT_MOTION_PHASE(45, 90, 45, 90, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_RF, ROBOTCTL_LR)
  }
};

static const struct robot_motion_gait_s g_right =
{
  "right",
  {
    ROBOT_MOTION_PHASE(45, 90, 45, 90, ROBOTCTL_RF, ROBOTCTL_LR),
    ROBOT_MOTION_PHASE(45, 135, 45, 135, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 135, 90, 135, ROBOTCTL_RF, ROBOTCTL_LR),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_LF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(45, 45, 90, 90, ROBOTCTL_RF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(135, 45, 135, 90, ROBOTCTL_RF, ROBOTCTL_LR),
    ROBOT_MOTION_PHASE(90, 90, 135, 90, ROBOTCTL_RF, ROBOTCTL_RR),
    ROBOT_MOTION_PHASE(90, 90, 90, 90, ROBOTCTL_RF, ROBOTCTL_LR)
  }
};

static const struct robot_motion_gait_s *
robot_motion_gait(enum robot_motion_direction_e direction)
{
  switch (direction)
    {
      case ROBOT_MOTION_FORWARD:
        return &g_forward;
      case ROBOT_MOTION_BACKWARD:
        return &g_backward;
      case ROBOT_MOTION_LEFT:
        return &g_left;
      case ROBOT_MOTION_RIGHT:
        return &g_right;
      default:
        return NULL;
    }
}

static int robot_motion_scale_interval(int interval_ms)
{
  return (interval_ms * ROBOT_MOTION_LIMB_TIME_NUM +
          ROBOT_MOTION_LIMB_TIME_DEN - 1) /
         ROBOT_MOTION_LIMB_TIME_DEN;
}

static int robot_motion_safe_interval(unsigned int period_ms)
{
  /* Keep the 3-degree POWER_SAFE increments.  The additional 5/4 time
   * scale lowers limb speed by approximately 25% without changing gait
   * geometry or the independent tail profile. */
  int interval_ms = (int)period_ms / 125;

  if (interval_ms < 12)
    {
      interval_ms = 12;
    }
  else if (interval_ms > 24)
    {
      interval_ms = 24;
    }

  interval_ms = robot_motion_scale_interval(interval_ms);
  if (interval_ms < ROBOT_MOTION_LIMB_MIN_INTERVAL_MS)
    {
      interval_ms = ROBOT_MOTION_LIMB_MIN_INTERVAL_MS;
    }

  return interval_ms;
}

int robot_motion_limb_interval(unsigned int period_ms)
{
  return robot_motion_safe_interval(period_ms);
}

static int robot_motion_safe_settle(unsigned int period_ms)
{
  int settle_ms = (int)period_ms / 20;

  if (settle_ms < 80)
    {
      settle_ms = 80;
    }
  else if (settle_ms > 150)
    {
      settle_ms = 150;
    }

  return settle_ms;
}

/* One-degree pulses separated by unloaded recovery gaps. The request's
 * amplitude is the actual endpoint offset; only the tail channel is touched. */
#define ROBOT_MOTION_TAIL_SEGMENT_DEG 1
#define ROBOT_MOTION_TAIL_OPEN_RECOVERY_MS 500
#define ROBOT_MOTION_TAIL_ENDPOINT_SETTLE_MS 20

static int robot_motion_tail_interval(unsigned int period_ms)
{
  (void)period_ms;
  return 20;
}

static int robot_motion_tail_reset_interval(void)
{
  return robot_motion_limb_interval(ROBOT_MOTION_DEFAULT_WALK_PERIOD);
}

static int robot_motion_tail_cooldown_ms(unsigned int period_ms,
                                         unsigned int amplitude_deg)
{
  int interval_ms = robot_motion_tail_interval(period_ms);

  /* center -> +A -> -A -> center has 4*A one-degree movements. */
  return (int)(period_ms / (4U * amplitude_deg)) -
         interval_ms - ROBOT_MOTION_TAIL_ENDPOINT_SETTLE_MS;
}

static int robot_motion_tail_cooldown(int cooldown_ms)
{
  while (cooldown_ms > 0)
    {
      int slice_ms = cooldown_ms > 25 ? 25 : cooldown_ms;

      if (robot_motion_cancel_check(NULL))
        {
          return -ECANCELED;
        }

      usleep((useconds_t)slice_ms * 1000);
      cooldown_ms -= slice_ms;
    }

  return 0;
}

static int robot_motion_tail_micro_move(struct robotctl_servo_ctx_s *ctx,
                                        int target, int interval_ms,
                                        int cooldown_ms)
{
  int ret;

  ret = robotctl_servo_move_one(ctx, ROBOTCTL_TAIL, target,
                                interval_ms,
                                ROBOT_MOTION_TAIL_ENDPOINT_SETTLE_MS);
  if (ret < 0)
    {
      return ret;
    }

  /*
   * robotctl_servo_move_one() stops PWM when the micro-move completes.  Keep
   * the rail unloaded briefly before the next one-degree segment.
   */
  return robot_motion_tail_cooldown(cooldown_ms);
}

static int robot_motion_tail_ramp_to(struct robotctl_servo_ctx_s *ctx,
                                     int target, int interval_ms,
                                     int cooldown_ms)
{
  int current;
  int next;
  int delta;
  int ret;

  if (ctx == NULL)
    {
      return -EINVAL;
    }

  current = ctx->angle[ROBOTCTL_TAIL];
  while (current != target)
    {
      if (robot_motion_cancel_check(NULL))
        {
          return -ECANCELED;
        }

      delta = target - current;
      next = current + (delta > 0 ? ROBOT_MOTION_TAIL_SEGMENT_DEG :
                                     -ROBOT_MOTION_TAIL_SEGMENT_DEG);

      printf("[ROBOT-MOTION] tail soft-step %d -> %d target=%d\n",
             current, next, target);
      ret = robot_motion_tail_micro_move(ctx, next, interval_ms, cooldown_ms);
      if (ret < 0)
        {
          return ret;
        }

      current = ctx->angle[ROBOTCTL_TAIL];
    }

  return 0;
}

static int robot_motion_home_power_safe(struct robotctl_servo_ctx_s *ctx,
                                        bool include_tail,
                                        int interval_ms)
{
  int count = include_tail ? ROBOTCTL_SERVO_COUNT : ROBOTCTL_SERVO_COUNT - 1;
  int i;

  if (interval_ms <= 0)
    {
      interval_ms = robot_motion_safe_interval(
        ROBOT_MOTION_DEFAULT_WALK_PERIOD);
    }

  if (interval_ms < ROBOT_MOTION_LIMB_MIN_INTERVAL_MS)
    {
      interval_ms = ROBOT_MOTION_LIMB_MIN_INTERVAL_MS;
    }

  printf("[ROBOT-MOTION] home profile interval=%d settle=%d\n",
         interval_ms, ROBOT_MOTION_HOME_SETTLE_MS);

  for (i = 0; i < count; i++)
    {
      enum robotctl_servo_e servo = g_home_order[i];
      int ret;

      if (robot_motion_music_active()) return -ECANCELED;
      printf("[ROBOT-MOTION] home move %s %d -> %d update=%s\n",
             robotctl_servo_name(servo), ctx->angle[servo],
             g_robotctl_home[servo],
             ctx->angle_known[servo] &&
             ctx->angle[servo] == g_robotctl_home[servo] ? "no" : "yes");
      ret = robotctl_servo_move_one(ctx, servo, g_robotctl_home[servo],
                                    interval_ms,
                                    ROBOT_MOTION_HOME_SETTLE_MS);
      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}

static enum robot_motion_phase_power_e robot_motion_phase_power(
    const struct robotctl_servo_ctx_s *ctx,
    const struct robot_motion_phase_s *phase, unsigned int *active)
{
  unsigned int count = 0;
  unsigned int delta = 0;
  unsigned int order;

  for (order = 0; order < 2; order++)
    {
      enum robotctl_servo_e servo = phase->order[order];
      int difference = phase->target[servo] - ctx->angle[servo];

      if (difference != 0)
        {
          count++;
          delta += (unsigned int)abs(difference);
        }
    }

  if (active != NULL)
    {
      *active = count;
    }

  if (count >= 2 && delta >= 45)
    {
      return ROBOT_MOTION_PHASE_HEAVY;
    }

  return count == 0 ? ROBOT_MOTION_PHASE_LIGHT :
                      ROBOT_MOTION_PHASE_MEDIUM;
}

static const char *robot_motion_phase_power_name(
    enum robot_motion_phase_power_e power)
{
  static const char *const names[] =
  {
    "light", "medium", "heavy"
  };

  return power >= ROBOT_MOTION_PHASE_LIGHT &&
         power <= ROBOT_MOTION_PHASE_HEAVY ? names[power] : "unknown";
}

static int robot_motion_phase_settle(int settle_ms,
                                     enum robot_motion_phase_power_e power)
{
  if (power == ROBOT_MOTION_PHASE_LIGHT)
    {
      settle_ms = settle_ms * 3 / 4;
      return settle_ms < 60 ? 60 : settle_ms;
    }

  if (power == ROBOT_MOTION_PHASE_HEAVY)
    {
      settle_ms = settle_ms * 5 / 4;
      return settle_ms > 200 ? 200 : settle_ms;
    }

  return settle_ms;
}

static int robot_motion_execute_gait(
    const struct robot_motion_request_s *request)
{
  const struct robot_motion_gait_s *gait =
    robot_motion_gait(request->direction);
  struct robotctl_servo_ctx_s ctx;
  int interval_ms;
  int settle_ms;
  int ret;
  unsigned int cycle;
  unsigned int phase;
  unsigned int order;
  unsigned int active_servos;
  unsigned int heavy_phases = 0;
  enum robot_motion_phase_power_e power;
  int phase_settle_ms;

  if (gait == NULL)
    {
      return -EINVAL;
    }

  printf("[ROBOT-MOTION] backend init begin\n");
  ret = robotctl_servo_open(&ctx);
  printf("[ROBOT-MOTION] backend init done rc=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  ctx.cancel_check = robot_motion_cancel_check;
  ctx.cancel_arg = NULL;

  interval_ms = robot_motion_safe_interval(request->period_ms);
  settle_ms = robot_motion_safe_settle(request->period_ms);
  printf("[ROBOT-MOTION] power-aware profile requested=%u start=%d "
         "cruise=%d end=%d stagger=0 max_active=1 settle=%d\n",
         request->period_ms, interval_ms + interval_ms / 2, interval_ms,
         interval_ms + interval_ms / 4, settle_ms);
  printf("[ROBOT-MOTION] preparing request id=%lu home_required=yes\n",
         (unsigned long)request->request_id);
  ret = robot_motion_home_power_safe(&ctx, false, interval_ms);
  if (ret >= 0)
    {
      for (cycle = 0; cycle < request->steps && ret >= 0; cycle++)
        {
          for (phase = 0; phase < ROBOT_MOTION_PHASE_COUNT && ret >= 0;
               phase++)
            {
              if (robot_motion_cancel_check(NULL))
                {
                  ret = -ECANCELED;
                  break;
                }

              power = robot_motion_phase_power(&ctx, &gait->phase[phase],
                                              &active_servos);
              phase_settle_ms = robot_motion_phase_settle(settle_ms, power);
              if (power == ROBOT_MOTION_PHASE_HEAVY)
                {
                  heavy_phases++;
                }
              printf("[ROBOT-MOTION] phase=%u class=%s active_servos=%u "
                     "settle=%d\n", phase + 1,
                     robot_motion_phase_power_name(power), active_servos,
                     phase_settle_ms);
              printf("[ROBOT-MOTION] %s phase %u/%d id=%lu\n",
                     gait->name, phase + 1, ROBOT_MOTION_PHASE_COUNT,
                     (unsigned long)request->request_id);
              for (order = 0; order < 2; order++)
                {
                  enum robotctl_servo_e servo = gait->phase[phase].order[order];

                  printf("[ROBOT-MOTION] servo %s current=%d target=%d "
                         "update=%s\n",
                         robotctl_servo_name(servo), ctx.angle[servo],
                         gait->phase[phase].target[servo],
                         ctx.angle[servo] == gait->phase[phase].target[servo] ?
                         "no" : "yes");
                  if (ctx.angle[servo] == gait->phase[phase].target[servo])
                    {
                      continue;
                    }

                  ret = robotctl_servo_move_one(
                    &ctx, servo, gait->phase[phase].target[servo],
                    interval_ms, phase_settle_ms);
                  if (ret < 0)
                    {
                      break;
                    }
                }
            }
        }
    }

  if (ret >= 0)
    {
      ret = robot_motion_cancel_check(NULL) ? -ECANCELED :
            robot_motion_home_power_safe(&ctx, false, interval_ms);
    }

  printf("[ROBOT-MOTION] summary writes=%u skipped=%u total_angle_delta=%u "
         "heavy_phases=%u\n", ctx.command_count, ctx.skipped_count,
         ctx.total_angle_delta, heavy_phases);
  robotctl_servo_close(&ctx);
  return ret;
}

static int robot_motion_execute_tail_wag(
    const struct robot_motion_request_s *request)
{
  struct robotctl_servo_ctx_s ctx;
  const int center = g_robotctl_home[ROBOTCTL_TAIL];
  const int right = center + (int)request->amplitude_deg;
  const int left = center - (int)request->amplitude_deg;
  int interval_ms = robot_motion_tail_interval(request->period_ms);
  int reset_interval_ms = robot_motion_tail_reset_interval();
  int cooldown_ms = robot_motion_tail_cooldown_ms(request->period_ms, request->amplitude_deg);
  int ret;
  unsigned int cycle;

  printf("[ROBOT-MOTION] tail wag begin cycles=%u requested_amplitude=%u "
         "right_offset=%d left_offset=%d period_ms=%u interval_ms=%d "
         "cooldown_ms=%d segment_deg=%d profile=music_micro_wag\n",
         request->steps, request->amplitude_deg,
         (int)request->amplitude_deg, (int)request->amplitude_deg,
         request->period_ms, interval_ms, cooldown_ms,
         ROBOT_MOTION_TAIL_SEGMENT_DEG);

  ret = robotctl_servo_open(&ctx);
  if (ret < 0)
    {
      return ret;
    }

  ctx.cancel_check = robot_motion_cancel_check;
  ctx.cancel_arg = NULL;
  if (abs(ctx.angle[ROBOTCTL_TAIL] - center) > ROBOT_MOTION_TAIL_MAX_AMPLITUDE)
    {
      robotctl_servo_close(&ctx);
      return -ERANGE; /* Do not make a large centering move during music. */
    }

  /*
   * Give the supply rail time to settle after opening the PWM backend.  This
   * is especially useful when music/TTS/audio amplification is already active.
   */
  ret = robot_motion_tail_cooldown(ROBOT_MOTION_TAIL_OPEN_RECOVERY_MS);

  /* Return through center using only bounded one-degree tail pulses. */
  if (ret >= 0)
    {
      ret = robot_motion_tail_ramp_to(&ctx, center, reset_interval_ms,
                                      cooldown_ms);
    }

  for (cycle = 0; ret >= 0 && cycle < request->steps; cycle++)
    {
      if (robot_motion_cancel_check(NULL))
        {
          ret = -ECANCELED;
          break;
        }

      printf("[ROBOT-MOTION] tail wag cycle=%u/%u sweep=center->right\n",
             cycle + 1, request->steps);
      ret = robot_motion_tail_ramp_to(&ctx, right, interval_ms, cooldown_ms);

      if (ret >= 0)
        {
          printf("[ROBOT-MOTION] tail wag cycle=%u/%u sweep=right->left\n",
                 cycle + 1, request->steps);
          ret = robot_motion_tail_ramp_to(&ctx, left, interval_ms, cooldown_ms);
        }

      if (ret >= 0)
        {
          printf("[ROBOT-MOTION] tail wag cycle=%u/%u sweep=left->center\n",
                 cycle + 1, request->steps);
          ret = robot_motion_tail_ramp_to(&ctx, center,
                                          reset_interval_ms, cooldown_ms);
        }
    }

  if (ret >= 0 && ctx.angle[ROBOTCTL_TAIL] != center)
    {
      ret = robot_motion_tail_ramp_to(&ctx, center,
                                      reset_interval_ms, cooldown_ms);
    }

  if (ret >= 0) ret = robot_motion_tail_cooldown(ROBOT_MOTION_TAIL_OPEN_RECOVERY_MS);
  robotctl_servo_close(&ctx);
  printf("[ROBOT-MOTION] tail wag done rc=%d\n", ret);
  return ret;
}

/* Move one bounded music step. The companion scheduler submits this request
 * every 1000 ms, so the worker never holds the servo rail for the whole song. */
static int robot_motion_execute_music_tail_step(
    const struct robot_motion_request_s *request)
{
  struct robotctl_servo_ctx_s ctx;
  const int center = g_robotctl_home[ROBOTCTL_TAIL];
  const int left = center - ROBOT_MOTION_MUSIC_TAIL_LIMIT_DEG;
  const int right = center + ROBOT_MOTION_MUSIC_TAIL_LIMIT_DEG;
  int direction;
  int current;
  int target;
  int ret;

  if (request == NULL || request->amplitude_deg !=
      ROBOT_MOTION_MUSIC_TAIL_STEP_DEG || request->period_ms !=
      ROBOT_MOTION_MUSIC_TAIL_STEP_MS)
    {
      return -EINVAL;
    }

  if (robot_motion_cancel_check(NULL)) return -ECANCELED;

  ret = robotctl_servo_open(&ctx);
  if (ret < 0)
    {
      return ret;
    }

  ctx.cancel_check = robot_motion_cancel_check;
  ctx.cancel_arg = NULL;
  current = ctx.angle[ROBOTCTL_TAIL];
  pthread_mutex_lock(&g_motion.lock);
  direction = g_motion.music_tail_direction < 0 ? -1 : 1;
  if (current <= left)
    {
      direction = 1;
    }
  else if (current >= right)
    {
      direction = -1;
    }
  pthread_mutex_unlock(&g_motion.lock);

  target = current + direction * ROBOT_MOTION_MUSIC_TAIL_STEP_DEG;
  if (current < left && target > left)
    {
      target = left;
    }
  else if (current > right && target < right)
    {
      target = right;
    }
  else if (current >= left && current <= right)
    {
      if (target < left)
        {
          target = left;
        }
      else if (target > right)
        {
          target = right;
        }
    }

  printf("[ROBOT-MOTION] music tail step %d -> %d direction=%s\n",
         current, target, direction < 0 ? "left" : "right");
  ret = robot_motion_tail_micro_move(&ctx, target, 20, 80);
  if (ret >= 0)
    {
      pthread_mutex_lock(&g_motion.lock);
      if (target <= left)
        {
          g_motion.music_tail_direction = 1;
        }
      else if (target >= right)
        {
          g_motion.music_tail_direction = -1;
        }
      else
        {
          g_motion.music_tail_direction = direction;
        }
      pthread_mutex_unlock(&g_motion.lock);
    }

  robotctl_servo_close(&ctx);
  return ret;
}

static void *robot_motion_worker(void *arg)
{
  (void)arg;
  printf("[ROBOT-MOTION] worker started\n");

  for (;;)
    {
      struct robot_motion_request_s request;
      int ret;

      pthread_mutex_lock(&g_motion.lock);
      while (!g_motion.request_pending)
        {
          pthread_cond_wait(&g_motion.cond, &g_motion.lock);
        }

      request = g_motion.request;
      g_motion.request_pending = false;
      g_motion.state = ROBOT_MOTION_STATE_RUNNING;
      pthread_mutex_unlock(&g_motion.lock);

      if (robot_world_state_set_motion_active) robot_world_state_set_motion_active(true);
      if (request.source == ROBOT_MOTION_SOURCE_MUSIC)
        {
          printf("[ROBOT-MOTION] start id=%lu action=music_tail_step\n",
                 (unsigned long)request.request_id);
          ret = robot_motion_execute_music_tail_step(&request);
        }
      else if (request.type == ROBOT_MOTION_REQUEST_TAIL_WAG)
        {
          printf("[ROBOT-MOTION] start id=%lu action=tail_wag\n",
                 (unsigned long)request.request_id);
          printf("[POWER-SAFE] mono_ms=%llu MOTION_START id=%lu "
                 "action=tail_wag\n",
                 (unsigned long long)robot_motion_now_ms(),
                 (unsigned long)request.request_id);
          ret = robot_motion_execute_tail_wag(&request);
        }
      else
        {
          printf("[ROBOT-MOTION] start id=%lu direction=%s\n",
                 (unsigned long)request.request_id,
                 robot_motion_direction_name(request.direction));
          printf("[POWER-SAFE] mono_ms=%llu MOTION_START id=%lu "
                 "direction=%s\n", (unsigned long long)robot_motion_now_ms(),
                 (unsigned long)request.request_id,
                 robot_motion_direction_name(request.direction));
          ret = robot_motion_execute_gait(&request);
        }

      if (robot_world_state_set_motion_active) robot_world_state_set_motion_active(false);
      pthread_mutex_lock(&g_motion.lock);
      g_motion.last_result = ret;
      g_motion.completed_request_id = request.request_id;
      g_motion.state = ret < 0 ? ROBOT_MOTION_STATE_ERROR :
                            ROBOT_MOTION_STATE_IDLE;
      printf("[POWER-SAFE] mono_ms=%llu MOTION_DONE id=%lu rc=%d\n",
             (unsigned long long)robot_motion_now_ms(),
             (unsigned long)request.request_id, ret);
      pthread_cond_broadcast(&g_motion.cond);
      pthread_mutex_unlock(&g_motion.lock);
      printf("[ROBOT-MOTION] done id=%lu rc=%d\n",
             (unsigned long)request.request_id, ret);
    }

  return NULL;
}

const char *robot_motion_direction_name(
    enum robot_motion_direction_e direction)
{
  static const char *const names[] =
  {
    "forward", "backward", "left", "right"
  };

  return direction >= 0 && direction < ROBOT_MOTION_DIRECTION_COUNT ?
         names[direction] : "unknown";
}

int robot_motion_direction_from_name(const char *name,
                                     enum robot_motion_direction_e *direction)
{
  int i;

  if (name == NULL || direction == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < ROBOT_MOTION_DIRECTION_COUNT; i++)
    {
      if (strcmp(name, robot_motion_direction_name(i)) == 0)
        {
          *direction = (enum robot_motion_direction_e)i;
          return 0;
        }
    }

  return -EINVAL;
}

unsigned int robot_motion_default_period(
    enum robot_motion_direction_e direction)
{
  return direction == ROBOT_MOTION_LEFT || direction == ROBOT_MOTION_RIGHT ?
         ROBOT_MOTION_DEFAULT_TURN_PERIOD : ROBOT_MOTION_DEFAULT_WALK_PERIOD;
}

int robot_motion_init(void)
{
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.worker_started)
    {
      pthread_mutex_unlock(&g_motion.lock);
      return 0;
    }
  if (g_motion.initializing)
    {
      while (g_motion.initializing && !g_motion.worker_started)
        {
          pthread_cond_wait(&g_motion.cond, &g_motion.lock);
        }

      ret = g_motion.worker_started ? 0 : -EAGAIN;
      pthread_mutex_unlock(&g_motion.lock);
      return ret;
    }
  g_motion.initializing = true;
  g_motion.next_request_id = 0;
  g_motion.completed_request_id = 0;
  pthread_mutex_unlock(&g_motion.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&attr, ROBOT_MOTION_WORKER_STACK);
      if (ret == 0)
        {
          ret = pthread_create(&g_motion.worker, &attr,
                               robot_motion_worker, NULL);
        }
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      int error = ret > 0 ? -ret : ret;

      pthread_mutex_lock(&g_motion.lock);
      g_motion.initializing = false;
      pthread_cond_broadcast(&g_motion.cond);
      pthread_mutex_unlock(&g_motion.lock);
      printf("[ROBOT-MOTION] worker create failed rc=%d\n", error);
      return error;
    }

  pthread_mutex_lock(&g_motion.lock);
  g_motion.worker_started = true;
  g_motion.initializing = false;
  pthread_cond_broadcast(&g_motion.cond);
  pthread_mutex_unlock(&g_motion.lock);
  pthread_detach(g_motion.worker);
  printf("[ROBOT-MOTION] initialized\n");
  return 0;
}

static int robot_motion_submit_request(
    enum robot_motion_source_e source,
    enum robot_motion_request_type_e type,
    enum robot_motion_direction_e direction,
    unsigned int steps, unsigned int period_ms, unsigned int amplitude_deg,
    uint32_t music_session_id)
{
  int ret;
  uint32_t request_id;

  if (source < 0 || source >= ROBOT_MOTION_SOURCE_COUNT ||
      type < 0 || type >= ROBOT_MOTION_REQUEST_TYPE_COUNT ||
      direction < 0 || direction >= ROBOT_MOTION_DIRECTION_COUNT ||
      steps < ROBOT_MOTION_MIN_STEPS || steps > ROBOT_MOTION_MAX_STEPS ||
      (type == ROBOT_MOTION_REQUEST_GAIT &&
       (period_ms < ROBOT_MOTION_MIN_PERIOD ||
        period_ms > ROBOT_MOTION_MAX_PERIOD)))
    {
      return -EINVAL;
    }

  if (type == ROBOT_MOTION_REQUEST_TAIL_WAG &&
      source != ROBOT_MOTION_SOURCE_MUSIC &&
      (steps < ROBOT_MOTION_TAIL_MIN_CYCLES ||
       steps > ROBOT_MOTION_TAIL_MAX_CYCLES ||
       period_ms < ROBOT_MOTION_TAIL_MIN_PERIOD ||
       period_ms > ROBOT_MOTION_TAIL_MAX_PERIOD ||
       amplitude_deg < ROBOT_MOTION_TAIL_MIN_AMPLITUDE ||
       amplitude_deg > ROBOT_MOTION_TAIL_MAX_AMPLITUDE))
    {
      return -EINVAL;
    }

  if (source == ROBOT_MOTION_SOURCE_MUSIC &&
      (type != ROBOT_MOTION_REQUEST_TAIL_WAG ||
       period_ms != ROBOT_MOTION_MUSIC_TAIL_STEP_MS ||
       amplitude_deg != ROBOT_MOTION_MUSIC_TAIL_STEP_DEG))
    {
      return -EINVAL;
    }

  if (robot_motion_music_active() &&
      (type == ROBOT_MOTION_REQUEST_GAIT || source != ROBOT_MOTION_SOURCE_MUSIC))
    return -EPERM;
  if (source == ROBOT_MOTION_SOURCE_MUSIC &&
      (music_session_id == 0 || !robot_music_media_motion_allowed ||
       !robot_music_media_motion_allowed(music_session_id)))
    return -EAGAIN;

  ret = robot_motion_init();
  if (ret < 0)
    {
      return ret;
    }

  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.request_pending ||
      g_motion.state == ROBOT_MOTION_STATE_RUNNING)
    {
      pthread_mutex_unlock(&g_motion.lock);
      if (type == ROBOT_MOTION_REQUEST_TAIL_WAG)
        {
          printf("[ROBOT-MOTION] reject busy action=tail_wag\n");
        }
      else
        {
          printf("[ROBOT-MOTION] reject busy direction=%s\n",
                 robot_motion_direction_name(direction));
        }
      return -EBUSY;
    }

  request_id = ++g_motion.next_request_id;
  if (request_id == 0)
    {
      request_id = ++g_motion.next_request_id;
    }
  g_motion.request.source = source;
  g_motion.request.type = type;
  g_motion.request.direction = direction;
  g_motion.request.steps = steps;
  g_motion.request.period_ms = period_ms;
  g_motion.request.amplitude_deg = amplitude_deg;
  g_motion.request.music_session_id = music_session_id;
  g_motion.request.request_id = request_id;
  g_motion.cancel_requested = false;
  g_motion.request_pending = true;
  g_motion.state = ROBOT_MOTION_STATE_RUNNING;
  pthread_cond_signal(&g_motion.cond);
  pthread_mutex_unlock(&g_motion.lock);

  if (type == ROBOT_MOTION_REQUEST_TAIL_WAG)
    {
      printf("[ROBOT-MOTION] submit id=%lu source=%d action=tail_wag "
             "cycles=%u period_ms=%u amplitude=%u\n",
             (unsigned long)request_id, source, steps, period_ms,
             amplitude_deg);
    }
  else
    {
      printf("[ROBOT-MOTION] submit id=%lu source=%d direction=%s steps=%u "
             "period_ms=%u\n", (unsigned long)request_id, source,
             robot_motion_direction_name(direction), steps, period_ms);
    }
  return (int)request_id;
}

int robot_motion_submit(enum robot_motion_source_e source,
                        enum robot_motion_direction_e direction,
                        unsigned int steps, unsigned int period_ms)
{
  return robot_motion_submit_request(source, ROBOT_MOTION_REQUEST_GAIT,
                                     direction, steps, period_ms, 0, 0);
}

static uint32_t robot_motion_elapsed_ms(const struct timespec *start)
{
  struct timespec now;
  int64_t sec;
  int64_t nsec;

  if (start == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
      return 0;
    }

  sec = (int64_t)now.tv_sec - start->tv_sec;
  nsec = (int64_t)now.tv_nsec - start->tv_nsec;
  if (nsec < 0)
    {
      sec--;
      nsec += 1000000000;
    }

  if (sec <= 0)
    {
      return nsec <= 0 ? 0 : (uint32_t)(nsec / 1000000);
    }

  if (sec > UINT32_MAX / 1000)
    {
      return UINT32_MAX;
    }

  return (uint32_t)(sec * 1000 + nsec / 1000000);
}

uint32_t robot_motion_default_timeout(unsigned int steps,
                                      unsigned int period_ms)
{
  uint64_t timeout = (uint64_t)steps * period_ms +
                     ROBOT_MOTION_WAIT_MARGIN_MS;

  if (timeout > ROBOT_MOTION_MAX_TIMEOUT_MS)
    {
      timeout = ROBOT_MOTION_MAX_TIMEOUT_MS;
    }

  /* A gait includes a sequential home pass, eight phases, and a final home
   * pass.  The requested gait period alone does not cover those operations.
   * Keep a one-step request from being cancelled while its bounded servo
   * ramps are still running. */
  if (timeout < ROBOT_MOTION_MIN_TIMEOUT_MS)
    {
      timeout = ROBOT_MOTION_MIN_TIMEOUT_MS;
    }

  return (uint32_t)timeout;
}

int robot_motion_wait_timeout(uint32_t request_id, uint32_t timeout_ms,
                              int *result)
{
  struct timespec start;
  struct timespec deadline;
  uint32_t elapsed;
  uint32_t remaining;
  uint32_t wait_ms;
  int wait_ret;

  if (request_id == 0 || timeout_ms == 0)
    {
      return -EINVAL;
    }

  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
    {
      return -EIO;
    }

  pthread_mutex_lock(&g_motion.lock);
  while (g_motion.completed_request_id < request_id)
    {
      elapsed = robot_motion_elapsed_ms(&start);
      if (elapsed >= timeout_ms)
        {
          pthread_mutex_unlock(&g_motion.lock);
          printf("[ROBOT-MOTION] wait timeout id=%lu elapsed_ms=%lu\n",
                 (unsigned long)request_id, (unsigned long)elapsed);
          return -ETIMEDOUT;
        }

      remaining = timeout_ms - elapsed;
      wait_ms = remaining > 250 ? 250 : remaining;
      if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        {
          pthread_mutex_unlock(&g_motion.lock);
          return -EIO;
        }

      deadline.tv_sec += wait_ms / 1000;
      deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L)
        {
          deadline.tv_sec++;
          deadline.tv_nsec -= 1000000000L;
        }

      wait_ret = pthread_cond_timedwait(&g_motion.cond, &g_motion.lock,
                                        &deadline);
      if (wait_ret != 0 && wait_ret != ETIMEDOUT)
        {
          pthread_mutex_unlock(&g_motion.lock);
          return -wait_ret;
        }
    }

  if (result != NULL)
    {
      *result = g_motion.last_result;
    }
  pthread_mutex_unlock(&g_motion.lock);
  printf("[ROBOT-MOTION] wait complete id=%lu rc=%d\n",
         (unsigned long)request_id, result != NULL ? *result : 0);
  return 0;
}

int robot_motion_wait(uint32_t request_id, int *result)
{
  if (request_id == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_motion.lock);
  while (g_motion.completed_request_id < request_id)
    {
      pthread_cond_wait(&g_motion.cond, &g_motion.lock);
    }

  if (result != NULL)
    {
      *result = g_motion.last_result;
    }
  pthread_mutex_unlock(&g_motion.lock);
  printf("[ROBOT-MOTION] wait complete id=%lu rc=%d\n",
         (unsigned long)request_id, result != NULL ? *result : 0);
  return 0;
}

int robot_motion_submit_and_wait(enum robot_motion_source_e source,
                                 enum robot_motion_direction_e direction,
                                 unsigned int steps, unsigned int period_ms,
                                 uint32_t timeout_ms, int *result)
{
  int request_id;
  int motion_result;
  int wait_result;

  request_id = robot_motion_submit(source, direction, steps, period_ms);
  if (request_id < 0)
    {
      return request_id;
    }

  printf("[ROBOT-MOTION] waiting id=%d timeout_ms=%lu\n", request_id,
         (unsigned long)timeout_ms);
  wait_result = robot_motion_wait_timeout((uint32_t)request_id, timeout_ms,
                                          &motion_result);
  if (wait_result == -ETIMEDOUT)
    {
      bool still_running;

      pthread_mutex_lock(&g_motion.lock);
      still_running = g_motion.completed_request_id < (uint32_t)request_id;
      if (still_running)
        {
          g_motion.cancel_requested = true;
        }
      else
        {
          motion_result = g_motion.last_result;
          /* The worker completed between the timed wait and this check.
           * Preserve its result instead of reporting a spurious timeout. */
          wait_result = 0;
        }
      pthread_mutex_unlock(&g_motion.lock);

      if (still_running)
        {
          printf("[ROBOT-MOTION] timeout stop requested id=%d\n", request_id);
          wait_result = robot_motion_wait((uint32_t)request_id,
                                          &motion_result);
          if (wait_result < 0)
            {
              return wait_result;
            }

          printf("[ROBOT-MOTION] timeout stopped id=%d worker_rc=%d\n",
                 request_id, motion_result);
          return -ETIMEDOUT;
        }
    }

  if (wait_result < 0)
    {
      return wait_result;
    }

  if (result != NULL)
    {
      *result = motion_result;
    }

  return motion_result;
}

int robot_motion_execute_sync(enum robot_motion_source_e source,
                              enum robot_motion_direction_e direction,
                              unsigned int steps, unsigned int period_ms)
{
  int result;

  return robot_motion_submit_and_wait(
    source, direction, steps, period_ms,
    robot_motion_default_timeout(steps, period_ms), &result);
}

static int robot_motion_tail_wag_for_session(enum robot_motion_source_e source,
                               unsigned int cycles, unsigned int period_ms,
                               unsigned int amplitude_deg, uint32_t session_id)
{
  int request_id;
  int motion_result;
  int wait_result;
  uint32_t timeout_ms;

  if (cycles < ROBOT_MOTION_TAIL_MIN_CYCLES ||
      cycles > ROBOT_MOTION_TAIL_MAX_CYCLES ||
      period_ms < ROBOT_MOTION_TAIL_MIN_PERIOD ||
      period_ms > ROBOT_MOTION_TAIL_MAX_PERIOD ||
      amplitude_deg < ROBOT_MOTION_TAIL_MIN_AMPLITUDE ||
      amplitude_deg > ROBOT_MOTION_TAIL_MAX_AMPLITUDE)
    {
      return -EINVAL;
    }

  /* Keep a generous wait budget for the brownout-aware segmented wag. */
  timeout_ms = (uint32_t)((uint64_t)cycles * period_ms * 4ULL +
                          ROBOT_MOTION_WAIT_MARGIN_MS);
  if (timeout_ms > ROBOT_MOTION_MAX_TIMEOUT_MS)
    {
      timeout_ms = ROBOT_MOTION_MAX_TIMEOUT_MS;
    }

  request_id = robot_motion_submit_request(
    source, ROBOT_MOTION_REQUEST_TAIL_WAG, ROBOT_MOTION_FORWARD, cycles,
    period_ms, amplitude_deg, session_id);
  if (request_id < 0)
    {
      return request_id;
    }

  printf("[ROBOT-MOTION] waiting tail_wag id=%d timeout_ms=%lu\n",
         request_id, (unsigned long)timeout_ms);
  wait_result = robot_motion_wait_timeout((uint32_t)request_id, timeout_ms,
                                          &motion_result);
  if (wait_result == -ETIMEDOUT)
    {
      bool still_running;

      pthread_mutex_lock(&g_motion.lock);
      still_running = g_motion.completed_request_id < (uint32_t)request_id;
      if (still_running)
        {
          g_motion.cancel_requested = true;
        }
      else
        {
          motion_result = g_motion.last_result;
        }
      pthread_mutex_unlock(&g_motion.lock);

      if (still_running)
        {
          wait_result = robot_motion_wait((uint32_t)request_id,
                                          &motion_result);
          if (wait_result < 0)
            {
              return wait_result;
            }
          return -ETIMEDOUT;
        }
    }

  if (wait_result < 0)
    {
      return wait_result;
    }

  return motion_result;
}

int robot_motion_tail_wag_sync(enum robot_motion_source_e source,
                               unsigned int cycles, unsigned int period_ms,
                               unsigned int amplitude_deg)
{
  if (source == ROBOT_MOTION_SOURCE_MUSIC) return -EPERM;
  return robot_motion_tail_wag_for_session(source, cycles, period_ms, amplitude_deg, 0);
}

int robot_motion_music_tail_wag_sync(uint32_t session_id, uint32_t event_id,
                                     unsigned int period_ms,
                                     unsigned int amplitude_deg)
{
  int ret;
  if (period_ms < ROBOT_MOTION_TAIL_MIN_PERIOD ||
      period_ms > ROBOT_MOTION_TAIL_MAX_PERIOD ||
      amplitude_deg < ROBOT_MOTION_TAIL_MIN_AMPLITUDE ||
      amplitude_deg > ROBOT_MOTION_TAIL_MAX_AMPLITUDE) return -EINVAL;
  if (robot_motion_is_busy()) return -EBUSY;
  if (!robot_music_media_claim_tail) return -ENOSYS;
  ret = robot_music_media_claim_tail(session_id, event_id);
  if (ret < 0) return ret;
  return robot_motion_tail_wag_for_session(ROBOT_MOTION_SOURCE_MUSIC, 1,
                                           period_ms, amplitude_deg, session_id);
}

int robot_motion_music_tail_repeat_async(uint32_t session_id, uint32_t event_id)
{
  int ret;
  if (robot_motion_is_busy()) return -EBUSY;
  if (!robot_music_media_claim_periodic_tail) return -ENOSYS;
  ret = robot_music_media_claim_periodic_tail(session_id, event_id);
  if (ret < 0) return ret;
  return robot_motion_submit_request(ROBOT_MOTION_SOURCE_MUSIC,
    ROBOT_MOTION_REQUEST_TAIL_WAG, ROBOT_MOTION_FORWARD, 1,
    ROBOT_MOTION_MUSIC_TAIL_STEP_MS, ROBOT_MOTION_MUSIC_TAIL_STEP_DEG,
    session_id);
}

int robot_motion_home_sync(void)
{
  struct robotctl_servo_ctx_s ctx;
  int ret;

  if (robot_motion_music_active()) return -EPERM;
  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.request_pending ||
      g_motion.state == ROBOT_MOTION_STATE_RUNNING)
    {
      pthread_mutex_unlock(&g_motion.lock);
      return -EBUSY;
    }
  g_motion.state = ROBOT_MOTION_STATE_RUNNING;
  pthread_mutex_unlock(&g_motion.lock);

  printf("[ROBOT-MOTION] backend init begin\n");
  ret = robotctl_servo_open(&ctx);
  printf("[ROBOT-MOTION] backend init done rc=%d\n", ret);
  if (ret >= 0)
    {
      ret = robot_motion_home_power_safe(
        &ctx, true,
        robot_motion_safe_interval(ROBOT_MOTION_DEFAULT_WALK_PERIOD));
      robotctl_servo_close(&ctx);
    }

  pthread_mutex_lock(&g_motion.lock);
  g_motion.last_result = ret;
  g_motion.state = ret < 0 ? ROBOT_MOTION_STATE_ERROR :
                            ROBOT_MOTION_STATE_IDLE;
  pthread_cond_broadcast(&g_motion.cond);
  pthread_mutex_unlock(&g_motion.lock);
  return ret;
}

int robot_motion_quiesce_music(void)
{
  uint32_t request_id;
  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.request.source != ROBOT_MOTION_SOURCE_MUSIC ||
      (!g_motion.request_pending && g_motion.state != ROBOT_MOTION_STATE_RUNNING)) {
    pthread_mutex_unlock(&g_motion.lock);
    return 0;
  }
  request_id = g_motion.request.request_id;
  g_motion.cancel_requested = true;
  if (g_motion.request_pending) {
    g_motion.request_pending = false;
    g_motion.completed_request_id = request_id;
    g_motion.last_result = -ECANCELED;
    g_motion.state = ROBOT_MOTION_STATE_IDLE;
    pthread_cond_broadcast(&g_motion.cond);
  }
  pthread_mutex_unlock(&g_motion.lock);
  /* Reuse the existing monotonic, bounded waiter. Driver stalls must report
   * failure rather than hold every later conversation indefinitely. */
  return robot_motion_wait_timeout(request_id,
    2U * ROBOT_MOTION_MUSIC_TAIL_STEP_MS, NULL);
}

bool robot_motion_is_busy(void)
{
  bool busy;

  pthread_mutex_lock(&g_motion.lock);
  busy = g_motion.request_pending ||
         g_motion.state == ROBOT_MOTION_STATE_RUNNING;
  pthread_mutex_unlock(&g_motion.lock);
  return busy;
}

enum robot_motion_state_e robot_motion_get_state(void)
{
  enum robot_motion_state_e state;

  pthread_mutex_lock(&g_motion.lock);
  state = g_motion.state;
  pthread_mutex_unlock(&g_motion.lock);
  return state;
}

int robot_motion_get_active(struct robot_motion_request_s *request)
{
  if (request == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_motion.lock);
  if (!g_motion.request_pending &&
      g_motion.state != ROBOT_MOTION_STATE_RUNNING)
    {
      pthread_mutex_unlock(&g_motion.lock);
      return -ENOENT;
    }
  *request = g_motion.request;
  pthread_mutex_unlock(&g_motion.lock);
  return 0;
}
