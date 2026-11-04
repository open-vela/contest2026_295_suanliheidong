#ifndef __APPS_ROBOT_VOICE_ROBOT_VOICE_CAPTURE_H
#define __APPS_ROBOT_VOICE_ROBOT_VOICE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/compiler.h>

/* Called on the caller's capture thread, never from the I2S/HPWORK callback.
 * PCM is gained mono PCM16; its pointer is borrowed until this call returns.
 * The sink must not wait for ASR, HTTP, Agent, or playback. */
typedef int (*robot_voice_capture_sink_t)(const uint8_t *pcm, size_t len,
                                         void *arg);

int robot_voice_capture_stream(robot_voice_capture_sink_t sink, void *arg,
                                FAR volatile bool *stop_requested);

int robot_voice_capture_record(uint8_t *buf, size_t cap, size_t *out_len,
                               unsigned int duration_ms);

int robot_voice_capture_record_interruptible(
  uint8_t *buf, size_t cap, size_t *out_len, unsigned int duration_ms,
  FAR volatile bool *stop_requested);

#endif
