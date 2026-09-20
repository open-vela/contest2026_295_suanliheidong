/****************************************************************************
 * board/contest_board/src/contest_i2s.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_CONTEST_BOARD_SRC_CONTEST_I2S_H
#define __BOARD_CONTEST_BOARD_SRC_CONTEST_I2S_H

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/audio/i2s.h>

FAR struct i2s_dev_s *contest_i2s_initialize(int port);
void contest_i2s_set_rx_diagnostics(bool enabled);

#endif /* __BOARD_CONTEST_BOARD_SRC_CONTEST_I2S_H */
