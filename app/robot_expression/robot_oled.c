/*
 * Contest-local OLED queue and single hardware worker.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <arch/board/board.h>
#include <nuttx/lcd/lcd.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "robot_oled.h"
#include "robot_oled_expr.h"

/*
 * These are intentionally strong references.
 *
 * board_oled.c is contest-local and always provides these symbols, including
 * the -ENODEV stubs when the required SSD1306 Kconfig options are disabled.
 * Keeping these references weak can allow a static-library linker to leave the
 * board OLED object out of the final image, making both function pointers look
 * NULL at runtime even though board_oled.c exists in the source tree.
 */

extern int board_oled_initialize(void);
extern struct lcd_dev_s *board_oled_getdev(void);

#define ROBOT_OLED_QUEUE_DEPTH        8
#define ROBOT_OLED_STACK              8192
#define ROBOT_OLED_ANIMATION_TICK_MS  200

/*
 * The panel bring-up test has already passed.  Keep the destructive ALL ON /
 * ALL OFF startup sweep disabled during normal ai_agent operation so OLED
 * traffic does not overlap Wi-Fi/network bring-up.  Re-enable temporarily
 * only when diagnosing the physical display path.
 */

#define ROBOT_OLED_STARTUP_SELFTEST   0
#define ROBOT_OLED_SELFTEST_DELAY_US  180000

struct robot_oled_queue_s
{
  pthread_mutex_t lock;
  pthread_cond_t available;
  enum robot_expression_e slots[ROBOT_OLED_QUEUE_DEPTH];
  unsigned int head;
  unsigned int tail;
  unsigned int count;
};

static struct robot_oled_queue_s g_oled_queue =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .available = PTHREAD_COND_INITIALIZER,
};

static struct lcd_dev_s *g_oled_dev;
static struct lcd_planeinfo_s g_oled_plane;
static pthread_t g_oled_thread;
static bool g_oled_initialized;
static bool g_oled_thread_valid;
static bool g_oled_ready;
static bool g_oled_failed;
static bool g_oled_stop;

static void robot_oled_mark_failed(const char *stage, int ret)
{
  pthread_mutex_lock(&g_oled_queue.lock);
  g_oled_failed = true;
  g_oled_ready = false;
  pthread_cond_broadcast(&g_oled_queue.available);
  pthread_mutex_unlock(&g_oled_queue.lock);

  printf("[RV-OLED] FAIL stage=%s rc=%d\n", stage, ret);
}

static int robot_oled_flush_frame(
    const uint8_t frame[ROBOT_OLED_HEIGHT][ROBOT_OLED_ROW_BYTES])
{
  unsigned int row;

  if (g_oled_dev == NULL || g_oled_plane.putrun == NULL)
    {
      return -ENODEV;
    }

  for (row = 0; row < ROBOT_OLED_HEIGHT; row++)
    {
      int ret = g_oled_plane.putrun(g_oled_dev, row, 0, frame[row],
                                    ROBOT_OLED_WIDTH);
      if (ret < 0)
        {
          printf("[RV-OLED] putrun failed row=%u rc=%d\n", row, ret);
          return ret;
        }
    }

  return 0;
}

static int robot_oled_fill(bool on)
{
  uint8_t frame[ROBOT_OLED_HEIGHT][ROBOT_OLED_ROW_BYTES];

  memset(frame, on ? 0xff : 0x00, sizeof(frame));
  return robot_oled_flush_frame(frame);
}

static int robot_oled_render(enum robot_expression_e expression,
                             unsigned int animation_frame)
{
  uint8_t frame[ROBOT_OLED_HEIGHT][ROBOT_OLED_ROW_BYTES];

  memset(frame, 0, sizeof(frame));
  robot_oled_render_expression_frame(expression, animation_frame, frame);
  return robot_oled_flush_frame(frame);
}

static void robot_oled_deadline_after_ms(struct timespec *deadline,
                                         unsigned int delay_ms)
{
  clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += (time_t)(delay_ms / 1000u);
  deadline->tv_nsec += (long)(delay_ms % 1000u) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L)
    {
      deadline->tv_sec++;
      deadline->tv_nsec -= 1000000000L;
    }
}

static void *robot_oled_worker(void *arg)
{
  enum robot_expression_e current_expression = ROBOT_EXPRESSION_IDLE;
  unsigned int animation_tick = 0;
  unsigned int current_frame = 0;
  bool have_current = false;
  int ret;

  (void)arg;

  printf("[RV-OLED] init begin\n");

  ret = board_oled_initialize();
  printf("[RV-OLED] board init rc=%d\n", ret);
  if (ret < 0)
    {
      robot_oled_mark_failed("board_init", ret);
      return NULL;
    }

  g_oled_dev = board_oled_getdev();
  printf("[RV-OLED] board getdev dev=%p\n", (void *)g_oled_dev);
  if (g_oled_dev == NULL)
    {
      robot_oled_mark_failed("getdev", -ENODEV);
      return NULL;
    }

  if (g_oled_dev->getplaneinfo == NULL)
    {
      robot_oled_mark_failed("getplaneinfo_missing", -ENOSYS);
      return NULL;
    }

  memset(&g_oled_plane, 0, sizeof(g_oled_plane));
  ret = g_oled_dev->getplaneinfo(g_oled_dev, 0, &g_oled_plane);
  printf("[RV-OLED] getplaneinfo rc=%d\n", ret);
  if (ret < 0)
    {
      robot_oled_mark_failed("getplaneinfo", ret);
      return NULL;
    }

  if (g_oled_plane.putrun == NULL)
    {
      robot_oled_mark_failed("putrun_missing", -ENOSYS);
      return NULL;
    }

#if ROBOT_OLED_STARTUP_SELFTEST
  printf("[RV-OLED] selftest ALL_ON begin\n");
  ret = robot_oled_fill(true);
  printf("[RV-OLED] selftest ALL_ON rc=%d\n", ret);
  if (ret < 0)
    {
      robot_oled_mark_failed("selftest_all_on", ret);
      return NULL;
    }

  usleep(ROBOT_OLED_SELFTEST_DELAY_US);

  printf("[RV-OLED] selftest ALL_OFF begin\n");
  ret = robot_oled_fill(false);
  printf("[RV-OLED] selftest ALL_OFF rc=%d\n", ret);
  if (ret < 0)
    {
      robot_oled_mark_failed("selftest_all_off", ret);
      return NULL;
    }

  usleep(ROBOT_OLED_SELFTEST_DELAY_US);
#endif

  pthread_mutex_lock(&g_oled_queue.lock);
  g_oled_ready = true;
  pthread_cond_broadcast(&g_oled_queue.available);
  pthread_mutex_unlock(&g_oled_queue.lock);

  printf("[RV-OLED] worker ready %ux%u animation_tick_ms=%u\n",
         ROBOT_OLED_WIDTH, ROBOT_OLED_HEIGHT,
         (unsigned int)ROBOT_OLED_ANIMATION_TICK_MS);

  for (;;)
    {
      enum robot_expression_e expression = current_expression;
      unsigned int frame = current_frame;
      bool render_now = false;
      bool state_change = false;

      pthread_mutex_lock(&g_oled_queue.lock);
      while (g_oled_queue.count == 0 && !g_oled_stop)
        {
          struct timespec deadline;

          robot_oled_deadline_after_ms(&deadline,
                                       ROBOT_OLED_ANIMATION_TICK_MS);
          ret = pthread_cond_timedwait(&g_oled_queue.available,
                                       &g_oled_queue.lock, &deadline);
          if (ret == ETIMEDOUT)
            {
              break;
            }
        }

      if (g_oled_stop)
        {
          pthread_mutex_unlock(&g_oled_queue.lock);
          break;
        }

      if (g_oled_queue.count > 0)
        {
          expression = g_oled_queue.slots[g_oled_queue.head];
          g_oled_queue.head =
              (g_oled_queue.head + 1) % ROBOT_OLED_QUEUE_DEPTH;
          g_oled_queue.count--;
          state_change = !have_current || expression != current_expression;
          current_expression = expression;
          animation_tick = 0;
          frame = robot_oled_animation_frame(current_expression,
                                              animation_tick);
          current_frame = frame;
          have_current = true;
          render_now = true;
          printf("[RV-OLED] state expression=%d frame=%u pending=%u\n",
                 expression, frame, g_oled_queue.count);
        }
      else if (have_current)
        {
          unsigned int next_frame;

          animation_tick++;
          next_frame = robot_oled_animation_frame(current_expression,
                                                   animation_tick);
          if (next_frame != current_frame)
            {
              current_frame = next_frame;
              frame = next_frame;
              expression = current_expression;
              render_now = true;
            }
        }

      pthread_mutex_unlock(&g_oled_queue.lock);

      if (!render_now)
        {
          continue;
        }

      ret = robot_oled_render(expression, frame);
      if (ret < 0)
        {
          robot_oled_mark_failed("render", ret);
          break;
        }

      if (state_change)
        {
          printf("[RV-OLED] animation expression=%d started\n", expression);
        }
    }

  printf("[RV-OLED] worker exit\n");
  return NULL;
}

int robot_oled_init(void)
{
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_oled_queue.lock);
  if (g_oled_initialized)
    {
      ret = g_oled_failed ? -ENODEV : 0;
      pthread_mutex_unlock(&g_oled_queue.lock);
      return ret;
    }

  g_oled_initialized = true;
  g_oled_stop = false;
  g_oled_ready = false;
  g_oled_failed = false;
  g_oled_queue.head = 0;
  g_oled_queue.tail = 0;
  g_oled_queue.count = 0;
  pthread_mutex_unlock(&g_oled_queue.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&attr, ROBOT_OLED_STACK);
      if (ret == 0)
        {
          ret = pthread_create(&g_oled_thread, &attr, robot_oled_worker,
                               NULL);
        }

      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      pthread_mutex_lock(&g_oled_queue.lock);
      g_oled_failed = true;
      g_oled_initialized = false;
      pthread_mutex_unlock(&g_oled_queue.lock);
      printf("[RV-OLED] worker create failed rc=%d\n", ret);
      return -ret;
    }

  pthread_mutex_lock(&g_oled_queue.lock);
  g_oled_thread_valid = true;
  pthread_mutex_unlock(&g_oled_queue.lock);

  printf("[RV-OLED] worker created\n");
  return 0;
}

int robot_oled_stop(void)
{
  bool join_needed;

  pthread_mutex_lock(&g_oled_queue.lock);
  join_needed = g_oled_thread_valid;
  g_oled_stop = true;
  pthread_cond_broadcast(&g_oled_queue.available);
  pthread_mutex_unlock(&g_oled_queue.lock);

  if (join_needed)
    {
      pthread_join(g_oled_thread, NULL);
    }

  pthread_mutex_lock(&g_oled_queue.lock);
  g_oled_thread_valid = false;
  g_oled_initialized = false;
  g_oled_ready = false;
  g_oled_failed = false;
  g_oled_queue.head = 0;
  g_oled_queue.tail = 0;
  g_oled_queue.count = 0;
  pthread_mutex_unlock(&g_oled_queue.lock);

  g_oled_dev = NULL;
  memset(&g_oled_plane, 0, sizeof(g_oled_plane));
  return 0;
}

int robot_oled_submit(enum robot_expression_e expression)
{
  if (expression < 0 || expression >= ROBOT_EXPRESSION_COUNT)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_oled_queue.lock);

  if (!g_oled_initialized || g_oled_stop)
    {
      pthread_mutex_unlock(&g_oled_queue.lock);
      return -EAGAIN;
    }

  if (g_oled_failed)
    {
      pthread_mutex_unlock(&g_oled_queue.lock);
      return -ENODEV;
    }

  /*
   * Avoid piling up identical frames.  This also reduces I2C traffic when a
   * caller repeats the same state while the worker is still busy.
   */

  if (g_oled_queue.count > 0)
    {
      unsigned int last =
          (g_oled_queue.tail + ROBOT_OLED_QUEUE_DEPTH - 1) %
          ROBOT_OLED_QUEUE_DEPTH;

      if (g_oled_queue.slots[last] == expression)
        {
          pthread_mutex_unlock(&g_oled_queue.lock);
          return 0;
        }
    }

  if (g_oled_queue.count == ROBOT_OLED_QUEUE_DEPTH)
    {
      printf("[RV-OLED] queue full: dropping oldest expression=%d\n",
             g_oled_queue.slots[g_oled_queue.head]);
      g_oled_queue.head =
          (g_oled_queue.head + 1) % ROBOT_OLED_QUEUE_DEPTH;
      g_oled_queue.count--;
    }

  g_oled_queue.slots[g_oled_queue.tail] = expression;
  g_oled_queue.tail =
      (g_oled_queue.tail + 1) % ROBOT_OLED_QUEUE_DEPTH;
  g_oled_queue.count++;

  printf("[RV-OLED] enqueue expression=%d pending=%u ready=%d\n",
         expression, g_oled_queue.count, g_oled_ready ? 1 : 0);

  pthread_cond_signal(&g_oled_queue.available);
  pthread_mutex_unlock(&g_oled_queue.lock);
  return 0;
}

bool robot_oled_is_available(void)
{
  bool available;

  pthread_mutex_lock(&g_oled_queue.lock);
  available = g_oled_ready && !g_oled_failed;
  pthread_mutex_unlock(&g_oled_queue.lock);
  return available;
}
