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
#define ROBOT_MOTION_TAIL_DEFAULT_PERIOD 5400
#define ROBOT_MOTION_TAIL_DEFAULT_AMPLITUDE 3
#define ROBOT_MOTION_MIN_STEPS           1
#define ROBOT_MOTION_MAX_STEPS           10
#define ROBOT_MOTION_MIN_PERIOD          800
#define ROBOT_MOTION_MAX_PERIOD          3000
#define ROBOT_MOTION_TAIL_MIN_CYCLES     1
#define ROBOT_MOTION_TAIL_MAX_CYCLES     1
#define ROBOT_MOTION_TAIL_MIN_PERIOD     4500
#define ROBOT_MOTION_TAIL_MAX_PERIOD     6000
#define ROBOT_MOTION_TAIL_MIN_AMPLITUDE  3
#define ROBOT_MOTION_TAIL_MAX_AMPLITUDE  6
#define ROBOT_MOTION_MUSIC_TAIL_STEP_MS  1000
#define ROBOT_MOTION_MUSIC_TAIL_STEP_DEG 3
#define ROBOT_MOTION_MUSIC_TAIL_LIMIT_DEG 45
#define ROBOT_MOTION_WAIT_MARGIN_MS      30000
#define ROBOT_MOTION_MIN_TIMEOUT_MS      60000
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
  ROBOT_MOTION_SOURCE_MUSIC,
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
  uint32_t music_session_id;
};

const char *robot_motion_direction_name(
    enum robot_motion_direction_e direction);

int robot_motion_direction_from_name(const char *name,
                                     enum robot_motion_direction_e *direction);

unsigned int robot_motion_default_period(
    enum robot_motion_direction_e direction);

/* Return the common reduced-speed interval used by four-limb motion. */
int robot_motion_limb_interval(unsigned int period_ms);

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

int robot_motion_music_tail_wag_sync(uint32_t session_id, uint32_t event_id,
                                     unsigned int period_ms,
                                     unsigned int amplitude_deg);

/* Session-authorized runtime burst. Positive request id, never waits on DMA. */
int robot_motion_music_tail_repeat_async(uint32_t session_id, uint32_t event_id);

/* The legacy robotctl home command uses the same serialized motion gate. */
int robot_motion_home_sync(void);

bool robot_motion_is_busy(void);
enum robot_motion_state_e robot_motion_get_state(void);
int robot_motion_get_active(struct robot_motion_request_s *request);

/* Cancel/drain only the current music-tail request before a foreground task.
 * Reuses the motion worker's existing mutex/condition variable. */
int robot_motion_quiesce_music(void);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_MOTION_H */
