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
void robot_audio_playback_set_media_voice_mode(bool enable);
int robot_audio_playback_open(void);
/* Music URL playback uses a larger startup window to absorb HTTP jitter. */
int robot_audio_playback_open_music(void);
int robot_audio_playback_open_media(void);
/* Native NuttX Audio bridge: stage PCM before START, then return APBs from
 * real DMA progress. The startup window fits six 4096-byte Media periods. */
int robot_audio_playback_open_native_media(void);
int robot_audio_playback_start_native_media(void);
int robot_audio_playback_pause_native_media(bool paused);
int robot_audio_playback_end_input(void);
int robot_audio_playback_progress(uint64_t *completed, uint64_t *pending);
/* False while streaming music is refilling; never restarts the I2S owner. */
bool robot_audio_playback_music_audible(void);
/* Validated reply cache; continuity mode defers hardware allocation until
 * finish(). Hardware allocation errors can therefore be returned by finish().
 * Music retains its streaming prebuffer. */
int robot_audio_playback_open_voice(void);
/* Progressive voice retains a final real block for exact EOS. Network gaps
 * are filled with silence rather than replaying cyclic DMA payloads. */
int robot_audio_playback_open_voice_live(void);
void robot_audio_playback_set_turn_timing(unsigned long long commit_ms,
    unsigned long long user_end_ms, unsigned long long tts_start_ms);
void robot_audio_playback_set_sentence_timing(unsigned long run_id,
    unsigned int sequence, unsigned long long ready_ms);
int robot_audio_playback_write(const uint8_t *pcm, size_t len);
/* Current PCM waiting in the live playback ring; zero when closed. */
size_t robot_audio_playback_buffered_bytes(void);
int robot_audio_playback_finish(void);
void robot_audio_playback_close(void);
/* Freeze DMA and wake blocked producers, without freeing their live ring.
 * The current owner must join its producer(s), then call close(). */
uint64_t robot_audio_playback_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* __ROBOT_AUDIO_PLAYBACK_H */
