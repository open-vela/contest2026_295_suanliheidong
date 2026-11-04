#ifndef ROBOT_WAKEWORD_MODEL_H
#define ROBOT_WAKEWORD_MODEL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct robot_wakeword_model_config_s
{
  uint32_t sample_rate, window_samples, hop_samples;
  uint32_t frame_length, frame_step, fft_length, mel_bins, time_bins;
  float db_floor, inverse_window_sum;
  float threshold, min_rms, cooldown_seconds, hysteresis;
  const float *hann, *twiddle_real, *twiddle_imag;
  const uint16_t *mel_start, *mel_length;
  const uint32_t *mel_offset, *time_edges;
  const float *mel_weights;
};
extern const unsigned char g_robot_wakeword_model[];
extern const size_t g_robot_wakeword_model_size;
extern const char g_robot_wakeword_model_sha256[];
extern const struct robot_wakeword_model_config_s g_robot_wakeword_model_config;
extern const int8_t g_robot_wakeword_test_input[];
extern const size_t g_robot_wakeword_test_input_size;
extern const int8_t g_robot_wakeword_test_output[2];

int robot_wakeword_model_init(void);
void robot_wakeword_model_close(void);
int robot_wakeword_model_predict(const float *features, float *score);
int robot_wakeword_model_test(float *score);
#ifdef __cplusplus
}
#endif
#endif
