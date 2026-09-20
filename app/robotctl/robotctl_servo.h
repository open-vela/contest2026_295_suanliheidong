/****************************************************************************
 * contest2026_295_suanliheidong/app/robotctl/robotctl_servo.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CONTEST2026_295_SUANLIHEIDONG_APP_ROBOTCTL_ROBOTCTL_SERVO_H
#define __CONTEST2026_295_SUANLIHEIDONG_APP_ROBOTCTL_ROBOTCTL_SERVO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ROBOTCTL_SERVO_COUNT 5

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum robotctl_servo_e
{
  ROBOTCTL_RF = 0,
  ROBOTCTL_RR,
  ROBOTCTL_LR,
  ROBOTCTL_LF,
  ROBOTCTL_TAIL
};

struct robotctl_servo_ctx_s
{
  int fd;
  bool (*cancel_check)(void *arg);
  void *cancel_arg;
  int angle[ROBOTCTL_SERVO_COUNT];
  bool angle_known[ROBOTCTL_SERVO_COUNT];
  bool pwm_enabled;
  unsigned int command_count;
  unsigned int skipped_count;
  unsigned int total_angle_delta;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern const int g_robotctl_home[ROBOTCTL_SERVO_COUNT];

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

const char *robotctl_servo_name(enum robotctl_servo_e servo);
int robotctl_servo_from_name(const char *name);
int robotctl_servo_open(struct robotctl_servo_ctx_s *ctx);
void robotctl_servo_close(struct robotctl_servo_ctx_s *ctx);
int robotctl_servo_stop(struct robotctl_servo_ctx_s *ctx, bool announce);
int robotctl_servo_move_one(struct robotctl_servo_ctx_s *ctx,
                            enum robotctl_servo_e servo, int target,
                            int interval_ms, int settle_ms);
int robotctl_servo_move_all(struct robotctl_servo_ctx_s *ctx,
                            const int target[ROBOTCTL_SERVO_COUNT],
                            int duration_ms);

#endif
