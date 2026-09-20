#ifndef __APPS_ROBOT_VOICE_MIMO_ASR_H
#define __APPS_ROBOT_VOICE_MIMO_ASR_H

#include <stddef.h>
#include <stdint.h>

int mimo_asr_recognize(const char *api_key, const uint8_t *pcm, size_t pcm_len,
                       char *text, size_t text_cap);

#endif
