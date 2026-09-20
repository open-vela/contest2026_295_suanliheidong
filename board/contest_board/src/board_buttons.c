/****************************************************************************
 * board/contest_board/src/board_buttons.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_ARCH_BUTTONS) && \
    defined(CONFIG_CONTEST_BOARD_REGISTER_BUTTONS) && \
    CONFIG_CONTEST_BOARD_REGISTER_BUTTONS

#include <errno.h>
#include <stdint.h>

#include <nuttx/board.h>
#include <nuttx/irq.h>

#include "esp32s3_gpio.h"
#include "esp32s3_irq.h"

#include <arch/board/board.h>

static const uint8_t g_button_gpio[CONTEST_BOARD_NBUTTONS] =
{
  TOUCH_BUTTON_GPIO,
  BOOT_BUTTON_GPIO
};

uint32_t board_button_initialize(void)
{
  esp32s3_configgpio(TOUCH_BUTTON_GPIO, INPUT_FUNCTION_2 | PULLUP);
  esp32s3_configgpio(BOOT_BUTTON_GPIO, INPUT_FUNCTION_2 | PULLUP);

  return CONTEST_BOARD_NBUTTONS;
}

uint32_t board_buttons(void)
{
  uint32_t ret = 0;
  uint32_t i;

  for (i = 0; i < CONTEST_BOARD_NBUTTONS; i++)
    {
      /* The reference ESP32-S3 button boards use active-low inputs. */

      if (!esp32s3_gpioread(g_button_gpio[i]))
        {
          ret |= (1 << i);
        }
    }

  return ret;
}

#ifdef CONFIG_ARCH_IRQBUTTONS
int board_button_irq(int id, xcpt_t irqhandler, void *arg)
{
  int irq;
  int ret;

  if (id < 0 || id >= CONTEST_BOARD_NBUTTONS)
    {
      return -EINVAL;
    }

  irq = ESP32S3_PIN2IRQ(g_button_gpio[id]);

  if (irqhandler != NULL)
    {
      esp32s3_gpioirqdisable(irq);
      ret = irq_attach(irq, irqhandler, arg);
      if (ret < 0)
        {
          return ret;
        }

      esp32s3_gpioirqenable(irq, CHANGE);
    }
  else
    {
      esp32s3_gpioirqdisable(irq);
    }

  return 0;
}
#endif /* CONFIG_ARCH_IRQBUTTONS */

#endif /* CONFIG_ARCH_BUTTONS */
