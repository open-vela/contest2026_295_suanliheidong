#ifndef ROBOT_VOICE_THREADS_H
#define ROBOT_VOICE_THREADS_H
#include <pthread.h>
#include <sched.h>
#include "robot_voice_config.h"

/* Return pthread's positive errno. The caller destroys attr after success
 * or failure. Host fixtures keep normal POSIX scheduling without privileges.
 * RR uses the board time slice only when available. Every worker blocks
 * on I/O/conditions; no scheduler lock or affinity binding is introduced. */
static inline int rv_thread_attributes(pthread_attr_t *attr, size_t stack,
                                       int priority)
{
  int ret = pthread_attr_setstacksize(attr, stack);
#if defined(__NuttX__) && ROBOT_VOICE_EXPLICIT_SCHED
  struct sched_param param = {0};
  param.sched_priority = priority;
  if (ret == 0) ret = pthread_attr_setschedpolicy(attr, SCHED_FIFO);
  if (ret == 0) ret = pthread_attr_setschedparam(attr, &param);
  if (ret == 0) ret = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
#elif defined(__NuttX__) && ROBOT_VOICE_FAIR_SCHED && defined(CONFIG_RR_INTERVAL) && CONFIG_RR_INTERVAL > 0
  /* Keep the caller's real priority. RR only shares CPU among peers and
   * leaves Wi-Fi/HPWORK priorities untouched. Fall back with no RR support. */
  struct sched_param param = {0};
  int policy;
  (void)priority;
  if (ret == 0) ret = pthread_getschedparam(pthread_self(), &policy, &param);
  if (ret == 0) ret = pthread_attr_setschedpolicy(attr, SCHED_RR);
  if (ret == 0) ret = pthread_attr_setschedparam(attr, &param);
  if (ret == 0) ret = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
#else
  (void)priority;
#endif
  return ret;
}
#endif
