/*
 * Contest-local robot world state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "robot_world_state.h"

static pthread_mutex_t g_world_lock = PTHREAD_MUTEX_INITIALIZER;
static struct robot_world_state_s g_world;

static uint64_t robot_world_monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)ts.tv_nsec / 1000000ULL;
}

int robot_world_state_init(void)
{
  pthread_mutex_lock(&g_world_lock);
  if (!g_world.initialized)
    {
      memset(&g_world, 0, sizeof(g_world));
      g_world.initialized = true;
      g_world.proactive_enabled = true;
      g_world.boot_ms = robot_world_monotonic_ms();
      g_world.last_user_activity_ms = g_world.boot_ms;
      printf("[ROBOT-WORLD] initialized boot_ms=%llu enabled=1\n",
             (unsigned long long)g_world.boot_ms);
    }
  pthread_mutex_unlock(&g_world_lock);
  return 0;
}

void robot_world_state_set_enabled(bool enabled)
{
  pthread_mutex_lock(&g_world_lock);
  g_world.proactive_enabled = enabled;
  pthread_mutex_unlock(&g_world_lock);
  printf("[ROBOT-WORLD] proactive_enabled=%d\n", enabled ? 1 : 0);
}

void robot_world_state_note_user_activity(void)
{
  uint64_t now_ms = robot_world_monotonic_ms();

  pthread_mutex_lock(&g_world_lock);
  if (!g_world.initialized)
    {
      g_world.initialized = true;
      g_world.proactive_enabled = true;
      g_world.boot_ms = now_ms;
    }
  g_world.last_user_activity_ms = now_ms;
  g_world.user_has_interacted = true;
  g_world.idle_event_fired = false;
  g_world.idle_event_pending = false;
  pthread_mutex_unlock(&g_world_lock);
  printf("[ROBOT-WORLD] user activity at=%llu\n",
         (unsigned long long)now_ms);
}

static void robot_world_state_ensure_locked(uint64_t now_ms)
{
  if (!g_world.initialized)
    {
      memset(&g_world, 0, sizeof(g_world));
      g_world.initialized = true;
      g_world.proactive_enabled = true;
      g_world.boot_ms = now_ms;
      g_world.last_user_activity_ms = now_ms;
    }
}

void robot_world_state_set_conversation_active(bool active)
{
  pthread_mutex_lock(&g_world_lock);
  robot_world_state_ensure_locked(robot_world_monotonic_ms());
  g_world.conversation_active = active;
  pthread_mutex_unlock(&g_world_lock);
}

void robot_world_state_set_motion_active(bool active)
{
  pthread_mutex_lock(&g_world_lock);
  robot_world_state_ensure_locked(robot_world_monotonic_ms());
  g_world.motion_active = active;
  pthread_mutex_unlock(&g_world_lock);
}

void robot_world_state_set_tts_active(bool active)
{
  pthread_mutex_lock(&g_world_lock);
  robot_world_state_ensure_locked(robot_world_monotonic_ms());
  g_world.tts_active = active;
  pthread_mutex_unlock(&g_world_lock);
}

void robot_world_state_set_power_risk(bool risk)
{
  pthread_mutex_lock(&g_world_lock);
  robot_world_state_ensure_locked(robot_world_monotonic_ms());
  g_world.power_risk = risk;
  pthread_mutex_unlock(&g_world_lock);
}

int robot_world_state_update_music_session(enum robot_music_state_e state,
  uint32_t session_id, const char *track, const char *category,
  const char *mood, uint16_t tempo_bpm, bool *changed)
{
  uint64_t now_ms = robot_world_monotonic_ms();
  bool local_changed;
  if (changed != NULL) *changed = false;
  if (state < ROBOT_MUSIC_UNKNOWN || state > ROBOT_MUSIC_STOPPED)
    return -EINVAL;
  pthread_mutex_lock(&g_world_lock);
  robot_world_state_ensure_locked(now_ms);
  if (session_id != 0 &&
      ((state == ROBOT_MUSIC_PLAYING &&
        (session_id < g_world.music_session_id ||
         (session_id == g_world.music_session_id && !g_world.music_playing))) ||
       (state != ROBOT_MUSIC_PLAYING && session_id != g_world.music_session_id)))
    {
      pthread_mutex_unlock(&g_world_lock);
      return -ESTALE;
    }
  local_changed = (session_id != 0 && session_id != g_world.music_session_id) ||
    g_world.music_playing != (state == ROBOT_MUSIC_PLAYING) ||
    g_world.music_tempo_bpm != tempo_bpm ||
    strncmp(g_world.music_track, track != NULL ? track : "", sizeof(g_world.music_track)) != 0 ||
    strncmp(g_world.music_category, category != NULL ? category : "", sizeof(g_world.music_category)) != 0 ||
    strncmp(g_world.music_mood, mood != NULL ? mood : "", sizeof(g_world.music_mood)) != 0;
  if (local_changed)
    {
      if (session_id != 0) g_world.music_session_id = session_id;
      g_world.music_playing = state == ROBOT_MUSIC_PLAYING;
      g_world.music_event_id = 0;
      g_world.music_tempo_bpm = tempo_bpm;
      snprintf(g_world.music_track, sizeof(g_world.music_track), "%s", track != NULL ? track : "");
      snprintf(g_world.music_category, sizeof(g_world.music_category), "%s", category != NULL ? category : "");
      snprintf(g_world.music_mood, sizeof(g_world.music_mood), "%s", mood != NULL ? mood : "");
      g_world.last_music_update_ms = now_ms;
    }
  pthread_mutex_unlock(&g_world_lock);
  if (changed != NULL) *changed = local_changed;
  return 0;
}

int robot_world_state_update_music(enum robot_music_state_e state,
  const char *track, const char *category, const char *mood,
  uint16_t tempo_bpm, bool *changed)
{
  return robot_world_state_update_music_session(state, 0, track, category,
                                                 mood, tempo_bpm, changed);
}

void robot_world_state_arm_music_event(uint32_t session_id, uint32_t event_id)
{
  pthread_mutex_lock(&g_world_lock);
  if (g_world.music_playing && g_world.music_session_id == session_id)
    g_world.music_event_id = event_id;
  pthread_mutex_unlock(&g_world_lock);
}

int robot_world_state_claim_music_reaction(uint32_t session_id, uint32_t event_id)
{
  int ret = 0;
  pthread_mutex_lock(&g_world_lock);
  if (!g_world.proactive_enabled || session_id == 0 || event_id == 0 ||
      !g_world.music_playing || g_world.music_session_id != session_id ||
      g_world.music_event_id != event_id) ret = -ESTALE;
  else if (g_world.music_react_session_id == session_id) ret = -EALREADY;
  else g_world.music_react_session_id = session_id;
  pthread_mutex_unlock(&g_world_lock);
  return ret;
}

static int robot_world_state_claim_tail(uint32_t session_id, uint32_t event_id,
                                         bool repeat)
{
  uint64_t now = robot_world_monotonic_ms();
  int ret = 0;
  pthread_mutex_lock(&g_world_lock);
  if (session_id == 0 || event_id == 0 || !g_world.music_playing ||
      g_world.music_session_id != session_id || g_world.music_event_id != event_id)
    ret = -ESTALE;
  else if (!repeat && g_world.music_tail_session_id == session_id)
    ret = -EALREADY;
  else if (!g_world.proactive_enabled || g_world.power_risk)
    ret = -EPERM;
  else if (g_world.motion_active || g_world.tts_active || g_world.conversation_active)
    ret = -EBUSY;
  else if (g_world.last_music_tail_ms != 0 &&
           now - g_world.last_music_tail_ms < ROBOT_MUSIC_TAIL_COOLDOWN_MS)
    ret = -EAGAIN;
  else
    {
      g_world.music_tail_session_id = session_id;
      g_world.last_music_tail_ms = now;
    }
  pthread_mutex_unlock(&g_world_lock);
  return ret;
}

int robot_world_state_claim_music_tail(uint32_t session_id, uint32_t event_id)
{ return robot_world_state_claim_tail(session_id, event_id, false); }

int robot_world_state_claim_music_tail_repeat(uint32_t session_id, uint32_t event_id)
{ return robot_world_state_claim_tail(session_id, event_id, true); }

void robot_world_state_note_proactive(uint32_t event_id,
                                      enum robot_event_type_e type)
{
  pthread_mutex_lock(&g_world_lock);
  g_world.last_proactive_ms = robot_world_monotonic_ms();
  g_world.last_event_id = event_id;
  if (type >= 0 && type < ROBOT_EVENT_TYPE_COUNT)
    {
      g_world.last_event_type_ms[type] = g_world.last_proactive_ms;
    }
  if (event_id > g_world.last_processed_event_id)
    g_world.last_processed_event_id = event_id;
  if (type >= ROBOT_EVENT_MUSIC_STARTED && type <= ROBOT_EVENT_MUSIC_STOPPED)
    g_world.last_music_event_id = event_id;
  if (type == ROBOT_EVENT_IDLE_LONG)
    {
      g_world.idle_event_fired = true;
    }
  g_world.idle_event_pending = false;
  pthread_mutex_unlock(&g_world_lock);
}

bool robot_world_state_try_queue_idle(uint64_t now_ms, uint64_t idle_ms,
                                      uint64_t cooldown_ms)
{
  bool accept = false;

  pthread_mutex_lock(&g_world_lock);
  if (g_world.initialized && g_world.proactive_enabled &&
      g_world.user_has_interacted && !g_world.idle_event_fired &&
      !g_world.idle_event_pending &&
      now_ms - g_world.last_user_activity_ms >= idle_ms &&
      (g_world.last_proactive_ms == 0 ||
       now_ms - g_world.last_proactive_ms >= cooldown_ms))
    {
      g_world.idle_event_pending = true;
      accept = true;
    }
  pthread_mutex_unlock(&g_world_lock);
  return accept;
}

void robot_world_state_cancel_idle_queue(void)
{
  pthread_mutex_lock(&g_world_lock);
  g_world.idle_event_pending = false;
  pthread_mutex_unlock(&g_world_lock);
}

int robot_world_state_snapshot(struct robot_world_state_s *out)
{
  if (out == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_world_lock);
  *out = g_world;
  pthread_mutex_unlock(&g_world_lock);
  return 0;
}
