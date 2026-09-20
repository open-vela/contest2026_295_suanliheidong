/****************************************************************************
 * board/contest_board/src/contest_board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_CONTEST_BOARD_SRC_CONTEST_BOARD_H
#define __BOARD_CONTEST_BOARD_SRC_CONTEST_BOARD_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int contest_board_bringup(void);

#ifdef CONFIG_I2C_DRIVER
int board_i2c_init(void);
#endif

#endif /* __BOARD_CONTEST_BOARD_SRC_CONTEST_BOARD_H */
