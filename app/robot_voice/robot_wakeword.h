#ifndef ROBOT_WAKEWORD_H
#define ROBOT_WAKEWORD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct robot_wakeword_result_s
{
  unsigned int epoch;
  bool triggered;
  float score, raw_score, rms, threshold;
  uint64_t end_sample, skipped_windows;
};
size_t robot_wakeword_window_samples(void);
int robot_wakeword_init(void);
void robot_wakeword_close(void);
/* Fast copy only. Never runs FFT or Invoke on the microphone task. */
int robot_wakeword_feed(const uint8_t *pcm, size_t bytes,
                       unsigned int epoch, bool active);
/* A single inference worker calls poll. 1=new score, 0=no score/stale result. */
int robot_wakeword_poll(struct robot_wakeword_result_s *result);
int robot_wakeword_selftest(void);
#ifdef __cplusplus
}
#endif
#endif
