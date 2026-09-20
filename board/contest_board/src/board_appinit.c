/****************************************************************************
 * board/contest_board/src/board_appinit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/board.h>

#include "contest_board.h"

#ifdef CONFIG_BOARDCTL
/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
  (void)arg;

#ifdef CONFIG_BOARD_LATE_INITIALIZE
  return 0;
#else
  return contest_board_bringup();
#endif
}
#endif /* CONFIG_BOARDCTL */
