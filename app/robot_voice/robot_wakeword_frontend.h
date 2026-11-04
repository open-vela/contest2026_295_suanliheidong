#ifndef ROBOT_WAKEWORD_FRONTEND_H
#define ROBOT_WAKEWORD_FRONTEND_H
#include "robot_wakeword_model.h"
#ifdef __cplusplus
extern "C" {
#endif
struct robot_wakeword_frontend_s;
int robot_wakeword_frontend_create(
  const struct robot_wakeword_model_config_s *config,
  struct robot_wakeword_frontend_s **out);
void robot_wakeword_frontend_destroy(struct robot_wakeword_frontend_s *ctx);
/* pcm always contains window_samples, with zero padding on the left.
 * end_sample/generation identify consecutive windows for exact STFT reuse. */
int robot_wakeword_frontend_compute(struct robot_wakeword_frontend_s *ctx,
  const int16_t *pcm, uint64_t end_sample, uint64_t generation, float *features);
#ifdef __cplusplus
}
#endif
#endif
