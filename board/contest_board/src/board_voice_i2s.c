/****************************************************************************
 * board/contest_board/src/board_voice_i2s.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_ESP32S3_I2S

#include <stddef.h>

#include <nuttx/audio/i2s.h>

#include <arch/board/board.h>

#include "esp32s3_i2s.h"

#if defined(CONFIG_CONTEST_BOARD_I2S0_RX) || \
    defined(CONFIG_CONTEST_BOARD_I2S1_TX)
#  include "contest_i2s.h"
#endif

struct i2s_dev_s *board_voice_mic_i2s(void)
{
#ifdef CONFIG_CONTEST_BOARD_I2S0_RX
  return contest_i2s_initialize(ESP32S3_I2S0);
#elif defined(CONFIG_ESP32S3_I2S0)
  return esp32s3_i2sbus_initialize(ESP32S3_I2S0);
#else
  return NULL;
#endif
}

struct i2s_dev_s *board_voice_speaker_i2s(void)
{
#ifdef CONFIG_CONTEST_BOARD_I2S1_TX
  return contest_i2s_initialize(ESP32S3_I2S1);
#elif defined(CONFIG_ESP32S3_I2S1)
  return esp32s3_i2sbus_initialize(ESP32S3_I2S1);
#else
  return NULL;
#endif
}

void board_voice_mic_set_diagnostics(bool enabled)
{
#ifdef CONFIG_CONTEST_BOARD_I2S0_RX
  contest_i2s_set_rx_diagnostics(enabled);
#else
  (void)enabled;
#endif
}

#endif /* CONFIG_ESP32S3_I2S */
