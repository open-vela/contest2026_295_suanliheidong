/****************************************************************************
 * board/contest_board/src/board_boot.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "contest_board.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_board_initialize
 *
 * Description:
 *   This entry point runs after memory setup and before device
 *   initialization.  The minimum board has no verified peripheral that
 *   requires early initialization.
 *
 ****************************************************************************/

void esp32s3_board_initialize(void)
{
}

#ifdef CONFIG_BOARD_LATE_INITIALIZE
/****************************************************************************
 * Name: board_late_initialize
 ****************************************************************************/

void board_late_initialize(void)
{
  (void)contest_board_bringup();
}
#endif
