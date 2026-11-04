#include "robot_voice_diag.h"
#include "robot_voice_clock.h"
#include "robot_voice_config.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROBOT_VOICE_TIME_UTC_OFFSET_MIN < -720 || ROBOT_VOICE_TIME_UTC_OFFSET_MIN > 840
#  error ROBOT_VOICE_TIME_UTC_OFFSET_MIN out of supported timezone range
#endif

static pthread_mutex_t clock_lock = PTHREAD_MUTEX_INITIALIZER;
static bool confirmed;
static time_t confirmed_utc;
static unsigned long long confirmed_mono;

static unsigned long long mono_ms(void)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
  return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool plausible(time_t utc)
{
  /* Range checking is only a sanity check, never proof of NTP success. */
  return (int64_t)utc >= INT64_C(1704067200) &&
         (int64_t)utc < INT64_C(4102444800);
}

int robot_voice_clock_confirm(void)
{
  time_t utc = time(NULL);
  if (!plausible(utc)) return -ERANGE;
  pthread_mutex_lock(&clock_lock);
  confirmed_utc = utc;
  confirmed_mono = mono_ms();
  confirmed = true;
  pthread_mutex_unlock(&clock_lock);
  RV_VERBOSE("[RV-CLOCK] explicitly_confirmed utc=%lld offset_min=%d\n",
         (long long)utc, ROBOT_VOICE_TIME_UTC_OFFSET_MIN);
  return 0;
}

void robot_voice_clock_clear(void)
{
  pthread_mutex_lock(&clock_lock);
  confirmed = false;
  pthread_mutex_unlock(&clock_lock);
}

static bool trusted_now(time_t utc)
{
  unsigned long long now = mono_ms();
  bool valid;
  pthread_mutex_lock(&clock_lock);
  valid = confirmed && plausible(utc) && now >= confirmed_mono &&
          now - confirmed_mono <= ROBOT_VOICE_CLOCK_VALID_MS;
  if (valid) {
    int64_t drift = (int64_t)utc - (int64_t)confirmed_utc -
                    (int64_t)((now - confirmed_mono) / 1000);
    valid = drift >= -5 && drift <= 5;
  }
  if (!valid) confirmed = false;
  pthread_mutex_unlock(&clock_lock);
  return valid;
}

/* Normalize only whitespace and sentence punctuation. Use exact whole-turn
 * matches so "几点，然后向前走" can never swallow the action suffix. */
static bool normalize(const char *text, char *out, size_t cap)
{
  static const char *const separators[] = {"，", "。", "？", "！", NULL};
  size_t n = 0;
  if (!text) return false;
  while (*text) {
    bool skip = false;
    if (strchr(" \t\r\n,.?!", (unsigned char)*text)) { text++; continue; }
    for (unsigned int i = 0; separators[i]; i++) {
      size_t bytes = strlen(separators[i]);
      if (!strncmp(text, separators[i], bytes)) { text += bytes; skip = true; break; }
    }
    if (skip) continue;
    if (n + 1 >= cap) return false;
    out[n++] = *text++;
  }
  out[n] = 0;
  return true;
}

bool robot_voice_clock_reply_at(const char *text, time_t utc, bool trusted,
                                char *reply, size_t cap)
{
  static const char *const time_questions[] = {
    "现在几点", "现在几点钟", "几点了", "几点钟了", "现在是几点",
    "现在是几点钟", "现在几点了", "现在时间是多少", "现在是什么时间",
    "现在的时间", "现在的时间是多少", NULL
  };
  static const char *const date_questions[] = {
    "今天几号", "今天是几号", "今天日期", "今天是什么日期", NULL
  };
  char normalized[192];
  const char *question;
  bool match_time = false, match_date = false;
  if (!reply || cap < 128 || !normalize(text, normalized, sizeof(normalized))) return false;
  question = normalized;
  const char *prefixes[] = {"请你告诉我", "请告诉我", "告诉我", "请问", NULL};
  for (unsigned int i = 0; prefixes[i]; i++) {
    size_t n = strlen(prefixes[i]);
    if (!strncmp(question, prefixes[i], n)) { question += n; break; }
  }
  for (unsigned int i = 0; time_questions[i]; i++)
    if (!strcmp(question, time_questions[i])) match_time = true;
  for (unsigned int i = 0; date_questions[i]; i++)
    if (!strcmp(question, date_questions[i])) match_date = true;
  if (!match_time && !match_date) return false;
  if (!trusted || !plausible(utc)) {
    snprintf(reply, cap, "设备时钟还没确认同步，暂时不能准确报时。");
    return true;
  }
  int64_t local64 = (int64_t)utc + ROBOT_VOICE_TIME_UTC_OFFSET_MIN * 60;
  time_t local = (time_t)local64;
  struct tm t;
  if ((int64_t)local != local64 || !gmtime_r(&local, &t)) {
    snprintf(reply, cap, "设备时钟读取失败，暂时不能准确报时。");
    return true;
  }
  if (match_date)
    snprintf(reply, cap, "今天是%d年%d月%d日。", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  else
    snprintf(reply, cap, "现在是%d点%02d分。", t.tm_hour, t.tm_min);
  return true;
}

bool robot_voice_clock_reply(const char *text, char *reply, size_t cap)
{
  time_t utc = time(NULL);
  bool trusted = trusted_now(utc);
  bool handled = robot_voice_clock_reply_at(text, utc, trusted, reply, cap);
  if (handled)
    RV_VERBOSE("[RV-CLOCK] route=local trusted=%u utc=%lld offset_min=%d\n",
           trusted, (long long)utc, ROBOT_VOICE_TIME_UTC_OFFSET_MIN);
  return handled;
}

void robot_voice_clock_diag(void)
{
  time_t utc = time(NULL);
  RV_VERBOSE("[RV-CLOCK] trusted=%u utc=%lld offset_min=%d\n",
         trusted_now(utc), (long long)utc, ROBOT_VOICE_TIME_UTC_OFFSET_MIN);
}

void robot_voice_clock_context(char *out, size_t cap)
{
  if (!out || !cap) return;
  time_t utc = time(NULL);
  int64_t local64 = (int64_t)utc + ROBOT_VOICE_TIME_UTC_OFFSET_MIN * 60;
  time_t local = (time_t)local64;
  struct tm t;
  if (!trusted_now(utc) || (int64_t)local != local64 || !gmtime_r(&local, &t)) {
    snprintf(out, cap, "设备时间状态：时钟未确认同步，没有可信的当前时间，不能猜测。");
    return;
  }
  snprintf(out, cap, "设备时间状态：已确认的设备时钟为%d年%d月%d日%d点%02d分，"
           "UTC偏移%d分钟。仅本轮请求有效，不能将它当作以后的当前时间。",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
           ROBOT_VOICE_TIME_UTC_OFFSET_MIN);
}
