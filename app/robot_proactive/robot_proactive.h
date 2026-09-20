/*
 * Contest-local proactive agent runtime.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_PROACTIVE_H
#define __CONTEST_ROBOT_PROACTIVE_H

#include <stdbool.h>

#include "robot_event.h"
#include "robot_world_state.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOT_PROACTIVE_IDLE_MS       (60ULL * 1000ULL)
#define ROBOT_PROACTIVE_COOLDOWN_MS   (60ULL * 1000ULL)
#define ROBOT_PROACTIVE_QUEUE_DEPTH   8
#define ROBOT_PROACTIVE_STACK_SIZE    (8 * 1024)
#define ROBOT_PROACTIVE_EVENT_FRESHNESS_MS (5ULL * 60ULL * 1000ULL)
#define ROBOT_PROACTIVE_MUSIC_COOLDOWN_MS (10ULL * 1000ULL)

int robot_proactive_init(void);
int robot_proactive_start(void);
int robot_proactive_stop(void);
int robot_proactive_post(enum robot_event_type_e type, const char *detail);
int robot_proactive_post_ex(enum robot_event_type_e type, const char *source,
                            const char *detail,
                            const struct robot_event_payload_s *payload);
int robot_proactive_music_update(enum robot_music_state_e state,
                                 const char *track, const char *category,
                                 const char *mood, uint16_t tempo_bpm);
int robot_proactive_set_enabled(bool enabled);
int robot_proactive_get_status(struct robot_world_state_s *out);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_PROACTIVE_H */
