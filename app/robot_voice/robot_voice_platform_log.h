#ifndef ROBOT_VOICE_PLATFORM_LOG_H
#define ROBOT_VOICE_PLATFORM_LOG_H
/* Included ONLY for C inputs containing the listed board trace tags. The CC
 * wrapper selects these translation units; libc is never globally interposed.
 * There are no tasks, locks, allocations or changes to driver scheduling. */
#ifndef ROBOT_VOICE_PLATFORM_TRACE
#  define ROBOT_VOICE_PLATFORM_TRACE 0
#endif
#if defined(ROBOT_VOICE_FILTER_PLATFORM_TRACE) && !ROBOT_VOICE_PLATFORM_TRACE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>

/* Severity occupies the low three bits (LOG_EMERG..LOG_DEBUG). Some
 * openvela/NuttX headers omit the BSD LOG_PRIMASK extension. Keep this
 * private so a force-include never adds foreign names to system headers. */
#define RV_PLATFORM_LOG_LEVEL_MASK 0x07

static inline int rv_platform_trace_hidden(const char *fmt)
{
  static const char *const tags[] = {
    "[C-I2S", "[WLAN-TXSUBMIT]", "[WLAN-TXDONE]", "[WLAN-BUF]",
    "[WLAN-TLS]", "[WLAN-TXFLOW]", "[RV-OLED]", "[BOARD-OLED]",
    "[RV-EXPR]", NULL
  };
  static const char *const errors[] = {
    "failed", "Failed", "FAILED", "ERROR", " error:", " error=",
    "timeout", "timed out", "corrupt", "overflow", "invalid", NULL
  };
  unsigned int i;
  if (!fmt) return 0;
  /* Preserve explicit faults, but not periodic fail=%u / error-count fields. */
  for (i = 0; errors[i]; i++) if (strstr(fmt, errors[i])) return 0;
  for (i = 0; tags[i]; i++) if (strstr(fmt, tags[i])) return 1;
  return 0;
}

static inline int rv_platform_printf(const char *fmt, ...)
  __attribute__((format(printf, 1, 2)));
static inline int rv_platform_printf(const char *fmt, ...)
{
  int ret = 0;
  va_list ap;
  va_start(ap, fmt);
  if (!rv_platform_trace_hidden(fmt)) ret = vprintf(fmt, ap);
  va_end(ap);
  return ret;
}

static inline int rv_platform_fprintf(FILE *stream, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
static inline int rv_platform_fprintf(FILE *stream, const char *fmt, ...)
{
  int ret = 0;
  va_list ap;
  va_start(ap, fmt);
  if ((stream != stdout && stream != stderr) || !rv_platform_trace_hidden(fmt))
    ret = vfprintf(stream, fmt, ap);
  va_end(ap);
  return ret;
}

static inline void rv_platform_syslog(int priority, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
static inline void rv_platform_syslog(int priority, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  if ((priority & RV_PLATFORM_LOG_LEVEL_MASK) <= LOG_WARNING ||
      !rv_platform_trace_hidden(fmt))
    {
      /* Preserve NuttX's disabled kernel syslog case as well as its
       * CONFIG_SYSLOG_TO_SCHED_NOTE vsyslog macro. */
#if !defined(__KERNEL__) || defined(CONFIG_SYSLOG) || defined(CONFIG_SYSLOG_TO_SCHED_NOTE)
      vsyslog(priority, fmt, ap);
#endif
    }
  va_end(ap);
}

#define printf(...) rv_platform_printf(__VA_ARGS__)
#define fprintf(...) rv_platform_fprintf(__VA_ARGS__)
#ifdef syslog
#  undef syslog
#endif
#define syslog(...) rv_platform_syslog(__VA_ARGS__)
#endif
#endif
