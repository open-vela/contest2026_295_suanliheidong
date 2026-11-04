/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROBOT_TASK_H
#define ROBOT_TASK_H
#include <stdbool.h>

/* Reuse the Agent task and its existing inbound queue. Begin after dequeue,
 * end after the task AND its asynchronous speech have finished. Never call
 * these from WLAN/WebClient callbacks or while holding a voice/music lock. */
int robot_task_begin(void) __attribute__((weak));
void robot_task_end(void) __attribute__((weak));
/* Release a successful outer scope without starting music during shutdown. */
void robot_task_end_without_resume(void) __attribute__((weak));
void robot_task_resume_if_idle(void) __attribute__((weak));
int robot_task_acquire_music_transport(void) __attribute__((weak));
void robot_task_release_music_transport(void) __attribute__((weak));
#endif
