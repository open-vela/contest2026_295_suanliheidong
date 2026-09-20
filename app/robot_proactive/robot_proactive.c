/*
 * Contest-local proactive agent runtime.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "agent_config.h"
#include "core/message_bus.h"
#include "robot_proactive.h"
#include "robot_world_state.h"

struct robot_proactive_runtime_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct robot_event_s queue[ROBOT_PROACTIVE_QUEUE_DEPTH];
  unsigned int head;
  unsigned int tail;
  unsigned int count;
  uint32_t next_event_id;
  bool initialized;
  bool started;
  bool stop_requested;
  pthread_t thread;
  bool pending_music_valid;
  struct robot_event_s pending_music;
};

static struct robot_proactive_runtime_s g_proactive =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER,
};

static uint64_t robot_proactive_monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void robot_proactive_realtime_after_ms(struct timespec *ts,
                                              uint32_t timeout_ms)
{
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += (time_t)(timeout_ms / 1000U);
  ts->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
    }
}

static void robot_proactive_drop(const struct robot_event_s *event,
                                 const char *reason)
{
  printf("[ROBOT-PROACTIVE] drop id=%u type=%s reason=%s\n",
         event->id, robot_event_type_name(event->type), reason);
}

static bool robot_proactive_is_music_event(enum robot_event_type_e type)
{
  return type >= ROBOT_EVENT_MUSIC_STARTED &&
         type <= ROBOT_EVENT_MUSIC_STOPPED;
}

static void robot_proactive_defer_music(const struct robot_event_s *event,
                                        const char *reason)
{
  pthread_mutex_lock(&g_proactive.lock);
  if (!g_proactive.pending_music_valid ||
      event->id >= g_proactive.pending_music.id)
    {
      g_proactive.pending_music = *event;
      g_proactive.pending_music_valid = true;
      printf("[ROBOT-PROACTIVE] music deferred id=%u reason=%s\n",
             event->id, reason);
    }
  pthread_cond_signal(&g_proactive.cond);
  pthread_mutex_unlock(&g_proactive.lock);
}

static int robot_proactive_send_to_agent(const struct robot_event_s *event,
                                         const struct robot_world_state_s *state)
{
  char content[768];
  agent_msg_t msg;
  int len;
  int ret;
  uint64_t now_ms = robot_proactive_monotonic_ms();
  uint64_t idle_seconds = 0;

  if (event->type == ROBOT_EVENT_IDLE_LONG &&
      now_ms >= state->last_user_activity_ms)
    {
      idle_seconds = (now_ms - state->last_user_activity_ms) / 1000ULL;
    }

  len = snprintf(content, sizeof(content),
                 "[ROBOT_INTERNAL_EVENT]\n"
                 "event_id=%u\n"
                 "created_ms=%llu\n"
                 "type=%s\n"
                 "source=%s\n"
                 "idle_seconds=%llu\n"
                 "detail=%s\n"
                 "music_playing=%d\n"
                 "music_track=%s\n"
                 "music_category=%s\n"
                 "music_mood=%s\n"
                 "music_tempo_bpm=%u\n\n"
                 "This is a structured internal robot event, not user speech.\n"
                 "Use the matching proactive Skill when available. Decide whether\n"
                 "a brief natural interaction is useful; keep spoken output short.",
                 event->id, (unsigned long long)event->created_ms,
                 robot_event_type_name(event->type),
                 event->source,
                 (unsigned long long)idle_seconds, event->detail,
                 state->music_playing ? 1 : 0, state->music_track,
                 state->music_category, state->music_mood,
                 (unsigned int)state->music_tempo_bpm);
  if (len < 0 || (size_t)len >= sizeof(content))
    {
      return -EOVERFLOW;
    }

  memset(&msg, 0, sizeof(msg));
  strncpy(msg.channel, AGENT_CHAN_VOICE, sizeof(msg.channel) - 1);
  strncpy(msg.chat_id, "robot_proactive", sizeof(msg.chat_id) - 1);
  msg.content = strdup(content);
  if (msg.content == NULL)
    {
      return -ENOMEM;
    }

  ret = message_bus_push_inbound(&msg);
  if (ret != OK)
    {
      free(msg.content);
      return -EAGAIN;
    }

  printf("[ROBOT-PROACTIVE] inbound id=%u channel=%s chat_id=%s\n",
         event->id, msg.channel, msg.chat_id);
  return 0;
}

static void robot_proactive_process(const struct robot_event_s *event)
{
  struct robot_world_state_s state;
  uint64_t now_ms;
  int ret;

  if (robot_world_state_snapshot(&state) < 0 || !state.initialized)
    {
      robot_proactive_drop(event, "world-unavailable");
      return;
    }

  if (!state.proactive_enabled)
    {
      robot_proactive_drop(event, "disabled");
      return;
    }

  now_ms = robot_proactive_monotonic_ms();
  if (event->id <= state.last_processed_event_id)
    {
      robot_proactive_drop(event, "duplicate-event-id");
      return;
    }

  if (state.last_proactive_ms != 0 &&
      now_ms - state.last_proactive_ms < ROBOT_PROACTIVE_COOLDOWN_MS)
    {
      if (robot_proactive_is_music_event(event->type))
        {
          robot_proactive_defer_music(event, "cooldown");
        }
      else
        {
          robot_proactive_drop(event, "cooldown");
        }
      return;
    }

  if (event->type >= 0 && event->type < ROBOT_EVENT_TYPE_COUNT &&
      state.last_event_type_ms[event->type] != 0 &&
      now_ms - state.last_event_type_ms[event->type] <
        (event->type >= ROBOT_EVENT_MUSIC_STARTED &&
         event->type <= ROBOT_EVENT_MUSIC_STOPPED ?
         ROBOT_PROACTIVE_MUSIC_COOLDOWN_MS : ROBOT_PROACTIVE_COOLDOWN_MS))
    {
      if (robot_proactive_is_music_event(event->type))
        {
          robot_proactive_defer_music(event, "event-cooldown");
        }
      else
        {
          robot_proactive_drop(event, "event-cooldown");
        }
      return;
    }

  if (event->type != ROBOT_EVENT_MANUAL_TEST &&
      event->created_ms != 0 && now_ms >= event->created_ms &&
      now_ms - event->created_ms > ROBOT_PROACTIVE_EVENT_FRESHNESS_MS)
    {
      robot_proactive_drop(event, "stale");
      return;
    }

  if (event->type != ROBOT_EVENT_MANUAL_TEST &&
      (state.conversation_active || state.motion_active || state.tts_active))
    {
      if (robot_proactive_is_music_event(event->type))
        {
          robot_proactive_defer_music(event, "robot-busy");
        }
      else
        {
          robot_proactive_drop(event, "robot-busy");
        }
      return;
    }

  if (event->type == ROBOT_EVENT_MANUAL_TEST)
    {
      /* Manual tests intentionally bypass scenario-specific predicates. */
    }
  else if (event->type == ROBOT_EVENT_IDLE_LONG)
    {
      if (!state.user_has_interacted)
        {
          robot_proactive_drop(event, "no-user-activity");
          return;
        }
      if (state.idle_event_fired)
        {
          robot_proactive_drop(event, "already-fired");
          return;
        }
      if (now_ms - state.last_user_activity_ms < ROBOT_PROACTIVE_IDLE_MS &&
          strcmp(event->detail, "manual diagnostic trigger") != 0)
        {
          robot_proactive_drop(event, "not-idle");
          return;
        }
    }
  else if (event->type == ROBOT_EVENT_USER_ACTIVITY)
    {
      robot_proactive_drop(event, "state-only-event");
      return;
    }
  else if (event->type < ROBOT_EVENT_MUSIC_STARTED ||
           event->type > ROBOT_EVENT_MEAL_WINDOW)
    {
      robot_proactive_drop(event, "unknown-type");
      return;
    }

  printf("[ROBOT-PROACTIVE] accept id=%u type=%s\n",
         event->id, robot_event_type_name(event->type));
  ret = robot_proactive_send_to_agent(event, &state);
  if (ret < 0)
    {
      printf("[ROBOT-PROACTIVE] inbound failed id=%u rc=%d\n",
             event->id, ret);
    }
  else
    {
      robot_world_state_note_proactive(event->id, event->type);
    }
}

static void robot_proactive_try_pending_music(void)
{
  struct robot_event_s event;
  struct robot_world_state_s state;
  uint64_t now_ms;
  bool stale;
  bool eligible;

  pthread_mutex_lock(&g_proactive.lock);
  if (g_proactive.pending_music_valid)
    {
      event = g_proactive.pending_music;
    }
  else
    {
      pthread_mutex_unlock(&g_proactive.lock);
      return;
    }
  pthread_mutex_unlock(&g_proactive.lock);

  if (robot_world_state_snapshot(&state) < 0 || !state.initialized ||
      !state.proactive_enabled || state.conversation_active ||
      state.motion_active || state.tts_active)
    {
      return;
    }

  now_ms = robot_proactive_monotonic_ms();
  stale = event.created_ms != 0 && now_ms >= event.created_ms &&
          now_ms - event.created_ms > ROBOT_PROACTIVE_EVENT_FRESHNESS_MS;
  eligible = !stale &&
             (state.last_proactive_ms == 0 ||
              now_ms - state.last_proactive_ms >= ROBOT_PROACTIVE_COOLDOWN_MS) &&
             (state.last_event_type_ms[event.type] == 0 ||
              now_ms - state.last_event_type_ms[event.type] >=
                ROBOT_PROACTIVE_MUSIC_COOLDOWN_MS);
  if (!eligible)
    {
      if (stale)
        {
          robot_proactive_drop(&event, "pending-stale");
          pthread_mutex_lock(&g_proactive.lock);
          if (g_proactive.pending_music_valid &&
              g_proactive.pending_music.id == event.id)
            {
              g_proactive.pending_music_valid = false;
            }
          pthread_mutex_unlock(&g_proactive.lock);
        }
      return;
    }

  pthread_mutex_lock(&g_proactive.lock);
  if (g_proactive.pending_music_valid &&
      g_proactive.pending_music.id == event.id)
    {
      g_proactive.pending_music_valid = false;
      eligible = true;
    }
  else
    {
      eligible = false;
    }
  pthread_mutex_unlock(&g_proactive.lock);

  if (eligible)
    {
      printf("[ROBOT-PROACTIVE] dispatch deferred music id=%u\n", event.id);
      robot_proactive_process(&event);
    }
}

static void *robot_proactive_worker(void *arg)
{
  (void)arg;

  printf("[ROBOT-PROACTIVE] worker started stack=%u queue=%u\n",
         ROBOT_PROACTIVE_STACK_SIZE, ROBOT_PROACTIVE_QUEUE_DEPTH);
  while (true)
    {
      struct robot_event_s event;
      bool have_event = false;
      struct timespec deadline;
      struct robot_world_state_s state;

      pthread_mutex_lock(&g_proactive.lock);
      if (g_proactive.stop_requested)
        {
          pthread_mutex_unlock(&g_proactive.lock);
          break;
        }

      if (g_proactive.count > 0)
        {
          event = g_proactive.queue[g_proactive.head];
          g_proactive.head = (g_proactive.head + 1) %
                             ROBOT_PROACTIVE_QUEUE_DEPTH;
          g_proactive.count--;
          have_event = true;
        }
      else
        {
          robot_proactive_realtime_after_ms(&deadline, 1000);
          pthread_cond_timedwait(&g_proactive.cond, &g_proactive.lock,
                                 &deadline);
        }
      pthread_mutex_unlock(&g_proactive.lock);

      if (have_event)
        {
          robot_proactive_process(&event);
          continue;
        }

      robot_proactive_try_pending_music();

      if (robot_world_state_snapshot(&state) == 0 && state.initialized &&
          state.proactive_enabled && state.user_has_interacted &&
          robot_world_state_try_queue_idle(robot_proactive_monotonic_ms(),
                                           ROBOT_PROACTIVE_IDLE_MS,
                                           ROBOT_PROACTIVE_COOLDOWN_MS))
        {
          if (robot_proactive_post(ROBOT_EVENT_IDLE_LONG,
                                   "user inactive for the configured idle interval") < 0)
            {
              robot_world_state_cancel_idle_queue();
            }
        }
    }

  pthread_mutex_lock(&g_proactive.lock);
  g_proactive.started = false;
  pthread_mutex_unlock(&g_proactive.lock);
  printf("[ROBOT-PROACTIVE] worker stopped\n");
  return NULL;
}

int robot_proactive_init(void)
{
  pthread_mutex_lock(&g_proactive.lock);
  if (!g_proactive.initialized)
    {
      g_proactive.initialized = true;
      g_proactive.next_event_id = 1;
      g_proactive.stop_requested = false;
    }
  pthread_mutex_unlock(&g_proactive.lock);
  return robot_world_state_init();
}

int robot_proactive_start(void)
{
  int ret;
  pthread_attr_t attr;

  robot_proactive_init();
  pthread_mutex_lock(&g_proactive.lock);
  if (g_proactive.started)
    {
      pthread_mutex_unlock(&g_proactive.lock);
      return 0;
    }
  g_proactive.stop_requested = false;
  /* Publish ownership before create so a fast second caller cannot create
   * another worker while the first pthread_create is still in progress. */
  g_proactive.started = true;
  pthread_mutex_unlock(&g_proactive.lock);

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return -ret;
    }
  ret = pthread_attr_setstacksize(&attr, ROBOT_PROACTIVE_STACK_SIZE);
  if (ret == 0)
    {
      ret = pthread_create(&g_proactive.thread, &attr,
                           robot_proactive_worker, NULL);
    }
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_proactive.lock);
      g_proactive.started = false;
      pthread_mutex_unlock(&g_proactive.lock);
      printf("[ROBOT-PROACTIVE] start failed rc=%d\n", ret);
      return -ret;
    }
  return 0;
}

int robot_proactive_stop(void)
{
  pthread_t thread;
  bool started;

  pthread_mutex_lock(&g_proactive.lock);
  started = g_proactive.started;
  thread = g_proactive.thread;
  g_proactive.stop_requested = true;
  pthread_cond_broadcast(&g_proactive.cond);
  pthread_mutex_unlock(&g_proactive.lock);
  if (started)
    {
      return -pthread_join(thread, NULL);
    }
  return 0;
}

int robot_proactive_post_ex(enum robot_event_type_e type, const char *source,
                            const char *detail,
                            const struct robot_event_payload_s *payload)
{
  struct robot_event_s *event;
  uint32_t event_id;

  if (type < 0 || type >= ROBOT_EVENT_TYPE_COUNT)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_proactive.lock);
  if (!g_proactive.initialized || g_proactive.count >= ROBOT_PROACTIVE_QUEUE_DEPTH)
    {
      pthread_mutex_unlock(&g_proactive.lock);
      printf("[ROBOT-EVENT] drop type=%s reason=queue-full-or-uninitialized\n",
             robot_event_type_name(type));
      return -EAGAIN;
    }

  event = &g_proactive.queue[g_proactive.tail];
  memset(event, 0, sizeof(*event));
  event->id = g_proactive.next_event_id++;
  event->type = type;
  event->created_ms = robot_proactive_monotonic_ms();
  event->priority = type == ROBOT_EVENT_IDLE_LONG ? 1 : 0;
  strncpy(event->source, source != NULL ? source : "manual",
          sizeof(event->source) - 1);
  event->source[sizeof(event->source) - 1] = '\0';
  if (detail != NULL)
    {
      strncpy(event->detail, detail, sizeof(event->detail) - 1);
    }
  if (payload != NULL)
    {
      event->payload = *payload;
    }
  g_proactive.tail = (g_proactive.tail + 1) % ROBOT_PROACTIVE_QUEUE_DEPTH;
  g_proactive.count++;
  event_id = event->id;
  pthread_cond_signal(&g_proactive.cond);
  pthread_mutex_unlock(&g_proactive.lock);

  printf("[ROBOT-EVENT] post id=%u type=%s\n",
         event_id, robot_event_type_name(type));
  return 0;
}

int robot_proactive_post(enum robot_event_type_e type, const char *detail)
{
  return robot_proactive_post_ex(type, "manual", detail, NULL);
}

int robot_proactive_music_update(enum robot_music_state_e music_state,
                                 const char *track, const char *category,
                                 const char *mood, uint16_t tempo_bpm)
{
  struct robot_world_state_s before;
  struct robot_event_payload_s payload;
  enum robot_event_type_e type;
  bool changed = false;
  int ret;

  if (robot_world_state_snapshot(&before) < 0)
    {
      return -EAGAIN;
    }
  ret = robot_world_state_update_music(music_state, track, category, mood,
                                       tempo_bpm, &changed);
  if (ret < 0 || !changed)
    {
      return ret;
    }

  if (music_state == ROBOT_MUSIC_PLAYING)
    {
      type = before.music_playing ? ROBOT_EVENT_MUSIC_CHANGED :
                                    ROBOT_EVENT_MUSIC_STARTED;
    }
  else
    {
      type = ROBOT_EVENT_MUSIC_STOPPED;
    }

  memset(&payload, 0, sizeof(payload));
  payload.kind = ROBOT_EVENT_PAYLOAD_MUSIC;
  payload.data.music.state = music_state;
  payload.data.music.tempo_bpm = tempo_bpm;
  strncpy(payload.data.music.track, track != NULL ? track : "",
          sizeof(payload.data.music.track) - 1);
  strncpy(payload.data.music.category, category != NULL ? category : "",
          sizeof(payload.data.music.category) - 1);
  strncpy(payload.data.music.mood, mood != NULL ? mood : "",
          sizeof(payload.data.music.mood) - 1);
  return robot_proactive_post_ex(type, "music", "music state changed",
                                 &payload);
}

int robot_proactive_set_enabled(bool enabled)
{
  robot_world_state_set_enabled(enabled);
  pthread_mutex_lock(&g_proactive.lock);
  pthread_cond_signal(&g_proactive.cond);
  pthread_mutex_unlock(&g_proactive.lock);
  return 0;
}

int robot_proactive_get_status(struct robot_world_state_s *out)
{
  return robot_world_state_snapshot(out);
}
