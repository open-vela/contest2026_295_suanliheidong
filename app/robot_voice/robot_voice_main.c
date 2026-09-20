#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "agent_config.h"
#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "core/session_mgr.h"
#include "infra/config_store.h"
#include "infra/network_manager.h"
#include "mimo_asr.h"
#include "mimo_llm.h"
#include "mimo_tts.h"
#include "robot_voice_capture.h"
#include "robot_audio_playback.h"
#include "robot_music_player.h"
#include "robot_expression.h"
#include "robot_motion.h"
#include "robot_motion_tool.h"
#include "robot_voice_config.h"
#include "robot_proactive.h"
#include "robot_music_observer.h"
#include "robot_network_adapter.h"
#include "robot_ffmpeg_amix_compat.h"

#define ROBOT_MEDIA_STARTUP_GRACE_US       (1500 * 1000)

/*
 * Keep the board's configured capture window as the baseline, but give
 * voice_start three additional seconds before submitting the PCM to ASR.
 * This is intentionally contest-local so the official voice configuration
 * file does not need to be changed.
 */
#ifndef CONTEST_VOICE_CAPTURE_EXTRA_SECONDS
#  define CONTEST_VOICE_CAPTURE_EXTRA_SECONDS 3
#endif
#define CONTEST_VOICE_CAPTURE_SECONDS \
  (ROBOT_VOICE_CAPTURE_SECONDS + CONTEST_VOICE_CAPTURE_EXTRA_SECONDS)

static bool g_robot_media_initialized;
static pthread_mutex_t g_robot_media_init_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Media lifecycle policy
 * ----------------------
 *
 * Do not start mediad from voice_channel_init() and do not poll wlan0 while
 * set_wifi is associating / obtaining DHCP.
 *
 * Media follows the same lazy one-shot lifecycle style as the OLED: the first
 * eligible caller performs initialization, and all later callers reuse it.  In the normal vela> workflow:
 *
 *   set_wifi ...
 *     -> Wi-Fi/DHCP completes
 *   set_voice_asr ...   (or set_voice_tts ...)
 *     -> robot_media_runtime_init_once()
 *     -> mediad &
 *     -> IPC startup grace
 *   ask ...
 *
 * This gives Media time to become ready before the first ask, including the
 * official "播放" NL fast-path which can reach music_play before the normal
 * outbound "working" / OLED status is emitted.
 *
 * network_is_connected() is queried once at the trigger point.  There is no
 * background polling and contest code does not modify wlan0.
 */
static int robot_media_runtime_init_once(void)
{
  int ret;

  pthread_mutex_lock(&g_robot_media_init_lock);

  if (g_robot_media_initialized)
    {
      pthread_mutex_unlock(&g_robot_media_init_lock);
      return 0;
    }

  if (!network_is_connected())
    {
      printf("[RV-MEDIA] init deferred: WiFi is not connected yet\n");
      pthread_mutex_unlock(&g_robot_media_init_lock);
      return -ENETDOWN;
    }

  printf("[RV-MEDIA] init begin -> launching: mediad &\n");

  /*
   * On this NuttX/OpenVela shell, system("mediad &") may return -1 even when
   * the background mediad task was successfully created.  The runtime log
   * then shows "mediad [pid:prio]" and Media graph initialization.
   *
   * Therefore the system() return value is not a reliable launch-success
   * signal for a background NSH command.  Mark the one-shot launch as claimed
   * before calling system() so set_voice_tts cannot start a second daemon while
   * the first one is booting.
   */
  g_robot_media_initialized = true;
  ret = system("mediad &");
  if (ret != 0)
    {
      printf("[RV-MEDIA] background shell returned rc=%d; "
             "continuing because mediad may already be running\n", ret);
    }

  /*
   * Give the first daemon time to publish Media IPC before the first ask.
   */
  usleep(ROBOT_MEDIA_STARTUP_GRACE_US);

  printf("[RV-MEDIA] init complete; subsequent calls are no-op\n");
  pthread_mutex_unlock(&g_robot_media_init_lock);
  return 0;
}

#include "robot_skill_installer.h"
#include "robot_world_state.h"

/* Strong definitions live in the application entry object so the linker
 * cannot discard them while resolving ai_agent's weak simulation stubs. */
static char g_voice_asr_backend[32] = "none";
static char g_voice_tts_backend[32] = "none";
static pthread_mutex_t g_voice_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_voice_speak_lock = PTHREAD_MUTEX_INITIALIZER;

#define CONTEST_VOICE_RECORD_STACK  (16 * 1024)
#define CONTEST_VOICE_ASR_STACK     (24 * 1024)
#define CONTEST_CLI_TTS_STACK       (16 * 1024)
#define CONTEST_CLI_TTS_MOTION_WAIT_MS 15000

/* TTS software gain.
 * MiMo TTS returns signed 16-bit PCM. 1/2 amplitude is about -6.02 dB.
 * This changes spoken/TTS output only; music may use a separate playback path.
 */
#define ROBOT_VOICE_TTS_GAIN_NUM 1
#define ROBOT_VOICE_TTS_GAIN_DEN 2
#define ROBOT_VOICE_TTS_SCALE_CHUNK 512
static volatile int g_voice_running;
static volatile int g_voice_speaking;
static volatile int g_voice_oneshot;
static volatile int g_voice_thread_valid;
static volatile bool g_voice_reply_pending;
static volatile bool g_voice_stop_requested;
static volatile bool g_voice_worker_exited;
static pthread_t g_voice_thread;
static int robot_voice_pcm_sink(const uint8_t *pcm, size_t len, void *arg);
static int robot_voice_cli_tts_submit(const char *text);
int voice_channel_speak(const char *text);

/*
 * Demo latency policy:
 *   1 = deterministic single-action voice commands execute locally.
 *   0 = deterministic single-action voice commands still go through MiMo,
 *       but use a stateless short prompt on chat_id "voice_action".
 *
 * Complex / compound commands always go through the official ai_agent.
 */
#ifndef ROBOT_VOICE_LOCAL_ACTION_FAST_PATH
#  define ROBOT_VOICE_LOCAL_ACTION_FAST_PATH 1
#endif
static bool g_cli_tap_registered;
extern pthread_mutex_t g_stdout_lock;

/* The official Agent emits a CLI working-status message immediately before
 * each LLM request.  The contest app owns the outbound tap and mirrors the
 * official CLI formatting while using that message as the expression hook.
 */
static const char *const g_cli_working_phrases[] =
{
  "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x80\x9D\xE8\x80\x83\xE4\xB8\xAD\x2E\x2E\x2E",
  "\xE7\xA8\x8D\xE7\xAD\x89\xEF\xBC\x8C\xE5\xA4\x84\xE7\x90\x86\xE4\xB8\xAD\x2E\x2E\x2E",
  "\xE8\xAE\xA9\xE6\x88\x91\xE6\x9F\xA5\xE4\xB8\x80\xE4\xB8\x8B\xE2\x80\xA6",
  "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\x2E\x2E\x2E",
  "\xE9\xA9\xAC\xE4\xB8\x8A\xE5\xA5\xBD\x2E\x2E\x2E",
};

static bool robot_voice_is_cli_working_status(const char *text)
{
  unsigned int i;

  if (text == NULL)
    {
      return false;
    }

  for (i = 0; i < sizeof(g_cli_working_phrases) /
                  sizeof(g_cli_working_phrases[0]); i++)
    {
      if (strcmp(text, g_cli_working_phrases[i]) == 0)
        {
          return true;
        }
    }

  return false;
}

static void robot_voice_cli_outbound_tap(const agent_msg_t *msg,
                                         void *cookie)
{
  bool working;

  (void)cookie;
  if (msg == NULL || msg->content == NULL)
    {
      return;
    }

  working = robot_voice_is_cli_working_status(msg->content);
  if (working)
    {
      robot_world_state_set_conversation_active(true);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_AGENT,
                                  ROBOT_EXPRESSION_THINKING, 0);
      printf("[RV-EXPR] CLI working status -> thinking\n");
    }
  else
    {
      int tts_ret;

      robot_world_state_set_conversation_active(false);
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_AGENT);
      printf("[RV-EXPR] CLI final reply -> clear agent expression + queue TTS\n");

      /*
       * `ask` uses the CLI channel.  Speak only the final Agent reply, never
       * the short working-status messages above.  TTS is queued on a detached
       * worker so the message-bus outbound tap stays non-blocking.
       */
      tts_ret = robot_voice_cli_tts_submit(msg->content);
      if (tts_ret < 0)
        {
          printf("[CLI-TTS] queue failed rc=%d\n", tts_ret);
        }
    }

  /* mbus_tap consumes the message, so preserve the official CLI output. */
  pthread_mutex_lock(&g_stdout_lock);
  printf("\n[Agent]: %s\nvela> ", msg->content);
  fflush(stdout);
  pthread_mutex_unlock(&g_stdout_lock);
}

static unsigned long long robot_voice_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (unsigned long long)ts.tv_sec * 1000ULL +
         (unsigned long long)ts.tv_nsec / 1000000ULL;
}

enum contest_voice_state_e
{
  CONTEST_VOICE_IDLE = 0,
  CONTEST_VOICE_RECORDING,
  CONTEST_VOICE_ASR,
  CONTEST_VOICE_WAIT_AGENT,
  CONTEST_VOICE_SPEAKING,
  CONTEST_VOICE_STOPPING,
};

static volatile enum contest_voice_state_e g_voice_state = CONTEST_VOICE_IDLE;

static const char *contest_voice_state_name(enum contest_voice_state_e state)
{
  switch (state)
    {
      case CONTEST_VOICE_RECORDING:
        return "RECORDING";
      case CONTEST_VOICE_ASR:
        return "ASR";
      case CONTEST_VOICE_WAIT_AGENT:
        return "WAIT_AGENT";
      case CONTEST_VOICE_SPEAKING:
        return "SPEAKING";
      case CONTEST_VOICE_STOPPING:
        return "STOPPING";
      case CONTEST_VOICE_IDLE:
      default:
        return "IDLE";
    }
}

static void contest_voice_set_state(enum contest_voice_state_e state)
{
  enum contest_voice_state_e previous = g_voice_state;

  g_voice_state = state;
  if (previous != state)
    {
      printf("[contest_voice] state %s -> %s\n",
             contest_voice_state_name(previous), contest_voice_state_name(state));
    }
}

struct voice_file_sink_s
{
  int fd;
};

static int robot_voice_file_sink(const uint8_t *pcm, size_t len, void *arg)
{
  struct voice_file_sink_s *sink = arg;
  ssize_t written;

  if (sink == NULL || sink->fd < 0)
    {
      return -EINVAL;
    }

  written = write(sink->fd, pcm, len);
  if (written != (ssize_t)len)
    {
      return -EIO;
    }

  return robot_audio_playback_write(pcm, len);
}

static int voice_get_api_key(char *key, size_t cap)
{
  if (key == NULL || cap < 2 ||
      claw_config_get(AGENT_CFG_KEY_API_KEY, key, cap) != OK ||
      key[0] == '\0')
    {
      return -ENOENT;
    }
  return 0;
}

struct contest_asr_job_s
{
  const uint8_t *pcm;
  size_t pcm_len;
  int ret;
  char text[512];
};

static void *contest_voice_asr_worker(void *arg)
{
  struct contest_asr_job_s *job = arg;
  char key[256];

  if (job == NULL)
    {
      return NULL;
    }

  job->ret = voice_get_api_key(key, sizeof(key));
  if (job->ret < 0)
    {
      printf("[contest_voice] ASR skipped: configure set_llm first\n");
      return NULL;
    }

  job->text[0] = '\0';
  printf("[RV-ASR] worker start bytes=%zu stack=%u\n",
         job->pcm_len, (unsigned int)CONTEST_VOICE_ASR_STACK);
  job->ret = mimo_asr_recognize(key, job->pcm, job->pcm_len,
                                job->text, sizeof(job->text));
  memset(key, 0, sizeof(key));
  printf("[RV-ASR] worker done rc=%d\n", job->ret);
  return NULL;
}

static int contest_voice_run_asr(const uint8_t *pcm, size_t pcm_len,
                                 char *text, size_t text_cap)
{
  struct contest_asr_job_s *job;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (pcm == NULL || pcm_len == 0 || text == NULL || text_cap == 0)
    {
      return -EINVAL;
    }

  job = calloc(1, sizeof(*job));
  if (job == NULL)
    {
      return -ENOMEM;
    }

  job->pcm = pcm;
  job->pcm_len = pcm_len;

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      free(job);
      return -ret;
    }

  ret = pthread_attr_setstacksize(&attr, CONTEST_VOICE_ASR_STACK);
  if (ret == 0)
    {
      ret = pthread_create(&thread, &attr, contest_voice_asr_worker, job);
    }
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      printf("[contest_voice] failed to start ASR worker: %d\n", ret);
      free(job);
      return -ret;
    }

  ret = pthread_join(thread, NULL);
  if (ret != 0)
    {
      printf("[contest_voice] failed to join ASR worker: %d\n", ret);
      free(job);
      return -ret;
    }

  ret = job->ret;
  if (ret == 0)
    {
      strncpy(text, job->text, text_cap - 1);
      text[text_cap - 1] = '\0';
    }
  else
    {
      text[0] = '\0';
    }

  free(job);
  return ret;
}

static void contest_voice_wait_for_reply_cycle(void)
{
  /* Once ASR text has entered ai_agent, keep the microphone idle until the
   * outbound voice reply has gone through TTS.  voice_channel_speak() changes
   * WAIT_AGENT -> SPEAKING -> RECORDING, which releases this wait. */

  while (!g_voice_stop_requested)
    {
      if (!g_voice_speaking && g_voice_state == CONTEST_VOICE_RECORDING)
        {
          return;
        }

      usleep(20 * 1000);
    }
}


enum robot_voice_simple_action_e
{
  ROBOT_VOICE_ACTION_NONE = 0,
  ROBOT_VOICE_ACTION_TAIL_WAG,
  ROBOT_VOICE_ACTION_FORWARD,
  ROBOT_VOICE_ACTION_BACKWARD,
  ROBOT_VOICE_ACTION_LEFT,
  ROBOT_VOICE_ACTION_RIGHT,
  ROBOT_VOICE_ACTION_HAPPY,
};

static bool robot_voice_has_any(const char *text,
                                const char *const *keywords)
{
  unsigned int i;

  if (text == NULL || keywords == NULL)
    {
      return false;
    }

  for (i = 0; keywords[i] != NULL; i++)
    {
      if (strstr(text, keywords[i]) != NULL)
        {
          return true;
        }
    }

  return false;
}

/*
 * Only classify short, single-action commands here.
 * Compound requests intentionally fall back to MiMo so the Agent can plan.
 */
static enum robot_voice_simple_action_e
robot_voice_classify_simple_action(const char *text)
{
  static const char *const tail_kw[] =
  {
    "摇尾巴", "摇一摇尾巴", "摇摇尾巴", "摆尾巴", NULL
  };
  static const char *const forward_kw[] =
  {
    "向前走", "往前走", "前进一步", "前进", NULL
  };
  static const char *const backward_kw[] =
  {
    "向后走", "往后走", "后退一步", "后退", NULL
  };
  static const char *const left_kw[] =
  {
    "向左转", "往左转", "左转", NULL
  };
  static const char *const right_kw[] =
  {
    "向右转", "往右转", "右转", NULL
  };
  static const char *const happy_kw[] =
  {
    "开心一点", "开心表情", "做个开心", "笑一个", NULL
  };
  bool matched[6];
  int count = 0;
  int selected = -1;
  int i;

  if (text == NULL || text[0] == '\0' || strlen(text) > 96)
    {
      return ROBOT_VOICE_ACTION_NONE;
    }

  matched[0] = robot_voice_has_any(text, tail_kw);
  matched[1] = robot_voice_has_any(text, forward_kw);
  matched[2] = robot_voice_has_any(text, backward_kw);
  matched[3] = robot_voice_has_any(text, left_kw);
  matched[4] = robot_voice_has_any(text, right_kw);
  matched[5] = robot_voice_has_any(text, happy_kw);

  for (i = 0; i < 6; i++)
    {
      if (matched[i])
        {
          count++;
          selected = i;
        }
    }

  /* More than one action family = compound request -> MiMo. */
  if (count != 1)
    {
      return ROBOT_VOICE_ACTION_NONE;
    }

  return (enum robot_voice_simple_action_e)(selected + 1);
}

static const char *
robot_voice_action_mimo_prompt(enum robot_voice_simple_action_e action)
{
  switch (action)
    {
      case ROBOT_VOICE_ACTION_TAIL_WAG:
        return "只调用 robot_tail_wag，使用默认参数。";
      case ROBOT_VOICE_ACTION_FORWARD:
        return "只调用 robot_move，direction=forward，steps=1。";
      case ROBOT_VOICE_ACTION_BACKWARD:
        return "只调用 robot_move，direction=backward，steps=1。";
      case ROBOT_VOICE_ACTION_LEFT:
        return "只调用 robot_move，direction=left，steps=1。";
      case ROBOT_VOICE_ACTION_RIGHT:
        return "只调用 robot_move，direction=right，steps=1。";
      case ROBOT_VOICE_ACTION_HAPPY:
        return "只调用 robot_set_expression，expression=happy。";
      case ROBOT_VOICE_ACTION_NONE:
      default:
        return NULL;
    }
}

static int
robot_voice_execute_local_action(enum robot_voice_simple_action_e action)
{
  enum robot_motion_direction_e direction;

  switch (action)
    {
      case ROBOT_VOICE_ACTION_TAIL_WAG:
        return robot_motion_tail_wag_sync(
          ROBOT_MOTION_SOURCE_CLI,
          ROBOT_MOTION_TAIL_DEFAULT_CYCLES,
          ROBOT_MOTION_TAIL_DEFAULT_PERIOD,
          ROBOT_MOTION_TAIL_DEFAULT_AMPLITUDE);

      case ROBOT_VOICE_ACTION_FORWARD:
        direction = ROBOT_MOTION_FORWARD;
        break;
      case ROBOT_VOICE_ACTION_BACKWARD:
        direction = ROBOT_MOTION_BACKWARD;
        break;
      case ROBOT_VOICE_ACTION_LEFT:
        direction = ROBOT_MOTION_LEFT;
        break;
      case ROBOT_VOICE_ACTION_RIGHT:
        direction = ROBOT_MOTION_RIGHT;
        break;

      case ROBOT_VOICE_ACTION_HAPPY:
        return robot_expression_set(ROBOT_EXPRESSION_SOURCE_AGENT,
                                    ROBOT_EXPRESSION_HAPPY, 1800);

      case ROBOT_VOICE_ACTION_NONE:
      default:
        return -EINVAL;
    }

  return robot_motion_execute_sync(ROBOT_MOTION_SOURCE_CLI, direction, 1,
                                   robot_motion_default_period(direction));
}

#define ROBOT_VOICE_LOCAL_MUSIC_PATH "/etc/media/test.wav"

static bool robot_voice_rewrite_local_music_request(const char *text,
                                                     char *out,
                                                     size_t out_size)
{
  static const char *const exact_requests[] =
  {
    "播放音乐",
    "播放测试音乐",
    "播放本地音乐",
    "放音乐",
    "放一下音乐",
    "放点音乐",
    "放一首音乐",
    "来点音乐",
    "来首音乐",
    "给我放音乐",
    "给我放点音乐",
    "给我来点音乐",
    "给我来首音乐",
  };
  unsigned int i;
  bool explicit_local = false;

  if (text == NULL || out == NULL || out_size == 0)
    {
      return false;
    }

  if (strstr(text, "test.wav") != NULL ||
      strstr(text, "测试音乐") != NULL ||
      strstr(text, "本地音乐") != NULL)
    {
      explicit_local = true;
    }

  if (!explicit_local)
    {
      for (i = 0; i < sizeof(exact_requests) / sizeof(exact_requests[0]); i++)
        {
          if (strcmp(text, exact_requests[i]) == 0)
            {
              explicit_local = true;
              break;
            }
        }
    }

  if (!explicit_local)
    {
      return false;
    }

  /*
   * Important: do not include the Chinese fast-path trigger "播放" or the
   * token "play " (with a trailing space) in the rewritten Agent message.
   * The official agent_loop checks those before the LLM.  "robot_play_music"
   * contains "play_" and therefore does not match that English trigger.
   */
  snprintf(out, out_size,
           "请调用 robot_play_music，参数 path 设为 %s",
           ROBOT_VOICE_LOCAL_MUSIC_PATH);
  return true;
}

static void *voice_record_worker(void *arg)
{
  size_t pcm_cap = (size_t)ROBOT_VOICE_CAPTURE_RATE * 2 *
                   CONTEST_VOICE_CAPTURE_SECONDS;
  uint8_t *pcm = malloc(pcm_cap);
  char text[512];
  char routed_text[512];
  (void)arg;

  if (pcm == NULL)
    {
      printf("[contest_voice] worker cleanup: pcm allocation failed\n");
      goto cleanup;
    }

  while (!g_voice_stop_requested)
    {
      size_t pcm_len = 0;
      int ret;

      contest_voice_set_state(CONTEST_VOICE_RECORDING);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                                  ROBOT_EXPRESSION_LISTENING, 0);
      ret = robot_voice_capture_record_interruptible(
          pcm, pcm_cap, &pcm_len, CONTEST_VOICE_CAPTURE_SECONDS * 1000,
          &g_voice_stop_requested);
      printf("[RV-VOICE] capture returned rc=%d bytes=%zu\n", ret, pcm_len);
      if (ret < 0 || g_voice_stop_requested)
        {
          break;
        }
      if (g_voice_speaking)
        {
          continue;
        }
      contest_voice_set_state(CONTEST_VOICE_ASR);
      printf("[RV-VOICE] invoking ASR bytes=%zu on dedicated worker\n", pcm_len);
      ret = contest_voice_run_asr(pcm, pcm_len, text, sizeof(text));
      if (g_voice_stop_requested)
        {
          break;
        }
      if (ret < 0 || text[0] == '\0')
        {
          (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
          (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                      ROBOT_EXPRESSION_ERROR, 1500);
          printf("[contest_voice] ASR failed: %d\n", ret);
          if (g_voice_oneshot)
            {
              break;
            }
          continue;
        }

      printf("[RV-ASR] text: %s\n", text);
      robot_world_state_note_user_activity();
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                                  ROBOT_EXPRESSION_THINKING, 0);

      enum robot_voice_simple_action_e simple_action =
        robot_voice_classify_simple_action(text);

#if ROBOT_VOICE_LOCAL_ACTION_FAST_PATH
      if (simple_action != ROBOT_VOICE_ACTION_NONE)
        {
          unsigned long long fast_start = robot_voice_now_ms();
          int action_ret;

          printf("[RV-FAST] local action=%d begin\n", (int)simple_action);
          contest_voice_set_state(CONTEST_VOICE_WAIT_AGENT);
          robot_world_state_set_conversation_active(true);
          g_voice_reply_pending = true;

          action_ret = robot_voice_execute_local_action(simple_action);

          printf("[RV-FAST] local action=%d rc=%d latency_ms=%llu\n",
                 (int)simple_action, action_ret,
                 robot_voice_now_ms() - fast_start);

          if (action_ret == 0)
            {
              /* Motion has completed here, so TTS no longer conflicts with PWM. */
              (void)voice_channel_speak("好呀。");
            }
          else
            {
              g_voice_reply_pending = false;
              robot_world_state_set_conversation_active(false);
              (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
              (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                          ROBOT_EXPRESSION_ERROR, 1500);
              if (g_voice_running && !g_voice_stop_requested)
                {
                  contest_voice_set_state(CONTEST_VOICE_RECORDING);
                }
            }

          if (g_voice_oneshot)
            {
              break;
            }

          continue;
        }
#endif

      const char *agent_text = text;
      const char *agent_chat_id = "voice";

      /*
       * When local fast path is disabled, keep deterministic actions in MiMo
       * Tool Call, but do not attach the normal long voice conversation history.
       */
      if (simple_action != ROBOT_VOICE_ACTION_NONE)
        {
          const char *short_prompt =
            robot_voice_action_mimo_prompt(simple_action);

          if (short_prompt != NULL)
            {
              agent_text = short_prompt;
              agent_chat_id = "voice_action";
              (void)session_clear(agent_chat_id);
              printf("[RV-FAST] MiMo stateless action route: '%s' -> '%s'\n",
                     text, agent_text);
            }
        }
      else if (robot_voice_rewrite_local_music_request(text, routed_text,
                                                        sizeof(routed_text)))
        {
          agent_text = routed_text;
          printf("[RV-MUSIC-ROUTE] rewrite: '%s' -> '%s'\n",
                 text, agent_text);
        }

      agent_msg_t msg;
      memset(&msg, 0, sizeof(msg));
      strncpy(msg.channel, AGENT_CHAN_VOICE, sizeof(msg.channel) - 1);
      strncpy(msg.chat_id, agent_chat_id, sizeof(msg.chat_id) - 1);
      msg.content = strdup(agent_text);
      contest_voice_set_state(CONTEST_VOICE_WAIT_AGENT);
      robot_world_state_set_conversation_active(true);
      printf("[RV-VOICE] push inbound voice:%s\n", agent_chat_id);
      g_voice_reply_pending = true;
      if (msg.content == NULL || message_bus_push_inbound(&msg) != OK)
        {
          free(msg.content);
          g_voice_reply_pending = false;
          robot_world_state_set_conversation_active(false);
          (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
          (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                      ROBOT_EXPRESSION_ERROR, 1500);
          printf("[contest_voice] message bus submit failed\n");
          if (g_voice_oneshot)
            {
              break;
            }
          continue;
        }

      if (g_voice_oneshot)
        {
          break;
        }

      contest_voice_wait_for_reply_cycle();
    }

cleanup:
  free(pcm);
  if (!g_voice_reply_pending)
    {
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
    }
  pthread_mutex_lock(&g_voice_lock);
  g_voice_running = 0;
  g_voice_worker_exited = true;
  contest_voice_set_state(CONTEST_VOICE_IDLE);

  /*
   * Force the contest-local FFmpeg amix compatibility object into libapps.
   * The function itself is intentionally a no-op.
   */
  robot_ffmpeg_amix_compat_link_anchor();

  pthread_mutex_unlock(&g_voice_lock);
  printf("[contest_voice] worker cleanup\n");
  return NULL;
}

int voice_channel_init(void)
{
  g_voice_running = 0;
  g_voice_speaking = 0;
  g_voice_oneshot = 0;
  g_voice_thread_valid = 0;
  g_voice_reply_pending = false;
  g_voice_stop_requested = false;
  g_voice_worker_exited = true;
  contest_voice_set_state(CONTEST_VOICE_IDLE);

  /*
   * Wi-Fi ownership policy:
   * Do not start the contest-local RV-NET guard here.
   *
   * The official Agent/network manager is the single owner of wlan0.
   * The former guard could issue ifdown/ifup/renew while set_wifi was still
   * associating or obtaining DHCP, which could make netlib_obtain_ipv4addr()
   * fail and leave set_wifi unable to connect.
   */
  printf("[contest_voice] RV-NET active repair disabled; official network manager owns wlan0\n");

  if (robot_skill_installer_ensure() < 0)
    {
      printf("[contest_voice] proactive skill unavailable\n");
    }
  (void)robot_expression_register_tool();
  (void)robot_motion_register_tool();
  (void)robot_music_register_tool();
  if (!g_cli_tap_registered)
    {
      if (mbus_tap_register(AGENT_CHAN_CLI,
                            robot_voice_cli_outbound_tap, NULL) == OK)
        {
          g_cli_tap_registered = true;
          printf("[RV-EXPR] CLI outbound tap registered\n");
        }
      else
        {
          printf("[RV-EXPR] CLI outbound tap unavailable\n");
        }
    }
  /*
   * Intentionally no runtime/network bootstrap worker here.
   *
   * set_wifi runs with no contest-side wlan0 polling.  mediad is started later
   * by the first successful set_voice_asr / set_voice_tts call after Wi-Fi is
   * already connected.
   */
  printf("[contest_voice] no background Wi-Fi polling; Media starts during post-WiFi voice setup\n");
  printf("[contest_voice] voice_channel_init\n");
  return 0;
}

static int voice_channel_start_mode(int oneshot)
{
  pthread_mutex_lock(&g_voice_lock);
  if (g_voice_thread_valid)
    {
      pthread_mutex_unlock(&g_voice_lock);
      return -EBUSY;
    }
  g_voice_running = 1;
  g_voice_oneshot = oneshot;
  g_voice_reply_pending = false;
  g_voice_stop_requested = false;
  g_voice_worker_exited = false;
  contest_voice_set_state(CONTEST_VOICE_RECORDING);
  pthread_attr_t attr;
  int create_ret;

  create_ret = pthread_attr_init(&attr);
  if (create_ret == 0)
    {
      create_ret = pthread_attr_setstacksize(&attr, CONTEST_VOICE_RECORD_STACK);
      if (create_ret == 0)
        {
          create_ret = pthread_create(&g_voice_thread, &attr,
                                      voice_record_worker, NULL);
        }
      pthread_attr_destroy(&attr);
    }

  if (create_ret != 0)
    {
      g_voice_running = 0;
      g_voice_oneshot = 0;
      g_voice_worker_exited = true;
      contest_voice_set_state(CONTEST_VOICE_IDLE);
      pthread_mutex_unlock(&g_voice_lock);
      printf("[contest_voice] failed to start record worker: %d\n", create_ret);
      return -EAGAIN;
    }
  g_voice_thread_valid = 1;
  pthread_mutex_unlock(&g_voice_lock);
  printf("[contest_voice] voice_channel_start mode=%s\n",
         oneshot ? "once" : "continuous");
  return 0;
}

int voice_channel_start(void)
{
  if (strcmp(g_voice_asr_backend, "mimo-v2.5-asr") != 0)
    {
      return -ENOSYS;
    }
  return voice_channel_start_mode(0);
}

int voice_channel_stop(void)
{
  enum contest_voice_state_e state;
  bool join_needed = false;
  pthread_t tid = 0;

  pthread_mutex_lock(&g_voice_lock);
  if (!g_voice_thread_valid)
    {
      pthread_mutex_unlock(&g_voice_lock);
      return 0;
    }
  g_voice_stop_requested = true;
  g_voice_reply_pending = false;
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
  state = g_voice_state;
  contest_voice_set_state(CONTEST_VOICE_STOPPING);
  tid = g_voice_thread;
  join_needed = !pthread_equal(pthread_self(), tid);
  pthread_mutex_unlock(&g_voice_lock);

  printf("[contest_voice] stop requested state=%s\n",
         contest_voice_state_name(state));
  if (join_needed)
    {
      printf("[contest_voice] waiting capture callbacks and worker cleanup\n");
      pthread_join(tid, NULL);
    }

  pthread_mutex_lock(&g_voice_lock);
  if (g_voice_worker_exited)
    {
      g_voice_thread_valid = 0;
      g_voice_oneshot = 0;
    }
  pthread_mutex_unlock(&g_voice_lock);
  printf("[contest_voice] voice_channel_stop state=%s\n",
         contest_voice_state_name(g_voice_state));
  return 0;
}

int voice_channel_speak(const char *text)
{
  char key[256];
  int ret;
  if (!text || text[0] == '\0')
    {
      return -EINVAL;
    }
  if (strcmp(g_voice_tts_backend, "mimo-v2.5-tts") != 0)
    {
      g_voice_reply_pending = false;
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      return -ENOSYS;
    }

  pthread_mutex_lock(&g_voice_speak_lock);
  ret = voice_get_api_key(key, sizeof(key));
  if (ret < 0)
    {
      g_voice_reply_pending = false;
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      pthread_mutex_unlock(&g_voice_speak_lock);
      return ret;
    }
  if (robot_motion_is_busy())
    {
      g_voice_reply_pending = false;
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
      printf("[POWER-SAFE] mono_ms=%llu TTS_REJECT motion_active\n",
             robot_voice_now_ms());
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      pthread_mutex_unlock(&g_voice_speak_lock);
      return -EBUSY;
    }
  ret = robot_audio_playback_open();
  if (ret < 0)
    {
      g_voice_reply_pending = false;
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      pthread_mutex_unlock(&g_voice_speak_lock);
      return ret;
    }
  g_voice_speaking = 1;
  robot_world_state_set_tts_active(true);
  contest_voice_set_state(CONTEST_VOICE_SPEAKING);
  printf("[POWER-SAFE] mono_ms=%llu TTS_START\n", robot_voice_now_ms());
  (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                              ROBOT_EXPRESSION_SPEAKING, 0);
  printf("[contest_voice] voice_channel_speak text_bytes=%zu\n", strlen(text));
  ret = mimo_tts_speak_stream(key, text, robot_voice_pcm_sink, NULL);
  if (ret == 0)
    {
      ret = robot_audio_playback_finish();
    }
  robot_audio_playback_close();
  g_voice_speaking = 0;
  robot_world_state_set_tts_active(false);
  robot_world_state_set_conversation_active(false);
  g_voice_reply_pending = false;
  printf("[POWER-SAFE] mono_ms=%llu TTS_DONE rc=%d\n",
         robot_voice_now_ms(), ret);
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
  if (ret < 0)
    {
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
    }
  memset(key, 0, sizeof(key));
  if (g_voice_running && !g_voice_stop_requested)
    {
      contest_voice_set_state(CONTEST_VOICE_RECORDING);
    }
  pthread_mutex_unlock(&g_voice_speak_lock);
  return ret;
}


struct contest_cli_tts_job_s
{
  char *text;
};

static void *robot_voice_cli_tts_worker(void *arg)
{
  struct contest_cli_tts_job_s *job = arg;
  unsigned int waited_ms = 0;
  int ret;

  if (job == NULL)
    {
      return NULL;
    }

  if (job->text == NULL || job->text[0] == '\0')
    {
      free(job->text);
      free(job);
      return NULL;
    }

  /*
   * Servo motion and audio amplification are intentionally serialized on this
   * board.  A final Agent reply may arrive immediately after robot_tail_wag,
   * so wait for the motion layer to become idle instead of losing the spoken
   * reply to voice_channel_speak()'s -EBUSY power guard.
   */
  while (robot_motion_is_busy() &&
         waited_ms < CONTEST_CLI_TTS_MOTION_WAIT_MS)
    {
      if ((waited_ms % 1000) == 0)
        {
          printf("[CLI-TTS] waiting for motion to finish waited_ms=%u\n",
                 waited_ms);
        }

      usleep(100 * 1000);
      waited_ms += 100;
    }

  if (robot_motion_is_busy())
    {
      printf("[CLI-TTS] skipped: motion still busy after %u ms\n", waited_ms);
      free(job->text);
      free(job);
      return NULL;
    }

  printf("[CLI-TTS] speak begin bytes=%zu waited_ms=%u\n",
         strlen(job->text), waited_ms);

  ret = voice_channel_speak(job->text);

  printf("[CLI-TTS] speak done rc=%d\n", ret);

  free(job->text);
  free(job);
  return NULL;
}

static int robot_voice_cli_tts_submit(const char *text)
{
  struct contest_cli_tts_job_s *job;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  /*
   * Do not queue CLI speech before TTS has been configured.  This avoids
   * flashing an error expression on every `ask` during early setup.
   */
  if (strcmp(g_voice_tts_backend, "mimo-v2.5-tts") != 0)
    {
      printf("[CLI-TTS] skipped: configure set_voice_tts mimo-v2.5-tts first\n");
      return -ENOSYS;
    }

  job = calloc(1, sizeof(*job));
  if (job == NULL)
    {
      return -ENOMEM;
    }

  job->text = strdup(text);
  if (job->text == NULL)
    {
      free(job);
      return -ENOMEM;
    }

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      free(job->text);
      free(job);
      return -ret;
    }

  ret = pthread_attr_setstacksize(&attr, CONTEST_CLI_TTS_STACK);
  if (ret == 0)
    {
      ret = pthread_create(&thread, &attr,
                           robot_voice_cli_tts_worker, job);
    }

  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      free(job->text);
      free(job);
      return -ret;
    }

  pthread_detach(thread);
  printf("[CLI-TTS] queued bytes=%zu\n", strlen(text));
  return 0;
}

int voice_channel_test_tts(const char *text, const char *out_path)
{
  char key[256];
  struct voice_file_sink_s sink;
  int fd;
  int ret;
  if (!text || !out_path ||
      strcmp(g_voice_tts_backend, "mimo-v2.5-tts") != 0 ||
      voice_get_api_key(key, sizeof(key)) < 0)
    {
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      return -EINVAL;
    }

  if (robot_motion_is_busy())
    {
      printf("[POWER-SAFE] mono_ms=%llu TTS_REJECT motion_active\n",
             robot_voice_now_ms());
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      memset(key, 0, sizeof(key));
      return -EBUSY;
    }

  fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      return -errno;
    }
  sink.fd = fd;

  ret = robot_audio_playback_open();
  if (ret < 0)
    {
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      close(fd);
      return ret;
    }
  g_voice_speaking = 1;
  robot_world_state_set_tts_active(true);
  (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                              ROBOT_EXPRESSION_SPEAKING, 0);
  ret = mimo_tts_speak_stream(key, text, robot_voice_file_sink, &sink);
  if (ret == 0)
    {
      ret = robot_audio_playback_finish();
    }
  robot_audio_playback_close();
  g_voice_speaking = 0;
  robot_world_state_set_tts_active(false);
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
  if (ret < 0)
    {
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
    }
  close(fd);
  printf("[contest_voice] voice_channel_test_tts text_len=%zu path=%s rc=%d\n",
         strlen(text), out_path, ret);
  return ret;
}

int voice_channel_test_asr(const char *pcm_path)
{
  char key[256];
  char text[512];
  uint8_t *pcm = NULL;
  size_t len;
  int fd;
  int ret;
  if (!pcm_path || strcmp(g_voice_asr_backend, "mimo-v2.5-asr") != 0 ||
      voice_get_api_key(key, sizeof(key)) < 0)
    {
      return -EINVAL;
    }

  fd = open(pcm_path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }
  len = (size_t)lseek(fd, 0, SEEK_END);
  if (len == 0 || len > ROBOT_VOICE_MAX_BODY)
    {
      close(fd);
      return -EFBIG;
    }
  lseek(fd, 0, SEEK_SET);
  pcm = malloc(len);
  if (pcm == NULL)
    {
      close(fd);
      return -ENOMEM;
    }
  if (read(fd, pcm, len) != (ssize_t)len)
    {
      free(pcm);
      close(fd);
      return -EIO;
    }
  close(fd);
  memset(key, 0, sizeof(key));
  ret = contest_voice_run_asr(pcm, len, text, sizeof(text));
  free(pcm);
  if (ret == 0)
    {
      printf("[contest_voice] ASR: %s\n", text);
    }
  return ret;
}

const char *voice_tts_get_backend(void)
{
  return g_voice_tts_backend;
}

int voice_tts_set_backend(const char *name)
{
  if (!name || strcmp(name, "mimo-v2.5-tts") != 0)
    {
      return -ENOENT;
    }

  strncpy(g_voice_tts_backend, name, sizeof(g_voice_tts_backend) - 1);
  g_voice_tts_backend[sizeof(g_voice_tts_backend) - 1] = '\0';

  /*
   * In the normal vela> workflow this runs after set_wifi.  Start Media here
   * rather than waiting for the first music tool: the official "播放" fast-path
   * can call music_play before the outbound working/OLED status exists.
   * Media initialization is one-shot, like the OLED initializer.
   * Media failure must not make backend selection itself fail.
   */
  (void)robot_media_runtime_init_once();
  return 0;
}

const char *voice_asr_get_backend(void)
{
  return g_voice_asr_backend;
}

int voice_asr_set_backend(const char *name)
{
  if (!name || strcmp(name, "mimo-v2.5-asr") != 0)
    {
      return -ENOENT;
    }

  strncpy(g_voice_asr_backend, name, sizeof(g_voice_asr_backend) - 1);
  g_voice_asr_backend[sizeof(g_voice_asr_backend) - 1] = '\0';

  /*
   * Whichever set_voice_* command runs first after Wi-Fi starts Media.
   * The one-shot guard makes the second command a no-op for Media startup.
   */
  (void)robot_media_runtime_init_once();
  return 0;
}

static char g_api_key[256];

static int robot_voice_pcm_sink(const uint8_t *pcm, size_t len, void *arg)
{
  uint8_t scaled[ROBOT_VOICE_TTS_SCALE_CHUNK];
  size_t offset = 0;

  (void)arg;

  if (pcm == NULL || len == 0)
    {
      return -EINVAL;
    }

  /*
   * TTS PCM is signed 16-bit little-endian.  Scale sample amplitude to 1/2
   * before handing it to the existing audio playback path.
   */
  while (offset < len)
    {
      size_t chunk = len - offset;
      size_t even;
      size_t i;
      int ret;

      if (chunk > sizeof(scaled))
        {
          chunk = sizeof(scaled);
        }

      /* Keep PCM16 sample pairs intact. */
      if ((chunk & 1u) != 0 && chunk > 1)
        {
          chunk--;
        }

      even = chunk & ~(size_t)1u;
      for (i = 0; i < even; i += 2)
        {
          uint16_t raw = (uint16_t)pcm[offset + i] |
                         ((uint16_t)pcm[offset + i + 1] << 8);
          int16_t sample = (int16_t)raw;
          int16_t quieter =
            (int16_t)((sample * ROBOT_VOICE_TTS_GAIN_NUM) /
                      ROBOT_VOICE_TTS_GAIN_DEN);

          scaled[i] = (uint8_t)((uint16_t)quieter & 0xff);
          scaled[i + 1] = (uint8_t)(((uint16_t)quieter >> 8) & 0xff);
        }

      /* PCM16 should always arrive aligned. Preserve a final odd byte rather
       * than dropping it if an upstream callback ever violates that contract. */
      if (chunk > even)
        {
          scaled[even] = pcm[offset + even];
        }

      ret = robot_audio_playback_write(scaled, chunk);
      if (ret < 0)
        {
          return ret;
        }

      offset += chunk;
    }

  return 0;
}

static int robot_voice_once(void)
{
  int ret;

  if (strcmp(g_voice_asr_backend, "mimo-v2.5-asr") != 0 ||
      strcmp(g_voice_tts_backend, "mimo-v2.5-tts") != 0)
    {
      printf("robot_voice: select mimo-v2.5-asr and mimo-v2.5-tts first\n");
      return -ENOSYS;
    }

  ret = voice_channel_start_mode(1);
  if (ret < 0)
    {
      return ret;
    }

  while (g_voice_running)
    {
      usleep(100000);
    }

  return voice_channel_stop();
}

static int robot_voice_capture_test(void)
{
  size_t cap = ROBOT_VOICE_CAPTURE_RATE * 2 * CONTEST_VOICE_CAPTURE_SECONDS;
  uint8_t *pcm = malloc(cap);
  size_t len = 0;
  int ret;
  if (!pcm) return -ENOMEM;
  ret = robot_voice_capture_record(pcm, cap, &len,
                                   CONTEST_VOICE_CAPTURE_SECONDS * 1000);
  printf("[RV-CAP] test rc=%d bytes=%zu\n", ret, len);
  free(pcm);
  return ret;
}

static int robot_voice_asr_test(void)
{
  size_t cap = ROBOT_VOICE_CAPTURE_RATE * 2 * CONTEST_VOICE_CAPTURE_SECONDS;
  uint8_t *pcm = malloc(cap);
  char text[512];
  size_t len = 0;
  int ret;
  if (!pcm) return -ENOMEM;
  ret = robot_voice_capture_record(pcm, cap, &len,
                                   CONTEST_VOICE_CAPTURE_SECONDS * 1000);
  if (ret == 0) ret = contest_voice_run_asr(pcm, len, text, sizeof(text));
  if (ret == 0) printf("ASR: %s\n", text);
  free(pcm);
  return ret;
}


static int robot_voice_tts_test(const char *text)
{
  int ret = robot_audio_playback_open();
  if (ret < 0) return ret;
  ret = mimo_tts_speak_stream(g_api_key, text, robot_voice_pcm_sink, NULL);
  if (ret == 0) ret = robot_audio_playback_finish();
  robot_audio_playback_close();
  return ret;
}

int main(int argc, char *argv[])
{
  if (g_api_key[0] == '\0')
    {
      voice_get_api_key(g_api_key, sizeof(g_api_key));
    }

  if (argc >= 3 && strcmp(argv[1], "set_key") == 0)
    {
      strncpy(g_api_key, argv[2], sizeof(g_api_key) - 1);
      g_api_key[sizeof(g_api_key) - 1] = '\0';
      if (claw_config_set(AGENT_CFG_KEY_API_KEY, g_api_key) != OK)
        {
          printf("robot_voice: failed to save API key\n");
          return 1;
        }
      printf("robot_voice: API key saved\n");
      return 0;
    }
  if (argc >= 2 && strcmp(argv[1], "once") == 0)
    {
      if (g_api_key[0] == '\0') { printf("robot_voice: use set_key first\n"); return 1; }
      return robot_voice_once();
    }
  if (argc >= 2 && strcmp(argv[1], "test_capture") == 0)
    {
      return robot_voice_capture_test();
    }
  if (argc >= 2 && strcmp(argv[1], "test_asr") == 0)
    {
      if (g_api_key[0] == '\0') { printf("robot_voice: use set_key first\n"); return 1; }
      return robot_voice_asr_test();
    }
  if (argc >= 3 && strcmp(argv[1], "test_llm") == 0)
    {
      if (g_api_key[0] == '\0') { printf("robot_voice: use set_key first\n"); return 1; }
      char reply[2048];
      int ret = mimo_llm_chat(g_api_key, argv[2], reply, sizeof(reply));
      if (ret == 0) printf("LLM: %s\n", reply);
      return ret;
    }
  if (argc >= 3 && strcmp(argv[1], "music_play") == 0)
    {
      return robot_music_play_file(argv[2]);
    }
  if (argc >= 3 && strcmp(argv[1], "test_wav") == 0)
    {
      return robot_music_play_file(argv[2]);
    }
  if (argc >= 3 && strcmp(argv[1], "test_tts") == 0)
    {
      if (g_api_key[0] == '\0') { printf("robot_voice: use set_key first\n"); return 1; }
      return robot_voice_tts_test(argv[2]);
    }
  if (argc >= 2 && strcmp(argv[1], "diag") == 0)
    {
      printf("robot_voice: host=%s asr=%s llm=%s tts=%s capture=%dHz tts=%dHz\n",
             ROBOT_VOICE_HOST, ROBOT_VOICE_ASR_MODEL, ROBOT_VOICE_LLM_MODEL,
             ROBOT_VOICE_TTS_MODEL, ROBOT_VOICE_CAPTURE_RATE, ROBOT_VOICE_TTS_RATE);
      return 0;
    }
  printf("Usage: robot_voice set_key <api_key> | once | test_capture | "
         "test_asr | test_llm <text> | music_play <path> | "
         "test_wav <path> | test_tts <text> | diag\n");
  return 0;
}
