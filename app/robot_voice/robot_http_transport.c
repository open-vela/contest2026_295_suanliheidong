/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <infra/network_manager.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include "robot_task.h"

/* One contest-local gate covers official Agent HTTPS and voice HTTP. */
static pthread_mutex_t g_http_transport_lock = PTHREAD_MUTEX_INITIALIZER;
/* Replace the old bool, rather than allocate another scheduler or queue.
 * The upper bits count foreground scopes; the low bits own the transport. */
static uint8_t g_http_transport_state;
#define RV_TRANSPORT_BUSY 1U
#define RV_TRANSPORT_MUSIC 2U
#define RV_TASK_UNIT 4U
extern int robot_music_media_suspend(void) __attribute__((weak));
extern int robot_music_media_resume(void) __attribute__((weak));
extern int robot_motion_quiesce_music(void) __attribute__((weak));
extern bool robot_voice_task_can_resume(void) __attribute__((weak));
extern bool agent_scheduler_can_resume_music(void) __attribute__((weak));

int robot_task_begin(void)
{
  int ret = 0;
  pthread_mutex_lock(&g_http_transport_lock);
  if (g_http_transport_state > UINT8_MAX - RV_TASK_UNIT)
    ret = -EOVERFLOW;
  else
    g_http_transport_state += RV_TASK_UNIT;
  pthread_mutex_unlock(&g_http_transport_lock);
  if (ret < 0) return ret;
  if (robot_music_media_suspend != NULL) ret = robot_music_media_suspend();
  if (ret == 0 && robot_motion_quiesce_music != NULL)
    ret = robot_motion_quiesce_music();
  if (ret < 0)
    {
      pthread_mutex_lock(&g_http_transport_lock);
      g_http_transport_state -= RV_TASK_UNIT;
      pthread_mutex_unlock(&g_http_transport_lock);
      /* Suspend/quiesce may have succeeded before a later handoff failed. */
      robot_task_resume_if_idle();
    }
  return ret;
}

void robot_task_resume_if_idle(void)
{
  bool idle;
  pthread_mutex_lock(&g_http_transport_lock);
  idle = g_http_transport_state == 0;
  pthread_mutex_unlock(&g_http_transport_lock);
  if (idle && (!agent_scheduler_can_resume_music || agent_scheduler_can_resume_music()) &&
      (!robot_voice_task_can_resume || robot_voice_task_can_resume()) &&
      robot_music_media_resume != NULL)
    (void)robot_music_media_resume();
}

void robot_task_end_without_resume(void)
{
  pthread_mutex_lock(&g_http_transport_lock);
  if (g_http_transport_state >= RV_TASK_UNIT)
    g_http_transport_state -= RV_TASK_UNIT;
  pthread_mutex_unlock(&g_http_transport_lock);
}

void robot_task_end(void)
{
  robot_task_end_without_resume();
  robot_task_resume_if_idle();
}

int robot_task_acquire_music_transport(void)
{
  int ret = -EBUSY;
  pthread_mutex_lock(&g_http_transport_lock);
  if (g_http_transport_state == 0)
    {
      g_http_transport_state = RV_TRANSPORT_BUSY | RV_TRANSPORT_MUSIC;
      ret = 0;
    }
  pthread_mutex_unlock(&g_http_transport_lock);
  return ret;
}

void robot_task_release_music_transport(void)
{
  pthread_mutex_lock(&g_http_transport_lock);
  if (g_http_transport_state & RV_TRANSPORT_MUSIC)
    g_http_transport_state &= ~(RV_TRANSPORT_BUSY | RV_TRANSPORT_MUSIC);
  pthread_mutex_unlock(&g_http_transport_lock);
  /* No resume here: a stopping worker may be joined under the music lock. */
}

static uint64_t http_transport_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)ts.tv_nsec / 1000000ULL;
}

int network_acquire_http_transport(uint32_t timeout_ms)
{
  uint64_t deadline = http_transport_now_ms() + timeout_ms;
  int ret = robot_task_begin();
  if (ret < 0) return ret;

  /* Do not use pthread_cond_timedwait(CLOCK_REALTIME) here.  The board starts
   * with an unset clock and synchronizes it after Wi-Fi comes up; a jump from
   * 1970 to the current date would make a valid transport wait expire
   * immediately.  A short monotonic polling interval keeps the existing
   * single-HTTPS-request policy without making the wait sensitive to wall
   * clock corrections. */
  for (;;)
    {
      uint64_t now;
      uint32_t wait_ms;

      pthread_mutex_lock(&g_http_transport_lock);
      if (!(g_http_transport_state & RV_TRANSPORT_BUSY))
        {
          g_http_transport_state |= RV_TRANSPORT_BUSY;
          g_http_transport_state -= RV_TASK_UNIT;
          pthread_mutex_unlock(&g_http_transport_lock);
          return 0;
        }
      pthread_mutex_unlock(&g_http_transport_lock);

      now = http_transport_now_ms();
      if (now >= deadline)
        {
          pthread_mutex_lock(&g_http_transport_lock);
          g_http_transport_state -= RV_TASK_UNIT;
          pthread_mutex_unlock(&g_http_transport_lock);
          /* A timed-out waiter must not leave music parked indefinitely. */
          robot_task_resume_if_idle();
          return -ETIMEDOUT;
        }

      wait_ms = (uint32_t)(deadline - now);
      if (wait_ms > 20)
        {
          wait_ms = 20;
        }
      usleep((useconds_t)wait_ms * 1000);
    }
}

void network_release_http_transport(void)
{
  pthread_mutex_lock(&g_http_transport_lock);
  if (!(g_http_transport_state & RV_TRANSPORT_MUSIC))
    g_http_transport_state &= ~RV_TRANSPORT_BUSY;
  pthread_mutex_unlock(&g_http_transport_lock);
  /* Resume only after releasing the gate; the idle predicate also checks
   * pending Agent work and voice/TTS activity. */
  robot_task_resume_if_idle();
}
