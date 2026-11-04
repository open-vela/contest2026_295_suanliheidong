/* Music-only jitter gate. It never stops or re-primes the cyclic DMA ring. */
#ifndef CONTEST_ROBOT_MUSIC_BUFFER_POLICY_H
#define CONTEST_ROBOT_MUSIC_BUFFER_POLICY_H
#include <stdbool.h>
#include <stddef.h>
/*
 * At 24 kHz mono PCM16, 24 KiB is about 500 ms and 64 KiB is about 1.3 s.
 * The old 8/192 KiB hysteresis turned a short HTTP gap into several seconds
 * of silence. Keep hysteresis, but recover at a short, bounded watermark.
 */
#define ROBOT_MUSIC_BUFFER_LOW_BYTES (8U * 1024U)
#define ROBOT_MUSIC_BUFFER_RECOVER_BYTES (64U * 1024U)
static inline bool robot_music_buffer_hold(bool holding, size_t available,
                                          bool eos, size_t capacity)
{
  size_t high = ROBOT_MUSIC_BUFFER_RECOVER_BYTES;
  if (eos) return false; /* Always drain the real EOF tail. */
  if (high > capacity) high = capacity;
  return holding ? available < high : available < ROBOT_MUSIC_BUFFER_LOW_BYTES;
}
#endif
