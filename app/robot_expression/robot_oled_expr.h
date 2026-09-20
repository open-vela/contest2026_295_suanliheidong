/*
 * Contest-local OLED expression renderer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_OLED_EXPR_H
#define __CONTEST_ROBOT_OLED_EXPR_H

#include <stdint.h>

#include "robot_expression.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOT_OLED_WIDTH  128
#define ROBOT_OLED_HEIGHT 64
#define ROBOT_OLED_ROW_BYTES (ROBOT_OLED_WIDTH / 8)

/* Return the semantic animation frame for a 200 ms animation tick.
 * The value only changes when the visible bitmap should change, allowing the
 * OLED worker to avoid unnecessary full-screen I2C refreshes. */
unsigned int robot_oled_animation_frame(enum robot_expression_e expression,
                                        uint32_t tick);

void robot_oled_render_expression_frame(
    enum robot_expression_e expression, unsigned int animation_frame,
    uint8_t frame[ROBOT_OLED_HEIGHT][ROBOT_OLED_ROW_BYTES]);

/* Compatibility wrapper: render the first frame of an expression. */
void robot_oled_render_expression(enum robot_expression_e expression,
                                  uint8_t frame[ROBOT_OLED_HEIGHT]
                                                [ROBOT_OLED_ROW_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_OLED_EXPR_H */
