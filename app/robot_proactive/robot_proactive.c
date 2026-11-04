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
#include "robot_music_media.h"
#include "robot_expression.h"
#include "contest_skill_resource.h"

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
  uint64_t music_stop_quiet_until_ms;
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

static const struct contest_skill_resource_s *robot_music_companion_skill(void)
{
  for (size_t i = 0; i < contest_skill_resource_count; i++)
    if (strcmp(contest_skill_resources[i].filename, "robot-music-companion.md") == 0)
      return &contest_skill_resources[i];
  return NULL;
}

static int robot_proactive_send_to_agent(const struct robot_event_s *event,
                                         const struct robot_world_state_s *state)
{
  bool music = robot_proactive_is_music_event(event->type);
  const struct contest_skill_resource_s *skill = music ? robot_music_companion_skill() : NULL;
  size_t capacity = music ? 512 : 2048;
  char *content;
  agent_msg_t msg;
  int len;
  int ret;
  uint64_t now_ms = robot_proactive_monotonic_ms();
  uint64_t idle_seconds = now_ms >= state->last_user_activity_ms ?
    (now_ms - state->last_user_activity_ms) / 1000ULL : 0;
  uint32_t session = event->payload.kind == ROBOT_EVENT_PAYLOAD_MUSIC ?
    event->payload.data.music.session_id : 0;

  if (music && skill == NULL) return -ENOENT;
  if (capacity > 8192) return -EOVERFLOW;
  content = malloc(capacity);
  if (content == NULL) return -ENOMEM;
  if (music) {
    /* Only stable routing tokens are needed by the local Skill. Keep the
     * compiled Markdown in ROM rather than copying it into the bus heap. */
    len = snprintf(content, capacity,
      "[ROBOT_INTERNAL_EVENT]\n"
      "event_id=%u\ncreated_ms=%llu\ntype=%s\nsource=%s\n"
      "session_id=%lu\n"
      "[CONTEST_SKILL name=robot-music-companion mode=local]\n",
      event->id, (unsigned long long)event->created_ms,
      robot_event_type_name(event->type), event->source, (unsigned long)session);
  } else {
    len = snprintf(content, capacity,
      "[ROBOT_INTERNAL_EVENT]\n"
      "event_id=%u\ncreated_ms=%llu\ntype=%s\nsource=%s\n"
      "session_id=%lu\nidle_seconds=%llu\ndetail=%s\n"
      "music_playing=%d\nmusic_track=%s\nmusic_category=%s\n"
      "music_mood=%s\nmusic_tempo_bpm=%u\n"
      "motion_active=%d\ntts_active=%d\npower_risk=%d\n\n"
      "Use the matching proactive Skill. This is structured robot context, not user speech.\n",
      event->id, (unsigned long long)event->created_ms,
      robot_event_type_name(event->type), event->source, (unsigned long)session,
      (unsigned long long)idle_seconds, event->detail,
      state->music_playing ? 1 : 0, state->music_track,
      state->music_category, state->music_mood, (unsigned int)state->music_tempo_bpm,
      state->motion_active, state->tts_active, state->power_risk);
  }
  if (len < 0 || (size_t)len >= capacity) { free(content); return -EOVERFLOW; }
  memset(&msg, 0, sizeof(msg));
  strncpy(msg.channel, music ? "system" : AGENT_CHAN_VOICE, sizeof(msg.channel) - 1);
  strncpy(msg.chat_id, music ? ROBOT_MUSIC_COMPANION_CHAT_ID : "robot_proactive",
          sizeof(msg.chat_id) - 1);
  msg.content = content;
  if (music)
    {
      msg.flags |= AGENT_MSG_FLAG_INTERNAL_MUSIC | AGENT_MSG_FLAG_NO_TTS;
      if (event->type != ROBOT_EVENT_MUSIC_STOPPED)
        robot_world_state_arm_music_event(session, event->id);
    }
  if (!music) msg.flags |= AGENT_MSG_FLAG_DEFER_MUSIC;
  ret = message_bus_push_inbound(&msg);
  if (ret != OK)
    {
      if (music) robot_world_state_arm_music_event(session, 0);
      free(content);
      return -EAGAIN;
    }

  printf("[ROBOT-PROACTIVE] inbound id=%u channel=%s chat_id=%s skill=%s session=%lu\n",
    event->id, msg.channel, msg.chat_id, music ? "robot-music-companion" : "matching",
    (unsigned long)session);
  return 0;
}

static int robot_music_event_number(const char *content, const char *key,
                                    uint32_t *value)
{
  const char *field = strstr(content, key);
  char *end;
  unsigned long n;
  if (!field) return -EINVAL;
  field += strlen(key);
  if (*field < '0' || *field > '9') return -EINVAL;
  errno = 0;
  n = strtoul(field, &end, 10);
  if (errno || !n || n > UINT32_MAX || *end != '\n') return -EINVAL;
  *value = (uint32_t)n;
  return 0;
}

int robot_proactive_apply_music_skill(const char *content)
{
  uint32_t session, event;
  int ret;
  const struct contest_skill_resource_s *skill = robot_music_companion_skill();
  if (!content || strncmp(content, "[ROBOT_INTERNAL_EVENT]\n",
                         sizeof("[ROBOT_INTERNAL_EVENT]\n") - 1) ||
      !strstr(content, "\n[CONTEST_SKILL name=robot-music-companion mode=local]\n") ||
      !skill || !skill->content || !skill->size) return -EINVAL;
  if (robot_music_event_number(content, "\nsession_id=", &session) < 0 ||
      robot_music_event_number(content, "\nevent_id=", &event) < 0) return -EINVAL;
  if (strstr(content, "\ntype=music_stopped\n")) return 0;
  if (!strstr(content, "\ntype=music_started\n") &&
      !strstr(content, "\ntype=music_changed\n")) return -EINVAL;
  /* Deterministic implementation of the registered companion policy. The
   * session/event token is checked again by the media owner. Only arm here;
   * the existing session worker performs small tail steps while audible. */
  ret = robot_music_media_companion_start(session, event, false, true);
  printf("[ROBOT-SKILL] apply name=robot-music-companion mode=local session=%lu event=%lu rc=%d\n",
         (unsigned long)session, (unsigned long)event, ret);
  return ret == -EALREADY ? 0 : ret;
}

static void robot_proactive_process(const struct robot_event_s *event)
{
  struct robot_world_state_s state;
  uint64_t now_ms;
  int ret;
  bool music = robot_proactive_is_music_event(event->type);

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
  if (event->type == ROBOT_EVENT_IDLE_LONG)
    {
      bool music_stop_quiet;
      pthread_mutex_lock(&g_proactive.lock);
      music_stop_quiet = now_ms < g_proactive.music_stop_quiet_until_ms;
      pthread_mutex_unlock(&g_proactive.lock);
      if (music_stop_quiet)
        {
          robot_world_state_cancel_idle_queue();
          robot_proactive_drop(event, "music-stop-cooldown");
          return;
        }
    }
  if (music && event->payload.kind == ROBOT_EVENT_PAYLOAD_MUSIC &&
      event->payload.data.music.session_id != 0 &&
      (event->payload.data.music.session_id != state.music_session_id ||
       ((event->type != ROBOT_EVENT_MUSIC_STOPPED) != state.music_playing)))
    {
      robot_proactive_drop(event, "superseded-music-session");
      return;
    }
  if (event->id <= (music ? state.last_music_event_id : state.last_processed_event_id))
    {
      robot_proactive_drop(event, "duplicate-event-id");
      return;
    }

  if (!music && state.last_proactive_ms != 0 &&
      now_ms - state.last_proactive_ms < ROBOT_PROACTIVE_COOLDOWN_MS)
    {
      robot_proactive_drop(event, "cooldown");
      return;
    }
  if (!music && event->type >= 0 && event->type < ROBOT_EVENT_TYPE_COUNT &&
      state.last_event_type_ms[event->type] != 0 &&
      now_ms - state.last_event_type_ms[event->type] < ROBOT_PROACTIVE_COOLDOWN_MS)
    {
      robot_proactive_drop(event, "event-cooldown");
      return;
    }

  if (event->type != ROBOT_EVENT_MANUAL_TEST &&
      event->created_ms != 0 && now_ms >= event->created_ms &&
      now_ms - event->created_ms > ROBOT_PROACTIVE_EVENT_FRESHNESS_MS)
    {
      robot_proactive_drop(event, "stale");
      return;
    }

  if (!music && event->type != ROBOT_EVENT_MANUAL_TEST &&
      event->type != ROBOT_EVENT_MUSIC_STOPPED &&
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
      if (music) robot_proactive_defer_music(event, "inbound-retry");
    }
  else
    {
      robot_world_state_note_proactive(event->id, event->type);
    }
}

static void robot_proactive_try_pending_music(void)
{
  struct robot_event_s event;
  bool have_event = false;
  pthread_mutex_lock(&g_proactive.lock);
  if (g_proactive.pending_music_valid)
    {
      event = g_proactive.pending_music;
      g_proactive.pending_music_valid = false;
      have_event = true;
    }
  pthread_mutex_unlock(&g_proactive.lock);
  if (have_event) robot_proactive_process(&event);
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

      uint64_t now_ms = robot_proactive_monotonic_ms();
      bool music_stop_quiet;
      pthread_mutex_lock(&g_proactive.lock);
      music_stop_quiet = now_ms < g_proactive.music_stop_quiet_until_ms;
      pthread_mutex_unlock(&g_proactive.lock);

      if (!music_stop_quiet && robot_world_state_snapshot(&state) == 0 && state.initialized &&
          state.proactive_enabled && state.user_has_interacted &&
          robot_world_state_try_queue_idle(now_ms,
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
      pthread_mutex_lock(&g_proactive.lock);
      g_proactive.started = false;
      pthread_mutex_unlock(&g_proactive.lock);
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
  bool pending = false;

  if (type < 0 || type >= ROBOT_EVENT_TYPE_COUNT)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_proactive.lock);
  if (!g_proactive.initialized ||
      (g_proactive.count >= ROBOT_PROACTIVE_QUEUE_DEPTH &&
       !robot_proactive_is_music_event(type)))
    {
      pthread_mutex_unlock(&g_proactive.lock);
      printf("[ROBOT-EVENT] drop type=%s reason=queue-full-or-uninitialized\n",
             robot_event_type_name(type));
      return -EAGAIN;
    }

  pending = g_proactive.count >= ROBOT_PROACTIVE_QUEUE_DEPTH;
  event = pending ? &g_proactive.pending_music : &g_proactive.queue[g_proactive.tail];
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
  if (pending) g_proactive.pending_music_valid = true;
  else
    {
      g_proactive.tail = (g_proactive.tail + 1) % ROBOT_PROACTIVE_QUEUE_DEPTH;
      g_proactive.count++;
    }
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

int robot_proactive_music_session_update(enum robot_music_state_e music_state,
  uint32_t session_id, const char *track, const char *category,
  const char *mood, uint16_t tempo_bpm)
{
  struct robot_world_state_s before;
  struct robot_event_payload_s payload;
  enum robot_event_type_e type;
  bool changed = false;
  int ret;
  if (robot_world_state_snapshot(&before) < 0) return -EAGAIN;
  ret = robot_world_state_update_music_session(music_state, session_id,
    track, category, mood, tempo_bpm, &changed);
  if (ret < 0 || !changed) return ret;
  type = music_state == ROBOT_MUSIC_PLAYING ?
    (before.music_playing ? ROBOT_EVENT_MUSIC_CHANGED : ROBOT_EVENT_MUSIC_STARTED) :
    ROBOT_EVENT_MUSIC_STOPPED;
  if (type == ROBOT_EVENT_MUSIC_STOPPED)
    {
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_AGENT);
      pthread_mutex_lock(&g_proactive.lock);
      g_proactive.music_stop_quiet_until_ms =
        robot_proactive_monotonic_ms() + 5000ULL;
      pthread_mutex_unlock(&g_proactive.lock);
    }
  memset(&payload, 0, sizeof(payload));
  payload.kind = ROBOT_EVENT_PAYLOAD_MUSIC;
  payload.data.music.state = music_state;
  payload.data.music.session_id = session_id;
  payload.data.music.tempo_bpm = tempo_bpm;
  snprintf(payload.data.music.track, sizeof(payload.data.music.track), "%s", track != NULL ? track : "");
  snprintf(payload.data.music.category, sizeof(payload.data.music.category), "%s", category != NULL ? category : "");
  snprintf(payload.data.music.mood, sizeof(payload.data.music.mood), "%s", mood != NULL ? mood : "");
  return robot_proactive_post_ex(type, "music-dma", "confirmed playback transition", &payload);
}

int robot_proactive_music_update(enum robot_music_state_e music_state,
  const char *track, const char *category, const char *mood, uint16_t tempo_bpm)
{
  return robot_proactive_music_session_update(music_state, 0, track, category, mood, tempo_bpm);
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
