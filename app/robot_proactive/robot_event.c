/*
 * Contest-local robot event helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "robot_event.h"

const char *robot_event_type_name(enum robot_event_type_e type)
{
  switch (type)
    {
      case ROBOT_EVENT_TEST:
        return "manual_test";
      case ROBOT_EVENT_IDLE_LONG:
        return "idle_long";
      case ROBOT_EVENT_USER_ACTIVITY:
        return "user_activity";
      case ROBOT_EVENT_MUSIC_STARTED:
        return "music_started";
      case ROBOT_EVENT_MUSIC_CHANGED:
        return "music_changed";
      case ROBOT_EVENT_MUSIC_STOPPED:
        return "music_stopped";
      case ROBOT_EVENT_WEATHER_REFRESH:
        return "weather_refresh";
      case ROBOT_EVENT_UV_HIGH:
        return "uv_high";
      case ROBOT_EVENT_WEATHER_ALERT:
        return "weather_alert";
      case ROBOT_EVENT_FEISHU_MESSAGE:
        return "feishu_message";
      case ROBOT_EVENT_FEISHU_WORKLOAD_HIGH:
        return "feishu_workload_high";
      case ROBOT_EVENT_MEAL_WINDOW:
        return "meal_window";
      default:
        return "unknown";
    }
}
