/*
 * Contest-local observer for the official Music DJ status API.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_MUSIC_OBSERVER_H
#define __CONTEST_ROBOT_MUSIC_OBSERVER_H

#ifdef __cplusplus
extern "C"
{
#endif

int robot_music_observer_start(void);
int robot_music_observer_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_MUSIC_OBSERVER_H */
