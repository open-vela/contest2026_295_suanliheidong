#include "robot_wakeword_frontend.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct robot_wakeword_frontend_s
{
  const struct robot_wakeword_model_config_s *c;
  float *rows, *real, *imag, *power;
  uint32_t frames;
  uint64_t end_sample, generation;
  int cached;
};

void robot_wakeword_frontend_destroy(struct robot_wakeword_frontend_s *s)
{
  if (!s) return;
  free(s->rows); free(s->real); free(s->imag); free(s->power); free(s);
}

int robot_wakeword_frontend_create(
  const struct robot_wakeword_model_config_s *c,
  struct robot_wakeword_frontend_s **out)
{
  struct robot_wakeword_frontend_s *s;
  if (!out || !c) return -EINVAL;
  *out = NULL;
  if (c->sample_rate != 16000 || c->window_samples < c->frame_length ||
      c->window_samples > 160000 || !c->frame_step || !c->frame_length ||
      c->fft_length < c->frame_length || c->fft_length > 4096 ||
      (c->fft_length & (c->fft_length - 1)) || !c->time_bins ||
      !c->mel_bins || c->mel_bins > 128 || !(c->db_floor < 0) ||
      !c->hann || !c->twiddle_real || !c->twiddle_imag || !c->mel_start ||
      !c->mel_length || !c->mel_offset || !c->time_edges || !c->mel_weights)
    return -EINVAL;
  s = calloc(1, sizeof(*s));
  if (!s) return -ENOMEM;
  s->c = c;
  s->frames = 1 + (c->window_samples - c->frame_length) / c->frame_step;
  if (c->time_bins > s->frames) { free(s); return -EINVAL; }
  s->rows = calloc((size_t)s->frames * c->mel_bins, sizeof(float));
  s->real = malloc(c->fft_length * sizeof(float));
  s->imag = malloc(c->fft_length * sizeof(float));
  s->power = malloc((c->fft_length / 2 + 1) * sizeof(float));
  if (!s->rows || !s->real || !s->imag || !s->power)
    { robot_wakeword_frontend_destroy(s); return -ENOMEM; }
  *out = s;
  return 0;
}

static void fft(struct robot_wakeword_frontend_s *s)
{
  const uint32_t n = s->c->fft_length;
  uint32_t i, j = 0;
  for (i = 1; i < n; i++)
    {
      uint32_t bit = n >> 1;
      while (j & bit) { j ^= bit; bit >>= 1; }
      j ^= bit;
      if (i < j)
        {
          float t = s->real[i]; s->real[i] = s->real[j]; s->real[j] = t;
          t = s->imag[i]; s->imag[i] = s->imag[j]; s->imag[j] = t;
        }
    }
  for (uint32_t length = 2; length <= n; length <<= 1)
    for (uint32_t base = 0; base < n; base += length)
      for (j = 0; j < length / 2; j++)
        {
          uint32_t k = j * (n / length), a = base + j, b = a + length / 2;
          float wr = s->c->twiddle_real[k], wi = s->c->twiddle_imag[k];
          float tr = wr * s->real[b] - wi * s->imag[b];
          float ti = wr * s->imag[b] + wi * s->real[b];
          s->real[b] = s->real[a] - tr; s->imag[b] = s->imag[a] - ti;
          s->real[a] += tr; s->imag[a] += ti;
        }
}

static void frame(struct robot_wakeword_frontend_s *s, const int16_t *pcm,
                  float *out)
{
  const struct robot_wakeword_model_config_s *c = s->c;
  uint32_t i;
  int nonzero = 0;
  for (i = 0; i < c->frame_length; i++)
    {
      nonzero |= pcm[i] != 0;
      s->real[i] = (pcm[i] / 32768.0f) * c->hann[i];
    }
  if (!nonzero) { memset(out, 0, c->mel_bins * sizeof(float)); return; }
  memset(s->real + i, 0, (c->fft_length - i) * sizeof(float));
  memset(s->imag, 0, c->fft_length * sizeof(float));
  fft(s);
  for (i = 0; i <= c->fft_length / 2; i++)
    {
      float re = s->real[i] * c->inverse_window_sum;
      float im = s->imag[i] * c->inverse_window_sum;
      s->power[i] = re * re + im * im;
    }
  const float floor_power = powf(10.0f, c->db_floor / 10.0f);
  for (i = 0; i < c->mel_bins; i++)
    {
      float energy = 0;
      for (uint32_t j = 0; j < c->mel_length[i]; j++)
        energy += s->power[c->mel_start[i] + j] *
                  c->mel_weights[c->mel_offset[i] + j];
      float db = 10.0f * log10f(fmaxf(energy, floor_power));
      db = fmaxf(c->db_floor, fminf(0.0f, db));
      out[i] = (db - c->db_floor) / -c->db_floor;
    }
}

int robot_wakeword_frontend_compute(struct robot_wakeword_frontend_s *s,
  const int16_t *pcm, uint64_t end_sample, uint64_t generation, float *out)
{
  uint32_t first = 0;
  if (!s || !pcm || !out) return -EINVAL;
  const struct robot_wakeword_model_config_s *c = s->c;
  /* Reuse overlapping STFT rows. For the default 100 ms hop this computes
   * only ten new FFTs per score, rather than all 298 FFTs each time. */
  if (s->cached && s->generation == generation && end_sample > s->end_sample)
    {
      uint64_t delta = end_sample - s->end_sample;
      uint64_t rows = delta / c->frame_step;
      if (delta % c->frame_step == 0 && rows < s->frames)
        {
          first = s->frames - (uint32_t)rows;
          memmove(s->rows, s->rows + rows * c->mel_bins,
                  (size_t)first * c->mel_bins * sizeof(float));
        }
    }
  for (uint32_t i = first; i < s->frames; i++)
    frame(s, pcm + i * c->frame_step, s->rows + i * c->mel_bins);
  for (uint32_t t = 0; t < c->time_bins; t++)
    for (uint32_t m = 0; m < c->mel_bins; m++)
      {
        float sum = 0;
        for (uint32_t f = c->time_edges[t]; f < c->time_edges[t + 1]; f++)
          sum += s->rows[f * c->mel_bins + m];
        out[t * c->mel_bins + m] = sum /
          (c->time_edges[t + 1] - c->time_edges[t]);
      }
  s->cached = 1; s->end_sample = end_sample; s->generation = generation;
  return 0;
}
