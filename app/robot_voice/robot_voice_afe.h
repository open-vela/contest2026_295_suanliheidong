#ifndef ROBOT_VOICE_AFE_H
#define ROBOT_VOICE_AFE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "robot_voice_config.h"

#if ROBOT_VOICE_CAPTURE_RATE != 16000 || ROBOT_VOICE_CHANNELS != 1 || ROBOT_VOICE_BITS != 16
#  error AFE requires 16000 Hz mono PCM16
#endif

#define ROBOT_AFE_SAMPLES (ROBOT_VOICE_CAPTURE_RATE * ROBOT_VOICE_AFE_FRAME_MS / 1000)
#define ROBOT_AFE_BYTES (ROBOT_AFE_SAMPLES * sizeof(int16_t))

enum robot_voice_afe_mode_e
{
  ROBOT_AFE_CLEAN = 0,          /* VAD/KWS use integer HP; ASR remains raw. */
  ROBOT_AFE_RAW = 1,            /* Full audio bypass; VAD confirmation stays. */
  ROBOT_AFE_KWS_RAW = 2        /* KWS/ASR raw; VAD uses integer HP. */
};

struct robot_voice_afe_stats_s
{
  uint64_t input_samples;
  uint64_t output_samples;
  uint64_t frames;
  uint64_t speech_frames;
  uint64_t clipped_samples;
  uint64_t discarded_samples;  /* Explicit stream discontinuity only. */
  uint64_t overruns;
  uint32_t max_process_us;
  unsigned int raw_level, clean_level, noise_level, threshold;
  unsigned int partial_samples;
};

struct robot_voice_afe_frame_s
{
  /* Borrowed, aligned PCM16; both contain exactly ROBOT_AFE_SAMPLES.
   * Clean is an integer HP branch with no overlap delay. */
  const int16_t *raw;
  const int16_t *clean;
  const int16_t *audio;        /* ASR branch: always raw, every sample. */
  const int16_t *kws;
  unsigned int level, threshold;
  bool voiced;                /* Actual GMM evidence + relative energy. */
};

struct robot_voice_afe_s;
typedef int (*robot_voice_afe_sink_t)(const struct robot_voice_afe_frame_s *, void *);

struct robot_voice_afe_s *robot_voice_afe_create(enum robot_voice_afe_mode_e mode);
void robot_voice_afe_destroy(struct robot_voice_afe_s *afe);
/* Suppression clears only VAD state on a transition. HP state remains continuous. */
void robot_voice_afe_suppress(struct robot_voice_afe_s *afe, bool suppressed);
int robot_voice_afe_push(struct robot_voice_afe_s *afe, const uint8_t *pcm,
                         size_t len, robot_voice_afe_sink_t sink, void *arg);
void robot_voice_afe_stats(const struct robot_voice_afe_s *afe,
                           struct robot_voice_afe_stats_s *out);

/* Finite once/test_asr input: validate only; preserve every byte.
 * test_capture remains a raw diagnostic recording. */
int robot_voice_afe_recording(uint8_t *pcm, size_t len,
                             enum robot_voice_afe_mode_e mode);

/* Rolling votes count independent classifier decisions, never hangover. */
static inline unsigned int robot_voice_onset_vote(unsigned int *history,
                                                  bool speech)
{
  unsigned int bits, count = 0;
  *history = ((*history << 1) | (speech ? 1u : 0u)) &
             ((1u << ROBOT_VOICE_VAD_WINDOW_FRAMES) - 1u);
  for (bits = *history; bits; bits >>= 1) count += bits & 1u;
  return count;
}
#endif
