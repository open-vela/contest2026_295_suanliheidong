/*
 * Contest-local OLED output worker.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_OLED_H
#define __CONTEST_ROBOT_OLED_H

#include "robot_expression.h"

#ifdef __cplusplus
extern "C"
{
#endif

int robot_oled_init(void);
int robot_oled_stop(void);
int robot_oled_submit(enum robot_expression_e expression);
bool robot_oled_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_OLED_H */
