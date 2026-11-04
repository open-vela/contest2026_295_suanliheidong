#ifndef ROBOT_VOICE_DIAG_H
#define ROBOT_VOICE_DIAG_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "robot_voice_config.h"
/* Default: user-visible states, timings and errors only. */
#define RV_EVENT(...) printf(__VA_ARGS__)
#define RV_VERBOSE(...) do { if (ROBOT_VOICE_LOG_VERBOSE) printf(__VA_ARGS__); } while (0)
#define RV_TEXT(...) do { if (ROBOT_VOICE_LOG_TEXT) printf(__VA_ARGS__); } while (0)
#define RV_DIALOGUE(...) do { if (ROBOT_VOICE_LOG_DIALOGUE) printf(__VA_ARGS__); } while (0)
/* -1 means the endpoint is unavailable, rather than a fabricated zero. */
static inline long long rv_elapsed_ms(unsigned long long from,
                                     unsigned long long to)
{
  return from && to >= from ? (long long)(to - from) : -1;
}
static inline uint32_t rv_pcm_hash(uint32_t hash, const uint8_t *p, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++) { hash ^= p[i]; hash *= UINT32_C(16777619); }
  return hash;
}
static inline unsigned long long rv_diag_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#define RV_PCM_HASH_INIT UINT32_C(2166136261)
#endif
