/****************************************************************************
 * contest2026_295_suanliheidong/app/robotctl/robotctl_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "robot_motion.h"
#include "robotctl_servo.h"

#define ROBOTCTL_VERSION  "Stage 3 POWER_SAFE"
#define ROBOTCTL_PWM_PATH "/dev/pwm0"
#define ROBOTCTL_I2C_PATH "/dev/i2c0"

static int robotctl_parse_number(const char *text, int minimum, int maximum,
                                 int *result)
{
  char *end;
  long value;

  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || *end != '\0' || value < minimum || value > maximum)
    {
      return -EINVAL;
    }

  *result = (int)value;
  return 0;
}

static int robotctl_parse_motion_args(enum robot_motion_direction_e direction,
                                      int argc, char *argv[], int *steps,
                                      int *period_ms)
{
  *steps = ROBOT_MOTION_DEFAULT_STEPS;
  *period_ms = robot_motion_default_period(direction);

  if (argc > 4)
    {
      return -EINVAL;
    }

  if (argc >= 3 &&
      robotctl_parse_number(argv[2], ROBOT_MOTION_MIN_STEPS,
                            ROBOT_MOTION_MAX_STEPS, steps) < 0)
    {
      return -EINVAL;
    }

  if (argc >= 4 &&
      robotctl_parse_number(argv[3], ROBOT_MOTION_MIN_PERIOD,
                            ROBOT_MOTION_MAX_PERIOD, period_ms) < 0)
    {
      return -EINVAL;
    }

  return 0;
}

static void robotctl_usage(void)
{
  printf("Usage: robotctl <command> [arguments]\n");
  printf("  help\n");
  printf("  status\n");
  printf("  home\n");
  printf("    Sequentially move servos to HOME pose.\n");
  printf("  off\n");
  printf("    Stop all servo PWM output; does not move to HOME.\n");
  printf("    Gear resistance can remain after PWM is stopped.\n");
  printf("  servo <lf|lr|rf|rr|tail> <angle>\n");
  printf("    Test one servo only; angle range is 0..180.\n");
  printf("  servo-off\n");
  printf("    Alias of off.\n");
  printf("  forward|backward|left|right [steps] [period_ms]\n");
  printf("    POWER_SAFE gait; steps 1..10, period_ms 800..3000.\n");
}

static void robotctl_status(void)
{
  printf("robotctl %s\n", ROBOTCTL_VERSION);
  printf("PWM device: %s\n", access(ROBOTCTL_PWM_PATH, F_OK) == 0 ?
         "available" : "unavailable");
  printf("I2C device: %s\n", access(ROBOTCTL_I2C_PATH, F_OK) == 0 ?
         "available" : "unavailable");
  printf("Motion mode: POWER_SAFE\n");
  printf("Max simultaneous servos with nonzero duty: 1\n");
  printf("Motion state: %s\n", robot_motion_is_busy() ? "RUNNING" : "IDLE");
  printf("PWM hardware state: not queryable across invocations\n");
  printf("OLED: not implemented\n");
}

static int robotctl_off(void)
{
  struct robotctl_servo_ctx_s ctx;
  int ret;

  if (robot_motion_is_busy())
    {
      fprintf(stderr, "robotctl: motion busy\n");
      return -EBUSY;
    }

  ret = robotctl_servo_open(&ctx);
  if (ret < 0)
    {
      return ret;
    }

  ret = robotctl_servo_stop(&ctx, true);
  robotctl_servo_close(&ctx);
  return ret;
}

static int robotctl_single_servo(int argc, char *argv[])
{
  struct robotctl_servo_ctx_s ctx;
  int servo;
  int angle;
  int ret;

  if (argc != 4)
    {
      fprintf(stderr,
              "robotctl: servo requires <lf|lr|rf|rr|tail> <angle>\n");
      return -EINVAL;
    }

  servo = robotctl_servo_from_name(argv[2]);
  if (servo < 0 || robotctl_parse_number(argv[3], 0, 180, &angle) < 0)
    {
      fprintf(stderr, "robotctl: invalid servo name or angle 0..180\n");
      return -EINVAL;
    }

  if (robot_motion_is_busy())
    {
      fprintf(stderr, "robotctl: motion busy\n");
      return -EBUSY;
    }

  ret = robotctl_servo_open(&ctx);
  if (ret < 0)
    {
      return ret;
    }

  printf("robotctl: move %s %d -> %d\n", robotctl_servo_name(servo),
         ctx.angle[servo], angle);
  ret = robotctl_servo_move_one(&ctx, servo, angle, 30, 200);
  robotctl_servo_close(&ctx);
  return ret;
}

static int robotctl_motion(enum robot_motion_direction_e direction,
                           int argc, char *argv[])
{
  int period_ms;
  int steps;
  int ret;

  ret = robotctl_parse_motion_args(direction, argc, argv, &steps, &period_ms);
  if (ret < 0)
    {
      fprintf(stderr,
              "robotctl: motion requires steps 1..10 and period_ms "
              "800..3000\n");
      return ret;
    }

  return robot_motion_execute_sync(ROBOT_MOTION_SOURCE_CLI, direction,
                                   (unsigned int)steps,
                                   (unsigned int)period_ms);
}

int main(int argc, char *argv[])
{
  int ret;

  if (argc < 2 || strcmp(argv[1], "help") == 0)
    {
      robotctl_usage();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      robotctl_status();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "servo-off") == 0)
    {
      ret = robotctl_off();
    }
  else if (strcmp(argv[1], "home") == 0)
    {
      ret = robot_motion_home_sync();
    }
  else if (strcmp(argv[1], "servo") == 0)
    {
      ret = robotctl_single_servo(argc, argv);
    }
  else if (strcmp(argv[1], "forward") == 0)
    {
      ret = robotctl_motion(ROBOT_MOTION_FORWARD, argc, argv);
    }
  else if (strcmp(argv[1], "backward") == 0)
    {
      ret = robotctl_motion(ROBOT_MOTION_BACKWARD, argc, argv);
    }
  else if (strcmp(argv[1], "left") == 0)
    {
      ret = robotctl_motion(ROBOT_MOTION_LEFT, argc, argv);
    }
  else if (strcmp(argv[1], "right") == 0)
    {
      ret = robotctl_motion(ROBOT_MOTION_RIGHT, argc, argv);
    }
  else
    {
      fprintf(stderr, "robotctl: unknown command: %s\n", argv[1]);
      robotctl_usage();
      return EXIT_FAILURE;
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
