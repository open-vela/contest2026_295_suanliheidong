/****************************************************************************
 * board/contest_board/src/board_oled.c
 *
 * Contest-local SSD1306 board glue.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#if defined(CONFIG_LCD_SSD1306) && defined(CONFIG_LCD_SSD1306_I2C) && \
    defined(CONFIG_ESP32S3_I2C0)

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/ssd1306.h>

#include "esp32s3_i2c.h"

static struct lcd_dev_s *g_oled;
static bool g_oled_powered;

int board_oled_initialize(void)
{
  struct i2c_master_s *i2c;
  int ret;

  printf("[BOARD-OLED] init begin\n");

  if (g_oled == NULL)
    {
      i2c = esp32s3_i2cbus_initialize(0);
      if (i2c == NULL)
        {
          printf("[BOARD-OLED] FAIL stage=i2c0 rc=%d\n", -ENODEV);
          return -ENODEV;
        }

      printf("[BOARD-OLED] i2c0 ready\n");

      g_oled = ssd1306_initialize(i2c, NULL, 0);
      if (g_oled == NULL)
        {
          printf("[BOARD-OLED] FAIL stage=ssd1306 rc=%d\n", -ENODEV);
          return -ENODEV;
        }

      printf("[BOARD-OLED] ssd1306 ready dev=%p\n", (void *)g_oled);
    }
  else
    {
      printf("[BOARD-OLED] reuse dev=%p powered=%d\n",
             (void *)g_oled, g_oled_powered ? 1 : 0);
    }

  if (g_oled->setpower == NULL)
    {
      printf("[BOARD-OLED] FAIL stage=setpower rc=%d\n", -ENOSYS);
      return -ENOSYS;
    }

  /*
   * Always request panel power here instead of returning immediately when
   * g_oled is already non-NULL.  This makes repeated initialization safe if
   * the LCD device was created successfully but power-up previously failed,
   * or if another path powered the display down.
   */

  ret = g_oled->setpower(g_oled, CONFIG_LCD_MAXPOWER);
  printf("[BOARD-OLED] setpower level=%d rc=%d\n",
         CONFIG_LCD_MAXPOWER, ret);

  if (ret < 0)
    {
      g_oled_powered = false;
      printf("[BOARD-OLED] FAIL stage=setpower rc=%d\n", ret);
      return ret;
    }

  g_oled_powered = true;
  printf("[BOARD-OLED] init success\n");
  return OK;
}

struct lcd_dev_s *board_oled_getdev(void)
{
  return g_oled;
}

#else

int board_oled_initialize(void)
{
  printf("[BOARD-OLED] disabled: require CONFIG_LCD_SSD1306, "
         "CONFIG_LCD_SSD1306_I2C and CONFIG_ESP32S3_I2C0\n");
  return -ENODEV;
}

struct lcd_dev_s *board_oled_getdev(void)
{
  return NULL;
}

#endif
