/*
 * Contest-local robot event model.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONTEST_ROBOT_EVENT_H
#define __CONTEST_ROBOT_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

enum robot_event_type_e
{
  ROBOT_EVENT_MANUAL_TEST = 0,
  ROBOT_EVENT_IDLE_LONG,
  ROBOT_EVENT_USER_ACTIVITY,
  ROBOT_EVENT_MUSIC_STARTED,
  ROBOT_EVENT_MUSIC_CHANGED,
  ROBOT_EVENT_MUSIC_STOPPED,
  ROBOT_EVENT_WEATHER_REFRESH,
  ROBOT_EVENT_UV_HIGH,
  ROBOT_EVENT_WEATHER_ALERT,
  ROBOT_EVENT_FEISHU_MESSAGE,
  ROBOT_EVENT_FEISHU_WORKLOAD_HIGH,
  ROBOT_EVENT_MEAL_WINDOW,
  ROBOT_EVENT_TYPE_COUNT,
};

/* Keep the old diagnostic spelling source-compatible. */
#define ROBOT_EVENT_TEST ROBOT_EVENT_MANUAL_TEST

enum robot_music_state_e
{
  ROBOT_MUSIC_UNKNOWN = 0,
  ROBOT_MUSIC_PLAYING,
  ROBOT_MUSIC_STOPPED,
};

enum robot_event_payload_kind_e
{
  ROBOT_EVENT_PAYLOAD_NONE = 0,
  ROBOT_EVENT_PAYLOAD_MUSIC,
};

struct robot_event_payload_s
{
  enum robot_event_payload_kind_e kind;
  union
    {
      struct
        {
          enum robot_music_state_e state;
          uint16_t tempo_bpm;
          char track[64];
          char category[32];
          char mood[32];
        } music;
    } data;
};

struct robot_event_s
{
  uint32_t id;
  enum robot_event_type_e type;
  uint64_t created_ms;
  unsigned int priority;
  char source[24];
  char detail[128];
  struct robot_event_payload_s payload;
};

const char *robot_event_type_name(enum robot_event_type_e type);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_ROBOT_EVENT_H */
