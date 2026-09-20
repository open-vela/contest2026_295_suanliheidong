#ifndef __ROBOT_AUDIO_PLAYBACK_H
#define __ROBOT_AUDIO_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

void robot_audio_playback_set_stop_on_short_underrun(bool enable);
int robot_audio_playback_open(void);
int robot_audio_playback_write(const uint8_t *pcm, size_t len);
int robot_audio_playback_finish(void);
void robot_audio_playback_close(void);

#ifdef __cplusplus
}
#endif

#endif /* __ROBOT_AUDIO_PLAYBACK_H */
