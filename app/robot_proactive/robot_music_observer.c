/* Compatibility entry points only. The local playback adapter emits confirmed
 * session events; a second music_status polling worker must never be started. */
#include <stdio.h>
#include "robot_music_observer.h"
int robot_music_observer_start(void)
{
  printf("[ROBOT-MUSIC] status observer disabled; direct DMA events active\n");
  return 0;
}
int robot_music_observer_stop(void) { return 0; }
