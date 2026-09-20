#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "tools/tool_registry.h"

#include "robot_audio_playback.h"
#include "robot_expression.h"
#include "robot_music_player.h"

#define ROBOT_MUSIC_SAMPLE_RATE      24000
#define ROBOT_MUSIC_CHANNELS         1
#define ROBOT_MUSIC_BITS             16
#define ROBOT_MUSIC_READ_BYTES       1024

#define ROBOT_MUSIC_TOOL_NAME        "robot_play_music"
#define ROBOT_MUSIC_STOP_TOOL_NAME   "robot_stop_music"
#define ROBOT_MUSIC_DEFAULT_PATH     "/etc/media/test.wav"
#define ROBOT_MUSIC_PATH_MAX         256
#define ROBOT_MUSIC_WORKER_STACK     (16 * 1024)

/* Music software gain: 1/2 amplitude ~= -6.02 dB. */
#define ROBOT_MUSIC_GAIN_NUM          1
#define ROBOT_MUSIC_GAIN_DEN          2

struct robot_music_job_s
{
  char path[ROBOT_MUSIC_PATH_MAX];
};

static pthread_mutex_t g_robot_music_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_robot_music_cond = PTHREAD_COND_INITIALIZER;
static bool g_robot_music_active;
static bool g_robot_music_stop_requested;
static bool g_robot_music_tool_registered;
static char g_robot_music_path[ROBOT_MUSIC_PATH_MAX];
static int g_robot_music_last_result;

/* robot_media_player.c provides this when the official Media path is linked. */
extern int robot_media_stop_active(void) __attribute__((weak));

static bool robot_music_stop_requested(void)
{
  bool stop;

  pthread_mutex_lock(&g_robot_music_lock);
  stop = g_robot_music_stop_requested;
  pthread_mutex_unlock(&g_robot_music_lock);
  return stop;
}

static void robot_music_scale_pcm16_half(uint8_t *buf, size_t len)
{
  size_t i;

  for (i = 0; i + 1 < len; i += 2)
    {
      uint16_t raw = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
      int16_t sample = (int16_t)raw;
      int16_t scaled =
        (int16_t)((sample * ROBOT_MUSIC_GAIN_NUM) / ROBOT_MUSIC_GAIN_DEN);

      buf[i] = (uint8_t)((uint16_t)scaled & 0xff);
      buf[i + 1] = (uint8_t)(((uint16_t)scaled >> 8) & 0xff);
    }
}

static uint16_t robot_music_u16le(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t robot_music_u32le(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static int robot_music_read_exact(int fd, uint8_t *buf, size_t len)
{
  size_t done = 0;

  while (done < len)
    {
      ssize_t nread = read(fd, buf + done, len - done);

      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (nread == 0)
        {
          return -EIO;
        }

      done += (size_t)nread;
    }

  return 0;
}

static int robot_music_skip(int fd, uint32_t bytes)
{
  if (bytes == 0)
    {
      return 0;
    }

  if (lseek(fd, (off_t)bytes, SEEK_CUR) < 0)
    {
      return -errno;
    }

  return 0;
}

int robot_music_play_file(const char *path)
{
  uint8_t riff[12];
  uint8_t chunk[8];
  uint8_t fmt[16];
  uint8_t pcm[ROBOT_MUSIC_READ_BYTES];
  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint16_t bits_per_sample = 0;
  uint32_t sample_rate = 0;
  uint32_t data_bytes = 0;
  off_t data_offset = (off_t)-1;
  bool fmt_found = false;
  bool playback_opened = false;
  bool music_expression_set = false;
  int fd = -1;
  int ret = 0;

  if (path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  printf("[ROBOT-MUSIC] source=%s\n", path);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      ret = -errno;
      printf("[ROBOT-MUSIC] open failed rc=%d\n", ret);
      return ret;
    }

  ret = robot_music_read_exact(fd, riff, sizeof(riff));
  if (ret < 0)
    {
      printf("[ROBOT-MUSIC] failed to read RIFF header rc=%d\n", ret);
      goto out;
    }

  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
    {
      printf("[ROBOT-MUSIC] unsupported container: expected RIFF/WAVE\n");
      ret = -EINVAL;
      goto out;
    }

  for (;;)
    {
      uint32_t chunk_size;
      off_t chunk_data_offset;
      ssize_t nread;

      nread = read(fd, chunk, sizeof(chunk));
      if (nread == 0)
        {
          break;
        }

      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          goto out;
        }

      if (nread != (ssize_t)sizeof(chunk))
        {
          ret = -EIO;
          goto out;
        }

      chunk_size = robot_music_u32le(chunk + 4);
      chunk_data_offset = lseek(fd, 0, SEEK_CUR);
      if (chunk_data_offset < 0)
        {
          ret = -errno;
          goto out;
        }

      if (memcmp(chunk, "fmt ", 4) == 0)
        {
          uint32_t remaining;

          if (chunk_size < sizeof(fmt))
            {
              printf("[ROBOT-MUSIC] invalid fmt chunk size=%lu\n",
                     (unsigned long)chunk_size);
              ret = -EINVAL;
              goto out;
            }

          ret = robot_music_read_exact(fd, fmt, sizeof(fmt));
          if (ret < 0)
            {
              goto out;
            }

          audio_format = robot_music_u16le(fmt);
          channels = robot_music_u16le(fmt + 2);
          sample_rate = robot_music_u32le(fmt + 4);
          bits_per_sample = robot_music_u16le(fmt + 14);
          fmt_found = true;

          remaining = chunk_size - sizeof(fmt);
          ret = robot_music_skip(fd, remaining);
          if (ret < 0)
            {
              goto out;
            }
        }
      else
        {
          if (memcmp(chunk, "data", 4) == 0 && data_offset < 0)
            {
              data_offset = chunk_data_offset;
              data_bytes = chunk_size;
            }

          ret = robot_music_skip(fd, chunk_size);
          if (ret < 0)
            {
              goto out;
            }
        }

      if ((chunk_size & 1u) != 0)
        {
          ret = robot_music_skip(fd, 1);
          if (ret < 0)
            {
              goto out;
            }
        }
    }

  if (!fmt_found || data_offset < 0 || data_bytes == 0)
    {
      printf("[ROBOT-MUSIC] missing fmt/data chunk fmt=%d data_bytes=%lu\n",
             fmt_found ? 1 : 0, (unsigned long)data_bytes);
      ret = -EINVAL;
      goto out;
    }

  printf("[ROBOT-MUSIC] PCM format=%u channels=%u rate=%lu bits=%u "
         "data_bytes=%lu\n",
         audio_format, channels, (unsigned long)sample_rate,
         bits_per_sample, (unsigned long)data_bytes);

  if (audio_format != 1 ||
      channels != ROBOT_MUSIC_CHANNELS ||
      sample_rate != ROBOT_MUSIC_SAMPLE_RATE ||
      bits_per_sample != ROBOT_MUSIC_BITS)
    {
      printf("[ROBOT-MUSIC] unsupported audio; need PCM16LE/24000Hz/mono\n");
      ret = -ENOTSUP;
      goto out;
    }

  if ((data_bytes & 1u) != 0)
    {
      printf("[ROBOT-MUSIC] invalid odd PCM data size=%lu\n",
             (unsigned long)data_bytes);
      ret = -EINVAL;
      goto out;
    }

  if (lseek(fd, data_offset, SEEK_SET) < 0)
    {
      ret = -errno;
      goto out;
    }

  ret = robot_audio_playback_open();
  if (ret < 0)
    {
      printf("[ROBOT-MUSIC] audio playback busy/open failed rc=%d\n", ret);
      goto out;
    }

  playback_opened = true;

  /* Attach a low-priority animated music face only after the real audio path
   * has opened successfully. Higher-priority voice/reaction/system faces can
   * override it, and it automatically becomes visible again afterwards. */
  if (robot_expression_set(ROBOT_EXPRESSION_SOURCE_MUSIC,
                           ROBOT_EXPRESSION_SINGING, 0) == 0)
    {
      music_expression_set = true;
    }

  printf("[ROBOT-MUSIC] playback start via robot_audio_playback\n");

  {
    uint32_t remaining = data_bytes;

    while (remaining > 0)
      {
        size_t want;
        ssize_t nread;

        if (robot_music_stop_requested())
          {
            printf("[ROBOT-MUSIC] stop observed before next PCM chunk\n");
            break;
          }

        want = remaining < sizeof(pcm) ? (size_t)remaining : sizeof(pcm);
        nread = read(fd, pcm, want);

        if (nread < 0)
          {
            if (errno == EINTR)
              {
                continue;
              }

            ret = -errno;
            printf("[ROBOT-MUSIC] PCM read failed rc=%d\n", ret);
            break;
          }

        if (nread == 0)
          {
            ret = -EIO;
            printf("[ROBOT-MUSIC] unexpected EOF remaining=%lu\n",
                   (unsigned long)remaining);
            break;
          }

        if (((size_t)nread & 1u) != 0)
          {
            ret = -EIO;
            printf("[ROBOT-MUSIC] odd PCM read bytes=%ld\n", (long)nread);
            break;
          }

        robot_music_scale_pcm16_half(pcm, (size_t)nread);
        ret = robot_audio_playback_write(pcm, (size_t)nread);
        if (ret < 0)
          {
            printf("[ROBOT-MUSIC] playback write failed rc=%d\n", ret);
            break;
          }

        remaining -= (uint32_t)nread;
      }
  }

  if (ret == 0 && !robot_music_stop_requested())
    {
      ret = robot_audio_playback_finish();
      if (ret < 0)
        {
          printf("[ROBOT-MUSIC] playback finish failed rc=%d\n", ret);
        }
    }
  else if (robot_music_stop_requested())
    {
      /* Immediate user stop: close playback without draining queued music. */
      printf("[ROBOT-MUSIC] playback stopped by request\n");
      ret = 0;
    }

  printf("[ROBOT-MUSIC] playback done rc=%d\n", ret);

out:
  if (music_expression_set)
    {
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_MUSIC);
    }

  if (playback_opened)
    {
      robot_audio_playback_close();
    }

  if (fd >= 0)
    {
      close(fd);
    }

  return ret;
}

bool robot_music_is_playing(void)
{
  bool active;

  pthread_mutex_lock(&g_robot_music_lock);
  active = g_robot_music_active;
  pthread_mutex_unlock(&g_robot_music_lock);
  return active;
}

static void *robot_music_worker(void *arg)
{
  struct robot_music_job_s *job = arg;
  int ret;

  if (job == NULL)
    {
      return NULL;
    }

  printf("[ROBOT-MUSIC] worker start path=%s\n", job->path);
  ret = robot_music_play_file(job->path);

  pthread_mutex_lock(&g_robot_music_lock);
  g_robot_music_last_result = ret;
  g_robot_music_active = false;
  g_robot_music_stop_requested = false;
  g_robot_music_path[0] = '\0';
  pthread_cond_broadcast(&g_robot_music_cond);
  pthread_mutex_unlock(&g_robot_music_lock);

  printf("[ROBOT-MUSIC] worker done rc=%d\n", ret);
  free(job);
  return NULL;
}

int robot_music_stop(void)
{
  bool local_active;
  int media_ret = 0;
  int wait_loops = 0;

  pthread_mutex_lock(&g_robot_music_lock);
  local_active = g_robot_music_active;
  if (local_active)
    {
      g_robot_music_stop_requested = true;
      printf("[ROBOT-MUSIC] stop requested path=%s\n", g_robot_music_path);
    }
  pthread_mutex_unlock(&g_robot_music_lock);

  /* Stop official/online Media playback too, when that compatibility layer
   * is linked. This makes one voice command stop either music backend. */
  if (robot_media_stop_active != NULL)
    {
      media_ret = robot_media_stop_active();
    }

  /* Wait briefly until local worker closes robot_audio_playback, otherwise
   * immediate confirmation TTS can race the still-owned speaker device. */
  while (local_active)
    {
      pthread_mutex_lock(&g_robot_music_lock);
      local_active = g_robot_music_active;
      pthread_mutex_unlock(&g_robot_music_lock);

      if (!local_active)
        {
          break;
        }

      if (++wait_loops >= 100) /* 100 * 20 ms = 2 s */
        {
          printf("[ROBOT-MUSIC] stop wait timeout; worker still active\n");
          return -ETIMEDOUT;
        }

      usleep(20 * 1000);
    }

  printf("[ROBOT-MUSIC] stop complete media_rc=%d\n", media_ret);
  return media_ret < 0 ? media_ret : 0;
}

static char *robot_music_tool_get_tools(void)
{
  cJSON *tools = cJSON_CreateArray();
  cJSON *play = NULL;
  cJSON *stop = NULL;
  char *json;

  if (tools == NULL)
    {
      return NULL;
    }

  play = cJSON_Parse(
    "{\"name\":\"robot_play_music\","
    "\"description\":\"Play local bundled WAV music on this robot.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"default\":\"/etc/media/test.wav\"}},"
    "\"required\":[]}}");

  stop = cJSON_Parse(
    "{\"name\":\"robot_stop_music\","
    "\"description\":\"Stop any currently playing robot music immediately.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}");

  if (play == NULL || stop == NULL)
    {
      cJSON_Delete(play);
      cJSON_Delete(stop);
      cJSON_Delete(tools);
      return NULL;
    }

  cJSON_AddItemToArray(tools, play);
  cJSON_AddItemToArray(tools, stop);

  json = cJSON_PrintUnformatted(tools);
  cJSON_Delete(tools);
  return json;
}

static int robot_music_tool_execute(const char *name,
                                    const char *input_json,
                                    char *output,
                                    size_t output_size)
{
  struct robot_music_job_s *job = NULL;
  cJSON *root = NULL;
  cJSON *path_json;
  const char *path = ROBOT_MUSIC_DEFAULT_PATH;
  pthread_attr_t attr;
  pthread_t thread;
  int fd;
  int ret;

  if (name == NULL)
    {
      return ERROR;
    }

  if (output == NULL || output_size == 0)
    {
      return ERROR;
    }

  if (strcmp(name, ROBOT_MUSIC_STOP_TOOL_NAME) == 0)
    {
      ret = robot_music_stop();
      snprintf(output, output_size,
               ret == 0 ?
               "{\"ok\":true,\"state\":\"STOPPED\"}" :
               "{\"ok\":false,\"state\":\"STOP_ERROR\",\"rc\":%d}",
               ret);
      return ret == 0 ? OK : ERROR;
    }

  if (strcmp(name, ROBOT_MUSIC_TOOL_NAME) != 0)
    {
      return ERROR;
    }

  if (input_json != NULL && input_json[0] != '\0')
    {
      root = cJSON_Parse(input_json);
      if (root == NULL)
        {
          snprintf(output, output_size,
                   "{\"ok\":false,\"error\":\"invalid JSON\"}");
          return ERROR;
        }

      path_json = cJSON_GetObjectItemCaseSensitive(root, "path");
      if (path_json != NULL)
        {
          const char *value = cJSON_GetStringValue(path_json);

          if (value == NULL || value[0] == '\0')
            {
              cJSON_Delete(root);
              snprintf(output, output_size,
                       "{\"ok\":false,\"error\":\"path must be a string\"}");
              return ERROR;
            }

          path = value;
        }
    }

  if (path[0] != '/' || strstr(path, "://") != NULL)
    {
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"only absolute local WAV paths are supported\"}");
      return ERROR;
    }

  if (strlen(path) >= ROBOT_MUSIC_PATH_MAX)
    {
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"path too long\"}");
      return ERROR;
    }

  /* Fail fast before starting a detached worker.  The WAV format itself is
   * validated by robot_music_play_file() in the worker. */
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      ret = -errno;
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"open failed\",\"rc\":%d}", ret);
      return ERROR;
    }
  close(fd);

  pthread_mutex_lock(&g_robot_music_lock);
  if (g_robot_music_active)
    {
      char current[ROBOT_MUSIC_PATH_MAX];

      snprintf(current, sizeof(current), "%s", g_robot_music_path);
      pthread_mutex_unlock(&g_robot_music_lock);
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"music already playing\","
               "\"path\":\"%s\"}", current);
      return ERROR;
    }

  job = calloc(1, sizeof(*job));
  if (job == NULL)
    {
      pthread_mutex_unlock(&g_robot_music_lock);
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"out of memory\"}");
      return ERROR;
    }

  snprintf(job->path, sizeof(job->path), "%s", path);
  snprintf(g_robot_music_path, sizeof(g_robot_music_path), "%s", path);
  g_robot_music_stop_requested = false;
  g_robot_music_active = true;
  g_robot_music_last_result = 0;
  pthread_mutex_unlock(&g_robot_music_lock);
  cJSON_Delete(root);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&attr, ROBOT_MUSIC_WORKER_STACK);
    }
  if (ret == 0)
    {
      ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }
  if (ret == 0)
    {
      ret = pthread_create(&thread, &attr, robot_music_worker, job);
    }
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      pthread_mutex_lock(&g_robot_music_lock);
      g_robot_music_active = false;
      g_robot_music_path[0] = '\0';
      g_robot_music_last_result = -ret;
      pthread_mutex_unlock(&g_robot_music_lock);
      free(job);

      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"worker start failed\",\"rc\":%d}",
               -ret);
      return ERROR;
    }

  snprintf(output, output_size,
           "{\"ok\":true,\"state\":\"PLAYING\",\"path\":\"%s\","
           "\"backend\":\"robot_audio_playback\"}", path);
  printf("[ROBOT-MUSIC] Tool started path=%s\n", path);
  return OK;
}

int robot_music_register_tool(void)
{
  if (g_robot_music_tool_registered)
    {
      return 0;
    }

  tool_registry_register_provider("contest_robot_music",
                                  robot_music_tool_get_tools,
                                  robot_music_tool_execute);

  /* Safe both before and after tool_registry_init(): mark the cached tools
   * JSON dirty, and let the normal registry path rebuild it on demand. */
  tool_registry_invalidate();
  g_robot_music_tool_registered = true;

  printf("[ROBOT-MUSIC] Tool registered name=%s default=%s\n",
         ROBOT_MUSIC_TOOL_NAME, ROBOT_MUSIC_DEFAULT_PATH);
  return 0;
}

