/*
 * Contest-local synchronization of generated Skill resources.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <agent_config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "contest_skill_resource.h"
#include "robot_skill_installer.h"

#define ROBOT_SKILL_NAME "robot-proactive-idle.md"
#define ROBOT_SKILL_PATH_SIZE 192
#define ROBOT_SKILL_TMP_SIZE 224

static pthread_mutex_t g_robot_skill_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_robot_skill_exists;
static bool g_robot_skill_matches;
static int g_robot_skill_last_result = -EAGAIN;
static size_t g_skill_current;
static size_t g_skill_created;
static size_t g_skill_updated;
static size_t g_skill_failed;

static int robot_skill_mkdir_p(const char *directory)
{
  char path[ROBOT_SKILL_PATH_SIZE];
  char *cursor;
  int ret;

  if (directory == NULL || strlen(directory) >= sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  strcpy(path, directory);
  while (strlen(path) > 1 && path[strlen(path) - 1] == '/')
    {
      path[strlen(path) - 1] = '\0';
    }

  for (cursor = path + 1; *cursor != '\0'; cursor++)
    {
      if (*cursor != '/')
        {
          continue;
        }
      *cursor = '\0';
      ret = mkdir(path, 0755);
      *cursor = '/';
      if (ret != 0 && errno != EEXIST)
        {
          int saved_errno = errno;
          printf("[ROBOT-SKILL] dir=%s stage=mkdir errno=%d\n", path,
                 saved_errno);
          return -saved_errno;
        }
    }

  ret = mkdir(path, 0755);
  if (ret != 0 && errno != EEXIST)
    {
      int saved_errno = errno;
      printf("[ROBOT-SKILL] dir=%s stage=mkdir errno=%d\n", path,
             saved_errno);
      return -saved_errno;
    }
  return 0;
}

static int robot_skill_make_paths(
  const struct contest_skill_resource_s *resource, char *path,
  size_t path_size, char *tmp, size_t tmp_size)
{
  int n;

  if (resource == NULL || resource->filename == NULL ||
      strchr(resource->filename, '/') != NULL)
    {
      return -EINVAL;
    }

  n = snprintf(path, path_size, "%s%s", AGENT_SKILLS_DIR,
               resource->filename);
  if (n < 0 || (size_t)n >= path_size)
    {
      return -ENAMETOOLONG;
    }

  n = snprintf(tmp, tmp_size, "%s%s.tmp", AGENT_SKILLS_DIR,
               resource->filename);
  if (n < 0 || (size_t)n >= tmp_size)
    {
      return -ENAMETOOLONG;
    }
  return 0;
}

static bool robot_skill_matches(
  const struct contest_skill_resource_s *resource, const char *path)
{
  FILE *file;
  unsigned char buffer[128];
  size_t offset = 0;

  file = fopen(path, "rb");
  if (file == NULL)
    {
      printf("[ROBOT-SKILL] name=%s stage=open-target errno=%d\n",
             resource->filename, errno);
      return false;
    }

  while (offset < resource->size)
    {
      size_t want = resource->size - offset;
      size_t got;

      if (want > sizeof(buffer))
        {
          want = sizeof(buffer);
        }
      got = fread(buffer, 1, want, file);
      if (got != want)
        {
          printf("[ROBOT-SKILL] name=%s stage=read-existing errno=%d\n",
                 resource->filename, errno != 0 ? errno : EIO);
          fclose(file);
          return false;
        }
      if (memcmp(buffer, resource->content + offset, got) != 0)
        {
          fclose(file);
          return false;
        }
      offset += got;
    }

  if (fgetc(file) != EOF || ferror(file) != 0)
    {
      printf("[ROBOT-SKILL] name=%s stage=read-existing errno=%d\n",
             resource->filename, errno != 0 ? errno : EIO);
      fclose(file);
      return false;
    }

  if (fclose(file) != 0)
    {
      printf("[ROBOT-SKILL] name=%s stage=close errno=%d\n",
             resource->filename, errno != 0 ? errno : EIO);
      return false;
    }
  return true;
}

static int robot_skill_write_atomic(
  const struct contest_skill_resource_s *resource, const char *path,
  const char *tmp)
{
  FILE *file;
  size_t written;
  int saved_errno;

  file = fopen(tmp, "wb");
  if (file == NULL)
    {
      saved_errno = errno;
      printf("[ROBOT-SKILL] name=%s stage=open-temp errno=%d\n",
             resource->filename, saved_errno);
      return -saved_errno;
    }

  written = fwrite(resource->content, 1, resource->size, file);
  if (written != resource->size)
    {
      saved_errno = errno != 0 ? errno : EIO;
      printf("[ROBOT-SKILL] name=%s stage=write errno=%d\n",
             resource->filename, saved_errno);
      fclose(file);
      unlink(tmp);
      return -saved_errno;
    }

  if (fflush(file) != 0)
    {
      saved_errno = errno != 0 ? errno : EIO;
      printf("[ROBOT-SKILL] name=%s stage=fflush errno=%d\n",
             resource->filename, saved_errno);
      fclose(file);
      unlink(tmp);
      return -saved_errno;
    }

  if (fclose(file) != 0)
    {
      saved_errno = errno != 0 ? errno : EIO;
      printf("[ROBOT-SKILL] name=%s stage=close errno=%d\n",
             resource->filename, saved_errno);
      unlink(tmp);
      return -saved_errno;
    }

  if (rename(tmp, path) != 0)
    {
      saved_errno = errno != 0 ? errno : EIO;
      printf("[ROBOT-SKILL] name=%s stage=rename errno=%d\n",
             resource->filename, saved_errno);
      unlink(tmp);
      return -saved_errno;
    }
  return 0;
}

static int robot_skill_sync_one(
  const struct contest_skill_resource_s *resource, size_t *current,
  size_t *created, size_t *updated)
{
  char path[ROBOT_SKILL_PATH_SIZE];
  char tmp[ROBOT_SKILL_TMP_SIZE];
  struct stat st;
  bool existed;
  int ret;

  ret = robot_skill_make_paths(resource, path, sizeof(path), tmp,
                               sizeof(tmp));
  if (ret < 0)
    {
      printf("[ROBOT-SKILL] name=%s stage=check errno=%d\n",
             resource->filename != NULL ? resource->filename : "(null)",
             -ret);
      return ret;
    }

  existed = stat(path, &st) == 0;
  printf("[ROBOT-SKILL] name=%s stage=stat-target result=%s\n",
         resource->filename, existed ? "exists" : "missing");
  if (existed && robot_skill_matches(resource, path))
    {
      (*current)++;
      return 0;
    }

  ret = robot_skill_write_atomic(resource, path, tmp);
  if (ret < 0)
    {
      return ret;
    }

  if (existed)
    {
      (*updated)++;
    }
  else
    {
      (*created)++;
    }
  return 0;
}

int robot_skill_installer_ensure(void)
{
  size_t i;
  int ret;
  int first_error = 0;

  pthread_mutex_lock(&g_robot_skill_lock);
  g_skill_current = 0;
  g_skill_created = 0;
  g_skill_updated = 0;
  g_skill_failed = 0;
  g_robot_skill_exists = false;
  g_robot_skill_matches = false;

  ret = robot_skill_mkdir_p(AGENT_SKILLS_DIR);
  if (ret < 0)
    {
      g_robot_skill_last_result = ret;
      pthread_mutex_unlock(&g_robot_skill_lock);
      return ret;
    }
  printf("[ROBOT-SKILL] dir=%s stage=check-dir result=ready\n",
         AGENT_SKILLS_DIR);

  for (i = 0; i < contest_skill_resource_count; i++)
    {
      ret = robot_skill_sync_one(&contest_skill_resources[i],
                                 &g_skill_current, &g_skill_created,
                                 &g_skill_updated);
      if (ret < 0)
        {
          g_skill_failed++;
          if (first_error == 0)
            {
              first_error = ret;
            }
        }
    }

  {
    char proactive_path[ROBOT_SKILL_PATH_SIZE];
    struct stat st;
    snprintf(proactive_path, sizeof(proactive_path), "%s%s", AGENT_SKILLS_DIR,
             ROBOT_SKILL_NAME);
    g_robot_skill_exists = stat(proactive_path, &st) == 0;
    for (i = 0; i < contest_skill_resource_count; i++)
      {
        if (strcmp(contest_skill_resources[i].filename,
                   ROBOT_SKILL_NAME) == 0)
          {
            g_robot_skill_matches =
              g_robot_skill_exists &&
              robot_skill_matches(&contest_skill_resources[i], proactive_path);
            break;
          }
      }
  }

  g_robot_skill_last_result = first_error != 0 ? first_error :
    (g_skill_created != 0 ? ROBOT_SKILL_INSTALL_CREATED :
     (g_skill_updated != 0 ? ROBOT_SKILL_INSTALL_UPDATED :
      ROBOT_SKILL_INSTALL_ALREADY_CURRENT));
  printf("[ROBOT-SKILL] sync total=%zu current=%zu created=%zu "
         "updated=%zu failed=%zu\n",
         contest_skill_resource_count, g_skill_current, g_skill_created,
         g_skill_updated, g_skill_failed);
  pthread_mutex_unlock(&g_robot_skill_lock);
  return first_error;
}

size_t robot_skill_installer_count(void)
{
  size_t value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = contest_skill_resource_count;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

size_t robot_skill_installer_current_count(void)
{
  size_t value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = g_skill_current;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

size_t robot_skill_installer_created_count(void)
{
  size_t value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = g_skill_created;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

size_t robot_skill_installer_updated_count(void)
{
  size_t value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = g_skill_updated;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

size_t robot_skill_installer_failed_count(void)
{
  size_t value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = g_skill_failed;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

const char *robot_skill_installer_path(void)
{
  return AGENT_SKILLS_DIR ROBOT_SKILL_NAME;
}

bool robot_skill_installer_exists(void)
{
  bool value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = g_robot_skill_exists;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

bool robot_skill_installer_matches(void)
{
  bool value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = g_robot_skill_matches;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

int robot_skill_installer_last_result(void)
{
  int value;

  pthread_mutex_lock(&g_robot_skill_lock);
  value = g_robot_skill_last_result;
  pthread_mutex_unlock(&g_robot_skill_lock);
  return value;
}

const char *robot_skill_installer_result_name(int result)
{
  switch (result)
    {
      case ROBOT_SKILL_INSTALL_ALREADY_CURRENT:
        return "already-current";
      case ROBOT_SKILL_INSTALL_CREATED:
        return "created";
      case ROBOT_SKILL_INSTALL_UPDATED:
        return "updated";
      default:
        return "failed";
    }
}
