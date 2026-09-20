/*
 * Contest-local robot expression manager.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_EXPRESSION_H
#define __CONTEST_ROBOT_EXPRESSION_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

enum robot_expression_e
{
  ROBOT_EXPRESSION_IDLE = 0,
  ROBOT_EXPRESSION_LISTENING,
  ROBOT_EXPRESSION_THINKING,
  ROBOT_EXPRESSION_HAPPY,
  ROBOT_EXPRESSION_SPEAKING,
  ROBOT_EXPRESSION_ERROR,
  ROBOT_EXPRESSION_EXCITED,
  ROBOT_EXPRESSION_WINK,
  ROBOT_EXPRESSION_LOVE,
  ROBOT_EXPRESSION_SURPRISED,
  ROBOT_EXPRESSION_COOL,
  ROBOT_EXPRESSION_PLAYFUL,
  ROBOT_EXPRESSION_SLEEPY,
  ROBOT_EXPRESSION_SINGING,
  ROBOT_EXPRESSION_CURIOUS,
  ROBOT_EXPRESSION_PROUD,
  ROBOT_EXPRESSION_COUNT,
};

enum robot_expression_source_e
{
  ROBOT_EXPRESSION_SOURCE_SYSTEM = 0,
  ROBOT_EXPRESSION_SOURCE_AGENT,
  /* Short-lived proactive reactions: above VOICE so a fun face can briefly
   * animate the beginning of a spoken reply, then automatically reveal the
   * underlying SPEAKING state when the reaction expires. */
  ROBOT_EXPRESSION_SOURCE_REACTION,
  ROBOT_EXPRESSION_SOURCE_VOICE,
  ROBOT_EXPRESSION_SOURCE_BEHAVIOR,
  ROBOT_EXPRESSION_SOURCE_MUSIC,
  ROBOT_EXPRESSION_SOURCE_COUNT,
};

int robot_expression_init(void);
int robot_expression_deinit(void);

/* duration_ms == 0 means persistent for VOICE and the default semantic
 * duration for SYSTEM/AGENT. Values above 10000 are rejected. */
int robot_expression_set(enum robot_expression_source_e source,
                         enum robot_expression_e expression,
                         unsigned int duration_ms);
int robot_expression_clear(enum robot_expression_source_e source);

bool robot_expression_is_available(void);
const char *robot_expression_name(enum robot_expression_e expression);

/* Registers the provider after the official tool registry is initialized. */
int robot_expression_register_tool(void);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_EXPRESSION_H */
