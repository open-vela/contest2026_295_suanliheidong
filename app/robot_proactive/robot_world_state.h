/*
 * Contest-local robot world state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_WORLD_STATE_H
#define __CONTEST_ROBOT_WORLD_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "robot_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct robot_world_state_s
{
  bool initialized;
  bool proactive_enabled;
  uint64_t boot_ms;
  uint64_t last_user_activity_ms;
  uint64_t last_proactive_ms;
  uint32_t last_event_id;
  bool user_has_interacted;
  bool conversation_active;
  bool motion_active;
  bool tts_active;
  int current_expression;
  bool idle_event_fired;
  bool idle_event_pending;
  bool music_playing;
  uint16_t music_tempo_bpm;
  char music_track[64];
  char music_category[32];
  char music_mood[32];
  uint64_t last_music_update_ms;
  int temperature_c;
  int uv_index;
  char weather_condition[32];
  uint64_t weather_updated_ms;
  uint64_t last_feishu_message_ms;
  uint32_t recent_feishu_message_count;
  uint32_t urgent_feishu_message_count;
  bool feishu_workload_signal;
  unsigned int local_hour;
  char meal_window[16];
  char fitness_goal[24];
  bool weight_management;
  bool low_gi_preference;
  uint64_t last_event_type_ms[ROBOT_EVENT_TYPE_COUNT];
  uint32_t last_processed_event_id;
};

int robot_world_state_init(void);
void robot_world_state_set_enabled(bool enabled);
void robot_world_state_note_user_activity(void);
void robot_world_state_set_conversation_active(bool active);
void robot_world_state_set_motion_active(bool active);
void robot_world_state_set_tts_active(bool active);
int robot_world_state_update_music(enum robot_music_state_e state,
                                   const char *track, const char *category,
                                   const char *mood, uint16_t tempo_bpm,
                                   bool *changed);
void robot_world_state_note_proactive(uint32_t event_id,
                                      enum robot_event_type_e type);
bool robot_world_state_try_queue_idle(uint64_t now_ms, uint64_t idle_ms,
                                      uint64_t cooldown_ms);
void robot_world_state_cancel_idle_queue(void);
int robot_world_state_snapshot(struct robot_world_state_s *out);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_WORLD_STATE_H */
