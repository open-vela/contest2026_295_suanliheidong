#ifndef __APPS_ROBOT_VOICE_MIMO_TTS_H
#define __APPS_ROBOT_VOICE_MIMO_TTS_H

#include <stddef.h>
#include <stdint.h>

typedef int (*mimo_tts_pcm_cb_t)(const uint8_t *pcm, size_t len, void *arg);
int mimo_tts_speak_stream(const char *api_key, const char *text,
                          mimo_tts_pcm_cb_t cb, void *arg);

#endif
