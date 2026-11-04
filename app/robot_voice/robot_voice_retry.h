#ifndef ROBOT_VOICE_RETRY_H
#define ROBOT_VOICE_RETRY_H

#include <errno.h>
#include <stdbool.h>
#include <time.h>

#include "robot_voice_config.h"

typedef int (*robot_voice_retry_attempt_t)(void *arg, unsigned int timeout_ms);
typedef bool (*robot_voice_retry_cancel_t)(void *arg);

static inline unsigned long long robot_voice_retry_now_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (unsigned long long)ts.tv_sec * 1000ULL +
         (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static inline bool robot_voice_retryable(int result)
{
  if (result >= 0) return false;
  switch (result)
    {
      case -EINVAL:
      case -ENOMEM:
      case -ECANCELED:
      case -EOVERFLOW:
      case -ENOSPC:
      case -ENOSYS:
        return false;
      default:
        return true;
    }
}

/* Retry after an initial failed request. The initial request keeps its normal
 * service timeout; retries share one strict deadline and each receives the
 * remaining time as its HTTP timeout. Cancellation is polled during backoff. */
static inline int robot_voice_retry_after_failure(
  robot_voice_retry_attempt_t attempt, void *attempt_arg, int last_result,
  robot_voice_retry_cancel_t cancelled, void *cancel_arg,
  unsigned int *retries_out)
{
  unsigned long long start, deadline;
  unsigned int retry;

  if (retries_out) *retries_out = 0;
  if (!robot_voice_retryable(last_result) || !attempt) return last_result;
  start = robot_voice_retry_now_ms();
  deadline = start + ROBOT_VOICE_RETRY_BUDGET_MS;

  for (retry = 1; retry <= ROBOT_VOICE_RETRY_MAX; retry++)
    {
      unsigned long long now;
      unsigned int delay = ROBOT_VOICE_RETRY_DELAY_MS * retry;
      if (cancelled && cancelled(cancel_arg)) return -ECANCELED;
      now = robot_voice_retry_now_ms();
      if (now >= deadline) break;
      if (delay > deadline - now) delay = (unsigned int)(deadline - now);

      while (robot_voice_retry_now_ms() - now < delay)
        {
          struct timespec nap = { .tv_sec = 0, .tv_nsec = 20000000L };
          unsigned long long current = robot_voice_retry_now_ms();
          if (cancelled && cancelled(cancel_arg)) return -ECANCELED;
          if (current >= deadline || current - now >= delay) break;
          if (deadline - current < 20) nap.tv_nsec = (long)(deadline - current) * 1000000L;
          (void)nanosleep(&nap, NULL);
        }

      now = robot_voice_retry_now_ms();
      if (cancelled && cancelled(cancel_arg)) return -ECANCELED;
      if (now >= deadline) break;
      last_result = attempt(attempt_arg, (unsigned int)(deadline - now));
      if (retries_out) *retries_out = retry;
      if (last_result == 0 || !robot_voice_retryable(last_result)) return last_result;
    }

  return last_result;
}

#endif
