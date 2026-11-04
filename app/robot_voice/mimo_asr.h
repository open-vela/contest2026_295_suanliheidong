#ifndef __APPS_ROBOT_VOICE_MIMO_ASR_H
#define __APPS_ROBOT_VOICE_MIMO_ASR_H

#include <stddef.h>
#include <stdint.h>

/* Borrowed cumulative transcript. Only wake detection may use partial text;
 * commands require the successfully validated final result. Negative cancels. */
typedef int (*mimo_asr_partial_cb_t)(const char *text, void *arg);
int mimo_asr_recognize_progress(const char *, const uint8_t *, size_t,
                               char *, size_t, mimo_asr_partial_cb_t, void *);
int mimo_asr_recognize(const char *api_key, const uint8_t *pcm, size_t pcm_len,
                       char *text, size_t text_cap);

#endif
