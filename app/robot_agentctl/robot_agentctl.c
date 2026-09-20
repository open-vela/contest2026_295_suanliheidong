/*
 * Contest-local proactive runtime diagnostic command.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "robot_event.h"
#include "robot_proactive.h"
#include "robot_skill_installer.h"
#include "robot_world_state.h"

static uint64_t robot_agentctl_monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void robot_agentctl_print_status(void)
{
  struct robot_world_state_s state;
  uint64_t now_ms;
  uint64_t idle_ms = 0;

  if (robot_proactive_get_status(&state) < 0)
    {
      printf("robot_agentctl: state unavailable\n");
      return;
    }

  now_ms = robot_agentctl_monotonic_ms();
  if (state.user_has_interacted && now_ms >= state.last_user_activity_ms)
    {
      idle_ms = now_ms - state.last_user_activity_ms;
    }

  printf("proactive: initialized=%s enabled=%s\n",
         state.initialized ? "yes" : "no",
         state.proactive_enabled ? "yes" : "no");
  printf("user_interacted: %s\n",
         state.user_has_interacted ? "yes" : "no");
  printf("last_user_activity_ms: %llu\n",
         (unsigned long long)state.last_user_activity_ms);
  printf("last_proactive_ms: %llu\n",
         (unsigned long long)state.last_proactive_ms);
  printf("idle_event_fired: %s pending=%s idle_ms=%llu\n",
         state.idle_event_fired ? "yes" : "no",
         state.idle_event_pending ? "yes" : "no",
         (unsigned long long)idle_ms);
  printf("last_event_id: %u\n", state.last_event_id);
  printf("conversation_active: %s motion_active: %s tts_active: %s\n",
         state.conversation_active ? "yes" : "no",
         state.motion_active ? "yes" : "no",
         state.tts_active ? "yes" : "no");
  printf("music: %s track=%s category=%s mood=%s tempo=%u\n",
         state.music_playing ? "playing" : "stopped", state.music_track,
         state.music_category, state.music_mood,
         (unsigned int)state.music_tempo_bpm);
  printf("skill_path: %s\n", robot_skill_installer_path());
  printf("skill_exists: %s matches: %s install_result: %s\n",
         robot_skill_installer_exists() ? "yes" : "no",
         robot_skill_installer_matches() ? "yes" : "no",
         robot_skill_installer_result_name(
             robot_skill_installer_last_result()));
}

int main(int argc, char *argv[])
{
  int ret;

  ret = robot_proactive_init();
  if (ret < 0)
    {
      return 1;
    }
  ret = robot_skill_installer_ensure();
  if (ret < 0)
    {
      return 1;
    }
  ret = robot_proactive_start();
  if (ret < 0)
    {
      return 1;
    }

  if (argc >= 2 && strcmp(argv[1], "status") == 0)
    {
      robot_agentctl_print_status();
      return 0;
    }
  if (argc >= 2 && strcmp(argv[1], "on") == 0)
    {
      robot_proactive_set_enabled(true);
      printf("robot_agentctl: proactive on\n");
      return 0;
    }
  if (argc >= 2 && strcmp(argv[1], "off") == 0)
    {
      robot_proactive_set_enabled(false);
      printf("robot_agentctl: proactive off\n");
      return 0;
    }
  if (argc >= 3 && strcmp(argv[1], "trigger") == 0)
    {
      enum robot_event_type_e type;
      if (strcmp(argv[2], "test") == 0)
        {
          type = ROBOT_EVENT_TEST;
        }
      else if (strcmp(argv[2], "idle") == 0)
        {
          type = ROBOT_EVENT_IDLE_LONG;
        }
      else
        {
          printf("robot_agentctl: trigger expects test or idle\n");
          return 1;
        }
      ret = robot_proactive_post(type, "manual diagnostic trigger");
      return ret < 0 ? 1 : 0;
    }
  if (argc >= 3 && strcmp(argv[1], "music") == 0)
    {
      printf("robot_agentctl: music is a debug/test injection; it does not control the official player\n");
      const char *track = argc >= 4 ? argv[3] : "";
      const char *category = argc >= 5 ? argv[4] : "";
      const char *mood = argc >= 6 ? argv[5] : "";
      unsigned int tempo = argc >= 7 ? (unsigned int)strtoul(argv[6], NULL, 10) : 0;
      enum robot_music_state_e state;

      if (strcmp(argv[2], "start") == 0 || strcmp(argv[2], "change") == 0)
        {
          state = ROBOT_MUSIC_PLAYING;
        }
      else if (strcmp(argv[2], "stop") == 0)
        {
          state = ROBOT_MUSIC_STOPPED;
        }
      else
        {
          printf("robot_agentctl: music expects start, change, or stop\n");
          return 1;
        }
      ret = robot_proactive_music_update(state, track, category, mood,
                                         (uint16_t)tempo);
      printf("robot_agentctl: music %s rc=%d\n", argv[2], ret);
      return ret < 0 ? 1 : 0;
    }
  if (argc >= 2 && strcmp(argv[1], "activity") == 0)
    {
      robot_world_state_note_user_activity();
      printf("robot_agentctl: user activity recorded\n");
      return 0;
    }

  printf("Usage: robot_agentctl status | on | off | trigger test | "
         "trigger idle | activity | music start [track category mood tempo] | "
         "music change [track category mood tempo] | music stop\n");
  return 1;
}
