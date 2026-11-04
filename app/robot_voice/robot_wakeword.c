#include "robot_wakeword.h"
#include "robot_wakeword_config.h"
#include "robot_wakeword_frontend.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_wake_lock = PTHREAD_MUTEX_INITIALIZER;
static struct robot_wakeword_frontend_s *g_front;
static int16_t *g_ring, *g_snapshot;
static float *g_features;
static size_t g_capacity;
static uint64_t g_total, g_last_scored, g_generation, g_last_trigger;
static uint64_t g_candidate_last_end;
static unsigned int g_candidate_hits;
static unsigned int g_epoch;
static float g_score_window[5];
static float g_score_sum;
static unsigned int g_score_count, g_score_pos;
static bool g_active, g_armed, g_has_trigger;

#if ROBOT_WAKEWORD_SCORE_AVG_WINDOW < 3 || ROBOT_WAKEWORD_SCORE_AVG_WINDOW > 5
#  error ROBOT_WAKEWORD_SCORE_AVG_WINDOW must be between 3 and 5
#endif
#if ROBOT_WAKEWORD_CONFIRM_HITS < 1
#  error ROBOT_WAKEWORD_CONFIRM_HITS must be at least one
#endif

static void reset_score_average(void)
{
  memset(g_score_window, 0, sizeof(g_score_window));
  g_score_sum = 0;
  g_score_count = 0;
  g_score_pos = 0;
}

static float threshold(void)
{
  return ROBOT_WAKEWORD_THRESHOLD_OVERRIDE > 0 ?
    ROBOT_WAKEWORD_THRESHOLD_OVERRIDE : g_robot_wakeword_model_config.threshold;
}

void robot_wakeword_close(void)
{
  /* The controller joins capture + inference before close. */
  robot_wakeword_frontend_destroy(g_front); g_front = NULL;
  free(g_ring); g_ring = NULL; free(g_snapshot); g_snapshot = NULL;
  free(g_features); g_features = NULL;
  robot_wakeword_model_close();
}

int robot_wakeword_init(void)
{
  const struct robot_wakeword_model_config_s *c = &g_robot_wakeword_model_config;
  if (g_ring) return -EBUSY;
  int ret = robot_wakeword_model_init();
  if (ret < 0) return ret;
  if ((ROBOT_WAKEWORD_THRESHOLD_OVERRIDE != -1.0f &&
       !(ROBOT_WAKEWORD_THRESHOLD_OVERRIDE > 0 && ROBOT_WAKEWORD_THRESHOLD_OVERRIDE <= 1)) ||
      !isfinite(threshold()) || threshold() <= 0 || threshold() > 1 ||
      !c->hop_samples || c->hop_samples > c->window_samples ||
      !isfinite(c->min_rms) || c->min_rms < 0 ||
      !isfinite(c->cooldown_seconds) || c->cooldown_seconds < 0 ||
      !isfinite(c->hysteresis) || c->hysteresis < 0 || c->hysteresis >= 1)
    { ret = -EINVAL; goto fail; }
  ret = robot_wakeword_frontend_create(c, &g_front);
  if (ret < 0) goto fail;
  g_capacity = (size_t)c->window_samples + c->hop_samples;
  g_ring = calloc(g_capacity, sizeof(int16_t));
  g_snapshot = malloc(c->window_samples * sizeof(int16_t));
  g_features = malloc((size_t)c->time_bins * c->mel_bins * sizeof(float));
  if (!g_ring || !g_snapshot || !g_features) { ret = -ENOMEM; goto fail; }
  g_total = g_last_scored = g_last_trigger = g_candidate_last_end = 0;
  g_candidate_hits = 0; g_generation++;
  reset_score_average();
  g_active = false; g_armed = true; g_has_trigger = false; g_epoch = 0;
  printf("[TFLite-Wake] ready sample_rate=%lu window=%lu hop=%lu average_windows=%u sha256=%s\n",
    (unsigned long)c->sample_rate, (unsigned long)c->window_samples,
    (unsigned long)c->hop_samples,
    (unsigned int)ROBOT_WAKEWORD_SCORE_AVG_WINDOW, g_robot_wakeword_model_sha256);
  return 0;
fail:
  robot_wakeword_close();
  return ret;
}

size_t robot_wakeword_window_samples(void)
{
  return g_robot_wakeword_model_config.window_samples;
}

int robot_wakeword_feed(const uint8_t *pcm, size_t bytes,
                       unsigned int epoch, bool active)
{
  if ((bytes & 1) || (!pcm && bytes)) return -EINVAL;
  pthread_mutex_lock(&g_wake_lock);
  if (!g_ring) { pthread_mutex_unlock(&g_wake_lock); return -ENODEV; }
  if (active != g_active || epoch != g_epoch)
    {
      g_total = g_last_scored = g_last_trigger = 0;
      g_generation++; g_active = active; g_epoch = epoch;
      g_candidate_last_end = 0; g_candidate_hits = 0;
      reset_score_average();
      g_armed = true; g_has_trigger = false;
    }
  if (active)
    for (size_t i = 0; i < bytes; i += 2)
      {
        g_ring[g_total % g_capacity] =
          (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1] << 8));
        g_total++;
      }
  pthread_mutex_unlock(&g_wake_lock);
  return 0;
}

int robot_wakeword_poll(struct robot_wakeword_result_s *out)
{
  const struct robot_wakeword_model_config_s *c = &g_robot_wakeword_model_config;
  uint64_t end, gen, old_end;
  unsigned int epoch;
  size_t count, start, first;
  float raw, rms, gated_score;
  double energy = 0;
  int ret;
  if (!out) return -EINVAL;
  pthread_mutex_lock(&g_wake_lock);
  if (!g_ring || !g_active) { pthread_mutex_unlock(&g_wake_lock); return 0; }
  end = g_total / c->hop_samples * c->hop_samples;
  if (end == g_last_scored) { pthread_mutex_unlock(&g_wake_lock); return 0; }
  gen = g_generation; epoch = g_epoch; old_end = g_last_scored;
  count = end < c->window_samples ? (size_t)end : c->window_samples;
  memset(g_snapshot, 0, (c->window_samples - count) * sizeof(int16_t));
  start = (size_t)((end - count) % g_capacity);
  first = g_capacity - start;
  if (first > count) first = count;
  memcpy(g_snapshot + c->window_samples - count, g_ring + start,
         first * sizeof(int16_t));
  memcpy(g_snapshot + c->window_samples - count + first, g_ring,
         (count - first) * sizeof(int16_t));
  pthread_mutex_unlock(&g_wake_lock);
  ret = robot_wakeword_frontend_compute(g_front, g_snapshot, end, gen, g_features);
  if (ret < 0) return ret;
  ret = robot_wakeword_model_predict(g_features, &raw);
  if (ret < 0) return ret;
  for (size_t i = 0; i < c->window_samples; i++)
    { double value = g_snapshot[i] / 32768.0; energy += value * value; }
  rms = (float)sqrt(energy / c->window_samples);
  pthread_mutex_lock(&g_wake_lock);
  if (gen != g_generation || !g_active || epoch != g_epoch)
    { pthread_mutex_unlock(&g_wake_lock); return 0; }
  g_last_scored = end;
  memset(out, 0, sizeof(*out));
  out->epoch = epoch; out->end_sample = end; out->raw_score = raw;
  gated_score = rms >= c->min_rms ? raw : 0;
  if (g_score_count == ROBOT_WAKEWORD_SCORE_AVG_WINDOW)
    g_score_sum -= g_score_window[g_score_pos];
  else
    g_score_count++;
  g_score_window[g_score_pos] = gated_score;
  g_score_sum += gated_score;
  g_score_pos = (g_score_pos + 1) % ROBOT_WAKEWORD_SCORE_AVG_WINDOW;
  out->score = g_score_sum / g_score_count;
  out->rms = rms;
  out->threshold = threshold();
  out->skipped_windows = (end - old_end) / c->hop_samples - 1;
  /* Do not trigger until a complete configured moving-average window exists. */
  if (g_score_count < ROBOT_WAKEWORD_SCORE_AVG_WINDOW)
    {
      g_candidate_hits = 0;
      g_candidate_last_end = 0;
    }
  else if (out->score <= fmaxf(0, out->threshold - c->hysteresis))
    {
      g_armed = true;
      g_candidate_hits = 0;
      g_candidate_last_end = 0;
    }
  else if (out->score >= out->threshold)
    {
      if (out->skipped_windows != 0 || !g_candidate_hits ||
          end - g_candidate_last_end != c->hop_samples)
        g_candidate_hits = 1;
      else
        g_candidate_hits++;
      g_candidate_last_end = end;
    }
  else
    {
      g_candidate_hits = 0;
      g_candidate_last_end = 0;
    }
  if (g_armed && g_candidate_hits >= ROBOT_WAKEWORD_CONFIRM_HITS &&
      (!g_has_trigger || (double)(end - g_last_trigger) >=
       (double)c->cooldown_seconds * c->sample_rate))
    {
      out->triggered = true; g_armed = false;
      g_has_trigger = true; g_last_trigger = end;
      g_candidate_hits = 0;
      g_candidate_last_end = 0;
    }
  pthread_mutex_unlock(&g_wake_lock);
  return 1;
}

int robot_wakeword_selftest(void)
{
  float score = 0;
  int ret = robot_wakeword_model_init();
  if (!ret) ret = robot_wakeword_model_test(&score);
  printf("[TFLite-Wake] golden selftest rc=%d positive_score=%.6f\n", ret, (double)score);
  robot_wakeword_model_close();
  return ret;
}
