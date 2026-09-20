#ifndef __APPS_ROBOT_VOICE_MUSIC_PLAYER_H
#define __APPS_ROBOT_VOICE_MUSIC_PLAYER_H

#include <stdbool.h>

/*
 * Contest-local lightweight music playback.
 *
 * Current supported input:
 *   RIFF/WAVE, PCM16LE, 24000 Hz, mono
 *
 * Playback reuses robot_audio_playback, so it follows the exact same
 * PCM16 -> 32-bit I2S/DMA path already used by TTS.
 */

int robot_music_play_file(const char *path);
bool robot_music_is_playing(void);
int robot_music_register_tool(void);

#endif
