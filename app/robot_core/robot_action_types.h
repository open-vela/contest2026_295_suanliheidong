/*
 * Contest-local robot action type definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_ACTION_TYPES_H
#define __CONTEST_ROBOT_ACTION_TYPES_H

#ifdef __cplusplus
extern "C"
{
#endif

enum robot_action_type_e
{
  ROBOT_ACTION_EXPRESSION = 0,
  ROBOT_ACTION_MOTION,
  ROBOT_ACTION_POSTURE,
  ROBOT_ACTION_BEHAVIOR,
  ROBOT_ACTION_AUDIO,
  ROBOT_ACTION_SYSTEM,
  ROBOT_ACTION_TYPE_COUNT,
};

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_ACTION_TYPES_H */
