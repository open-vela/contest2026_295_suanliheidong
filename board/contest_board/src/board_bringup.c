/****************************************************************************
 * board/contest_board/src/board_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sys/mount.h>

#include <nuttx/board.h>

#ifdef CONFIG_ARCH_CHIP_ESP32S3
#  include "esp32s3_reset_reasons.h"
#endif

#if defined(CONFIG_EXAMPLES_AI_AGENT_VELA) || defined(CONFIG_FS_PROCFS)
#  include <syslog.h>
#endif

#ifdef CONFIG_INPUT_BUTTONS_LOWER
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_ESP32S3_LEDC
#  include "esp32s3_board_ledc.h"
#endif

#ifdef CONFIG_ESP32S3_I2S
#  include "esp32s3_i2s.h"
#endif

#if defined(CONFIG_CONTEST_BOARD_I2S0_RX) || \
    defined(CONFIG_CONTEST_BOARD_I2S1_TX)
#  include "contest_i2s.h"
#endif

#ifdef CONFIG_ESP32S3_RT_TIMER
#  include "esp32s3_rt_timer.h"
#endif

#ifdef CONFIG_ESP32S3_WIFI
#  include "esp32s3_board_wlan.h"
#endif

#include <arch/board/board.h>

#include "esp32s3_gpio.h"

#include "contest_board.h"

/****************************************************************************
 * Diagnostic Configuration
 ****************************************************************************/

/* Build F restores contest I2S/GDMA and PCM audio-device registration. */

#define CONTEST_DIAG_DISABLE_AUDIO_BRINGUP 0

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_ARCH_CHIP_ESP32S3
static const char *contest_reset_reason_name(soc_reset_reason_t reason)
{
  switch ((int)reason)
    {
      case 0x01:
        return "POWERON_OR_BROWNOUT_OR_SUPER_WDT";
      case RESET_REASON_CORE_SW:
        return "SW_RESET";
      case RESET_REASON_CORE_DEEP_SLEEP:
        return "DEEP_SLEEP";
      case RESET_REASON_CORE_MWDT0:
        return "CORE_MWDT0";
      case RESET_REASON_CORE_MWDT1:
        return "CORE_MWDT1";
      case RESET_REASON_CORE_RTC_WDT:
        return "CORE_RTC_WDT";
      case RESET_REASON_CPU0_MWDT0:
        return "CPU_MWDT0";
      case RESET_REASON_CPU0_SW:
        return "CPU_SW_RESET";
      case RESET_REASON_CPU0_RTC_WDT:
        return "CPU_RTC_WDT";
      case RESET_REASON_SYS_BROWN_OUT:
        return "BROWNOUT";
      case RESET_REASON_SYS_RTC_WDT:
        return "SYS_RTC_WDT";
      case RESET_REASON_CPU0_MWDT1:
        return "CPU_MWDT1";
      case RESET_REASON_SYS_SUPER_WDT:
        return "SUPER_WDT";
      case RESET_REASON_SYS_CLK_GLITCH:
        return "CLOCK_GLITCH";
      case RESET_REASON_CORE_EFUSE_CRC:
        return "EFUSE_CRC";
      case RESET_REASON_CORE_USB_UART:
        return "USB_UART";
      case RESET_REASON_CORE_USB_JTAG:
        return "USB_JTAG";
      case RESET_REASON_CORE_PWR_GLITCH:
        return "POWER_GLITCH";
      default:
        return "OTHER";
    }
}
#endif

/****************************************************************************
 * Name: contest_board_bringup
 ****************************************************************************/

int contest_board_bringup(void)
{
  int ret = 0;

#ifdef CONFIG_ARCH_CHIP_ESP32S3
  /* Record reset causes before peripheral initialization can obscure them. */

  soc_reset_reason_t procpu_reason = esp32s3_reset_reasons(0);
  soc_reset_reason_t appcpu_reason = esp32s3_reset_reasons(1);

  syslog(LOG_INFO,
         "[BOOT-DIAG] reset_reason procpu=%s(0x%x) appcpu=%s(0x%x)\n",
         contest_reset_reason_name(procpu_reason), (int)procpu_reason,
         contest_reset_reason_name(appcpu_reason), (int)appcpu_reason);
#endif

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0 && ret != -EBUSY)
    {
      syslog(LOG_WARNING, "WARNING: Failed to mount procfs at /proc: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  ret = nx_mount(NULL, "/data", "tmpfs", 0, NULL);
  if (ret < 0 && ret != -EBUSY)
    {
      syslog(LOG_WARNING, "WARNING: Failed to mount /data tmpfs: %d\n", ret);
    }
#endif

  ret = 0;

#ifdef CONFIG_ARCH_CHIP_ESP32S3
  /* Initialize the verified board LED as a safe, low-level GPIO output.
   * The electrical LED polarity is intentionally not assumed here.
   */

  esp32s3_configgpio(LED_GPIO_PIN, OUTPUT_FUNCTION_2);
  esp32s3_gpiowrite(LED_GPIO_PIN, 0);
#endif

#if defined(CONFIG_INPUT_BUTTONS_LOWER) && \
    defined(CONFIG_CONTEST_BOARD_REGISTER_BUTTONS) && \
    CONFIG_CONTEST_BOARD_REGISTER_BUTTONS
  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      return ret;
    }
#endif

#ifdef CONFIG_I2C_DRIVER
  ret = board_i2c_init();
  if (ret < 0)
    {
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S3_LEDC
  ret = esp32s3_pwm_setup();
  if (ret < 0)
    {
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S3_RT_TIMER
  ret = esp32s3_rt_timer_init();
  if (ret < 0)
    {
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S3_WIFI
  ret = board_wlan_init();
  if (ret < 0)
    {
      return ret;
    }
#endif

#if !CONTEST_DIAG_DISABLE_AUDIO_BRINGUP && defined(CONFIG_ESP32S3_I2S0)
#  ifdef CONFIG_CONTEST_BOARD_I2S0_RX
  if (contest_i2s_initialize(ESP32S3_I2S0) == NULL)
#  else
  if (esp32s3_i2sbus_initialize(ESP32S3_I2S0) == NULL)
#  endif
    {
      return -ENODEV;
    }
#endif

#if !CONTEST_DIAG_DISABLE_AUDIO_BRINGUP && defined(CONFIG_ESP32S3_I2S1)
#  ifdef CONFIG_CONTEST_BOARD_I2S1_TX
  if (contest_i2s_initialize(ESP32S3_I2S1) == NULL)
#  else
  if (esp32s3_i2sbus_initialize(ESP32S3_I2S1) == NULL)
#  endif
    {
      return -ENODEV;
    }
#endif

#if !CONTEST_DIAG_DISABLE_AUDIO_BRINGUP && \
    defined(CONFIG_CONTEST_BOARD_I2S1_TX) && \
    defined(CONFIG_DRIVERS_AUDIO) && defined(CONFIG_AUDIO_I2S)
  ret = board_voice_audio_initialize();
  if (ret < 0)
    {
      return ret;
    }
#endif

  return 0;
}
