/* Contest-local Agent music adapter. Playback remains in the existing backend.
 * Companion events follow local DMA progress, never music_status polling. */
#include <nuttx/config.h>

#include "robot_media_player.h"
#include "robot_audio_playback.h"
#include "robot_music_media.h"
#include "robot_proactive.h"
#include "robot_world_state.h"
#include "robot_expression.h"
#include "robot_motion.h"
#include <time.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

extern int robot_oled_set_loading(bool loading) __attribute__((weak));

#define MEDIA_EVENT_STOPPED 4
#define MEDIA_EVENT_COMPLETED 6
#define MUSIC_CONFIRM_STACK 8192
#define MUSIC_TAIL_INTERVAL_MS 1000ULL

extern unsigned long robot_voice_music_run_id(void) __attribute__((weak));
extern void robot_voice_music_started(void) __attribute__((weak));

struct rv_music_handle
{
  void *player;
  struct rv_music_handle *next;
  uintptr_t cookie_id;
  bool starting;
  bool started;
  bool stopping;
  bool closing;
  bool terminal_seen;
  bool loading;
  bool announced;
  bool stop_announced;
  unsigned int users;
  bool companion_enabled;
  bool companion_calm;
  bool companion_tail;
  uint32_t companion_event_id;
  uint32_t confirm_worker_session; /* reuses the former unused face index */
  uint64_t next_face_ms;
  uint64_t next_tail_ms;
  uint64_t next_tail_log_ms;
  uint32_t session_id;
  unsigned long run;
  char track[64];
  char title[64];
  char artist[64];
  void *event_cookie;
  robot_music_event_cb event_cb;
};

struct rv_music_worker_arg
{
  struct rv_music_handle *handle;
  uint32_t session_id;
};

static pthread_mutex_t g_music_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_music_changed = PTHREAD_COND_INITIALIZER;
static struct rv_music_handle *g_active_music;
static struct rv_music_handle *g_music_handles;
static uintptr_t g_next_cookie;
static uint32_t g_next_session;

static void robot_music_loading(bool loading)
{
  if (robot_oled_set_loading != NULL)
    {
      (void)robot_oled_set_loading(loading);
    }
}

void robot_music_media_link_anchor(void) {}

/* Called with g_music_lock: a late stop for an older player cannot stop the
 * current session, and COMPLETED + STOPPED + close collapse into one event. */
static void robot_music_media_end_locked(struct rv_music_handle *handle)
{
  if (handle->loading)
    {
      handle->loading = false;
      robot_music_loading(false);
    }
  handle->companion_enabled = false;
  handle->started = false;
  handle->terminal_seen = true;
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_MUSIC);
  if (g_active_music == handle) g_active_music = NULL;
  if (handle->announced && !handle->stop_announced)
    {
      handle->stop_announced = true;
      (void)robot_proactive_music_session_update(ROBOT_MUSIC_STOPPED,
        handle->session_id, handle->track, "", "", 0);
    }
}

static void robot_music_media_event_bridge(void *cookie, int event,
                                           int result, const char *extra)
{
  struct rv_music_handle *handle;
  robot_music_event_cb callback;
  void *callback_cookie;

  /* Numeric cookies reject superseded sessions and callbacks racing close.
   * The callback reference pins the wrapper while upper state is updated;
   * values are never recycled during normal use. */
  pthread_mutex_lock(&g_music_lock);
  for (handle = g_music_handles; handle != NULL; handle = handle->next)
    if (handle->cookie_id == (uintptr_t)cookie) break;
  if (handle == NULL || handle->closing)
    {
      pthread_mutex_unlock(&g_music_lock);
      return;
    }
  if (event == MEDIA_EVENT_COMPLETED || event == MEDIA_EVENT_STOPPED)
    robot_music_media_end_locked(handle);
  callback = handle->event_cb;
  callback_cookie = handle->event_cookie;
  if (callback != NULL) handle->users++;
  pthread_mutex_unlock(&g_music_lock);

  if (callback != NULL)
    {
      callback(callback_cookie, event, result, extra);
      pthread_mutex_lock(&g_music_lock);
      handle->users--;
      pthread_cond_broadcast(&g_music_changed);
      pthread_mutex_unlock(&g_music_lock);
    }
}

static uint64_t robot_music_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* The existing session worker drives the local policy. No second player,
 * status observer, periodic Agent message or network request is created. */
static void robot_music_companion_tick(struct rv_music_handle *handle, uint32_t worker_session)
{
  struct robot_world_state_s state;
  uint64_t now = robot_music_now_ms();
  uint32_t session = 0;
  uint32_t event = 0;
  bool tail_due = false;
  bool audible;
  if (robot_world_state_snapshot(&state) < 0) return;
  audible = robot_audio_playback_music_audible();
  pthread_mutex_lock(&g_music_lock);
  if (handle->session_id != worker_session ||
      handle->closing || handle->stopping || !handle->started ||
      g_active_music != handle || !handle->companion_enabled ||
      !state.proactive_enabled || state.power_risk ||
      state.conversation_active || state.tts_active || !audible)
    { pthread_mutex_unlock(&g_music_lock); return; }
  if (handle->companion_tail && !handle->companion_calm &&
      now >= handle->next_tail_ms)
    {
      session = handle->session_id;
      event = handle->companion_event_id;
      handle->next_tail_ms = now + MUSIC_TAIL_INTERVAL_MS;
      tail_due = true;
    }
  pthread_mutex_unlock(&g_music_lock);
  if (tail_due)
    {
      int ret = robot_motion_music_tail_repeat_async(session, event);
      if (ret <= 0)
        {
          pthread_mutex_lock(&g_music_lock);
          if (g_active_music == handle && now >= handle->next_tail_log_ms)
            {
              handle->next_tail_log_ms = now + 10000ULL;
              printf("[ROBOT-MUSIC] periodic-tail waiting session=%lu rc=%d (buffer/power/busy gate)\n",
                (unsigned long)session, ret);
            }
          pthread_mutex_unlock(&g_music_lock);
        }
      if (ret > 0)
        {
          pthread_mutex_lock(&g_music_lock);
          if (g_active_music == handle && handle->companion_enabled)
            handle->next_tail_ms = now + MUSIC_TAIL_INTERVAL_MS;
          pthread_mutex_unlock(&g_music_lock);
          printf("[ROBOT-MUSIC] periodic-tail session=%lu request=%d next_ms=%llu\n",
            (unsigned long)session, ret, MUSIC_TAIL_INTERVAL_MS);
        }
    }
}

static void *robot_music_confirm_worker(void *arg)
{
  struct rv_music_worker_arg *worker = arg;
  struct rv_music_handle *handle = worker->handle;
  uint32_t session = worker->session_id;
  free(worker);

  for (;;)
    {
      uint64_t completed = 0;
      uint64_t pending = 0;
      int ret;
      bool newly_confirmed = false;
      pthread_mutex_lock(&g_music_lock);
      bool current = handle->session_id == session && !handle->closing &&
                     !handle->stopping && handle->started && g_active_music == handle;
      pthread_mutex_unlock(&g_music_lock);
      if (!current) break;
      if (!robot_legacy_media_is_running(handle->player))
        { usleep(100 * 1000); continue; }
      ret = robot_audio_playback_progress(&completed, &pending);
      if (ret == 0 && completed != 0)
        {
          pthread_mutex_lock(&g_music_lock);
          if (handle->session_id == session &&
              !handle->closing && !handle->stopping && handle->started &&
              g_active_music == handle && !handle->announced &&
              robot_legacy_media_is_running(handle->player))
            {
              ret = robot_proactive_music_session_update(ROBOT_MUSIC_PLAYING,
                handle->session_id, handle->track, "", "", 0);
              handle->announced = true;
              /* Authorization comes from the Agent's local Skill dispatch,
               * never from a pre-enabled event_id=0 shortcut. */
              handle->companion_enabled = false;
              handle->companion_calm = false;
              handle->companion_tail = false;
              handle->companion_event_id = 0;
              handle->loading = false;
              handle->next_tail_ms = 0;
              newly_confirmed = true;
              syslog(ret == 0 ? LOG_INFO : LOG_WARNING,
                "[ROBOT-MUSIC] DMA confirmed session=%lu completed=%llu pending=%llu companion_rc=%d\n",
                (unsigned long)handle->session_id,
                (unsigned long long)completed, (unsigned long long)pending,
                ret);
            }
          pthread_mutex_unlock(&g_music_lock);
          if (newly_confirmed)
            {
              robot_music_loading(false);
            }
          if (newly_confirmed)
            {
              /* SINGING owns a frame animation for the confirmed song. */
              (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_MUSIC,
                                         ROBOT_EXPRESSION_SINGING, 0);
            }
        }
      robot_music_companion_tick(handle, session);
      usleep(100 * 1000);
    }
  pthread_mutex_lock(&g_music_lock);
  if (handle->confirm_worker_session == session) handle->confirm_worker_session = 0;
  handle->users--;
  pthread_cond_broadcast(&g_music_changed);
  pthread_mutex_unlock(&g_music_lock);
  return NULL;
}

/* Defer the existing policy worker until actual audio start. A staged song
 * must not consume this stack while the foreground reply is still speaking. */
static int robot_music_start_confirm_locked(struct rv_music_handle *handle)
{
  pthread_t thread;
  pthread_attr_t attr;
  int ret;
  if (!robot_legacy_media_is_running(handle->player) ||
      handle->confirm_worker_session == handle->session_id) return 0;
  struct rv_music_worker_arg *worker = malloc(sizeof(*worker));
  if (!worker) return -ENOMEM;
  worker->handle = handle;
  worker->session_id = handle->session_id;
  handle->users++;
  handle->confirm_worker_session = handle->session_id;
  ret = pthread_attr_init(&attr);
  if (!ret) {
    ret = pthread_attr_setstacksize(&attr, MUSIC_CONFIRM_STACK);
    if (!ret) ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!ret) ret = pthread_create(&thread, &attr, robot_music_confirm_worker, worker);
    pthread_attr_destroy(&attr);
  }
  if (ret) {
    free(worker);
    handle->users--;
    handle->confirm_worker_session = 0;
    syslog(LOG_WARNING, "[ROBOT-MUSIC] companion worker unavailable rc=%d\n", ret);
    return -ret;
  }
  return 0;
}

void *robot_music_media_open(const char *stream)
{
  struct rv_music_handle *handle = calloc(1, sizeof(*handle));
  int ret;
  if (handle == NULL) { errno = ENOMEM; return NULL; }
  handle->player = robot_legacy_media_open(stream);
  if (handle->player == NULL) { free(handle); return NULL; }
  pthread_mutex_lock(&g_music_lock);
  handle->cookie_id = ++g_next_cookie;
  if (handle->cookie_id == 0) handle->cookie_id = ++g_next_cookie;
  handle->next = g_music_handles;
  g_music_handles = handle;
  pthread_mutex_unlock(&g_music_lock);
  ret = robot_legacy_media_set_event_callback(handle->player,
    (void *)handle->cookie_id, robot_music_media_event_bridge);
  if (ret < 0)
    {
      (void)robot_music_media_close(handle, 0);
      errno = -ret;
      return NULL;
    }
  syslog(LOG_INFO, "[RV-MUSIC] backend=contest-legacy-http-helix companion=event-dma\n");
  return handle;
}

int robot_music_media_close(void *opaque, int pending_stop)
{
  struct rv_music_handle *handle = opaque;
  struct rv_music_handle **cursor;
  int ret;
  if (handle == NULL) return -EINVAL;
  pthread_mutex_lock(&g_music_lock);
  handle->closing = true;
  robot_music_media_end_locked(handle);
  pthread_mutex_unlock(&g_music_lock);
  (void)robot_legacy_media_set_event_callback(handle->player, NULL, NULL);
  pthread_mutex_lock(&g_music_lock);
  while (handle->users != 0)
    pthread_cond_wait(&g_music_changed, &g_music_lock);
  pthread_mutex_unlock(&g_music_lock);
  ret = robot_legacy_media_close(handle->player, pending_stop);
  pthread_mutex_lock(&g_music_lock);
  for (cursor = &g_music_handles; *cursor != NULL; cursor = &(*cursor)->next)
    if (*cursor == handle) { *cursor = handle->next; break; }
  pthread_mutex_unlock(&g_music_lock);
  free(handle);
  return ret;
}

int robot_music_media_set_track_info(void *opaque, const char *title,
                                     const char *artist)
{
  struct rv_music_handle *handle = opaque;

  if (handle == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_music_lock);
  snprintf(handle->title, sizeof(handle->title), "%s", title != NULL ? title : "");
  snprintf(handle->artist, sizeof(handle->artist), "%s", artist != NULL ? artist : "");
  pthread_mutex_unlock(&g_music_lock);
  return 0;
}

int robot_music_media_prepare(void *opaque, const char *url, const char *options)
{
  struct rv_music_handle *handle = opaque;
  int ret;
  if (handle == NULL) return -EINVAL;

  /* Loading is visual. Do not launch cloud speech while preparing music. */
  robot_music_loading(true);
  pthread_mutex_lock(&g_music_lock);
  handle->loading = true;
  pthread_mutex_unlock(&g_music_lock);

  ret = robot_legacy_media_prepare(handle->player, url, options);
  if (ret == 0)
    {
      pthread_mutex_lock(&g_music_lock);
      snprintf(handle->track, sizeof(handle->track), "%s", url != NULL ? url : "local PCM");
      for (char *p = handle->track; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
      pthread_mutex_unlock(&g_music_lock);
    }
  else
    {
      pthread_mutex_lock(&g_music_lock);
      handle->loading = false;
      pthread_mutex_unlock(&g_music_lock);
      robot_music_loading(false);
    }
  return ret;
}

int robot_music_media_set_event_callback(void *opaque, void *cookie,
                                         robot_music_event_cb callback)
{
  struct rv_music_handle *handle = opaque;
  if (handle == NULL) return -EINVAL;
  pthread_mutex_lock(&g_music_lock);
  if (handle->closing) { pthread_mutex_unlock(&g_music_lock); return -ECANCELED; }
  handle->event_cookie = cookie;
  handle->event_cb = callback;
  pthread_mutex_unlock(&g_music_lock);
  return 0;
}

int robot_music_media_start(void *opaque)
{
  struct rv_music_handle *handle = opaque;
  int ret;
  unsigned long run;
  if (handle == NULL) return -EINVAL;
  (void)robot_proactive_start();
  /* The voice run getter takes g_monitor.lock. Voice callbacks may already
   * hold that lock when querying music, so never call it under g_music_lock. */
  run = robot_voice_music_run_id != NULL ? robot_voice_music_run_id() : 0;
  pthread_mutex_lock(&g_music_lock);
  if (!handle->closing && handle->started && !handle->starting &&
      robot_legacy_media_is_paused(handle->player))
    {
      handle->users++;
      pthread_mutex_unlock(&g_music_lock);
      ret = robot_legacy_media_start(handle->player);
      pthread_mutex_lock(&g_music_lock);
      if (ret == 0) (void)robot_music_start_confirm_locked(handle);
      handle->users--;
      pthread_cond_broadcast(&g_music_changed);
      pthread_mutex_unlock(&g_music_lock);
      if (ret == 0 && robot_voice_music_started) robot_voice_music_started();
      return ret;
    }
  if (handle->closing || handle->started || handle->starting)
    { pthread_mutex_unlock(&g_music_lock); return -EBUSY; }
  handle->starting = true;
  handle->terminal_seen = false;
  handle->stopping = false;
  handle->announced = false;
  handle->stop_announced = false;
  handle->companion_enabled = false;
  handle->session_id = ++g_next_session;
  if (handle->session_id == 0) handle->session_id = ++g_next_session;
  handle->run = run;
  handle->cookie_id = ++g_next_cookie;
  if (handle->cookie_id == 0) handle->cookie_id = ++g_next_cookie;
  uintptr_t cookie_id = handle->cookie_id;
  handle->users++;
  pthread_mutex_unlock(&g_music_lock);
  (void)robot_legacy_media_set_event_callback(handle->player,
    (void *)cookie_id, robot_music_media_event_bridge);
  ret = robot_legacy_media_start(handle->player);
  if (ret < 0)
    {
      robot_music_loading(false);
    }
  pthread_mutex_lock(&g_music_lock);
  handle->starting = false;
  if (ret < 0)
    {
      handle->loading = false;
    }
  if (ret == 0 && !handle->closing && !handle->terminal_seen)
    {
      handle->started = true;
      g_active_music = handle;
      (void)robot_music_start_confirm_locked(handle);
    }
  handle->users--;
  pthread_cond_broadcast(&g_music_changed);
  pthread_mutex_unlock(&g_music_lock);
  if (ret == 0 && robot_voice_music_started) robot_voice_music_started();
  return ret;
}

int robot_music_media_stop(void *opaque)
{
  struct rv_music_handle *handle = opaque;
  int ret;
  if (handle == NULL) return -EINVAL;
  pthread_mutex_lock(&g_music_lock);
  handle->stopping = true;
  handle->users++;
  pthread_mutex_unlock(&g_music_lock);
  ret = robot_legacy_media_stop(handle->player);
  pthread_mutex_lock(&g_music_lock);
  if (ret == 0) robot_music_media_end_locked(handle);
  handle->users--;
  pthread_cond_broadcast(&g_music_changed);
  pthread_mutex_unlock(&g_music_lock);
  return ret;
}

int robot_music_media_pause(void *opaque)
{
  struct rv_music_handle *handle = opaque;
  return handle != NULL ? robot_legacy_media_pause(handle->player) : -EINVAL;
}
int robot_music_media_seek(void *opaque, unsigned int position)
{
  struct rv_music_handle *handle = opaque;
  return handle != NULL ? robot_legacy_media_seek(handle->player, position) : -EINVAL;
}
int robot_music_media_get_position(void *opaque, unsigned int *position)
{
  struct rv_music_handle *handle = opaque;
  return handle != NULL ? robot_legacy_media_get_position(handle->player, position) : -EINVAL;
}
int robot_music_media_get_duration(void *opaque, unsigned int *duration)
{
  struct rv_music_handle *handle = opaque;
  return handle != NULL ? robot_legacy_media_get_duration(handle->player, duration) : -EINVAL;
}
ssize_t robot_music_media_write_data(void *opaque, const void *data, size_t len)
{
  struct rv_music_handle *handle = opaque;
  return handle != NULL ? robot_legacy_media_write_data(handle->player, data, len) : -EINVAL;
}
int robot_music_media_set_volume(const char *stream, int volume)
{ (void)stream; (void)volume; return 0; }

bool robot_music_media_owns_reply(unsigned long run)
{
  bool owns;
  pthread_mutex_lock(&g_music_lock);
  owns = run != 0 && g_active_music != NULL && g_active_music->started && g_active_music->run == run;
  pthread_mutex_unlock(&g_music_lock);
  return owns;
}
bool robot_music_media_start_pending(void *opaque)
{
  struct rv_music_handle *handle = opaque;
  return handle != NULL && robot_legacy_media_start_pending(handle->player);
}

bool robot_music_media_is_active(void)
{
  bool active;
  pthread_mutex_lock(&g_music_lock);
  active = g_active_music != NULL && g_active_music->started && !g_active_music->closing &&
           robot_legacy_media_is_running(g_active_music->player);
  pthread_mutex_unlock(&g_music_lock);
  return active;
}

bool robot_music_media_session_ready(uint32_t session_id)
{
  uint64_t completed = 0;
  uint64_t pending = 0;
  bool ready;
  if (session_id == 0) return false;
  pthread_mutex_lock(&g_music_lock);
  ready = g_active_music != NULL && g_active_music->started &&
          !g_active_music->closing && !g_active_music->stopping && g_active_music->announced &&
          g_active_music->session_id == session_id;
  pthread_mutex_unlock(&g_music_lock);
  if (!ready || robot_audio_playback_progress(&completed, &pending) < 0 ||
      completed == 0 || pending < ROBOT_MUSIC_MOTION_READY_BYTES ||
      !robot_audio_playback_music_audible()) return false;
  pthread_mutex_lock(&g_music_lock);
  ready = g_active_music != NULL && g_active_music->started &&
          !g_active_music->closing && !g_active_music->stopping &&
          g_active_music->session_id == session_id;
  pthread_mutex_unlock(&g_music_lock);
  return ready;
}

bool robot_music_media_motion_allowed(uint32_t session_id)
{
  struct robot_world_state_s state;
  bool active;

  if (session_id == 0 || robot_world_state_snapshot(&state) < 0)
    {
      return false;
    }

  pthread_mutex_lock(&g_music_lock);
  active = g_active_music != NULL && g_active_music->started &&
           !g_active_music->closing && !g_active_music->stopping &&
           g_active_music->announced && g_active_music->companion_enabled &&
           g_active_music->session_id == session_id;
  pthread_mutex_unlock(&g_music_lock);

  return active && robot_music_media_session_ready(session_id) && state.proactive_enabled && !state.power_risk &&
         !state.tts_active && !state.conversation_active &&
         robot_audio_playback_music_audible();
}

int robot_music_media_react(uint32_t session_id, uint32_t event_id, bool calm)
{
  int ret = -ESTALE;
  pthread_mutex_lock(&g_music_lock);
  if (g_active_music != NULL && g_active_music->started &&
      !g_active_music->closing && !g_active_music->stopping && g_active_music->announced &&
      g_active_music->session_id == session_id)
    {
      ret = robot_world_state_claim_music_reaction(session_id, event_id);
      if (ret == 0)
        ret = robot_expression_set(ROBOT_EXPRESSION_SOURCE_AGENT,
          calm ? ROBOT_EXPRESSION_LISTENING : ROBOT_EXPRESSION_HAPPY, 8000);
    }
  pthread_mutex_unlock(&g_music_lock);
  return ret;
}

int robot_music_media_claim_tail(uint32_t session_id, uint32_t event_id)
{
  if (!robot_music_media_motion_allowed(session_id)) return -EAGAIN;
  return robot_world_state_claim_music_tail(session_id, event_id);
}

int robot_music_media_companion_start(uint32_t session_id, uint32_t event_id,
                                      bool calm, bool tail_enabled)
{
  struct robot_world_state_s state;
  int ret = -ESTALE;
  pthread_mutex_lock(&g_music_lock);
  if (session_id != 0 && event_id != 0 && g_active_music != NULL &&
      g_active_music->started && !g_active_music->closing &&
      !g_active_music->stopping && g_active_music->announced &&
      g_active_music->session_id == session_id &&
      robot_world_state_snapshot(&state) == 0 && state.music_playing &&
      state.proactive_enabled && state.music_session_id == session_id &&
      state.music_event_id == event_id)
    {
      struct rv_music_handle *handle = g_active_music;
      if (handle->companion_enabled && handle->companion_event_id == event_id)
        ret = -EALREADY;
      else
        {
          handle->companion_calm = calm;
          handle->companion_tail = tail_enabled && !calm;
          handle->companion_event_id = event_id;
          handle->next_face_ms = 0;
          handle->next_tail_ms = 0;
          handle->next_tail_log_ms = 0;
          handle->companion_enabled = true;
          ret = 0;
          printf("[ROBOT-MUSIC] companion armed session=%lu event=%lu tail=%d tail_step_ms=%llu\n",
            (unsigned long)session_id, (unsigned long)event_id,
            handle->companion_tail, MUSIC_TAIL_INTERVAL_MS);
        }
    }
  pthread_mutex_unlock(&g_music_lock);
  return ret;
}

int robot_music_media_claim_periodic_tail(uint32_t session_id, uint32_t event_id)
{
  bool authorized;
  pthread_mutex_lock(&g_music_lock);
  authorized = g_active_music != NULL && g_active_music->companion_enabled &&
               g_active_music->companion_tail &&
               g_active_music->session_id == session_id &&
               g_active_music->companion_event_id == event_id;
  pthread_mutex_unlock(&g_music_lock);
  if (!authorized) return -ESTALE;
  if (!robot_music_media_motion_allowed(session_id)) return -EAGAIN;
  if (event_id == 0) return -ESTALE;
  return robot_world_state_claim_music_tail_repeat(session_id, event_id);
}

static int robot_music_media_stop_borrowed(unsigned long run)
{
  struct rv_music_handle *handle;
  int ret;
  pthread_mutex_lock(&g_music_lock);
  handle = g_active_music;
  if (handle == NULL || !handle->started)
    { pthread_mutex_unlock(&g_music_lock); return 0; }
  if (run != 0 && handle->run > run)
    { pthread_mutex_unlock(&g_music_lock); return -ECANCELED; }
  handle->users++;
  pthread_mutex_unlock(&g_music_lock);
  ret = robot_music_media_stop(handle);
  pthread_mutex_lock(&g_music_lock);
  handle->users--;
  pthread_cond_broadcast(&g_music_changed);
  pthread_mutex_unlock(&g_music_lock);
  return ret;
}
int robot_music_media_stop_for_voice(unsigned long run)
{ return robot_music_media_stop_borrowed(run); }
int robot_media_stop_active(void)
{ return robot_music_media_stop_borrowed(0); }

/* These are task-level operations. The WebClient callback never waits for
 * TTS: suspend joins the HTTP/decode owner and closes I2S before returning. */
bool robot_music_media_has_task(void)
{
  bool active;
  pthread_mutex_lock(&g_music_lock);
  active = g_active_music && g_active_music->started && !g_active_music->closing;
  pthread_mutex_unlock(&g_music_lock);
  return active;
}

int robot_music_media_suspend(void)
{
  struct rv_music_handle *handle;
  int ret;
  pthread_mutex_lock(&g_music_lock);
  handle = g_active_music;
  if (!handle || !handle->started || handle->closing)
    { pthread_mutex_unlock(&g_music_lock); return 0; }
  handle->users++;
  pthread_mutex_unlock(&g_music_lock);
  ret = robot_legacy_media_suspend(handle->player);
  pthread_mutex_lock(&g_music_lock);
  handle->next_tail_log_ms = 0; /* Reuse this timer for failed-resume backoff. */
  handle->users--;
  pthread_cond_broadcast(&g_music_changed);
  pthread_mutex_unlock(&g_music_lock);
  if (ret == 0) {
    (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_MUSIC);
  }
  return ret;
}

int robot_music_media_resume(void)
{
  struct rv_music_handle *handle;
  uint64_t now = robot_music_now_ms();
  int ret;
  bool running;
  pthread_mutex_lock(&g_music_lock);
  handle = g_active_music;
  if (!handle || !handle->started || handle->closing || handle->stopping ||
      robot_legacy_media_is_running(handle->player))
    { pthread_mutex_unlock(&g_music_lock); return 0; }
  if (now < handle->next_tail_log_ms)
    { pthread_mutex_unlock(&g_music_lock); return -EAGAIN; }
  handle->users++;
  pthread_mutex_unlock(&g_music_lock);
  ret = robot_legacy_media_resume_active();
  running = robot_legacy_media_is_running(handle->player);
  pthread_mutex_lock(&g_music_lock);
  handle->next_tail_log_ms = ret < 0 ? now + 1000 : 0;
  if (running) (void)robot_music_start_confirm_locked(handle);
  if (running && handle->announced)
    (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_MUSIC,
                               ROBOT_EXPRESSION_SINGING, 0);
  handle->users--;
  pthread_cond_broadcast(&g_music_changed);
  pthread_mutex_unlock(&g_music_lock);
  if (running && robot_voice_music_started) robot_voice_music_started();
  return ret;
}
