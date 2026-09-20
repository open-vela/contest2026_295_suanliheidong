/****************************************************************************
 * contest2026_295_suanliheidong/app/robotctl/robotctl_servo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <nuttx/timers/pwm.h>

#include "robotctl_servo.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ROBOTCTL_PWM_PATH         "/dev/pwm0"
#define ROBOTCTL_PWM_FREQUENCY    50
#define ROBOTCTL_SERVO_PERIOD_US  20000
#define ROBOTCTL_SERVO_MIN_US     500
#define ROBOTCTL_SERVO_MAX_US     2500
#define ROBOTCTL_NORMAL_UPDATE_MS 10
#define ROBOTCTL_SAFE_STEP_DEG    3
#define ROBOTCTL_RAMP_START_PERCENT 20
#define ROBOTCTL_RAMP_END_PERCENT   80

#if CONFIG_PWM_NCHANNELS < ROBOTCTL_SERVO_COUNT
#  error "robotctl requires five PWM channels"
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

const int g_robotctl_home[ROBOTCTL_SERVO_COUNT] =
{
  90, 90, 90, 90, 85
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int g_trim[ROBOTCTL_SERVO_COUNT] =
{
  0, 0, 0, 0, 0
};

static const char *const g_servo_names[ROBOTCTL_SERVO_COUNT] =
{
  "RF", "RR", "LR", "LF", "TAIL"
};

struct robotctl_servo_runtime_s
{
  int angle[ROBOTCTL_SERVO_COUNT];
  bool angle_known[ROBOTCTL_SERVO_COUNT];
};

static struct robotctl_servo_runtime_s g_servo_runtime;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int robotctl_clamp_angle(int angle)
{
  if (angle < 0)
    {
      return 0;
    }

  if (angle > 180)
    {
      return 180;
    }

  return angle;
}

static ub16_t robotctl_angle_to_duty(int angle)
{
  uint32_t pulse_us;

  angle = robotctl_clamp_angle(angle);
  pulse_us = ROBOTCTL_SERVO_MIN_US +
             ((ROBOTCTL_SERVO_MAX_US - ROBOTCTL_SERVO_MIN_US) * angle) / 180;

  return (ub16_t)(((uint64_t)pulse_us << 16) / ROBOTCTL_SERVO_PERIOD_US);
}

static int robotctl_duty_to_angle(ub16_t duty)
{
  uint32_t pulse_us;

  pulse_us = ((uint64_t)duty * ROBOTCTL_SERVO_PERIOD_US) >> 16;
  if (pulse_us < ROBOTCTL_SERVO_MIN_US ||
      pulse_us > ROBOTCTL_SERVO_MAX_US)
    {
      return -1;
    }

  return (int)(((pulse_us - ROBOTCTL_SERVO_MIN_US) * 180) /
               (ROBOTCTL_SERVO_MAX_US - ROBOTCTL_SERVO_MIN_US));
}

static int robotctl_apply_frame(struct robotctl_servo_ctx_s *ctx,
                                const int angle[ROBOTCTL_SERVO_COUNT],
                                unsigned int active_mask)
{
  struct pwm_info_s info;
  int saved_errno;
  int ret;
  int i;

  memset(&info, 0, sizeof(info));
  info.frequency = ROBOTCTL_PWM_FREQUENCY;

  for (i = 0; i < ROBOTCTL_SERVO_COUNT; i++)
    {
      info.channels[i].channel = i + 1;
      if ((active_mask & (1u << i)) != 0)
        {
          info.channels[i].duty =
            robotctl_angle_to_duty(angle[i] + g_trim[i]);
        }

      info.channels[i].cpol = PWM_CPOL_NDEF;
      info.channels[i].dcpol = PWM_DCPOL_LOW;
    }

  ret = ioctl(ctx->fd, PWMIOC_SETCHARACTERISTICS,
              (unsigned long)((uintptr_t)&info));
  if (ret < 0)
    {
      saved_errno = errno;
      fprintf(stderr,
              "robotctl: PWMIOC_SETCHARACTERISTICS failed errno=%d\n",
              saved_errno);
      return -saved_errno;
    }

  ret = ioctl(ctx->fd, PWMIOC_START, 0);
  if (ret < 0)
    {
      saved_errno = errno;
      fprintf(stderr, "robotctl: PWMIOC_START failed errno=%d\n",
              saved_errno);
      return -saved_errno;
    }

  ctx->pwm_enabled = true;
  return 0;
}

static int robotctl_servo_open_device(struct robotctl_servo_ctx_s *ctx)
{
  int saved_errno;

  ctx->fd = open(ROBOTCTL_PWM_PATH, O_RDONLY);
  if (ctx->fd < 0)
    {
      saved_errno = errno;
      fprintf(stderr, "robotctl: open %s failed errno=%d\n",
              ROBOTCTL_PWM_PATH, saved_errno);
      return -saved_errno;
    }

  return 0;
}

static void robotctl_servo_stop_and_close(struct robotctl_servo_ctx_s *ctx)
{
  if (ctx->fd >= 0)
    {
      (void)robotctl_servo_stop(ctx, false);
      robotctl_servo_close(ctx);
    }
}

static bool robotctl_servo_cancelled(struct robotctl_servo_ctx_s *ctx)
{
  return ctx->cancel_check != NULL &&
         ctx->cancel_check(ctx->cancel_arg);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

const char *robotctl_servo_name(enum robotctl_servo_e servo)
{
  if (servo < 0 || servo >= ROBOTCTL_SERVO_COUNT)
    {
      return "UNKNOWN";
    }

  return g_servo_names[servo];
}

int robotctl_servo_from_name(const char *name)
{
  int i;

  for (i = 0; i < ROBOTCTL_SERVO_COUNT; i++)
    {
      if (strcasecmp(name, g_servo_names[i]) == 0)
        {
          return i;
        }
    }

  return -EINVAL;
}

int robotctl_servo_open(struct robotctl_servo_ctx_s *ctx)
{
  struct pwm_info_s info;
  int angle;
  int ret;
  int i;

  ctx->cancel_check = NULL;
  ctx->cancel_arg = NULL;
  memcpy(ctx->angle, g_robotctl_home, sizeof(ctx->angle));
  memcpy(ctx->angle_known, g_servo_runtime.angle_known,
         sizeof(ctx->angle_known));
  for (i = 0; i < ROBOTCTL_SERVO_COUNT; i++)
    {
      if (ctx->angle_known[i])
        {
          ctx->angle[i] = g_servo_runtime.angle[i];
        }
    }
  ctx->pwm_enabled = false;
  ctx->command_count = 0;
  ctx->skipped_count = 0;
  ctx->total_angle_delta = 0;
  ret = robotctl_servo_open_device(ctx);
  if (ret < 0)
    {
      return ret;
    }

  memset(&info, 0, sizeof(info));
  if (ioctl(ctx->fd, PWMIOC_GETCHARACTERISTICS,
            (unsigned long)((uintptr_t)&info)) == 0 &&
      info.frequency == ROBOTCTL_PWM_FREQUENCY)
    {
      for (i = 0; i < ROBOTCTL_SERVO_COUNT; i++)
        {
          angle = robotctl_duty_to_angle(info.channels[i].duty);
          if (angle >= 0)
            {
              ctx->angle[i] = robotctl_clamp_angle(angle - g_trim[i]);
              ctx->angle_known[i] = true;
              g_servo_runtime.angle[i] = ctx->angle[i];
              g_servo_runtime.angle_known[i] = true;
            }
        }
    }

  return 0;
}

void robotctl_servo_close(struct robotctl_servo_ctx_s *ctx)
{
  if (ctx->fd >= 0)
    {
      close(ctx->fd);
      ctx->fd = -1;
    }
}

int robotctl_servo_stop(struct robotctl_servo_ctx_s *ctx, bool announce)
{
  int saved_errno;
  int ret;

  /* PWMIOC_STOP already removes the output waveform.  Do not send a
   * zero-duty frame here: robotctl_apply_frame() starts PWM after every
   * SETCHARACTERISTICS call, which creates an unnecessary START->STOP cycle
   * during every per-servo move. */
  printf("[ROBOT-SERVO] stop fd=%d pwm=%d\n", ctx->fd,
         ctx->pwm_enabled ? 1 : 0);
  ret = ioctl(ctx->fd, PWMIOC_STOP, 0);
  if (ret < 0)
    {
      saved_errno = errno;
      fprintf(stderr, "robotctl: PWMIOC_STOP failed errno=%d\n",
              saved_errno);
      return -saved_errno;
    }

  ctx->pwm_enabled = false;
  if (announce)
    {
      printf("robotctl: servo PWM stopped\n");
    }

  return 0;
}

int robotctl_servo_move_one(struct robotctl_servo_ctx_s *ctx,
                            enum robotctl_servo_e servo, int target,
                            int interval_ms, int settle_ms)
{
  int frame[ROBOTCTL_SERVO_COUNT];
  int current;
  int next;
  int previous;
  int total_steps;
  int step_index = 0;
  int delay_ms;
  int ret;

  target = robotctl_clamp_angle(target);
  current = ctx->angle[servo];
  if (ctx->angle_known[servo] && current == target)
    {
      ctx->skipped_count++;
      return 0;
    }

  if (ctx->fd < 0)
    {
      ret = robotctl_servo_open_device(ctx);
      if (ret < 0)
        {
          return ret;
        }
    }

  memcpy(frame, ctx->angle, sizeof(frame));
  total_steps = (abs(target - current) + ROBOTCTL_SAFE_STEP_DEG - 1) /
                ROBOTCTL_SAFE_STEP_DEG;
  if (total_steps < 1)
    {
      total_steps = 1;
    }

  do
    {
      if (robotctl_servo_cancelled(ctx))
        {
          robotctl_servo_stop_and_close(ctx);
          return -ECANCELED;
        }

      if (current < target)
        {
          next = current + ROBOTCTL_SAFE_STEP_DEG;
          if (next > target)
            {
              next = target;
            }
        }
      else if (current > target)
        {
          next = current - ROBOTCTL_SAFE_STEP_DEG;
          if (next < target)
            {
              next = target;
            }
        }
      else
        {
          next = target;
        }

      previous = current;
      frame[servo] = next;
      if (step_index == 0)
        {
          printf("[ROBOT-SERVO] first-frame servo=%s fd=%d current=%d "
                 "target=%d\n", robotctl_servo_name(servo), ctx->fd,
                 current, target);
        }
      ret = robotctl_apply_frame(ctx, frame, 1u << servo);
      if (ret < 0)
        {
          robotctl_servo_stop_and_close(ctx);
          return ret;
        }

      if (step_index == 0)
        {
          printf("[ROBOT-SERVO] first-frame-applied servo=%s\n",
                 robotctl_servo_name(servo));
        }

      current = next;
      ctx->angle[servo] = next;
      ctx->angle_known[servo] = true;
      g_servo_runtime.angle[servo] = next;
      g_servo_runtime.angle_known[servo] = true;
      ctx->command_count++;
      ctx->total_angle_delta += (unsigned int)abs(next - previous);
      if (current != target)
        {
          step_index++;
          delay_ms = interval_ms;
          if (step_index * 100 < total_steps * ROBOTCTL_RAMP_START_PERCENT)
            {
              delay_ms += interval_ms / 2;
            }
          else if (step_index * 100 >=
                   total_steps * ROBOTCTL_RAMP_END_PERCENT)
            {
              delay_ms += interval_ms / 4;
            }
          usleep(delay_ms * 1000);
        }
    }
  while (current != target);

  while (settle_ms > 0)
    {
      int slice_ms = settle_ms > 10 ? 10 : settle_ms;

      if (robotctl_servo_cancelled(ctx))
        {
          robotctl_servo_stop_and_close(ctx);
          return -ECANCELED;
        }

      usleep(slice_ms * 1000);
      settle_ms -= slice_ms;
    }

  if (robotctl_servo_cancelled(ctx))
    {
      robotctl_servo_stop_and_close(ctx);
      return -ECANCELED;
    }

  ret = robotctl_servo_stop(ctx, false);
  robotctl_servo_close(ctx);
  return ret;
}

int robotctl_servo_move_all(struct robotctl_servo_ctx_s *ctx,
                            const int target[ROBOTCTL_SERVO_COUNT],
                            int duration_ms)
{
  int frame[ROBOTCTL_SERVO_COUNT];
  int start[ROBOTCTL_SERVO_COUNT];
  int steps;
  int ret;
  int i;
  int step;

  memcpy(start, ctx->angle, sizeof(start));
  steps = duration_ms / ROBOTCTL_NORMAL_UPDATE_MS;
  if (steps < 1)
    {
      steps = 1;
    }

  for (step = 1; step <= steps; step++)
    {
      for (i = 0; i < ROBOTCTL_SERVO_COUNT; i++)
        {
          frame[i] = start[i] + ((target[i] - start[i]) * step) / steps;
        }

      ret = robotctl_apply_frame(ctx, frame,
                                 (1u << ROBOTCTL_SERVO_COUNT) - 1u);
      if (ret < 0)
        {
          return ret;
        }

      if (step != steps)
        {
          usleep(ROBOTCTL_NORMAL_UPDATE_MS * 1000);
        }
    }

  memcpy(ctx->angle, target, sizeof(ctx->angle));
  return 0;
}
