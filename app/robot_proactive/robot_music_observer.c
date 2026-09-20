/*
 * Contest-local observer for the official Music DJ status API.
 *
 * The official media player owns playback and its event callback.  This
 * module deliberately does not open another player or inspect private state;
 * it polls the public music_status Tool API at a low rate and reports only
 * confirmed state transitions to the contest World State.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "tools/tool_media.h"
#include "robot_music_observer.h"
#include "robot_proactive.h"

#define ROBOT_MUSIC_POLL_MS 2000
#define ROBOT_MUSIC_START_DELAY_MS 5000
#define ROBOT_MUSIC_STATUS_SIZE 768

enum robot_observed_music_state_e
{
  ROBOT_OBSERVED_MUSIC_UNKNOWN = 0,
  ROBOT_OBSERVED_MUSIC_PLAYING,
  ROBOT_OBSERVED_MUSIC_PAUSED,
  ROBOT_OBSERVED_MUSIC_STOPPED,
};

struct robot_music_observer_runtime_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  bool started;
  bool stop_requested;
  enum robot_observed_music_state_e last_state;
  char last_track[64];
};

static struct robot_music_observer_runtime_s g_music_observer =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER,
};

static int robot_music_observer_read_status(
    enum robot_observed_music_state_e *state, char *track, size_t track_size)
{
  char output[ROBOT_MUSIC_STATUS_SIZE];
  cJSON *root;
  cJSON *state_json;
  cJSON *url_json;
  const char *state_text;
  const char *url_text;

  if (state == NULL || track == NULL || track_size == 0)
    {
      return -EINVAL;
    }

  memset(output, 0, sizeof(output));
  if (tool_music_status_execute("{}", output, sizeof(output)) < 0)
    {
      return -EIO;
    }

  root = cJSON_Parse(output);
  if (root == NULL)
    {
      return -EINVAL;
    }

  state_json = cJSON_GetObjectItem(root, "state");
  state_text = cJSON_GetStringValue(state_json);
  url_json = cJSON_GetObjectItem(root, "url");
  url_text = cJSON_GetStringValue(url_json);
  track[0] = '\0';

  if (state_text != NULL && strcmp(state_text, "PLAYING") == 0)
    {
      *state = ROBOT_OBSERVED_MUSIC_PLAYING;
    }
  else if (state_text != NULL && strcmp(state_text, "PAUSED") == 0)
    {
      *state = ROBOT_OBSERVED_MUSIC_PAUSED;
    }
  else
    {
      *state = ROBOT_OBSERVED_MUSIC_STOPPED;
    }

  if (url_text != NULL)
    {
      snprintf(track, track_size, "%s", url_text);
    }

  cJSON_Delete(root);
  return 0;
}

static void robot_music_observer_sample(void)
{
  enum robot_observed_music_state_e observed;
  enum robot_observed_music_state_e previous;
  char track[64];
  char previous_track[64];
  bool report_playing;
  bool report_stopped;

  if (robot_music_observer_read_status(&observed, track, sizeof(track)) < 0)
    {
      return;
    }

  pthread_mutex_lock(&g_music_observer.lock);
  previous = g_music_observer.last_state;
  snprintf(previous_track, sizeof(previous_track), "%s",
           g_music_observer.last_track);
  g_music_observer.last_state = observed;
  if (observed == ROBOT_OBSERVED_MUSIC_PLAYING)
    {
      snprintf(g_music_observer.last_track,
               sizeof(g_music_observer.last_track), "%s", track);
    }
  else if (observed == ROBOT_OBSERVED_MUSIC_STOPPED)
    {
      g_music_observer.last_track[0] = '\0';
    }
  pthread_mutex_unlock(&g_music_observer.lock);

  report_playing = observed == ROBOT_OBSERVED_MUSIC_PLAYING;
  report_stopped = observed == ROBOT_OBSERVED_MUSIC_STOPPED &&
                   (previous == ROBOT_OBSERVED_MUSIC_PLAYING ||
                    previous == ROBOT_OBSERVED_MUSIC_PAUSED);

  if (report_playing)
    {
      /* PAUSED is state-only: keep the last confirmed track without waking
       * the Agent, then let PLAYING resume be observed normally. */
      if (previous != ROBOT_OBSERVED_MUSIC_PLAYING ||
          strcmp(previous_track, track) != 0)
        {
          printf("[ROBOT-MUSIC] official status PLAYING track_changed=%d\n",
                 strcmp(previous_track, track) == 0 ? 0 : 1);
        }
      (void)robot_proactive_music_update(ROBOT_MUSIC_PLAYING, track,
                                         "", "", 0);
    }
  else if (report_stopped)
    {
      printf("[ROBOT-MUSIC] official status STOPPED\n");
      (void)robot_proactive_music_update(ROBOT_MUSIC_STOPPED, "", "", "", 0);
    }
}

static void *robot_music_observer_worker(void *arg)
{
  struct timespec startup_deadline;
  bool stop_requested;

  (void)arg;

  /*
   * voice_channel_init() runs before the official agent loop and its first
   * tool-buffer allocations.  Do not call the official media Tool from that
   * startup window: tool_media lazily initializes its private media context
   * on the first status query, so an early query can race Agent startup.
   * Keep the observer thread alive, but defer the first query until all
   * normal Agent startup work has settled.
   */
  clock_gettime(CLOCK_REALTIME, &startup_deadline);
  startup_deadline.tv_sec += ROBOT_MUSIC_START_DELAY_MS / 1000;
  startup_deadline.tv_nsec +=
    (ROBOT_MUSIC_START_DELAY_MS % 1000) * 1000000L;
  if (startup_deadline.tv_nsec >= 1000000000L)
    {
      startup_deadline.tv_sec++;
      startup_deadline.tv_nsec -= 1000000000L;
    }

  pthread_mutex_lock(&g_music_observer.lock);
  if (!g_music_observer.stop_requested)
    {
      (void)pthread_cond_timedwait(&g_music_observer.cond,
                                   &g_music_observer.lock,
                                   &startup_deadline);
    }
  stop_requested = g_music_observer.stop_requested;
  pthread_mutex_unlock(&g_music_observer.lock);

  printf("[ROBOT-MUSIC] observer started delay_ms=%u interval_ms=%u\n",
         ROBOT_MUSIC_START_DELAY_MS, ROBOT_MUSIC_POLL_MS);
  if (stop_requested)
    {
      goto stopped;
    }

  while (true)
    {
      struct timespec ts;

      robot_music_observer_sample();
      pthread_mutex_lock(&g_music_observer.lock);
      if (g_music_observer.stop_requested)
        {
          pthread_mutex_unlock(&g_music_observer.lock);
          break;
        }
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += ROBOT_MUSIC_POLL_MS / 1000;
      ts.tv_nsec += (ROBOT_MUSIC_POLL_MS % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L)
        {
          ts.tv_sec++;
          ts.tv_nsec -= 1000000000L;
        }
      pthread_cond_timedwait(&g_music_observer.cond,
                             &g_music_observer.lock, &ts);
      if (g_music_observer.stop_requested)
        {
          pthread_mutex_unlock(&g_music_observer.lock);
          break;
        }
      pthread_mutex_unlock(&g_music_observer.lock);
    }

stopped:
  pthread_mutex_lock(&g_music_observer.lock);
  g_music_observer.started = false;
  pthread_mutex_unlock(&g_music_observer.lock);
  printf("[ROBOT-MUSIC] observer stopped\n");
  return NULL;
}

int robot_music_observer_start(void)
{
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_music_observer.lock);
  if (g_music_observer.started)
    {
      pthread_mutex_unlock(&g_music_observer.lock);
      return 0;
    }
  g_music_observer.stop_requested = false;
  g_music_observer.last_state = ROBOT_OBSERVED_MUSIC_UNKNOWN;
  g_music_observer.last_track[0] = '\0';
  /* Publish ownership before create so a fast worker cannot race a second
   * start call while the creator is still returning. */
  g_music_observer.started = true;
  pthread_mutex_unlock(&g_music_observer.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&attr, 8192);
      if (ret == 0)
        {
          ret = pthread_create(&g_music_observer.thread, &attr,
                               robot_music_observer_worker, NULL);
        }
      pthread_attr_destroy(&attr);
    }
  if (ret != 0)
    {
      pthread_mutex_lock(&g_music_observer.lock);
      g_music_observer.started = false;
      pthread_mutex_unlock(&g_music_observer.lock);
      printf("[ROBOT-MUSIC] observer start failed rc=%d\n", ret);
      return -ret;
    }
  return 0;
}

int robot_music_observer_stop(void)
{
  pthread_t thread;
  bool started;

  pthread_mutex_lock(&g_music_observer.lock);
  started = g_music_observer.started;
  thread = g_music_observer.thread;
  g_music_observer.stop_requested = true;
  pthread_cond_broadcast(&g_music_observer.cond);
  pthread_mutex_unlock(&g_music_observer.lock);
  if (started && !pthread_equal(pthread_self(), thread))
    {
      return -pthread_join(thread, NULL);
    }
  return 0;
}
