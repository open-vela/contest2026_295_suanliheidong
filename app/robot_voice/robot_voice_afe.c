/* 16 kHz mono front end: integer DC/high-pass -> WebRTC VAD.
 * Runs only on the capture thread, after RX has been requeued, never HPWORK.
 * The fixed frame adapter keeps every sample, including all non-speech.
 */
#include "robot_voice_afe.h"
#include "robot_voice_afe_vendor.h"
#include "vendor/libfvad/include/fvad.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int robot_afe_vad_evidence(Fvad *inst, const int16_t *frame, size_t length);

struct robot_voice_afe_s
{
  Fvad *vad;
  enum robot_voice_afe_mode_e mode;
  int16_t raw[ROBOT_AFE_SAMPLES];
  int16_t clean[ROBOT_AFE_SAMPLES];
  size_t used;
  int32_t hp_x, hp_y;
  unsigned int noise_q8;
  bool suppressed;
  struct robot_voice_afe_stats_s stats;
};

static uint64_t rv_afe_us(void)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
  return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static unsigned int rv_afe_level(const int16_t *pcm)
{
  int32_t mean = 0;
  uint32_t sum = 0;
  size_t i;
  for (i = 0; i < ROBOT_AFE_SAMPLES; i++) mean += pcm[i];
  mean /= ROBOT_AFE_SAMPLES;
  for (i = 0; i < ROBOT_AFE_SAMPLES; i++)
    {
      int32_t delta = (int32_t)pcm[i] - mean;
      sum += delta < 0 ? -delta : delta;
    }
  return sum / ROBOT_AFE_SAMPLES;
}

struct robot_voice_afe_s *robot_voice_afe_create(enum robot_voice_afe_mode_e mode)
{
  struct robot_voice_afe_s *a;
  if (mode < ROBOT_AFE_CLEAN || mode > ROBOT_AFE_KWS_RAW) return NULL;
  a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->mode = mode;
  a->noise_q8 = 32u * 256u;
  a->vad = fvad_new();
  if (!a->vad) goto failed;
  if (fvad_set_sample_rate(a->vad, ROBOT_VOICE_CAPTURE_RATE) < 0 ||
      fvad_set_mode(a->vad, ROBOT_VOICE_VAD_MODE) < 0)
    goto failed;
  return a;
failed:
  robot_voice_afe_destroy(a);
  return NULL;
}

void robot_voice_afe_destroy(struct robot_voice_afe_s *a)
{
  if (!a) return;
  if (a->vad) fvad_free(a->vad);
  free(a);
}

void robot_voice_afe_suppress(struct robot_voice_afe_s *a, bool suppressed)
{
  if (!a || a->suppressed == suppressed) return;
  a->suppressed = suppressed;
  fvad_reset(a->vad);
  (void)fvad_set_sample_rate(a->vad, ROBOT_VOICE_CAPTURE_RATE);
  (void)fvad_set_mode(a->vad, ROBOT_VOICE_VAD_MODE);
}

static int rv_afe_frame(struct robot_voice_afe_s *a,
                         robot_voice_afe_sink_t sink, void *arg)
{
  struct robot_voice_afe_frame_s frame;
  uint64_t start = rv_afe_us(), elapsed;
  unsigned int level, threshold;
  const int16_t *vad_audio;
  size_t i;
  int decision = 0;
  for (i = 0; i < ROBOT_AFE_SAMPLES; i++)
    {
      int32_t x = a->raw[i];
      /* Single integer DC blocker, with continuous state and no FFT,
       * overlap delay, gate or frame deletion. Only the VAD/KWS branch
       * can use this signal; ASR always gets byte-identical raw PCM. */
      int32_t y = x - a->hp_x + a->hp_y * 31 / 32;
      a->hp_x = x;
      if (y > 32767) y = 32767;
      if (y < -32768) y = -32768;
      a->hp_y = y;
      a->clean[i] = (int16_t)y;
      if (a->raw[i] == INT16_MIN || a->raw[i] == INT16_MAX)
        a->stats.clipped_samples++;
    }
  frame.raw = a->raw;
  frame.clean = a->clean;
  frame.audio = a->raw;
  frame.kws = a->mode == ROBOT_AFE_CLEAN ? a->clean : a->raw;
  vad_audio = a->mode == ROBOT_AFE_RAW ? a->raw : a->clean;
  level = rv_afe_level(vad_audio);
  threshold = (a->noise_q8 / 256u) * 3u / 2u + 20u;
  if (threshold < ROBOT_VOICE_VAD_MIN_LEVEL)
    threshold = ROBOT_VOICE_VAD_MIN_LEVEL;
  if (!a->suppressed)
    {
      decision = robot_afe_vad_evidence(a->vad, vad_audio, ROBOT_AFE_SAMPLES);
      if (decision < 0) return -EIO;
      /* >1 is WebRTC's own hangover, NOT a new speech observation. */
      if (decision != 1 || level < threshold)
        {
          unsigned int target = level * 256u;
          /* Fast downward, slow upward, with fractional precision. */
          if (target < a->noise_q8)
            a->noise_q8 -= (a->noise_q8 - target + 7u) / 8u;
          else
            a->noise_q8 += (target - a->noise_q8 + 63u) / 64u;
        }
    }
  frame.level = level;
  frame.threshold = threshold;
  frame.voiced = !a->suppressed && decision == 1 && level >= threshold;
  a->stats.frames++;
  a->stats.output_samples += ROBOT_AFE_SAMPLES;
  a->stats.speech_frames += frame.voiced ? 1u : 0u;
  a->stats.raw_level = rv_afe_level(a->raw);
  a->stats.clean_level = rv_afe_level(a->clean);
  a->stats.noise_level = a->noise_q8 / 256u;
  a->stats.threshold = threshold;
  elapsed = rv_afe_us();
  elapsed = elapsed >= start ? elapsed - start : 0;
  if (elapsed > a->stats.max_process_us)
    a->stats.max_process_us = elapsed > UINT32_MAX ? UINT32_MAX : elapsed;
  if (elapsed > ROBOT_VOICE_AFE_FRAME_MS * 1000u) a->stats.overruns++;
  return sink(&frame, arg);
}

int robot_voice_afe_push(struct robot_voice_afe_s *a, const uint8_t *pcm,
                         size_t len, robot_voice_afe_sink_t sink, void *arg)
{
  size_t take;
  int ret;
  if (!a || !sink || (!pcm && len) || (len & 1u)) return -EINVAL;
  while (len)
    {
      take = ROBOT_AFE_BYTES - a->used;
      if (take > len) take = len;
      memcpy((uint8_t *)a->raw + a->used, pcm, take);
      a->stats.input_samples += take / sizeof(int16_t);
      pcm += take;
      len -= take;
      a->used += take;
      if (a->used == ROBOT_AFE_BYTES)
        {
          a->used = 0;
          ret = rv_afe_frame(a, sink, arg);
          if (ret != 0) return ret;
        }
    }
  return 0;
}

void robot_voice_afe_stats(const struct robot_voice_afe_s *a,
                           struct robot_voice_afe_stats_s *out)
{
  if (!a || !out) return;
  *out = a->stats;
  out->partial_samples = a->used / sizeof(int16_t);
}

/* Compatibility entry point for once/test_asr. ASR owns noise handling;
 * never change, crop, delay or zero-pad its finite recording. */
int robot_voice_afe_recording(uint8_t *pcm, size_t len,
                             enum robot_voice_afe_mode_e mode)
{
  if ((!pcm && len) || (len & 1u) || mode < ROBOT_AFE_CLEAN || mode > ROBOT_AFE_KWS_RAW)
    return -EINVAL;
  return 0;
}
