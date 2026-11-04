#include "robot_voice_diag.h"
#include <nuttx/config.h>
#include "robot_voice_weather.h"
#include "robot_voice_http.h"
#include "robot_voice_config.h"
#include <netutils/cJSON.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

struct rv_city { const char *name, *english, *lat, *lon; };
static const struct rv_city cities[] = {
  {"北京", "Beijing", "39.9042", "116.4074"},
  {"上海", "Shanghai", "31.2304", "121.4737"},
  {"广州", "Guangzhou", "23.1291", "113.2644"},
  {"深圳", "Shenzhen", "22.5431", "114.0579"},
  {"成都", "Chengdu", "30.5728", "104.0668"},
  {"杭州", "Hangzhou", "30.2741", "120.1551"},
  {"武汉", "Wuhan", "30.5928", "114.3055"},
  {"南京", "Nanjing", "32.0603", "118.7969"},
  {"重庆", "Chongqing", "29.5630", "106.5516"},
  {"西安", "Xian", "34.3416", "108.9398"},
};
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static char cached_city[128], cached_reply[512];
static unsigned long long cache_time;
static int cached_day;

static unsigned long long now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool prefix(char *s, const char *word)
{
  size_t n = strlen(word);
  if (strncmp(s, word, n)) return false;
  memmove(s, s + n, strlen(s + n) + 1);
  return true;
}

static bool suffix(char *s, const char *word)
{
  size_t n = strlen(word), size = strlen(s);
  if (size < n || strcmp(s + size - n, word)) return false;
  s[size - n] = 0;
  return true;
}

static int normalize(char *city, int *day)
{
  static const char *lead[] = {"请问", "请你告诉我", "请告诉我", "告诉我", "查一下", "查询", "现在", "今日"};
  bool changed;
  *day = 0;
  do {
    changed = prefix(city, " ");
    if (prefix(city, "今天")) changed = true;
    if (prefix(city, "明天")) { *day = 1; changed = true; }
    if (prefix(city, "后天")) { *day = 2; changed = true; }
    for (unsigned int i = 0; i < sizeof(lead) / sizeof(lead[0]); i++)
      if (prefix(city, lead[i])) changed = true;
  } while (changed);
  do { changed = suffix(city, " ") || suffix(city, "的"); } while (changed);
  if (!city[0] || strstr(city, "，") || strstr(city, "然后") || strstr(city, "以后") ||
      strchr(city, ',') || strchr(city, '"') || strchr(city, '\\')) return -EINVAL;
  for (const unsigned char *p = (const unsigned char *)city; *p; p++)
    if (*p < 32 || *p == 127) return -EINVAL;
  for (unsigned int i = 0; i < sizeof(cities) / sizeof(cities[0]); i++) {
    char with_city[64];
    snprintf(with_city, sizeof(with_city), "%s市", cities[i].name);
    if (!strcmp(city, cities[i].name) || !strcasecmp(city, cities[i].english) || !strcmp(city, with_city)) {
      snprintf(city, 128, "%s", cities[i].name);
      return (int)i;
    }
  }
  return -ENOENT;  /* A valid other location uses the geocoding service. */
}

static int get_json(const char *host, const char *path, char *response, size_t cap,
                    unsigned long long deadline)
{
  unsigned long long now = now_ms();
  if (now >= deadline) return -ETIMEDOUT;
  return robot_voice_http_request(host, "443", "GET", path, NULL, NULL, 0,
      response, cap, NULL, NULL, (unsigned int)(deadline - now));
}

static int encode(const char *s, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t used = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.';
    size_t n = plain ? 1 : 3;
    if (used + n >= cap) return -EOVERFLOW;
    if (plain) out[used++] = *p;
    else { out[used++] = '%'; out[used++] = hex[*p >> 4]; out[used++] = hex[*p & 15]; }
  }
  out[used] = 0;
  return 0;
}

static const char *condition(int code)
{
  if (code == 0) return "晴";
  if (code == 1 || code == 2) return "多云";
  if (code == 3) return "阴";
  if (code == 45 || code == 48) return "有雾";
  if (code >= 51 && code <= 57) return "毛毛雨";
  if (code == 61 || code == 80) return "小雨";
  if (code == 63 || code == 81) return "中雨";
  if (code == 65 || code == 82) return "大雨";
  if (code == 66 || code == 67) return "冻雨";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "有雪";
  if (code == 95 || code == 96 || code == 99) return "雷雨";
  return "天气状况未明确";
}

static bool temperature(cJSON *item)
{
  return cJSON_IsNumber(item) && isfinite(item->valuedouble) &&
         item->valuedouble > -100 && item->valuedouble < 80;
}

int robot_voice_weather_execute(const char *input, char *out, size_t cap)
{
  cJSON *request = NULL, *json = NULL;
  char city[128], lat[24], lon[24], path[768], reply[512];
  char *response = NULL;
  int day = 0, index, ret = -EIO;
  unsigned long long start = now_ms(), deadline = start + ROBOT_VOICE_WEATHER_MS;
  if (!out || cap < 2) return -EINVAL;
  out[0] = 0;
  request = input ? cJSON_Parse(input) : NULL;
  cJSON *location = request ? cJSON_GetObjectItem(request, "location") : NULL;
  if (!cJSON_IsString(location) || !location->valuestring ||
      !location->valuestring[0] || strlen(location->valuestring) >= sizeof(city)) {
    snprintf(out, cap, "请告诉我需要查询哪个城市的天气。");
    cJSON_Delete(request);
    return -EINVAL;
  }
  snprintf(city, sizeof(city), "%s", location->valuestring);
  cJSON_Delete(request);
  index = normalize(city, &day);
  if (index == -EINVAL) {
    snprintf(out, cap, "请告诉我需要查询哪个城市的天气。");
    return -EINVAL;
  }
  pthread_mutex_lock(&cache_lock);
  if (cache_time && start >= cache_time && start - cache_time < 300000 &&
      day == cached_day && !strcmp(city, cached_city)) {
    snprintf(out, cap, "%s", cached_reply);
    pthread_mutex_unlock(&cache_lock);
    RV_VERBOSE("[RV-WEATHER] city=%s day=%d cache=hit\n", city, day);
    return 0;
  }
  pthread_mutex_unlock(&cache_lock);
  response = malloc(16384);
  if (!response) { ret = -ENOMEM; goto error; }
  if (index >= 0) {
    snprintf(lat, sizeof(lat), "%s", cities[index].lat);
    snprintf(lon, sizeof(lon), "%s", cities[index].lon);
  } else {
    char encoded[384];
    if (encode(city, encoded, sizeof(encoded)) < 0) goto error;
    snprintf(path, sizeof(path), "/v1/search?name=%s&count=1&language=zh&format=json", encoded);
    ret = get_json("geocoding-api.open-meteo.com", path, response, 16384, deadline);
    if (ret != 200) goto error;
    json = cJSON_Parse(response);
    cJSON *entry = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "results"), 0);
    cJSON *latitude = cJSON_GetObjectItem(entry, "latitude");
    cJSON *longitude = cJSON_GetObjectItem(entry, "longitude");
    if (!cJSON_IsNumber(latitude) || !cJSON_IsNumber(longitude) ||
        !isfinite(latitude->valuedouble) || !isfinite(longitude->valuedouble) ||
        fabs(latitude->valuedouble) > 90 || fabs(longitude->valuedouble) > 180) goto error;
    snprintf(lat, sizeof(lat), "%.4f", latitude->valuedouble);
    snprintf(lon, sizeof(lon), "%.4f", longitude->valuedouble);
    cJSON_Delete(json); json = NULL;
  }
  snprintf(path, sizeof(path), "/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=3", lat, lon);
  RV_VERBOSE("[RV-WEATHER] city=%s day=%d provider=Open-Meteo budget_ms=%u\n", city, day, ROBOT_VOICE_WEATHER_MS);
  ret = get_json("api.open-meteo.com", path, response, 16384, deadline);
  if (ret != 200) goto error;
  json = cJSON_Parse(response);
  cJSON *current = cJSON_GetObjectItem(json, "current");
  cJSON *daily = cJSON_GetObjectItem(json, "daily");
  cJSON *t = cJSON_GetObjectItem(current, "temperature_2m");
  cJSON *w = cJSON_GetObjectItem(current, "weather_code");
  cJSON *time = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "time"), day);
  cJSON *high = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), day);
  cJSON *low = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), day);
  if (!cJSON_IsString(time) || !temperature(high) || !temperature(low) || low->valuedouble > high->valuedouble) goto error;
  if (day == 0) {
    if (!temperature(t) || !cJSON_IsNumber(w)) goto error;
    snprintf(reply, sizeof(reply), "%s%s，当前约%.0f度，今天%.0f到%.0f度。", city, condition(w->valueint), t->valuedouble, low->valuedouble, high->valuedouble);
  } else {
    w = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "weather_code"), day);
    if (!cJSON_IsNumber(w)) goto error;
    snprintf(reply, sizeof(reply), "%s%s%s，%.0f到%.0f度。", city, day == 1 ? "明天" : "后天", condition(w->valueint), low->valuedouble, high->valuedouble);
  }
  pthread_mutex_lock(&cache_lock);
  snprintf(cached_city, sizeof(cached_city), "%s", city);
  snprintf(cached_reply, sizeof(cached_reply), "%s", reply);
  cache_time = now_ms(); cached_day = day;
  pthread_mutex_unlock(&cache_lock);
  snprintf(out, cap, "%s", reply);
  RV_VERBOSE("[RV-WEATHER] done city=%s forecast_date=%s ms=%llu\n", city, time->valuestring, now_ms() - start);
  cJSON_Delete(json); free(response);
  return 0;
error:
  /* No fabricated weather, no key requirement and no spoken English error. */
  snprintf(out, cap, "暂时没查到%s的天气预报，请稍后再试。", city);
  printf("[RV-WEATHER] failed city=%s rc=%d ms=%llu\n", city, ret, now_ms() - start);
  cJSON_Delete(json); free(response);
  return ret < 0 ? ret : -EIO;
}

#ifdef ROBOT_VOICE_AGENT_INTERPOSE
int tool_get_weather_execute(const char *input, char *out, size_t cap)
{
  return robot_voice_weather_execute(input, out, cap);
}
#endif

void robot_voice_weather_link_anchor(void)
{
  RV_VERBOSE("[RV-WEATHER] contest tool ready; provider=Open-Meteo city-normalization=yes\n");
}
