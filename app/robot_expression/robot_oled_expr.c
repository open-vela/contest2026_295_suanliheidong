/*
 * Contest-local expressive OLED face renderer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <string.h>

#include "robot_oled_expr.h"

static void robot_oled_pixel(uint8_t frame[ROBOT_OLED_HEIGHT]
                                             [ROBOT_OLED_ROW_BYTES],
                             int x, int y)
{
  if (x < 0 || x >= ROBOT_OLED_WIDTH || y < 0 || y >= ROBOT_OLED_HEIGHT)
    {
      return;
    }

#ifdef CONFIG_LCD_PACKEDMSFIRST
  frame[y][x >> 3] |= (uint8_t)(0x80u >> (x & 7));
#else
  frame[y][x >> 3] |= (uint8_t)(1u << (x & 7));
#endif
}

static void robot_oled_line(uint8_t frame[ROBOT_OLED_HEIGHT]
                                            [ROBOT_OLED_ROW_BYTES],
                            int x0, int y0, int x1, int y1)
{
  int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
  int sx = x0 < x1 ? 1 : -1;
  int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
  int sy = y0 < y1 ? 1 : -1;
  int error = dx + dy;

  for (;;)
    {
      robot_oled_pixel(frame, x0, y0);
      if (x0 == x1 && y0 == y1)
        {
          break;
        }

      int twice = 2 * error;
      if (twice >= dy)
        {
          error += dy;
          x0 += sx;
        }
      if (twice <= dx)
        {
          error += dx;
          y0 += sy;
        }
    }
}

static void robot_oled_rect(uint8_t frame[ROBOT_OLED_HEIGHT]
                                            [ROBOT_OLED_ROW_BYTES],
                            int left, int top, int right, int bottom,
                            bool filled)
{
  int x;
  int y;

  if (filled)
    {
      for (y = top; y <= bottom; y++)
        {
          for (x = left; x <= right; x++)
            {
              robot_oled_pixel(frame, x, y);
            }
        }
    }
  else
    {
      for (x = left; x <= right; x++)
        {
          robot_oled_pixel(frame, x, top);
          robot_oled_pixel(frame, x, bottom);
        }
      for (y = top; y <= bottom; y++)
        {
          robot_oled_pixel(frame, left, y);
          robot_oled_pixel(frame, right, y);
        }
    }
}

static void robot_oled_eye(uint8_t frame[ROBOT_OLED_HEIGHT]
                                           [ROBOT_OLED_ROW_BYTES],
                           int center_x, int center_y, int pupil_x,
                           int pupil_y)
{
  robot_oled_rect(frame, center_x - 13, center_y - 9,
                  center_x + 13, center_y + 9, false);
  robot_oled_rect(frame, center_x + pupil_x - 3, center_y + pupil_y - 4,
                  center_x + pupil_x + 3, center_y + pupil_y + 4, true);
}

static void robot_oled_closed_eye(uint8_t frame[ROBOT_OLED_HEIGHT]
                                                  [ROBOT_OLED_ROW_BYTES],
                                  int center_x, int center_y)
{
  robot_oled_line(frame, center_x - 12, center_y + 2,
                  center_x - 5, center_y - 3);
  robot_oled_line(frame, center_x - 5, center_y - 3,
                  center_x + 5, center_y - 3);
  robot_oled_line(frame, center_x + 5, center_y - 3,
                  center_x + 12, center_y + 2);
}

static void robot_oled_smile(uint8_t frame[ROBOT_OLED_HEIGHT]
                                             [ROBOT_OLED_ROW_BYTES],
                             int center_y, bool open)
{
  robot_oled_line(frame, 48, center_y, 54, center_y + 5);
  robot_oled_line(frame, 54, center_y + 5, 64, center_y + 7);
  robot_oled_line(frame, 64, center_y + 7, 74, center_y + 5);
  robot_oled_line(frame, 74, center_y + 5, 80, center_y);
  if (open)
    {
      robot_oled_line(frame, 53, center_y + 4, 75, center_y + 4);
    }
}

static void robot_oled_frown(uint8_t frame[ROBOT_OLED_HEIGHT]
                                             [ROBOT_OLED_ROW_BYTES])
{
  robot_oled_line(frame, 48, 52, 54, 47);
  robot_oled_line(frame, 54, 47, 64, 45);
  robot_oled_line(frame, 64, 45, 74, 47);
  robot_oled_line(frame, 74, 47, 80, 52);
}

static void robot_oled_cross_eye(uint8_t frame[ROBOT_OLED_HEIGHT]
                                                [ROBOT_OLED_ROW_BYTES],
                                 int center_x, int center_y)
{
  robot_oled_line(frame, center_x - 9, center_y - 9,
                  center_x + 9, center_y + 9);
  robot_oled_line(frame, center_x + 9, center_y - 9,
                  center_x - 9, center_y + 9);
}

static void robot_oled_heart(uint8_t frame[ROBOT_OLED_HEIGHT]
                                            [ROBOT_OLED_ROW_BYTES],
                             int center_x, int center_y)
{
  robot_oled_rect(frame, center_x - 8, center_y - 5,
                  center_x - 2, center_y + 1, true);
  robot_oled_rect(frame, center_x + 2, center_y - 5,
                  center_x + 8, center_y + 1, true);
  robot_oled_rect(frame, center_x - 10, center_y - 2,
                  center_x + 10, center_y + 2, true);
  robot_oled_rect(frame, center_x - 7, center_y + 3,
                  center_x + 7, center_y + 5, true);
  robot_oled_rect(frame, center_x - 4, center_y + 6,
                  center_x + 4, center_y + 7, true);
  robot_oled_pixel(frame, center_x, center_y + 8);
}

static void robot_oled_star(uint8_t frame[ROBOT_OLED_HEIGHT]
                                           [ROBOT_OLED_ROW_BYTES],
                            int center_x, int center_y)
{
  robot_oled_line(frame, center_x, center_y - 9,
                  center_x + 3, center_y - 2);
  robot_oled_line(frame, center_x + 3, center_y - 2,
                  center_x + 10, center_y - 2);
  robot_oled_line(frame, center_x + 10, center_y - 2,
                  center_x + 5, center_y + 3);
  robot_oled_line(frame, center_x + 5, center_y + 3,
                  center_x + 7, center_y + 10);
  robot_oled_line(frame, center_x + 7, center_y + 10,
                  center_x, center_y + 6);
  robot_oled_line(frame, center_x, center_y + 6,
                  center_x - 7, center_y + 10);
  robot_oled_line(frame, center_x - 7, center_y + 10,
                  center_x - 5, center_y + 3);
  robot_oled_line(frame, center_x - 5, center_y + 3,
                  center_x - 10, center_y - 2);
  robot_oled_line(frame, center_x - 10, center_y - 2,
                  center_x - 3, center_y - 2);
  robot_oled_line(frame, center_x - 3, center_y - 2,
                  center_x, center_y - 9);
}

static void robot_oled_sunglasses(uint8_t frame[ROBOT_OLED_HEIGHT]
                                                 [ROBOT_OLED_ROW_BYTES])
{
  robot_oled_rect(frame, 13, 20, 44, 37, true);
  robot_oled_rect(frame, 84, 20, 115, 37, true);
  robot_oled_line(frame, 44, 24, 84, 24);
  robot_oled_line(frame, 44, 25, 84, 25);
  robot_oled_line(frame, 13, 21, 4, 17);
  robot_oled_line(frame, 115, 21, 124, 17);
}

static void robot_oled_open_mouth(uint8_t frame[ROBOT_OLED_HEIGHT]
                                                 [ROBOT_OLED_ROW_BYTES],
                                  int left, int top, int right, int bottom)
{
  robot_oled_rect(frame, left, top, right, bottom, false);
  robot_oled_rect(frame, left + 2, top + 2, right - 2, bottom - 2, false);
}

static void robot_oled_note(uint8_t frame[ROBOT_OLED_HEIGHT]
                                           [ROBOT_OLED_ROW_BYTES],
                            int x, int y)
{
  robot_oled_rect(frame, x, y + 9, x + 5, y + 13, true);
  robot_oled_line(frame, x + 5, y + 10, x + 5, y - 2);
  robot_oled_line(frame, x + 5, y - 2, x + 12, y);
  robot_oled_line(frame, x + 12, y, x + 12, y + 4);
}

static void robot_oled_question(uint8_t frame[ROBOT_OLED_HEIGHT]
                                               [ROBOT_OLED_ROW_BYTES],
                                int x, int y)
{
  robot_oled_line(frame, x, y + 2, x + 4, y - 2);
  robot_oled_line(frame, x + 4, y - 2, x + 10, y - 2);
  robot_oled_line(frame, x + 10, y - 2, x + 13, y + 2);
  robot_oled_line(frame, x + 13, y + 2, x + 13, y + 6);
  robot_oled_line(frame, x + 13, y + 6, x + 7, y + 11);
  robot_oled_line(frame, x + 7, y + 11, x + 7, y + 15);
  robot_oled_rect(frame, x + 5, y + 20, x + 8, y + 23, true);
}

static void robot_oled_sleep_eye(uint8_t frame[ROBOT_OLED_HEIGHT]
                                                [ROBOT_OLED_ROW_BYTES],
                                 int center_x, int center_y)
{
  robot_oled_line(frame, center_x - 12, center_y,
                  center_x, center_y + 3);
  robot_oled_line(frame, center_x, center_y + 3,
                  center_x + 12, center_y);
}

static void robot_oled_proud_eye(uint8_t frame[ROBOT_OLED_HEIGHT]
                                                [ROBOT_OLED_ROW_BYTES],
                                 int center_x, int center_y, bool left)
{
  if (left)
    {
      robot_oled_line(frame, center_x - 11, center_y + 2,
                      center_x + 10, center_y - 4);
    }
  else
    {
      robot_oled_line(frame, center_x - 10, center_y - 4,
                      center_x + 11, center_y + 2);
    }
  robot_oled_rect(frame, center_x - 3, center_y + 2,
                  center_x + 3, center_y + 7, true);
}


static void robot_oled_half_eye(uint8_t frame[ROBOT_OLED_HEIGHT]
                                             [ROBOT_OLED_ROW_BYTES],
                                int center_x, int center_y)
{
  robot_oled_line(frame, center_x - 12, center_y - 1,
                  center_x + 12, center_y - 1);
  robot_oled_line(frame, center_x - 10, center_y + 2,
                  center_x + 10, center_y + 2);
  robot_oled_rect(frame, center_x - 2, center_y + 1,
                  center_x + 2, center_y + 5, true);
}

static void robot_oled_yawn_mouth(uint8_t frame[ROBOT_OLED_HEIGHT]
                                                [ROBOT_OLED_ROW_BYTES],
                                  bool wide)
{
  if (wide)
    {
      robot_oled_open_mouth(frame, 54, 42, 74, 61);
      robot_oled_rect(frame, 59, 54, 69, 59, false);
    }
  else
    {
      robot_oled_open_mouth(frame, 58, 46, 70, 58);
    }
}

unsigned int robot_oled_animation_frame(enum robot_expression_e expression,
                                        uint32_t tick)
{
  uint32_t phase;

  switch (expression)
    {
      case ROBOT_EXPRESSION_IDLE:
        /* About an 18 s personality loop at the OLED worker's 200 ms tick.
         * Most ticks map to frame 0, so they do not refresh the panel. */
        phase = tick % 90u;
        if (phase == 12u || phase == 42u)
          {
            return 1; /* blink */
          }
        if (phase == 24u || phase == 25u)
          {
            return 2; /* glance left */
          }
        if (phase == 30u || phase == 31u)
          {
            return 3; /* glance right */
          }
        if (phase >= 65u && phase <= 67u)
          {
            return 4; /* getting sleepy */
          }
        if (phase == 68u || phase == 72u)
          {
            return 5; /* small yawn */
          }
        if (phase >= 69u && phase <= 71u)
          {
            return 6; /* big yawn */
          }
        if (phase == 73u)
          {
            return 1; /* eyes closed after yawn */
          }
        return 0;

      case ROBOT_EXPRESSION_LISTENING:
        phase = tick % 8u;
        if (phase == 2u || phase == 3u)
          {
            return 1;
          }
        if (phase == 6u || phase == 7u)
          {
            return 2;
          }
        return 0;

      case ROBOT_EXPRESSION_THINKING:
        return (unsigned int)((tick / 2u) % 3u);

      case ROBOT_EXPRESSION_SPEAKING:
        if ((tick % 20u) == 19u)
          {
            return 4; /* occasional blink */
          }
        return (unsigned int)(tick % 4u);

      case ROBOT_EXPRESSION_HAPPY:
        return (tick % 14u) == 10u ? 1u : 0u;

      case ROBOT_EXPRESSION_EXCITED:
        return (unsigned int)(tick % 3u);

      case ROBOT_EXPRESSION_WINK:
        return (tick % 8u) < 2u ? 1u : 0u;

      case ROBOT_EXPRESSION_LOVE:
        return (unsigned int)((tick / 2u) % 2u);

      case ROBOT_EXPRESSION_SURPRISED:
        return (unsigned int)((tick / 2u) % 2u);

      case ROBOT_EXPRESSION_COOL:
        return (unsigned int)((tick / 3u) % 2u);

      case ROBOT_EXPRESSION_PLAYFUL:
        return (unsigned int)(tick % 3u);

      case ROBOT_EXPRESSION_SLEEPY:
        phase = tick % 16u;
        if (phase >= 9u && phase <= 12u)
          {
            return 2; /* yawn */
          }
        return phase >= 6u ? 1u : 0u;

      case ROBOT_EXPRESSION_SINGING:
        return (unsigned int)(tick % 4u);

      case ROBOT_EXPRESSION_CURIOUS:
        return (unsigned int)((tick / 2u) % 3u);

      case ROBOT_EXPRESSION_PROUD:
        return (unsigned int)((tick / 3u) % 2u);

      case ROBOT_EXPRESSION_ERROR:
      default:
        return 0;
    }
}

void robot_oled_render_expression_frame(
    enum robot_expression_e expression, unsigned int animation_frame,
    uint8_t frame[ROBOT_OLED_HEIGHT][ROBOT_OLED_ROW_BYTES])
{
  memset(frame, 0, ROBOT_OLED_HEIGHT * ROBOT_OLED_ROW_BYTES);

  switch (expression)
    {
      case ROBOT_EXPRESSION_LISTENING:
        robot_oled_rect(frame, 14, 17, 42, 43, true);
        robot_oled_rect(frame, 86, 17, 114, 43, true);
        if (animation_frame == 1)
          {
            robot_oled_rect(frame, 18, 23, 30, 37, false);
            robot_oled_rect(frame, 90, 23, 102, 37, false);
          }
        else if (animation_frame == 2)
          {
            robot_oled_rect(frame, 26, 23, 38, 37, false);
            robot_oled_rect(frame, 98, 23, 110, 37, false);
          }
        else
          {
            robot_oled_rect(frame, 22, 23, 34, 37, false);
            robot_oled_rect(frame, 94, 23, 106, 37, false);
          }
        robot_oled_rect(frame, 56, 47, 72, 57, true);
        break;

      case ROBOT_EXPRESSION_THINKING:
        if (animation_frame == 2)
          {
            robot_oled_eye(frame, 28, 30, -5, -3);
            robot_oled_eye(frame, 100, 30, -5, -3);
          }
        else
          {
            robot_oled_eye(frame, 28, 30, 5, -3);
            robot_oled_eye(frame, 100, 30, 5, -3);
          }
        robot_oled_line(frame, 52, 51, 76, 51);
        robot_oled_rect(frame, 83, 7, 91, 15, false);
        if (animation_frame >= 1)
          {
            robot_oled_rect(frame, 94, 2, 102, 8, false);
          }
        if (animation_frame >= 2)
          {
            robot_oled_rect(frame, 106, 9, 112, 15, false);
          }
        break;

      case ROBOT_EXPRESSION_HAPPY:
        if (animation_frame == 1)
          {
            robot_oled_closed_eye(frame, 28, 29);
            robot_oled_eye(frame, 100, 29, 2, 0);
          }
        else
          {
            robot_oled_closed_eye(frame, 28, 29);
            robot_oled_closed_eye(frame, 100, 29);
          }
        robot_oled_smile(frame, 45, true);
        break;

      case ROBOT_EXPRESSION_SPEAKING:
        if (animation_frame == 4)
          {
            robot_oled_closed_eye(frame, 28, 29);
            robot_oled_closed_eye(frame, 100, 29);
            robot_oled_open_mouth(frame, 58, 47, 70, 56);
          }
        else
          {
            robot_oled_eye(frame, 28, 29, 0, 0);
            robot_oled_eye(frame, 100, 29, 0, 0);
            if (animation_frame == 0)
              {
                robot_oled_line(frame, 56, 50, 72, 50);
              }
            else if (animation_frame == 1 || animation_frame == 3)
              {
                robot_oled_open_mouth(frame, 57, 46, 71, 55);
              }
            else
              {
                robot_oled_open_mouth(frame, 53, 42, 75, 59);
              }
          }
        break;

      case ROBOT_EXPRESSION_ERROR:
        robot_oled_cross_eye(frame, 28, 30);
        robot_oled_cross_eye(frame, 100, 30);
        robot_oled_frown(frame);
        break;

      case ROBOT_EXPRESSION_EXCITED:
        robot_oled_star(frame, 28, 27);
        robot_oled_star(frame, 100, 27);
        robot_oled_smile(frame, animation_frame == 1 ? 42 : 44, true);
        if (animation_frame == 2)
          {
            robot_oled_pixel(frame, 7, 10);
            robot_oled_pixel(frame, 120, 12);
            robot_oled_pixel(frame, 11, 17);
            robot_oled_pixel(frame, 116, 19);
          }
        break;

      case ROBOT_EXPRESSION_WINK:
        if (animation_frame == 1)
          {
            robot_oled_closed_eye(frame, 28, 29);
            robot_oled_closed_eye(frame, 100, 29);
          }
        else
          {
            robot_oled_closed_eye(frame, 28, 29);
            robot_oled_eye(frame, 100, 29, 2, 0);
          }
        robot_oled_smile(frame, 46, true);
        break;

      case ROBOT_EXPRESSION_LOVE:
        robot_oled_heart(frame, 28, 27);
        robot_oled_heart(frame, 100, 27);
        robot_oled_smile(frame, 45, true);
        if (animation_frame == 1)
          {
            robot_oled_pixel(frame, 9, 10);
            robot_oled_line(frame, 9, 7, 9, 13);
            robot_oled_line(frame, 6, 10, 12, 10);
            robot_oled_pixel(frame, 119, 9);
            robot_oled_line(frame, 119, 6, 119, 12);
            robot_oled_line(frame, 116, 9, 122, 9);
          }
        break;

      case ROBOT_EXPRESSION_SURPRISED:
        robot_oled_eye(frame, 28, 27, 0, 0);
        robot_oled_eye(frame, 100, 27, 0, 0);
        if (animation_frame == 0)
          {
            robot_oled_open_mouth(frame, 58, 46, 70, 58);
          }
        else
          {
            robot_oled_open_mouth(frame, 54, 42, 74, 61);
          }
        break;

      case ROBOT_EXPRESSION_COOL:
        robot_oled_sunglasses(frame);
        robot_oled_smile(frame, animation_frame ? 47 : 45, false);
        break;

      case ROBOT_EXPRESSION_PLAYFUL:
        if (animation_frame == 2)
          {
            robot_oled_eye(frame, 28, 27, 2, 1);
            robot_oled_closed_eye(frame, 100, 27);
          }
        else
          {
            robot_oled_closed_eye(frame, 28, 27);
            robot_oled_eye(frame, 100, 27, -2, 1);
          }
        robot_oled_open_mouth(frame, 54, 43, 74, 57);
        if (animation_frame == 1)
          {
            robot_oled_rect(frame, 56, 53, 63, 61, true);
          }
        else
          {
            robot_oled_rect(frame, 64, 53, 71, 61, true);
          }
        break;

      case ROBOT_EXPRESSION_SLEEPY:
        if (animation_frame == 2)
          {
            robot_oled_closed_eye(frame, 28, 28);
            robot_oled_closed_eye(frame, 100, 28);
            robot_oled_yawn_mouth(frame, true);
          }
        else
          {
            robot_oled_sleep_eye(frame, 28, 29);
            robot_oled_sleep_eye(frame, 100, 29);
            robot_oled_line(frame, 58, 49, 70, 49);
          }
        if (animation_frame != 0)
          {
            robot_oled_line(frame, 104, 8, 114, 8);
            robot_oled_line(frame, 114, 8, 104, 17);
            robot_oled_line(frame, 104, 17, 114, 17);
            robot_oled_line(frame, 113, 1, 121, 1);
            robot_oled_line(frame, 121, 1, 113, 7);
            robot_oled_line(frame, 113, 7, 121, 7);
          }
        break;

      case ROBOT_EXPRESSION_SINGING:
        if (animation_frame == 3)
          {
            robot_oled_eye(frame, 28, 28, 0, 0);
            robot_oled_eye(frame, 100, 28, 0, 0);
          }
        else
          {
            robot_oled_closed_eye(frame, 28, 28);
            robot_oled_closed_eye(frame, 100, 28);
          }
        if ((animation_frame & 1u) == 0)
          {
            robot_oled_open_mouth(frame, 58, 47, 70, 57);
            robot_oled_note(frame, 6, 8);
            robot_oled_note(frame, 111, 3);
          }
        else
          {
            robot_oled_open_mouth(frame, 54, 42, 74, 60);
            robot_oled_note(frame, 10, 3);
            robot_oled_note(frame, 107, 9);
          }
        break;

      case ROBOT_EXPRESSION_CURIOUS:
        if (animation_frame == 0)
          {
            robot_oled_eye(frame, 28, 30, 5, -1);
            robot_oled_eye(frame, 96, 30, 5, -1);
          }
        else if (animation_frame == 1)
          {
            robot_oled_eye(frame, 28, 30, -4, -1);
            robot_oled_eye(frame, 96, 30, -4, -1);
          }
        else
          {
            robot_oled_eye(frame, 28, 30, 0, 2);
            robot_oled_eye(frame, 96, 30, 0, 2);
          }
        robot_oled_line(frame, 53, 51, 72, 51);
        if (animation_frame != 1)
          {
            robot_oled_question(frame, 108, 6);
          }
        break;

      case ROBOT_EXPRESSION_PROUD:
        robot_oled_proud_eye(frame, 28, 27, true);
        robot_oled_proud_eye(frame, 100, 27, false);
        robot_oled_smile(frame, animation_frame ? 43 : 45,
                         animation_frame != 0);
        break;

      case ROBOT_EXPRESSION_IDLE:
      default:
        if (animation_frame == 1)
          {
            robot_oled_closed_eye(frame, 28, 30);
            robot_oled_closed_eye(frame, 100, 30);
            robot_oled_smile(frame, 47, false);
          }
        else if (animation_frame == 2)
          {
            robot_oled_eye(frame, 28, 30, -5, 1);
            robot_oled_eye(frame, 100, 30, -5, 1);
            robot_oled_smile(frame, 47, false);
          }
        else if (animation_frame == 3)
          {
            robot_oled_eye(frame, 28, 30, 5, 1);
            robot_oled_eye(frame, 100, 30, 5, 1);
            robot_oled_smile(frame, 47, false);
          }
        else if (animation_frame == 4)
          {
            robot_oled_half_eye(frame, 28, 30);
            robot_oled_half_eye(frame, 100, 30);
            robot_oled_line(frame, 59, 50, 69, 50);
          }
        else if (animation_frame == 5)
          {
            robot_oled_closed_eye(frame, 28, 29);
            robot_oled_closed_eye(frame, 100, 29);
            robot_oled_yawn_mouth(frame, false);
          }
        else if (animation_frame == 6)
          {
            robot_oled_closed_eye(frame, 28, 28);
            robot_oled_closed_eye(frame, 100, 28);
            robot_oled_yawn_mouth(frame, true);
            robot_oled_line(frame, 107, 8, 117, 8);
            robot_oled_line(frame, 117, 8, 107, 17);
            robot_oled_line(frame, 107, 17, 117, 17);
          }
        else
          {
            robot_oled_eye(frame, 28, 30, 0, 0);
            robot_oled_eye(frame, 100, 30, 0, 0);
            robot_oled_smile(frame, 47, false);
          }
        break;
    }
}

void robot_oled_render_expression(enum robot_expression_e expression,
                                  uint8_t frame[ROBOT_OLED_HEIGHT]
                                                [ROBOT_OLED_ROW_BYTES])
{
  robot_oled_render_expression_frame(expression, 0, frame);
}
