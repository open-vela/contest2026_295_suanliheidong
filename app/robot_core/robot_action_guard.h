/*
 * Contest-local guard for physical side-effect tools.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_ACTION_GUARD_H
#define __CONTEST_ROBOT_ACTION_GUARD_H

#include "robot_action_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Reusable policy text for every Tool that changes physical robot state.
 * Query-only Tools must not use this policy.
 */
#define ROBOT_SIDE_EFFECT_TOOL_POLICY \
  "This is a physical side-effect tool. Every new user request requires a fresh tool call. " \
  "Never claim that the physical action happened unless this tool was actually called in the current turn and returned ok=true. " \
  "A previous successful action does not satisfy a new request. "

int robot_action_guard_init(void);

void robot_action_guard_begin(enum robot_action_type_e type,
                              const char *tool_name);

void robot_action_guard_end(enum robot_action_type_e type,
                            const char *tool_name,
                            int action_result);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_ACTION_GUARD_H */
