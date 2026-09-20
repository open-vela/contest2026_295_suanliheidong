#ifndef __APPS_ROBOT_VOICE_ROBOT_VOICE_CAPTURE_H
#define __APPS_ROBOT_VOICE_ROBOT_VOICE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/compiler.h>

int robot_voice_capture_record(uint8_t *buf, size_t cap, size_t *out_len,
                               unsigned int duration_ms);

int robot_voice_capture_record_interruptible(
  uint8_t *buf, size_t cap, size_t *out_len, unsigned int duration_ms,
  FAR volatile bool *stop_requested);

#endif
