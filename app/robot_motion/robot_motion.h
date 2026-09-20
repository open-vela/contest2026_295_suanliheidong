/*
 * Contest-local shared robot motion capability.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_MOTION_H
#define __CONTEST_ROBOT_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOT_MOTION_DEFAULT_STEPS       1
#define ROBOT_MOTION_DEFAULT_WALK_PERIOD 2500
#define ROBOT_MOTION_DEFAULT_TURN_PERIOD 3000
#define ROBOT_MOTION_TAIL_DEFAULT_CYCLES 1
#define ROBOT_MOTION_TAIL_DEFAULT_PERIOD 3000
#define ROBOT_MOTION_TAIL_DEFAULT_AMPLITUDE 20
#define ROBOT_MOTION_MIN_STEPS           1
#define ROBOT_MOTION_MAX_STEPS           10
#define ROBOT_MOTION_MIN_PERIOD          800
#define ROBOT_MOTION_MAX_PERIOD          3000
#define ROBOT_MOTION_TAIL_MIN_CYCLES     1
#define ROBOT_MOTION_TAIL_MAX_CYCLES     2
#define ROBOT_MOTION_TAIL_MIN_PERIOD     2600
#define ROBOT_MOTION_TAIL_MAX_PERIOD     3000
#define ROBOT_MOTION_TAIL_MIN_AMPLITUDE  3
#define ROBOT_MOTION_TAIL_MAX_AMPLITUDE  20
#define ROBOT_MOTION_WAIT_MARGIN_MS      15000
#define ROBOT_MOTION_MAX_TIMEOUT_MS      120000

enum robot_motion_direction_e
{
  ROBOT_MOTION_FORWARD = 0,
  ROBOT_MOTION_BACKWARD,
  ROBOT_MOTION_LEFT,
  ROBOT_MOTION_RIGHT,
  ROBOT_MOTION_DIRECTION_COUNT,
};

enum robot_motion_source_e
{
  ROBOT_MOTION_SOURCE_SYSTEM = 0,
  ROBOT_MOTION_SOURCE_CLI,
  ROBOT_MOTION_SOURCE_AGENT,
  ROBOT_MOTION_SOURCE_BEHAVIOR,
  ROBOT_MOTION_SOURCE_COUNT,
};

enum robot_motion_request_type_e
{
  ROBOT_MOTION_REQUEST_GAIT = 0,
  ROBOT_MOTION_REQUEST_TAIL_WAG,
  ROBOT_MOTION_REQUEST_TYPE_COUNT,
};

enum robot_motion_state_e
{
  ROBOT_MOTION_STATE_IDLE = 0,
  ROBOT_MOTION_STATE_RUNNING,
  ROBOT_MOTION_STATE_ERROR,
};

struct robot_motion_request_s
{
  enum robot_motion_source_e source;
  enum robot_motion_request_type_e type;
  enum robot_motion_direction_e direction;
  unsigned int steps;
  unsigned int period_ms;
  unsigned int amplitude_deg;
  uint32_t request_id;
};

const char *robot_motion_direction_name(
    enum robot_motion_direction_e direction);

int robot_motion_direction_from_name(const char *name,
                                     enum robot_motion_direction_e *direction);

unsigned int robot_motion_default_period(
    enum robot_motion_direction_e direction);

int robot_motion_init(void);

/* Returns a positive request id when accepted, or a negative errno value. */
int robot_motion_submit(enum robot_motion_source_e source,
                        enum robot_motion_direction_e direction,
                        unsigned int steps, unsigned int period_ms);

int robot_motion_wait(uint32_t request_id, int *result);

int robot_motion_wait_timeout(uint32_t request_id, uint32_t timeout_ms,
                              int *result);

int robot_motion_submit_and_wait(enum robot_motion_source_e source,
                                 enum robot_motion_direction_e direction,
                                 unsigned int steps, unsigned int period_ms,
                                 uint32_t timeout_ms, int *result);

uint32_t robot_motion_default_timeout(unsigned int steps,
                                      unsigned int period_ms);

int robot_motion_execute_sync(enum robot_motion_source_e source,
                              enum robot_motion_direction_e direction,
                              unsigned int steps, unsigned int period_ms);

int robot_motion_tail_wag_sync(enum robot_motion_source_e source,
                               unsigned int cycles, unsigned int period_ms,
                               unsigned int amplitude_deg);

/* The legacy robotctl home command uses the same serialized motion gate. */
int robot_motion_home_sync(void);

bool robot_motion_is_busy(void);
enum robot_motion_state_e robot_motion_get_state(void);
int robot_motion_get_active(struct robot_motion_request_s *request);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_MOTION_H */
