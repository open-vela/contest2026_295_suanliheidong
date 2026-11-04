#include <nuttx/config.h>
#include <netutils/cJSON.h>

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
#include "mimo_client.h"
#include "mimo_llm.h"
#include "mimo_llm_stream.h"
#include "mimo_tts.h"
#include "robot_voice_capture.h"
#include "robot_audio_playback.h"
#include "robot_music_player.h"
#include "robot_task.h"
#include "robot_music_media.h"
#include "robot_expression.h"
#include "robot_motion.h"
#include "robot_motion_tool.h"
#include "robot_voice_config.h"
#include "robot_voice_diag.h"
#include "robot_voice_retry.h"
#include "robot_voice_prompts.h"
#include "robot_voice_threads.h"
#include "robot_voice_http.h"
#include "robot_voice_clock.h"
#include "robot_voice_wifi.h"
#include "robot_wakeword.h"
#include "robot_wakeword_config.h"
#if ROBOT_VOICE_CAPTURE_RATE != 16000 || ROBOT_VOICE_CHANNELS != 1 || ROBOT_VOICE_BITS != 16
#  error TFLite wakeword requires 16000 Hz mono PCM16 capture
#endif
#include "robot_proactive.h"
#include "robot_network_adapter.h"
#include "robot_ffmpeg_amix_compat.h"

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

/* The contest music backend owns HTTP decode and I2S playback.  Keep this
 * compatibility entry point for existing voice setup commands, but never
 * launch the native Media daemon or create a second audio graph. */
static int robot_media_runtime_init_once(void)
{
  pthread_mutex_lock(&g_robot_media_init_lock);
  if (!g_robot_media_initialized)
    {
      g_robot_media_initialized = true;
      printf("[RV-MEDIA] native Media disabled; contest HTTP/MP3 backend active\n");
    }
  pthread_mutex_unlock(&g_robot_media_init_lock);
  return 0;
}

#include "robot_skill_installer.h"
#include "robot_world_state.h"

/* Strong definitions live in the application entry object so the linker
 * cannot discard them while resolving ai_agent's weak simulation stubs. */
static char g_voice_asr_backend[32] = "mimo-v2.5-asr";
static char g_voice_tts_backend[32] = "mimo-v2.5-tts";
static pthread_mutex_t g_voice_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_voice_init_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_voice_initialized;
static pthread_mutex_t g_voice_speak_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tts_jobs_changed = PTHREAD_COND_INITIALIZER;
static unsigned int g_tts_jobs;

#define CONTEST_VOICE_RECORD_STACK  (16 * 1024)
#define CONTEST_VOICE_ASR_STACK     (24 * 1024)
#define CONTEST_CLI_TTS_STACK       (16 * 1024)
#define CONTEST_CLI_TTS_MOTION_WAIT_MS 15000
#define CONTEST_VOICE_CAPTURE_RESTART_LIMIT 8

/* TTS software gain: match the music path at 9/32 amplitude. */
#define ROBOT_VOICE_TTS_GAIN_NUM 9
#define ROBOT_VOICE_TTS_GAIN_DEN 32
#define ROBOT_VOICE_TTS_SCALE_CHUNK 512
static volatile int g_voice_running;
static volatile int g_voice_speaking;
static volatile int g_voice_oneshot;
static volatile int g_voice_thread_valid;
static volatile bool g_voice_reply_pending;
static volatile bool g_voice_stop_requested;
static volatile bool g_voice_worker_exited;
/* A failed monitor must be restartable after an unscoped Feishu reply. */
static volatile bool g_voice_restart_pending;
static volatile bool g_voice_explicit_stop;
static pthread_t g_voice_thread;
static int robot_voice_pcm_sink(const uint8_t *pcm, size_t len, void *arg);
static int robot_voice_cli_tts_submit(const char *text);
static int robot_voice_tts_submit(const char *text, bool agent_reply,
                                   unsigned long run_id);
static int voice_channel_speak_internal(const char *text, bool agent_reply,
                                        unsigned long run_id);
int voice_channel_start(void);
extern bool robot_music_media_owns_reply(unsigned long) __attribute__((weak));
extern bool robot_music_media_is_active(void) __attribute__((weak));
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
static bool g_voice_tap_registered;
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
      RV_VERBOSE("[RV-EXPR] CLI working status -> thinking\n");
    }
  else
    {
      int tts_ret;

      robot_world_state_set_conversation_active(false);
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_AGENT);
      RV_VERBOSE("[RV-EXPR] CLI final reply -> clear agent expression + queue TTS\n");

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
  CONTEST_VOICE_WAKE,
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
      case CONTEST_VOICE_WAKE:
        return "WAKE_LISTEN";
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
      RV_VERBOSE("[contest_voice] state %s -> %s\n",
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
  char text[ROBOT_VOICE_TEXT_CAP];
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
  RV_VERBOSE("[RV-ASR] worker start bytes=%zu stack=%u\n",
         job->pcm_len, (unsigned int)CONTEST_VOICE_ASR_STACK);
  job->ret = mimo_asr_recognize(key, job->pcm, job->pcm_len,
                                job->text, sizeof(job->text));
  memset(key, 0, sizeof(key));
  RV_VERBOSE("[RV-ASR] worker done rc=%d\n", job->ret);
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

extern void robot_voice_wifi_poll(void) __attribute__((weak));

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

      if (robot_voice_wifi_poll != NULL)
        {
          robot_voice_wifi_poll();
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

static int
robot_voice_execute_local_action_with_network(
  enum robot_voice_simple_action_e action)
{
  int ret;

  ret = robot_voice_wifi_ensure_connected(0);
  if (ret < 0)
    {
      return -ENETUNREACH;
    }

  ret = robot_voice_execute_local_action(action);
  if (ret == 0)
    {
      return 0;
    }

  RV_EVENT("[网络确认] local action failed rc=%d; retry after link check\n",
           ret);
  ret = robot_voice_wifi_reconnect_and_ensure(0);
  if (ret < 0)
    {
      return -ENETUNREACH;
    }

  ret = robot_voice_execute_local_action(action);
  return ret < 0 ? -ENETUNREACH : ret;
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

static void contest_voice_chat_id(char *out, size_t cap, bool action);
static void rv_note_agent_text(const char *text);
static int rv_local_reply(const char *text);
static void rv_claim_local_reply(void);
static void rv_reply_done(bool agent_reply);

static int contest_voice_route_text(const char *text)
{
  char routed_text[512];
  char chat_id[64];

  if (g_voice_stop_requested)
    {
      return -ECANCELED;
    }
  RV_VERBOSE("[RV-ASR] routed_text_bytes=%zu\n", strlen(text));
  RV_DIALOGUE("[ASR][最终] %s\n", text[0] ? text : "（空文本）");
  robot_world_state_note_user_activity();
  (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                              ROBOT_EXPRESSION_THINKING, 0);

  if (robot_voice_clock_reply(text, routed_text, sizeof(routed_text)))
    {
      return rv_local_reply(routed_text);
    }

  enum robot_voice_simple_action_e simple_action =
    robot_voice_classify_simple_action(text);

#if ROBOT_VOICE_LOCAL_ACTION_FAST_PATH
  if (simple_action != ROBOT_VOICE_ACTION_NONE)
    {
      unsigned long long fast_start = robot_voice_now_ms();
      int action_ret;

      RV_VERBOSE("[RV-FAST] local action=%d begin\n", (int)simple_action);
      rv_claim_local_reply();
      contest_voice_set_state(CONTEST_VOICE_WAIT_AGENT);
      robot_world_state_set_conversation_active(true);
      g_voice_reply_pending = true;

      action_ret = robot_voice_execute_local_action_with_network(simple_action);

      RV_VERBOSE("[RV-FAST] local action=%d rc=%d latency_ms=%llu\n",
             (int)simple_action, action_ret,
             robot_voice_now_ms() - fast_start);

      if (g_voice_stop_requested)
        {
          g_voice_reply_pending = false;
          robot_world_state_set_conversation_active(false);
          (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
          return -ECANCELED;
        }

      if (action_ret == 0)
        {
          /* The synchronous action has completed, so TTS no longer
           * conflicts with PWM. Keep the acknowledgement action-focused. */
          (void)voice_channel_speak("动作执行完毕");
          rv_reply_done(true); /* Local action + acknowledgement complete. */
        }
      else
        {
          g_voice_reply_pending = false;
          robot_world_state_set_conversation_active(false);
          (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
          (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                      ROBOT_EXPRESSION_ERROR, 1500);
          if (action_ret == -ENETUNREACH)
            {
              (void)voice_channel_speak("网络异常，请稍后再试");
            }
          if (g_voice_running && !g_voice_stop_requested)
            {
              contest_voice_set_state(CONTEST_VOICE_RECORDING);
            }
        }

      return action_ret;
    }
#endif

  /* Do not start an Agent/LLM action on a stale DHCP link.  The synchronous
   * gate retries Wi-Fi/DNS recovery before the request is dispatched. */
  if (robot_voice_wifi_ensure_connected(0) < 0)
    {
      RV_EVENT("[网络确认] command gate failed; skip Agent dispatch\n");
      return rv_local_reply("网络异常，请稍后再试");
    }

  const char *agent_text = text;
  contest_voice_chat_id(chat_id, sizeof(chat_id), false);
  const char *agent_chat_id = chat_id;

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
          contest_voice_chat_id(chat_id, sizeof(chat_id), true);
          agent_chat_id = chat_id;
          (void)session_clear(agent_chat_id);
          RV_TEXT("[RV-FAST] MiMo stateless action route: '%s' -> '%s'\n",
                 text, agent_text);
        }
    }
  else if (robot_voice_rewrite_local_music_request(text, routed_text,
                                                    sizeof(routed_text)))
    {
      agent_text = routed_text;
      RV_TEXT("[RV-MUSIC-ROUTE] rewrite: '%s' -> '%s'\n",
             text, agent_text);
    }

  agent_msg_t msg;
  memset(&msg, 0, sizeof(msg));
  strncpy(msg.channel, AGENT_CHAN_VOICE, sizeof(msg.channel) - 1);
  strncpy(msg.chat_id, agent_chat_id, sizeof(msg.chat_id) - 1);
  msg.content = strdup(agent_text);
  rv_note_agent_text(agent_text);
  contest_voice_set_state(CONTEST_VOICE_WAIT_AGENT);
  robot_world_state_set_conversation_active(true);
  RV_VERBOSE("[RV-VOICE] push inbound voice:%s\n", agent_chat_id);
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
      return -EIO;
    }

  return 0;
}

/* Continuous monitor is compiled in this translation unit. Acoustic DSP and
 * TFLM are separate C/C++ units with explicit Make/CMake build entries. */
#include "robot_task.h"
extern int robot_music_media_suspend(void) __attribute__((weak));
extern bool robot_music_media_has_task(void) __attribute__((weak));
extern bool robot_music_media_owns_reply(unsigned long) __attribute__((weak));
static void rv_return_to_wake_locked(void);
#include "robot_voice_afe.h"
#define RV_MS_BYTES(ms) ((size_t)ROBOT_VOICE_CAPTURE_RATE * 2 * (ms) / 1000)
#define RV_RESULT_COUNT (ROBOT_VOICE_ASR_WORKERS + ROBOT_VOICE_ASR_QUEUE)

#if ROBOT_VOICE_ASR_WORKERS < 1 || ROBOT_VOICE_ASR_WORKERS > 2
#  error ROBOT_VOICE_ASR_WORKERS must be 1 or 2
#endif

enum rv_monitor_phase_e
{
  RV_MON_GREETING = 0,
  RV_MON_WAKE,
  RV_MON_LISTEN,
  RV_MON_BUSY,
};

struct rv_monitor_job_s
{
  uint8_t *pcm;                 /* Immutable, owned by this job. */
  size_t pcm_len;
  size_t voice_end;            /* Last voiced sample offset in the segment. */
  size_t audio_start;
  unsigned int epoch;
  unsigned long speech_id;
  unsigned int retries;
  unsigned long final_seq;
  bool final;
  bool wake_context;
  unsigned long long created_ms;
  unsigned long long voice_end_ms, request_ms, returned_ms;
  int ret;
  char text[ROBOT_VOICE_TEXT_CAP];
};

struct rv_llm_speech_s;
struct rv_monitor_s
{
  pthread_mutex_t lock;
  pthread_cond_t changed;
  bool enabled;
  bool stop;
  bool muted;
  bool capture_exited;
  bool capture_ready;
  bool wake_handoff_pending;
  uint64_t wake_total_samples;
  size_t wake_buffered_samples;
  unsigned long playback_token;
  int acoustic_error;
  bool listening_announced, chat_announced;
  enum rv_monitor_phase_e announced_phase;
  int capture_result;
  bool capture_active;
  bool pending_failed;
  bool reply_claimed;           /* Exactly one final/TTS owner per round. */
  bool reply_locked;            /* First Flash content/tool delta -> drain. */
  int pending_error;
  unsigned int asr_failures;
  unsigned long long error_notice_ms;
  bool final_inflight[ROBOT_VOICE_ASR_WORKERS];
  struct rv_monitor_job_s *active_jobs[ROBOT_VOICE_ASR_WORKERS];
  struct rv_monitor_job_s *cached_preview;
  size_t queued_pcm;
  size_t inflight_pcm;
  unsigned long submitted_final;
  unsigned long consumed_final;
  unsigned long long pending_started_ms;
  char pending_text[ROBOT_VOICE_TEXT_CAP];
  char agent_text[ROBOT_VOICE_TEXT_CAP];
  char *spoken_prefix;        /* Heap-owned; only fully played sentences. */
  unsigned long spoken_run;
  bool stream_busy;
  struct rv_llm_speech_s *active_speech; /* Protected by monitor lock. */
  enum rv_monitor_phase_e phase;
  unsigned int epoch;
  unsigned long run_id;
  volatile unsigned long run_token;
  unsigned long speech_id;
  unsigned long frames;
  unsigned long frames_during_asr;
  enum robot_voice_afe_mode_e afe_mode;
  struct robot_voice_afe_stats_s afe_stats;
  unsigned long afe_restarts;
  uint32_t afe_sink_max_us;
  unsigned long afe_sink_overruns;
  unsigned long vad_rejected;
  unsigned long dropped;
  unsigned int in_flight;
  unsigned int queued;
  unsigned int completed;
  unsigned long long next_request_ms;
  unsigned long long echo_until_ms;
  unsigned long long last_voice_ms;
  unsigned long long chat_activity_ms;
  unsigned long long reply_start_ms;
  unsigned long long reply_user_end_ms;
  struct rv_monitor_job_s *queue[ROBOT_VOICE_ASR_QUEUE];
  struct rv_monitor_job_s *results[RV_RESULT_COUNT];
};

static struct rv_monitor_s g_monitor =
{
  .afe_mode = ROBOT_VOICE_AFE_MODE,
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .changed = PTHREAD_COND_INITIALIZER,
};

static void rv_cancel_active_speech_locked(void);
static void rv_lock_reply_locked(unsigned long run_id);

static unsigned long rv_run_token_load(void)
{
  return __atomic_load_n(&g_monitor.run_token, __ATOMIC_ACQUIRE);
}

static void rv_run_token_advance_locked(void)
{
  (void)__atomic_add_fetch(&g_monitor.run_token, 1, __ATOMIC_RELEASE);
}

static void rv_reply_done(bool agent_reply);

static void rv_claim_local_reply(void)
{
  pthread_mutex_lock(&g_monitor.lock);
  rv_lock_reply_locked(g_monitor.run_id);
  g_monitor.reply_claimed = true;
  g_monitor.agent_text[0] = 0;
  pthread_mutex_unlock(&g_monitor.lock);
}

static int rv_local_reply(const char *text)
{
  pthread_mutex_lock(&g_monitor.lock);
  unsigned long run_id = g_monitor.enabled ? g_monitor.run_id : 0;
  rv_lock_reply_locked(g_monitor.run_id);
  g_monitor.reply_claimed = true;
  g_monitor.agent_text[0] = 0;
  pthread_mutex_unlock(&g_monitor.lock);
  g_voice_reply_pending = true;
  contest_voice_set_state(CONTEST_VOICE_WAIT_AGENT);
  int ret = robot_voice_tts_submit(text, true, run_id);
  if (ret < 0) {
    g_voice_reply_pending = false;
    rv_reply_done(true);
  }
  return ret;
}

static void rv_note_agent_text(const char *text)
{
  pthread_mutex_lock(&g_monitor.lock);
  free(g_monitor.spoken_prefix);
  g_monitor.spoken_prefix = NULL;
  snprintf(g_monitor.agent_text, sizeof(g_monitor.agent_text), "%s", text);
  g_monitor.reply_claimed = false;
  pthread_mutex_unlock(&g_monitor.lock);
}

static void rv_http_agent_error(const char *body, int status, unsigned long token)
{
  cJSON *json = cJSON_Parse(body);
  cJSON *messages = json ? cJSON_GetObjectItem(json, "messages") : NULL;
  const char *user = NULL;
  unsigned long run_id = 0;
  bool voice_enabled;
  bool reply_claimed;
  bool user_found;
  bool same_turn;
  bool eligible;
  bool notify = false;
  enum rv_monitor_phase_e phase;
  for (int i = messages ? cJSON_GetArraySize(messages) - 1 : -1; i >= 0; i--) {
    cJSON *message = cJSON_GetArrayItem(messages, i);
    cJSON *role = cJSON_GetObjectItem(message, "role");
    cJSON *text = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(role) && !strcmp(role->valuestring, "user") && cJSON_IsString(text)) {
      user = text->valuestring; break;
    }
  }
  user_found = user != NULL;
  pthread_mutex_lock(&g_monitor.lock);
  voice_enabled = g_monitor.enabled;
  reply_claimed = g_monitor.reply_claimed;
  phase = g_monitor.phase;
  same_turn = user && g_monitor.agent_text[0] &&
              !strcmp(user, g_monitor.agent_text);
  eligible = (!token || token == rv_run_token_load()) &&
             voice_enabled && !g_voice_stop_requested && !reply_claimed &&
             (phase == RV_MON_GREETING || phase == RV_MON_BUSY) && same_turn;
  if (eligible) {
    /* Pin the error to this exact voice input, retire its Agent identity and
     * keep every new human segment. CLI/unrelated requests cannot end it. */
    rv_run_token_advance_locked();
    run_id = ++g_monitor.run_id;
    g_monitor.reply_claimed = true;
    g_monitor.agent_text[0] = 0;
    notify = true;
    g_monitor.error_notice_ms = robot_voice_now_ms();
  }
  pthread_mutex_unlock(&g_monitor.lock);
  cJSON_Delete(json);
  printf("[RV-AGENT-ERROR] status=%d voice_enabled=%u phase=%u user_found=%u "
         "turn_match=%u reply_claimed=%u eligible=%u notify=%u\n",
         status, voice_enabled, (unsigned int)phase, user_found, same_turn,
         reply_claimed, eligible, notify);
  if (run_id) {
    printf("[RV-AGENT] request failed status=%d notify=%u; new input retained\n", status, notify);
    const char *text = ROBOT_PROMPT_NETWORK_ERROR;
    if (!notify || robot_voice_tts_submit(text, true, run_id) < 0) {
      g_voice_reply_pending = false;
      rv_reply_done(true);
    }
  }
}

void robot_voice_http_on_agent_error(const char *body, int status)
{ rv_http_agent_error(body, status, 0); }

void robot_voice_http_on_agent_error_for_token(const char *body, int status,
                                              unsigned long token)
{
  if (token) rv_http_agent_error(body, status, token);
}

struct rv_segmenter_s
{
  struct robot_voice_afe_s *afe;
  struct robot_voice_afe_stats_s afe_previous;
  uint8_t *pre;
  size_t pre_head;
  size_t pre_used;
  uint8_t *speech;
  size_t used;
  size_t voiced;
  size_t silence;
  size_t stored_silence;
  unsigned int onset;
  unsigned int interrupt_onset;
  size_t last_voice_end;
  size_t early_voice_end;
  unsigned int epoch;
  unsigned long speech_id;
  bool active;
  bool overflow;
  bool wake_context;
  size_t wake_head, wake_used, wake_voiced;
  uint64_t wake_total;
  uint8_t wake_votes[ROBOT_VOICE_WAKE_CONTEXT_MS / ROBOT_VOICE_AFE_FRAME_MS];
};

/* Caller holds g_monitor.lock; the capture thread publishes snapshots. */
static void rv_afe_diag_locked(void)
{
  const struct robot_voice_afe_stats_s *a = &g_monitor.afe_stats;
  printf("[RV-POLICY] interrupt=valid-asr-before-flash locked=%u wake_cache_ms=%d wake_pending=%u cached_samples=%zu\n",
         g_monitor.reply_locked, ROBOT_VOICE_WAKE_CONTEXT_MS,
         g_monitor.wake_handoff_pending, g_monitor.wake_buffered_samples);
  printf("[RV-AFE] integer_hp=on asr=raw mode=%u vad_mode=%d min_level=%d min_voice_ms=%d "
         "frame_ms=%d onset=%d/%d pre_ms=%d "
         "in=%llu out=%llu partial=%u discarded=%llu frames=%llu speech=%llu "
         "raw=%u clean=%u noise=%u threshold=%u clipped=%llu "
         "max_us=%u overruns=%llu sink_max_us=%u sink_overruns=%lu rx_restarts=%lu "
         "vad_rejected=%lu aec=off\n",
         (unsigned int)g_monitor.afe_mode, ROBOT_VOICE_VAD_MODE,
         ROBOT_VOICE_VAD_MIN_LEVEL, ROBOT_VOICE_VAD_MIN_MS,
         ROBOT_VOICE_AFE_FRAME_MS, ROBOT_VOICE_VAD_START_FRAMES,
         ROBOT_VOICE_VAD_WINDOW_FRAMES, ROBOT_VOICE_PRE_ROLL_MS,
         (unsigned long long)a->input_samples, (unsigned long long)a->output_samples,
         a->partial_samples, (unsigned long long)a->discarded_samples,
         (unsigned long long)a->frames, (unsigned long long)a->speech_frames,
         a->raw_level, a->clean_level, a->noise_level, a->threshold,
         (unsigned long long)a->clipped_samples, a->max_process_us,
         (unsigned long long)a->overruns, g_monitor.afe_sink_max_us,
         g_monitor.afe_sink_overruns, g_monitor.afe_restarts,
         g_monitor.vad_rejected);
}

static void rv_job_free(struct rv_monitor_job_s *job)
{
  if (job != NULL)
    {
      free(job->pcm);
      free(job);
    }
}

static void rv_queue_remove_locked(unsigned int index)
{
  g_monitor.queued_pcm -= g_monitor.queue[index]->pcm_len;
  rv_job_free(g_monitor.queue[index]);
  g_monitor.queued--;
  memmove(&g_monitor.queue[index], &g_monitor.queue[index + 1],
          (g_monitor.queued - index) * sizeof(g_monitor.queue[0]));
}

static void rv_clear_locked(void)
{
  g_monitor.wake_handoff_pending = false;
  g_monitor.wake_total_samples = 0;
  g_monitor.wake_buffered_samples = 0;
  free(g_monitor.spoken_prefix);
  g_monitor.spoken_prefix = NULL;
  rv_job_free(g_monitor.cached_preview);
  g_monitor.cached_preview = NULL;
  unsigned int i;

  while (g_monitor.queued != 0)
    {
      rv_queue_remove_locked(0);
    }

  for (i = 0; i < g_monitor.completed; i++)
    {
      rv_job_free(g_monitor.results[i]);
    }

  g_monitor.completed = 0;
  g_monitor.pending_text[0] = '\0';
  g_monitor.pending_failed = false;
  g_monitor.pending_error = 0;
  g_monitor.pending_started_ms = 0;
}

/* Same lock arbitrates Flash's first meaningful reply and a new final ASR.
 * When Flash wins, retire candidates once; in-flight workers own their PCM
 * until they return. A subsequent tool/HTTP request stays in this round. */
static void rv_lock_reply_locked(unsigned long run_id)
{
  if (!g_monitor.enabled || g_monitor.stop || g_voice_stop_requested ||
      run_id != g_monitor.run_id || g_monitor.reply_locked ||
      (g_monitor.phase != RV_MON_BUSY && g_monitor.phase != RV_MON_GREETING)) return;
  g_monitor.reply_locked = true;
  g_monitor.muted = true;
  g_monitor.epoch++;
  while (g_monitor.queued) rv_queue_remove_locked(0);
  for (unsigned int i = 0; i < g_monitor.completed; i++)
    rv_job_free(g_monitor.results[i]);
  g_monitor.completed = 0;
  rv_job_free(g_monitor.cached_preview); g_monitor.cached_preview = NULL;
  g_monitor.submitted_final = g_monitor.consumed_final = 0;
  g_monitor.pending_text[0] = 0;
  g_monitor.pending_failed = false; g_monitor.pending_error = 0;
  g_monitor.pending_started_ms = 0;
  g_monitor.capture_active = false;
  g_monitor.wake_handoff_pending = false;
  g_monitor.listening_announced = false;
  pthread_cond_broadcast(&g_monitor.changed);
  RV_EVENT("[RV-TURN] Flash reply locked run=%lu; resume after full drain\n", run_id);
}

/* Optional transport session hooks permit the standalone monitor harness. */
extern int robot_voice_http_session_begin(void) __attribute__((weak));
extern void robot_voice_http_session_end(void) __attribute__((weak));

static void rv_wait_locked(unsigned int ms)
{
  struct timespec until;

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += ms / 1000;
  until.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L)
    {
      until.tv_sec++;
      until.tv_nsec -= 1000000000L;
    }

  (void)pthread_cond_timedwait(&g_monitor.changed, &g_monitor.lock, &until);
}

/* Skip only known separators, retaining the original UTF-8 command suffix. */
static const char *rv_skip_separators(const char *text)
{
  static const char *const punct[] =
  {
    "，", "。", "！", "？", "：", "；", "、", "“", "”", NULL
  };
  unsigned int i;

  for (;;)
    {
      if (*text != '\0' && strchr(" \t\r\n,!.?:;-_\"'", *text) != NULL)
        {
          text++;
          continue;
        }

      for (i = 0; punct[i] != NULL; i++)
        {
          size_t len = strlen(punct[i]);
          if (strncmp(text, punct[i], len) == 0)
            {
              text += len;
              break;
            }
        }

      if (punct[i] == NULL)
        {
          return text;
        }
    }
}

static const char *rv_phrase_tail(const char *text, const char *phrase)
{
  const unsigned char *p = (const unsigned char *)phrase;

  if (text == NULL)
    {
      return NULL;
    }

  while (*p != '\0')
    {
      unsigned char c;

      text = rv_skip_separators(text);
      c = (unsigned char)*text;
      if (c >= 'A' && c <= 'Z')
        {
          c += 'a' - 'A';
        }

      if (c != *p++)
        {
          return NULL;
        }

      text++;
    }

  /* Reject a different English name sharing the same prefix. */
  if ((*text >= 'A' && *text <= 'Z') || (*text >= 'a' && *text <= 'z'))
    {
      return NULL;
    }

  return rv_skip_separators(text);
}

static void contest_voice_chat_id(char *out, size_t cap, bool action)
{
  pthread_mutex_lock(&g_monitor.lock);
  if (g_monitor.enabled)
    {
      snprintf(out, cap, action ? "voice_action_%lu" : "voice_run_%lu",
               g_monitor.run_id);
    }
  else
    {
      snprintf(out, cap, "%s", action ? "voice_action" : "voice");
    }
  pthread_mutex_unlock(&g_monitor.lock);
}

static void rv_end_conversation_locked(void)
{
  if (g_monitor.chat_announced)
    RV_EVENT("[语音] 结束语音对话模式\n");
  g_monitor.chat_announced = false;
}

static void rv_listening_expression_locked(void)
{
  if (g_monitor.phase == RV_MON_WAKE || g_monitor.phase == RV_MON_LISTEN) {
    if (g_monitor.phase == RV_MON_WAKE) rv_end_conversation_locked();
    else if (!g_monitor.chat_announced) {
      g_monitor.chat_announced = true;
      RV_EVENT("[语音] 进入语音对话模式\n");
    }
    if (!g_monitor.listening_announced ||
        g_monitor.announced_phase != g_monitor.phase) {
      if (g_monitor.phase == RV_MON_WAKE)
        RV_EVENT("[语音] 正处于监听模式，等待唤醒词“你好,openvela”\n");
      else
        RV_EVENT("[语音] 正处于监听模式，等待语音输入\n");
    }
    g_monitor.announced_phase = g_monitor.phase;
    g_monitor.listening_announced = true;
  } else g_monitor.listening_announced = false;
  if (g_monitor.phase == RV_MON_WAKE)
    {
      contest_voice_set_state(CONTEST_VOICE_WAKE);
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
    }
  else if (g_monitor.phase == RV_MON_LISTEN)
    {
      contest_voice_set_state(CONTEST_VOICE_RECORDING);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                                  ROBOT_EXPRESSION_LISTENING, 0);
    }
  else
    {
      /* A CLI utterance must not clear an outstanding Agent/greeting turn. */
      contest_voice_set_state(CONTEST_VOICE_WAIT_AGENT);
      robot_world_state_set_conversation_active(true);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                                  ROBOT_EXPRESSION_THINKING, 0);
    }
}

/* A Flash reply locks recognition before TTS synthesis. Unscoped CLI
 * playback still waits for an active user utterance before muting capture. */
static bool rv_playback_begin_checked(bool (*cancelled)(void *), void *arg,
                                      bool allow_unscoped)
{
  bool allowed;
  pthread_mutex_lock(&g_monitor.lock);
  while (g_monitor.enabled && !g_monitor.stop &&
         (!g_voice_stop_requested || allow_unscoped) &&
         !(cancelled && cancelled(arg)) &&
         !g_monitor.reply_locked &&
         (g_monitor.capture_active || g_monitor.wake_handoff_pending ||
          (robot_voice_now_ms() >= g_monitor.last_voice_ms &&
           robot_voice_now_ms() - g_monitor.last_voice_ms <
             ROBOT_VOICE_TURN_END_MS)))
    {
      rv_wait_locked(50);
    }
  allowed = (!g_monitor.enabled ||
            ((!g_monitor.stop && !g_voice_stop_requested) || allow_unscoped)) &&
            !(cancelled && cancelled(arg));
  if (allowed && g_monitor.enabled)
    {
      g_monitor.muted = true;
      g_monitor.playback_token = rv_run_token_load();
      (void)robot_wakeword_feed(NULL, 0, g_monitor.epoch, false);
      g_monitor.listening_announced = false;
    }
  pthread_mutex_unlock(&g_monitor.lock);
  return allowed;
}

/* Only unscoped playback may reopen a gap. A committed Flash round remains
 * muted until its final TTS owner reports a fully drained reply. */
static void rv_playback_end_window(void)
{
  pthread_mutex_lock(&g_monitor.lock);
  if (g_monitor.enabled && !g_monitor.stop && !g_voice_stop_requested &&
      !g_monitor.reply_locked &&
      g_monitor.playback_token == rv_run_token_load()) {
    g_monitor.muted = false;
    g_monitor.echo_until_ms = ROBOT_VOICE_ECHO_GUARD_MS ?
      robot_voice_now_ms() + ROBOT_VOICE_ECHO_GUARD_MS : 0;
    pthread_cond_broadcast(&g_monitor.changed);
  }
  pthread_mutex_unlock(&g_monitor.lock);
}

/* Reopen capture only after a real PCM starvation interval and only when the
 * speaker queue is nearly empty, so echo protection covers its residual. */
static bool rv_playback_gap_listen_due(unsigned long long idle_since_ms)
{
  unsigned long long now = robot_voice_now_ms();
  return idle_since_ms != 0 && now >= idle_since_ms &&
         now - idle_since_ms >= ROBOT_VOICE_TTS_LISTEN_GAP_MS &&
         robot_audio_playback_buffered_bytes() <=
           ROBOT_VOICE_TTS_LISTEN_BUFFER_BYTES;
}

static void rv_reply_done_for_run(bool agent_reply, unsigned long run_id)
{
  pthread_mutex_lock(&g_monitor.lock);
  if (run_id && run_id != g_monitor.run_id)
    { pthread_mutex_unlock(&g_monitor.lock); return; }
  if (g_monitor.reply_locked && !agent_reply)
    { pthread_mutex_unlock(&g_monitor.lock); return; }
  if (agent_reply) {
    g_voice_reply_pending = false;
    g_monitor.reply_locked = false;
  }
  if (g_monitor.enabled && !g_voice_stop_requested)
    {
      /* No speaker mute means capture may already contain a new onset.
       * A download/empty reply finishing must not invent an echo interval. */
      if (g_monitor.muted)
        g_monitor.echo_until_ms = ROBOT_VOICE_ECHO_GUARD_MS ?
      robot_voice_now_ms() + ROBOT_VOICE_ECHO_GUARD_MS : 0;
      g_monitor.muted = false;
      g_monitor.chat_activity_ms = robot_voice_now_ms();
      if (agent_reply && g_monitor.phase == RV_MON_GREETING)
        {
          g_monitor.phase = RV_MON_WAKE;
          RV_VERBOSE("[RV-WAKE] ready: say 你好，OpenVela\n");
        }
      else if (agent_reply && g_monitor.phase == RV_MON_BUSY)
        {
          g_monitor.phase = RV_MON_LISTEN;
        }
      if (agent_reply && robot_music_media_has_task && robot_music_media_has_task())
        rv_return_to_wake_locked();
      else rv_listening_expression_locked();
      pthread_cond_broadcast(&g_monitor.changed);
    }
  pthread_mutex_unlock(&g_monitor.lock);
}

static void rv_reply_done(bool agent_reply)
{
  rv_reply_done_for_run(agent_reply, 0);
}

/* Registered only for VOICE. The tap owns no message memory. A new voice_start
 * uses a new chat_id, so an old Agent reply cannot reopen a stopped session. */
static bool rv_stream_accept_final_locked(const char *text, unsigned long run_id);

static void robot_voice_outbound_tap(const agent_msg_t *msg, void *cookie)
{
  char expected[64];
  char action[64];
  bool owned;
  bool accept = true;
  unsigned long run_id;
  unsigned long long commit_ms = 0;
  bool stream_owns_final = false;

  (void)cookie;
  if (msg == NULL || msg->content == NULL ||
      robot_voice_is_cli_working_status(msg->content))
    {
      return;
    }

  /* Music events are a silent tool-only turn, including delayed stop replies. */
  if (strcmp(msg->chat_id, ROBOT_MUSIC_COMPANION_CHAT_ID) == 0)
    {
      RV_EVENT("[ROBOT-MUSIC] companion reply text-only; no TTS\n");
      return;
    }

  owned = strncmp(msg->chat_id, "voice_run_", 10) == 0 ||
          strncmp(msg->chat_id, "voice_action_", 13) == 0;
  pthread_mutex_lock(&g_monitor.lock);
  run_id = g_monitor.run_id;
  snprintf(expected, sizeof(expected), "voice_run_%lu", g_monitor.run_id);
  snprintf(action, sizeof(action), "voice_action_%lu", g_monitor.run_id);
  if (owned)
    {
      accept = g_monitor.enabled && !g_voice_stop_requested &&
               !g_monitor.reply_claimed &&
               (g_monitor.phase == RV_MON_GREETING ||
                g_monitor.phase == RV_MON_BUSY) &&
               (strcmp(msg->chat_id, expected) == 0 ||
                strcmp(msg->chat_id, action) == 0);
    }
  else if (g_monitor.enabled)
    {
      accept = false; /* legacy/unrelated VOICE reply cannot enter this run */
    }
  if (owned && accept) {
    rv_lock_reply_locked(run_id);
    commit_ms = g_monitor.reply_start_ms;
    g_monitor.reply_claimed = true;
    g_monitor.agent_text[0] = 0;
    stream_owns_final = rv_stream_accept_final_locked(msg->content, run_id);
  }
  pthread_mutex_unlock(&g_monitor.lock);

  if (accept)
    {
      unsigned long long now = robot_voice_now_ms();
      RV_VERBOSE("[RV-AGENT] reply_received mono_ms=%llu run=%lu text_bytes=%zu commit_to_reply_ms=%llu\n",
             now, owned ? run_id : 0, strlen(msg->content),
             commit_ms && now >= commit_ms ? now - commit_ms : 0);
      int ret = 0;
      if (owned && robot_music_media_owns_reply && robot_music_media_owns_reply(run_id)) {
        RV_EVENT("[RV-MUSIC] music owns I2S; start confirmation remains text, run=%lu\n",run_id);
        rv_reply_done_for_run(true,run_id);
        return;
      }
      if (stream_owns_final)
        return; /* The detached speech owner completes this round after drain. */
      if (msg->content[0])
        ret = robot_voice_tts_submit(msg->content, true, owned ? run_id : 0);
      else {
        g_voice_reply_pending = false;
        robot_world_state_set_conversation_active(false);
        rv_reply_done(true);
      }
      if (ret < 0)
        {
          printf("[RV-TTS] reply queue failed rc=%d\n", ret);
          g_voice_reply_pending = false;
          rv_reply_done(true);
        }
    }
  else
    {
      RV_VERBOSE("[RV-VOICE] discard stale Agent reply chat_id=%s\n", msg->chat_id);
    }
}

/* All queue mutations occur under g_monitor.lock. Conversation previews may
 * be replaced; an in-flight job is never changed or freed by the producer. */
static void rv_enqueue_locked(struct rv_segmenter_s *seg, bool final)
{
  struct rv_monitor_job_s *job;
  size_t bytes = seg->used;
  size_t audio_start = 0;
  unsigned int i;
  if (g_monitor.phase == RV_MON_WAKE || g_monitor.phase == RV_MON_GREETING)
    {
      if (final && g_monitor.phase == RV_MON_WAKE)
        RV_EVENT("[唤醒] 待机语音段 segment=%lu 未提交 ASR\n", seg->speech_id);
      return;
    }

  /* Every final includes all captured samples, including trailing quiet. */
  for (i = 0; i < g_monitor.queued; )
    {
      if (!g_monitor.queue[i]->final &&
          g_monitor.queue[i]->speech_id == seg->speech_id)
        {
          rv_queue_remove_locked(i);
        }
      else
        {
          i++;
        }
    }
  if (g_monitor.queued == ROBOT_VOICE_ASR_QUEUE ||
      bytes > ROBOT_VOICE_QUEUED_PCM_MAX - g_monitor.queued_pcm -
                g_monitor.inflight_pcm)
    {
      for (i = 0; i < g_monitor.queued; )
        {
          if (!g_monitor.queue[i]->final)
            {
              rv_queue_remove_locked(i);
            }
          else
            {
              i++;
            }
        }
    }
  if (g_monitor.queued == ROBOT_VOICE_ASR_QUEUE ||
      bytes > ROBOT_VOICE_QUEUED_PCM_MAX - g_monitor.queued_pcm -
                g_monitor.inflight_pcm)
    {
      g_monitor.dropped++;
      if (final)
        {
          g_monitor.pending_failed = true;
          printf("[RV-TURN] audio queue full: reject incomplete turn; repeat required\n");
        }
      return;
    }
  job = calloc(1, sizeof(*job));
  if (job != NULL)
    {
      job->pcm = malloc(bytes);
    }
  if (job == NULL || job->pcm == NULL)
    {
      rv_job_free(job);
      g_monitor.dropped++;
      g_monitor.pending_failed |= final;
      printf("[RV-TURN] audio allocation failed final=%u\n", (unsigned int)final);
      return;
    }
  memcpy(job->pcm, seg->speech + audio_start, bytes);
  job->pcm_len = bytes;
  job->audio_start = audio_start;
  job->voice_end = seg->last_voice_end;
  job->epoch = g_monitor.epoch;
  job->speech_id = seg->speech_id;
  job->final = final;
  job->wake_context = seg->wake_context;
  if (final)
    {
      job->final_seq = ++g_monitor.submitted_final;
    }
  job->created_ms = robot_voice_now_ms();
  job->voice_end_ms = g_monitor.last_voice_ms;
  g_monitor.queue[g_monitor.queued++] = job;
  g_monitor.queued_pcm += bytes;
  RV_VERBOSE("[RV-MON] segment=%lu seq=%lu %s %s bytes=%zu queued=%u inflight=%u\n",
         job->speech_id, job->final_seq, "command",
         final ? "final" : "preview", bytes, g_monitor.queued,
         g_monitor.in_flight);
  pthread_cond_broadcast(&g_monitor.changed);
}

static void rv_pre_push(struct rv_segmenter_s *seg, const uint8_t *pcm,
                         size_t len)
{
  size_t cap = RV_MS_BYTES(ROBOT_VOICE_PRE_ROLL_MS);
  size_t first;

  if (len > cap)
    {
      pcm += len - cap;
      len = cap;
    }
  first = cap - seg->pre_head;
  if (first > len)
    {
      first = len;
    }
  memcpy(seg->pre + seg->pre_head, pcm, first);
  memcpy(seg->pre, pcm + first, len - first);
  seg->pre_head = (seg->pre_head + len) % cap;
  seg->pre_used += len;
  if (seg->pre_used > cap)
    {
      seg->pre_used = cap;
    }
}

static void rv_segment_reset(struct rv_segmenter_s *seg)
{
  seg->active = false;
  seg->wake_context = false;
  seg->used = 0;
  seg->voiced = 0;
  seg->silence = 0;
  seg->stored_silence = 0;
  seg->onset = 0;
  seg->interrupt_onset = 0;
  seg->overflow = false;
  seg->last_voice_end = seg->early_voice_end = 0;
  g_monitor.capture_active = false;
  pthread_cond_broadcast(&g_monitor.changed);
}

static void rv_confirm_interrupt_locked(unsigned long speech_id);

static void rv_wake_buffer_reset_locked(struct rv_segmenter_s *seg)
{
  seg->wake_head = seg->wake_used = seg->wake_voiced = 0;
  seg->wake_total = 0;
  memset(seg->wake_votes, 0, sizeof(seg->wake_votes));
  g_monitor.wake_total_samples = g_monitor.wake_buffered_samples = 0;
  g_monitor.wake_handoff_pending = false;
}

static void rv_wake_buffer_push_locked(struct rv_segmenter_s *seg,
                                       const uint8_t *pcm, bool voiced)
{
  const size_t cap = RV_MS_BYTES(ROBOT_VOICE_WAKE_CONTEXT_MS);
  uint8_t *ring = seg->speech + RV_MS_BYTES(ROBOT_VOICE_SEGMENT_MAX_MS) - cap;
  size_t slot = seg->wake_head / ROBOT_AFE_BYTES;
  if (seg->wake_used == cap && seg->wake_votes[slot])
    seg->wake_voiced -= ROBOT_AFE_BYTES;
  seg->wake_votes[slot] = voiced;
  if (voiced) seg->wake_voiced += ROBOT_AFE_BYTES;
  memcpy(ring + seg->wake_head, pcm, ROBOT_AFE_BYTES);
  seg->wake_head = (seg->wake_head + ROBOT_AFE_BYTES) % cap;
  if (seg->wake_used < cap) seg->wake_used += ROBOT_AFE_BYTES;
  seg->wake_total += ROBOT_AFE_BYTES;
  g_monitor.wake_total_samples = seg->wake_total / 2;
  g_monitor.wake_buffered_samples = seg->wake_used / 2;
}

static void rv_wake_handoff_locked(struct rv_segmenter_s *seg)
{
  const size_t cap = RV_MS_BYTES(ROBOT_VOICE_WAKE_CONTEXT_MS);
  size_t oldest = (seg->wake_head + cap - seg->wake_used) % cap;
  size_t first = cap - oldest;
  if (first > seg->wake_used) first = seg->wake_used;
  const uint8_t *ring = seg->speech + RV_MS_BYTES(ROBOT_VOICE_SEGMENT_MAX_MS) - cap;
  /* Standby uses the allocation's tail. The head is a disjoint destination,
   * so linearization copies each retained byte once, with no new allocation.
   * Never trim samples using the classifier or the model window endpoint. */
  memcpy(seg->speech, ring + oldest, first);
  memcpy(seg->speech + first, ring, seg->wake_used - first);
  seg->used = seg->wake_used;
  seg->voiced = seg->wake_voiced;
  if (seg->voiced < RV_MS_BYTES(ROBOT_VOICE_VAD_MIN_MS))
    seg->voiced = RV_MS_BYTES(ROBOT_VOICE_VAD_MIN_MS);
  seg->active = true;
  seg->wake_context = true;
  seg->speech_id = ++g_monitor.speech_id;
  seg->last_voice_end = seg->used;
  seg->early_voice_end = 0;
  seg->silence = seg->stored_silence = seg->onset = 0;
  g_monitor.capture_active = true;
  g_monitor.wake_handoff_pending = false;
  g_monitor.wake_total_samples = g_monitor.wake_buffered_samples = 0;
  RV_EVENT("[RV-WAKE-HANDOFF] segment=%lu retained=%zu bytes; full wake+command PCM\n",
           seg->speech_id, seg->used);
}

static int rv_monitor_pcm(const struct robot_voice_afe_frame_s *frame, void *arg)
{
  struct rv_segmenter_s *seg = arg;
  unsigned long long now = robot_voice_now_ms();
  const uint8_t *pcm = (const uint8_t *)frame->audio;
  const size_t len = ROBOT_AFE_BYTES;
  unsigned int level = frame->level;
  unsigned int threshold = frame->threshold;
  unsigned int votes = 0;
  bool voiced = frame->voiced;

  pthread_mutex_lock(&g_monitor.lock);
  g_monitor.frames++;
  if (!g_monitor.capture_ready) {
    g_monitor.capture_ready = true;
    pthread_cond_broadcast(&g_monitor.changed);
  }
  if (g_monitor.in_flight != 0)
    {
      g_monitor.frames_during_asr++;
    }
  if (seg->epoch != g_monitor.epoch)
    {
      rv_segment_reset(seg);
      seg->pre_used = 0;
      seg->epoch = g_monitor.epoch;
      rv_wake_buffer_reset_locked(seg);
    }
  int wake_ret = robot_wakeword_feed((const uint8_t *)frame->kws, len, g_monitor.epoch,
    !g_monitor.stop && !g_voice_stop_requested && !g_monitor.muted &&
    now >= g_monitor.echo_until_ms && g_monitor.phase == RV_MON_WAKE);
  if (wake_ret < 0)
    { pthread_mutex_unlock(&g_monitor.lock); return wake_ret; }
  if (g_monitor.stop || g_voice_stop_requested || g_monitor.muted ||
      now < g_monitor.echo_until_ms)
    {
      rv_segment_reset(seg);
      seg->pre_used = 0;
      rv_wake_buffer_reset_locked(seg);
      pthread_mutex_unlock(&g_monitor.lock);
      return 0;
    }

  if (g_monitor.phase == RV_MON_WAKE)
    {
      rv_wake_buffer_push_locked(seg, pcm, voiced);
      pthread_mutex_unlock(&g_monitor.lock);
      return 0;
    }
  if (g_monitor.wake_handoff_pending && seg->wake_used)
    rv_wake_handoff_locked(seg);

  if (!seg->active)
    {
      votes = robot_voice_onset_vote(&seg->onset, voiced);
      if (voiced && votes >= ROBOT_VOICE_VAD_START_FRAMES)
        {
          size_t cap = RV_MS_BYTES(ROBOT_VOICE_PRE_ROLL_MS);
          size_t oldest = (seg->pre_head + cap - seg->pre_used) % cap;
          size_t first = cap - oldest;
          if (first > seg->pre_used)
            {
              first = seg->pre_used;
            }
          memcpy(seg->speech, seg->pre + oldest, first);
          memcpy(seg->speech + first, seg->pre, seg->pre_used - first);
          seg->used = seg->pre_used;
          seg->voiced = (votes - 1u) * len;
          seg->silence = 0;
          seg->stored_silence = 0;
          seg->active = true;
          g_monitor.capture_active = true;
          seg->speech_id = ++g_monitor.speech_id;
          g_monitor.last_voice_ms = now;
          g_monitor.chat_activity_ms = now;
          if (g_monitor.phase == RV_MON_BUSY)
            {
              /* The onset vote opens capture. A separate duration gate
               * below preempts the old run only after sufficient actual
               * speech evidence; non-speech / hangover frames add nothing. */
              RV_EVENT("[RV-TURN] 候选语音 segment=%lu run=%lu level=%u threshold=%u；起音候选，ASR 确认有效指令且 Flash 尚未回复时优先处理\n",
                seg->speech_id, g_monitor.run_id, level, threshold);
            }
          seg->last_voice_end = seg->early_voice_end = 0;
          RV_VERBOSE("[RV-VAD] start segment=%lu level=%u threshold=%u\n",
                 seg->speech_id, level, threshold);
        }
    }

  if (seg->active)
    {
      size_t remaining = RV_MS_BYTES(ROBOT_VOICE_SEGMENT_MAX_MS) - seg->used;
      size_t take = len < remaining ? len : remaining;
      bool final;

      memcpy(seg->speech + seg->used, pcm, take);
      seg->used += take;
      if (voiced)
        {
          seg->voiced += take;
          seg->last_voice_end = seg->used;
          seg->silence = 0;
          seg->stored_silence = 0;
          g_monitor.last_voice_ms = now;
          g_monitor.chat_activity_ms = now;
        }
      else
        {
          seg->silence += take;
          seg->stored_silence += take;
        }
      /* Buffer limits must never turn half a sentence into a command. */
      if ((take < len || seg->used == RV_MS_BYTES(ROBOT_VOICE_SEGMENT_MAX_MS)) &&
          voiced && !seg->overflow)
        {
          seg->overflow = true;
          g_monitor.pending_failed = true;
          RV_VERBOSE("[RV-TURN] utterance exceeds %u ms; repeat in shorter turns\n",
                 ROBOT_VOICE_SEGMENT_MAX_MS);
        }
      /* Once full, count quiet in wall-audio bytes even without appending. */
      if (!voiced && take < len)
        {
          seg->silence += len - take;
        }
      final = seg->silence >= RV_MS_BYTES(ROBOT_VOICE_VAD_END_MS);
      if (final)
        {
          RV_VERBOSE("[RV-VAD] end segment=%lu last_voice_ms=%llu end_ms=%llu\n",
                 seg->speech_id, g_monitor.last_voice_ms, now);
        }
      if (final)
        {
          if (!seg->overflow &&
              seg->voiced >= RV_MS_BYTES(ROBOT_VOICE_VAD_MIN_MS))
            {
              rv_enqueue_locked(seg, true);
            }
          else if (!seg->overflow)
            {
              g_monitor.vad_rejected++;
              RV_VERBOSE("[RV-VAD-GATE] reject segment=%lu voiced_ms=%zu required_ms=%u\n",
                         seg->speech_id,
                         seg->voiced * 1000u /
                           (ROBOT_VOICE_CAPTURE_RATE * 2u),
                         ROBOT_VOICE_VAD_MIN_MS);
            }
          rv_segment_reset(seg);
        }

    }

  rv_pre_push(seg, pcm, len);
  pthread_mutex_unlock(&g_monitor.lock);
  return 0;
}

/* DMA blocks are deliberately unchanged. This adapter emits complete 20 ms
 * frames and keeps a partial frame across every 1024-byte RX boundary. */
static int rv_monitor_frame(const uint8_t *pcm, size_t len, void *arg)
{
  struct rv_segmenter_s *seg = arg;
  struct robot_voice_afe_stats_s current;
  struct timespec begin = {0}, end = {0};
  bool suppressed;
  int ret;
  if (!seg || !seg->afe || !pcm || !len || (len & 1u) || len > 1024)
    return -EINVAL;
  pthread_mutex_lock(&g_monitor.lock);
  suppressed = g_monitor.stop || g_voice_stop_requested || g_monitor.muted ||
               robot_voice_now_ms() < g_monitor.echo_until_ms;
  pthread_mutex_unlock(&g_monitor.lock);
  robot_voice_afe_suppress(seg->afe, suppressed);
  clock_gettime(CLOCK_MONOTONIC, &begin);
  ret = robot_voice_afe_push(seg->afe, pcm, len, rv_monitor_pcm, seg);
  clock_gettime(CLOCK_MONOTONIC, &end);
  uint64_t start = (uint64_t)begin.tv_sec * 1000000u + begin.tv_nsec / 1000u;
  uint64_t finish = (uint64_t)end.tv_sec * 1000000u + end.tv_nsec / 1000u;
  uint64_t elapsed = finish >= start ? finish - start : 0;
  robot_voice_afe_stats(seg->afe, &current);
  pthread_mutex_lock(&g_monitor.lock);
#define RV_AFE_ADD(field) g_monitor.afe_stats.field += current.field - seg->afe_previous.field
  RV_AFE_ADD(input_samples);
  RV_AFE_ADD(output_samples);
  RV_AFE_ADD(frames);
  RV_AFE_ADD(speech_frames);
  RV_AFE_ADD(clipped_samples);
  RV_AFE_ADD(overruns);
#undef RV_AFE_ADD
  g_monitor.afe_stats.partial_samples = current.partial_samples;
  g_monitor.afe_stats.raw_level = current.raw_level;
  g_monitor.afe_stats.clean_level = current.clean_level;
  g_monitor.afe_stats.noise_level = current.noise_level;
  g_monitor.afe_stats.threshold = current.threshold;
  if (current.max_process_us > g_monitor.afe_stats.max_process_us)
    g_monitor.afe_stats.max_process_us = current.max_process_us;
  if (elapsed > g_monitor.afe_sink_max_us)
    g_monitor.afe_sink_max_us = elapsed > UINT32_MAX ? UINT32_MAX : elapsed;
  if (elapsed > len * 1000000u / (ROBOT_VOICE_CAPTURE_RATE * 2u))
    g_monitor.afe_sink_overruns++;
  pthread_mutex_unlock(&g_monitor.lock);
  seg->afe_previous = current;
  return ret;
}

/* This is the sole activation entry point. It accepts only a fresh acoustic
 * score from the current capture epoch, outside TTS/echo guards. */
static bool rv_accept_acoustic_wake(const struct robot_wakeword_result_s *result)
{
  bool accepted = false;
  pthread_mutex_lock(&g_monitor.lock);
  if (result->triggered && g_monitor.enabled && !g_monitor.stop &&
      !g_voice_stop_requested && !g_monitor.muted &&
      robot_voice_now_ms() >= g_monitor.echo_until_ms &&
      result->epoch == g_monitor.epoch && g_monitor.phase == RV_MON_WAKE)
    {
      size_t window = robot_wakeword_window_samples();
      uint64_t oldest = g_monitor.wake_total_samples - g_monitor.wake_buffered_samples;
      uint64_t start = result->end_sample > window ? result->end_sample - window : 0;
      if (result->end_sample &&
          (result->end_sample > g_monitor.wake_total_samples || start < oldest))
        {
          RV_EVENT("[RV-WAKE-HANDOFF] stale/overrun wake context; repeat complete command\n");
          pthread_mutex_unlock(&g_monitor.lock);
          return false;
        }
      g_monitor.phase = RV_MON_LISTEN;
      g_monitor.wake_handoff_pending = g_monitor.wake_buffered_samples != 0;
      g_monitor.chat_activity_ms = robot_voice_now_ms();
      rv_listening_expression_locked();
      robot_world_state_note_user_activity();
      RV_EVENT("[唤醒][TFLite] score=%.6f threshold=%.6f\n",
               (double)result->score, (double)result->threshold);
      pthread_cond_broadcast(&g_monitor.changed);
      accepted = true;
    }
  pthread_mutex_unlock(&g_monitor.lock);
  if (accepted && robot_music_media_suspend)
    {
      int pause_ret = robot_music_media_suspend();
      if (pause_ret < 0) RV_EVENT("[RV-TASK] wake music handoff failed rc=%d\n", pause_ret);
    }
  return accepted;
}

static void *rv_acoustic_worker(void *arg)
{
  (void)arg;
  while (!g_voice_stop_requested)
    {
      struct robot_wakeword_result_s result;
      int ret = robot_wakeword_poll(&result);
      if (ret < 0)
        {
          RV_EVENT("[语音错误] TFLite 唤醒推理失败 rc=%d，停止监听\n", ret);
          g_voice_stop_requested = true;
          pthread_mutex_lock(&g_monitor.lock);
          g_monitor.acoustic_error = ret;
          g_monitor.stop = true;
          pthread_cond_broadcast(&g_monitor.changed);
          pthread_mutex_unlock(&g_monitor.lock);
          break;
        }
      if (ret > 0)
        {
          (void)rv_accept_acoustic_wake(&result);
        }
      usleep(ROBOT_WAKEWORD_POLL_MS * 1000);
    }
  return NULL;
}

static void *rv_capture_worker(void *arg)
{
  struct rv_segmenter_s seg;
  int ret = -ENOMEM;

  (void)arg;
  memset(&seg, 0, sizeof(seg));
  seg.pre = malloc(RV_MS_BYTES(ROBOT_VOICE_PRE_ROLL_MS));
  seg.speech = malloc(RV_MS_BYTES(ROBOT_VOICE_SEGMENT_MAX_MS));
  if (seg.pre != NULL && seg.speech != NULL)
    {
      unsigned int restarts = 0;
      do {
        unsigned long before;
        pthread_mutex_lock(&g_monitor.lock);
        before = g_monitor.frames;
        pthread_mutex_unlock(&g_monitor.lock);
        seg.afe = robot_voice_afe_create(g_monitor.afe_mode);
        if (!seg.afe) { ret = -ENOMEM; break; }
        memset(&seg.afe_previous, 0, sizeof(seg.afe_previous));
        ret = robot_voice_capture_stream(rv_monitor_frame, &seg,
                                          &g_voice_stop_requested);
        pthread_mutex_lock(&g_monitor.lock);
        g_monitor.afe_stats.discarded_samples += seg.afe_previous.partial_samples;
        g_monitor.afe_stats.partial_samples = 0;
        pthread_mutex_unlock(&g_monitor.lock);
        robot_voice_afe_destroy(seg.afe);
        seg.afe = NULL;
        if (g_voice_stop_requested ||
            (ret != -EIO && ret != -ETIMEDOUT && ret != -EPIPE)) break;
        pthread_mutex_lock(&g_monitor.lock);
        /* A recovered RX stream starts a fresh segment; an interrupted user
         * turn must never be executed from its surviving half. */
        if (seg.active || g_monitor.wake_handoff_pending)
          g_monitor.pending_failed = true;
        g_monitor.afe_restarts++;
        (void)robot_wakeword_feed(NULL, 0, g_monitor.epoch, false);
        g_monitor.capture_active = false;
        if (g_monitor.frames - before >= 32) restarts = 0;
        pthread_mutex_unlock(&g_monitor.lock);
        if (restarts++ >= CONTEST_VOICE_CAPTURE_RESTART_LIMIT) break;
        pthread_mutex_lock(&g_monitor.lock);
        rv_segment_reset(&seg); seg.pre_used = 0;
        rv_wake_buffer_reset_locked(&seg);
        pthread_mutex_unlock(&g_monitor.lock);
        RV_VERBOSE("[RV-RECOVER] RX restart=%u rc=%d; queued human audio retained\n",
               restarts, ret);
        usleep(200000);
      } while (!g_voice_stop_requested);
    }
  free(seg.pre);
  free(seg.speech);
  pthread_mutex_lock(&g_monitor.lock);
  g_monitor.capture_result = ret;
  g_monitor.capture_exited = true;
  pthread_cond_broadcast(&g_monitor.changed);
  pthread_mutex_unlock(&g_monitor.lock);
  RV_VERBOSE("[RV-MON] capture exit rc=%d\n", ret);
  return NULL;
}

/* ASR progress is used only to cancel stale conversation requests. */
static int rv_asr_check_cancel(const char *text, void *arg)
{
  struct rv_monitor_job_s *job = arg;
  int ret;
  (void)text;
  pthread_mutex_lock(&g_monitor.lock);
  ret = g_monitor.stop || g_voice_stop_requested ||
        job->epoch != g_monitor.epoch ?
        -ECANCELED : 0;
  pthread_mutex_unlock(&g_monitor.lock);
  return ret;
}

/* Finals take priority. At most one speculative preview may use a worker;
 * the other remains available for a newly completed user utterance. */
static int rv_asr_pick_locked(void)
{
  for (unsigned int i = 0; i < g_monitor.queued; i++)
    if (g_monitor.queue[i]->final) return i;
  if (!g_monitor.queued) return -1;
  for (unsigned int i = 0; i < ROBOT_VOICE_ASR_WORKERS; i++)
    if (g_monitor.active_jobs[i] && !g_monitor.active_jobs[i]->final) return -1;
  return 0;
}

static void *rv_asr_worker(void *arg)
{
  unsigned int worker = (unsigned int)(uintptr_t)arg;

  for (;;)
    {
      struct rv_monitor_job_s *job;
      char key[256];
      unsigned long frames;
      unsigned long long request_at;

      pthread_mutex_lock(&g_monitor.lock);
      int pick = -1;
      while (!g_monitor.stop) {
        pick = rv_asr_pick_locked();
        if (pick >= 0 && robot_voice_now_ms() >= g_monitor.next_request_ms) break;
        rv_wait_locked(50);
      }
      if (g_monitor.stop)
        {
          pthread_mutex_unlock(&g_monitor.lock);
          break;
        }
      job = g_monitor.queue[pick];
      g_monitor.queued--;
      memmove(&g_monitor.queue[pick], &g_monitor.queue[pick + 1],
              (g_monitor.queued - (unsigned int)pick) * sizeof(g_monitor.queue[0]));
      g_monitor.queued_pcm -= job->pcm_len;
      g_monitor.inflight_pcm += job->pcm_len;
      g_monitor.in_flight++;
      g_monitor.final_inflight[worker] = job->final;
      g_monitor.active_jobs[worker] = job;
      frames = g_monitor.frames;
      g_monitor.next_request_ms = robot_voice_now_ms() +
                                 ROBOT_VOICE_ASR_INTERVAL_MS;
      pthread_mutex_unlock(&g_monitor.lock);

      request_at = robot_voice_now_ms();
      job->request_ms = request_at;
      job->ret = voice_get_api_key(key, sizeof(key));
      if (job->ret == 0)
        {
          RV_VERBOSE("[RV-ASR] async worker=%u segment=%lu bytes=%zu begin\n",
                 worker, job->speech_id, job->pcm_len);
          job->ret = mimo_asr_recognize_progress(key, job->pcm, job->pcm_len,
                     job->text, sizeof(job->text), rv_asr_check_cancel, job);
        }
      job->returned_ms = robot_voice_now_ms();
      memset(key, 0, sizeof(key));
      if (job->ret == -EIO || job->ret == -ECONNRESET ||
          job->ret == -ENETDOWN || job->ret == -EHOSTUNREACH)
        robot_voice_http_close_idle();
      pthread_mutex_lock(&g_monitor.lock);
      if (job->ret == 0)
        {
          RV_TEXT("[RV-ASR] segment=%lu final=%u text=%s\n",
                 job->speech_id, (unsigned int)job->final, job->text);
        }
      g_monitor.active_jobs[worker] = NULL;
      g_monitor.in_flight--;
      g_monitor.inflight_pcm -= job->pcm_len;
      g_monitor.final_inflight[worker] = false;
      RV_VERBOSE("[RV-ASR] async worker=%u segment=%lu rc=%d queue_ms=%llu request_ms=%llu latency_ms=%llu "
             "captured_frames_while_waiting=%lu\n", worker, job->speech_id,
             job->ret, request_at - job->created_ms, robot_voice_now_ms() - request_at,
             robot_voice_now_ms() - job->created_ms, g_monitor.frames - frames);
      if (job->ret == -EAGAIN)
        {
          g_monitor.next_request_ms = robot_voice_now_ms() +
                                     ROBOT_VOICE_ASR_BACKOFF_MS;
          printf("[RV-MON] ASR quota backoff\n");
        }
      else if (job->ret < 0 && job->ret != -ECANCELED)
        {
          unsigned long long retry = robot_voice_now_ms() + 1000;
          if (g_monitor.next_request_ms < retry)
            {
              g_monitor.next_request_ms = retry;
            }
        }
      if (g_monitor.stop || job->epoch != g_monitor.epoch)
        {
          rv_job_free(job);
        }
      else
        {
          /* Reserve a slot for the earliest missing final. Without this,
           * one slow worker + many fast later results can deadlock both ASR
           * workers behind a full reorder buffer. */
          while (!g_monitor.stop && job->epoch == g_monitor.epoch &&
                 g_monitor.completed >=
                   (job->final && job->final_seq !=
                    g_monitor.consumed_final + 1 ?
                    RV_RESULT_COUNT - 1 : RV_RESULT_COUNT))
            {
              rv_wait_locked(50);
            }
          if (!g_monitor.stop && job->epoch == g_monitor.epoch)
            {
              free(job->pcm);
              job->pcm = NULL;
              g_monitor.results[g_monitor.completed++] = job;
            }
          else
            {
              rv_job_free(job);
            }
        }
      pthread_cond_broadcast(&g_monitor.changed);
      pthread_mutex_unlock(&g_monitor.lock);
    }
  robot_voice_http_close_idle();
  return NULL;
}

static const char *rv_wake_command_tail(const char *text)
{
  const char *start = rv_skip_separators(text), *tail;
  /* Only the full configured wake phrase may be found inside cached context.
   * A bare product name is removed only at the beginning, never mid-command. */
  for (const char *p = start; *p; p++)
    {
      if (((unsigned char)*p & 0xc0) == 0x80) continue;
      const char *previous = p;
      if (p != start)
        {
          previous--;
          while (previous > start && ((unsigned char)*previous & 0xc0) == 0x80)
            previous--;
        }
      if ((p == start || rv_skip_separators(previous) == p) &&
          (tail = rv_phrase_tail(p, "你好openvela")) != NULL) return tail;
    }
  tail = rv_phrase_tail(start, "openvela");
  if (tail) return tail;
  tail = rv_phrase_tail(start, "你好");
  if (tail && !*tail) return tail;
  return start;
}

static bool rv_end_chat(const char *text)
{
  static const char *const commands[] = {
    "退出语音对话模式", "结束语音对话模式", "退出语音对话", "结束语音对话",
    "退出语音模式", "结束语音模式", "退出语音聊天", "结束语音聊天",
    "退出聊天", "结束聊天", "退出对话", "结束对话", "停止聊天", "停止对话"
  };
  static const char *const prefixes[] = {"可以帮我", "可以", "请帮我", "请你", "帮我", "请", ""};
  for (size_t p = 0; p < sizeof(prefixes) / sizeof(*prefixes); p++) {
    const char *body = rv_phrase_tail(text, prefixes[p]);
    if (!body) continue;
    for (size_t i = 0; i < sizeof(commands) / sizeof(*commands); i++) {
      const char *tail = rv_phrase_tail(body, commands[i]);
      if (!tail) continue;
      if (!*tail) return true;
      const char *suffix = rv_phrase_tail(tail, "吧");
      if (suffix && !*suffix) return true;
    }
  }
  return false;
}

static void rv_return_to_wake_locked(void)
{
  rv_cancel_active_speech_locked();
  g_monitor.phase = RV_MON_WAKE;
  g_monitor.reply_locked = false;
  g_monitor.muted = false;
  g_monitor.epoch++;
  g_monitor.run_id++;
  rv_run_token_advance_locked();
  rv_clear_locked();
  /* Old-epoch jobs may finish later. They must not leave sequence holes that
   * block the first command after the next acoustic wake. */
  g_monitor.submitted_final = g_monitor.consumed_final = 0;
  g_monitor.wake_handoff_pending = false;
  g_monitor.wake_total_samples = 0;
  g_monitor.wake_buffered_samples = 0;
  g_monitor.capture_active = false;
  g_monitor.reply_claimed = true;
  g_monitor.agent_text[0] = 0;
  g_voice_reply_pending = false;
  robot_world_state_set_conversation_active(false);
  rv_listening_expression_locked();
}

/* Called under monitor lock only for a valid final ASR before Flash replies.
 * Preserve the new utterance's PCM and queues; retire only the old reply. */
static void rv_confirm_interrupt_locked(unsigned long speech_id)
{
  if (g_monitor.phase != RV_MON_BUSY || g_monitor.reply_locked) return;
  unsigned long old_run = g_monitor.run_id;
  rv_run_token_advance_locked();
  g_monitor.run_id++;
  g_monitor.phase = RV_MON_LISTEN;
  g_monitor.reply_claimed = true;
  g_monitor.agent_text[0] = '\0';
  g_monitor.pending_failed = false;
  g_monitor.pending_error = 0;
  g_monitor.stream_busy = false;
  free(g_monitor.spoken_prefix);
  g_monitor.spoken_prefix = NULL;
  g_voice_reply_pending = false;
  rv_cancel_active_speech_locked();
  RV_EVENT("[RV-TURN] 新指令优先 segment=%lu run=%lu->%lu；取消旧解析/旧回复\n",
    speech_id, old_run, g_monitor.run_id);
}

/* Final ASR jobs are consumed in capture order, not HTTP completion order.
 * Previews never activate the device or execute their partial transcript. */
static void rv_process_result(struct rv_monitor_job_s *job)
{
  const char *command;
  size_t used;
  size_t add;
  unsigned long exit_run = 0;

  pthread_mutex_lock(&g_monitor.lock);
  if (job->epoch != g_monitor.epoch || g_voice_stop_requested ||
      g_monitor.reply_locked)
    {
      goto done;
    }
  /* Defensive boundary: even an injected/stale ASR transcript containing
   * the wake phrase cannot open a conversation. */
  if (g_monitor.phase == RV_MON_WAKE || g_monitor.phase == RV_MON_GREETING)
    goto done;
  if (job->final)
    {
      g_monitor.consumed_final = job->final_seq;
      if (job->ret == 0)
        RV_DIALOGUE("[ASR][最终] segment=%lu %s\n", job->speech_id,
                    job->text[0] ? job->text : "（空文本）");
      if (job->returned_ms) {
        RV_EVENT("[耗时][ASR] segment=%lu 录音快照就绪到返回=%lldms 请求到返回=%lldms 末次人声到返回=%lldms rc=%d\n",
          job->speech_id, rv_elapsed_ms(job->created_ms, job->returned_ms),
          rv_elapsed_ms(job->request_ms, job->returned_ms),
          rv_elapsed_ms(job->voice_end_ms, job->returned_ms), job->ret);
      }
    }
  if (job->ret < 0)
    {
      if (job->ret == -ECANCELED)
        goto done;
      if (job->final)
        {
          RV_EVENT("[ASR] segment=%lu 识别失败 rc=%d\n", job->speech_id, job->ret);
        }
      if (job->final && g_monitor.phase == RV_MON_BUSY)
        {
          /* A failed candidate must not replace a reply that is still
           * downloading/playing, or queue a spurious network notice after
           * that reply. Normal listening ASR failures retain their policy. */
          RV_EVENT("[RV-TURN] 候选语音识别失败 segment=%lu rc=%d；保留旧回复\n",
            job->speech_id, job->ret);
          goto done;
        }
      if (job->final && g_monitor.phase != RV_MON_WAKE &&
          g_monitor.phase != RV_MON_GREETING)
        {
          g_monitor.pending_failed = true;
          g_monitor.pending_error = job->ret;
          g_monitor.asr_failures++;
          printf("[RV-RECOVER] ASR failures=%u rc=%d; listening remains enabled\n",
                 g_monitor.asr_failures, job->ret);
          printf("[RV-TURN] ASR final failed rc=%d; partial text will not execute\n",
                 job->ret);
        }
      goto done;
    }
  if (job->final) g_monitor.asr_failures = 0;
  if (!job->final) goto done;
  command = job->wake_context ? rv_wake_command_tail(job->text) : job->text;
  if (*rv_skip_separators(command) == '\0')
    {
      if (g_monitor.phase == RV_MON_BUSY)
        RV_EVENT("[RV-TURN] 候选语音无有效文本 segment=%lu；保留旧回复\n",
          job->speech_id);
      goto done;
    }
  if (rv_end_chat(command)) {
    rv_return_to_wake_locked();
    exit_run = g_monitor.run_id;
    RV_EVENT("[语音] 已识别退出指令，返回等待唤醒\n");
    goto done;
  }
  used = strlen(g_monitor.pending_text);
  add = strlen(command);
  if (add + (used != 0) >= sizeof(g_monitor.pending_text) - used)
    {
      g_monitor.pending_failed = true;
      printf("[RV-TURN] text overflow; partial text will not execute\n");
      goto done;
    }
  rv_confirm_interrupt_locked(job->speech_id);
  if (used == 0)
    {
      g_monitor.pending_started_ms = job->created_ms;
    }
  else
    {
      g_monitor.pending_text[used++] = ' ';
    }
  memcpy(g_monitor.pending_text + used, command, add + 1);
  RV_VERBOSE("[RV-TURN] append seq=%lu pending_bytes=%zu phase=%d\n",
         job->final_seq, strlen(g_monitor.pending_text), g_monitor.phase);
done:
  if (g_monitor.phase != RV_MON_WAKE && g_monitor.phase != RV_MON_GREETING &&
      job->epoch == g_monitor.epoch && !job->final && !job->ret && !job->audio_start &&
      (!g_monitor.cached_preview || job->speech_id > g_monitor.cached_preview->speech_id ||
       (job->speech_id == g_monitor.cached_preview->speech_id &&
        job->voice_end >= g_monitor.cached_preview->voice_end))) {
    rv_job_free(g_monitor.cached_preview); g_monitor.cached_preview = job; job = NULL;
  }
  pthread_cond_broadcast(&g_monitor.changed);
  pthread_mutex_unlock(&g_monitor.lock);
  rv_job_free(job);
  if (exit_run)
    (void)robot_voice_tts_submit(ROBOT_PROMPT_CHAT_EXIT, false, exit_run);
}

/* Controller only. No Agent/TTS call occurs while holding the capture lock.
 * Waiting for all submitted finals also protects the wake+compound-command
 * case when the two HTTP workers return out of order. */
static void rv_dispatch_pending(void)
{
  char routed[ROBOT_VOICE_TEXT_CAP];
  bool failed;
  int error;
  bool notify = true;
  unsigned long run_id;
  unsigned long long now = robot_voice_now_ms();

  pthread_mutex_lock(&g_monitor.lock);
  if (g_voice_stop_requested || g_monitor.phase != RV_MON_LISTEN ||
      g_monitor.wake_handoff_pending || g_monitor.muted || g_monitor.capture_active ||
      now < g_monitor.last_voice_ms ||
      now - g_monitor.last_voice_ms < ROBOT_VOICE_TURN_END_MS ||
      g_monitor.consumed_final != g_monitor.submitted_final ||
      (g_monitor.pending_text[0] == '\0' && !g_monitor.pending_failed))
    {
      pthread_mutex_unlock(&g_monitor.lock);
      return;
    }
  failed = g_monitor.pending_failed;
  error = g_monitor.pending_error;
  run_id = g_monitor.run_id;
  if (failed && error) {
    notify = true;
    g_monitor.error_notice_ms = now;
    printf("[RV-RECOVER] turn_failed cause=asr-service rc=%d notify=%u\n", error, notify);
  }
  snprintf(routed, sizeof(routed), "%s", failed ?
           (error ? ROBOT_PROMPT_NETWORK_ERROR : ROBOT_PROMPT_NOT_HEARD) :
           g_monitor.pending_text);
  g_monitor.pending_text[0] = '\0';
  g_monitor.pending_failed = false;
  g_monitor.pending_error = 0;
  if (!failed && rv_end_chat(routed))
    {
      rv_return_to_wake_locked();
      run_id = g_monitor.run_id;
      pthread_mutex_unlock(&g_monitor.lock);
      (void)robot_voice_tts_submit(ROBOT_PROMPT_CHAT_EXIT, false, run_id);
      return;
    }
  g_monitor.phase = RV_MON_BUSY;
  g_monitor.reply_locked = false;
  g_monitor.listening_announced = false;
  rv_run_token_advance_locked();
  run_id = ++g_monitor.run_id;
  g_monitor.reply_claimed = false;
  g_monitor.reply_start_ms = now;
  g_monitor.reply_user_end_ms = g_monitor.last_voice_ms;
  RV_VERBOSE("[RV-TURN] commit mono_ms=%llu run=%lu end_to_commit_ms=%llu complete=%u text_bytes=%zu\n",
         now, g_monitor.run_id, now - g_monitor.last_voice_ms, (unsigned int)!failed, strlen(routed));
  RV_TEXT("[RV-TURN] text=%s\n", routed);
  pthread_mutex_unlock(&g_monitor.lock);
  if (failed)
    {
      pthread_mutex_lock(&g_monitor.lock);
      g_monitor.reply_claimed = true;
      g_monitor.agent_text[0] = 0;
      pthread_mutex_unlock(&g_monitor.lock);
      if (robot_voice_tts_submit(routed, true, run_id) < 0)
        {
          rv_reply_done(true);
        }
    }
  else if (contest_voice_route_text(routed) < 0)
    {
      rv_reply_done(true);
    }
}

static bool rv_chat_idle_locked(unsigned long long now)
{
  if (g_monitor.phase != RV_MON_LISTEN || g_monitor.wake_handoff_pending || g_monitor.muted ||
      g_monitor.capture_active || g_monitor.pending_text[0] ||
      g_monitor.pending_failed || g_monitor.consumed_final != g_monitor.submitted_final ||
      now < g_monitor.chat_activity_ms ||
      now - g_monitor.chat_activity_ms < ROBOT_VOICE_CHAT_IDLE_MS) return false;
  rv_return_to_wake_locked();
  RV_EVENT("[语音] 连续 %u 秒无输入，退出对话，继续等待唤醒词\n",
           (unsigned int)(ROBOT_VOICE_CHAT_IDLE_MS / 1000));
  return true;
}

static void rv_agent_timeout_locked(void)
{
  g_voice_reply_pending = false;
  /* Retire only the Agent identity. Human audio remains valid across an
   * Agent timeout; a later result must not erase the user's next request. */
  rv_run_token_advance_locked();
  g_monitor.run_id++;
  g_monitor.reply_claimed = true;
  g_monitor.agent_text[0] = 0;
  g_monitor.reply_locked = false;
  g_monitor.muted = false;
  rv_cancel_active_speech_locked();
  g_monitor.phase = g_monitor.phase == RV_MON_GREETING ? RV_MON_WAKE : RV_MON_LISTEN;
  robot_world_state_set_conversation_active(false);
  rv_listening_expression_locked();
  printf("[RV-WAKE] Agent reply timed out; pending human input retained\n");
}

static struct rv_monitor_job_s *rv_take_result_locked(void)
{
  unsigned int i;
  for (i = 0; i < g_monitor.completed; i++)
    {
      struct rv_monitor_job_s *candidate = g_monitor.results[i];
      if (!candidate->final || candidate->final_seq ==
          g_monitor.consumed_final + 1)
        {
          g_monitor.completed--;
          memmove(&g_monitor.results[i], &g_monitor.results[i + 1],
                  (g_monitor.completed - i) * sizeof(g_monitor.results[0]));
          pthread_cond_broadcast(&g_monitor.changed);
          return candidate;
        }
    }
  return NULL;
}

static int rv_thread_start(pthread_t *thread, void *(*entry)(void *),
                            void *arg, size_t stack, int priority)
{
  pthread_attr_t attr;
  int ret = pthread_attr_init(&attr);

  if (ret != 0)
    {
      return -ret;
    }
  ret = rv_thread_attributes(&attr, stack, priority);
  if (ret == 0)
    {
      ret = pthread_create(thread, &attr, entry, arg);
    }
  pthread_attr_destroy(&attr);
  return ret == 0 ? 0 : -ret;
}

/* Controller waits for a real RX frame, rather than assuming pthread_create
 * means the microphone is ready. No Flash request is needed for this greeting. */
static int rv_startup_announce(void)
{
  unsigned long run_id;
  unsigned long long start = robot_voice_now_ms();
  int ret = 0;
  pthread_mutex_lock(&g_monitor.lock);
  while (!g_monitor.capture_ready && !g_monitor.capture_exited &&
         !g_monitor.stop && !g_voice_stop_requested &&
         robot_voice_now_ms() - start < ROBOT_VOICE_READY_TIMEOUT_MS)
    rv_wait_locked(50);
  if (g_voice_stop_requested || g_monitor.stop) ret = -ECANCELED;
  else if (g_monitor.capture_exited)
    ret = g_monitor.capture_result < 0 ? g_monitor.capture_result : -EIO;
  else if (!g_monitor.capture_ready) ret = -ETIMEDOUT;
  run_id = g_monitor.run_id;
  g_monitor.reply_start_ms = robot_voice_now_ms();
  pthread_mutex_unlock(&g_monitor.lock);
  if (ret < 0) return ret;
  RV_EVENT("[语音] 系统准备就绪，开始问好\n");
  ret = robot_voice_tts_submit(ROBOT_PROMPT_STARTUP, true, run_id);
  if (ret < 0) {
    printf("[语音错误] 启动问好提交失败 rc=%d\n", ret);
    rv_reply_done(true); /* keep wake listening available after TTS failure */
  }
  return 0;
}

static void *voice_monitor_worker(void *arg)
{
  pthread_t asr[ROBOT_VOICE_ASR_WORKERS];
  pthread_t capture, acoustic;
  unsigned int started = 0;
  bool capture_started = false, acoustic_started = false, wake_initialized = false;
  bool network_session = false;
  unsigned long long stats_at = robot_voice_now_ms();
  int ret = 0;

  (void)arg;
  pthread_mutex_lock(&g_monitor.lock);
  if (g_voice_stop_requested)
    {
      pthread_mutex_unlock(&g_monitor.lock);
      goto cleanup;
    }
  rv_clear_locked();
  g_monitor.enabled = true;
  g_monitor.stop = false;
  g_monitor.muted = true; /* startup speech cannot become a wake/command */
  g_monitor.capture_ready = false;
  g_monitor.listening_announced = g_monitor.chat_announced = false;
  g_monitor.capture_exited = false;
  g_monitor.capture_result = 0;
  g_monitor.acoustic_error = 0;
  g_monitor.epoch++;
  rv_run_token_advance_locked();
  g_monitor.run_id++;
  g_monitor.speech_id = 0;
  g_monitor.frames = 0;
  g_monitor.frames_during_asr = 0;
  memset(&g_monitor.afe_stats, 0, sizeof(g_monitor.afe_stats));
  g_monitor.afe_restarts = 0;
  g_monitor.afe_sink_max_us = 0;
  g_monitor.afe_sink_overruns = 0;
  g_monitor.vad_rejected = 0;
  g_monitor.dropped = 0;
  g_monitor.in_flight = 0;
  memset(g_monitor.final_inflight, 0, sizeof(g_monitor.final_inflight));
  g_monitor.capture_active = false;
  g_monitor.queued_pcm = 0;
  g_monitor.inflight_pcm = 0;
  g_monitor.submitted_final = 0;
  g_monitor.consumed_final = 0;
  g_monitor.next_request_ms = 0;
  g_monitor.asr_failures = 0;
  g_monitor.error_notice_ms = 0;
  g_monitor.reply_claimed = false;
  g_monitor.reply_locked = false;
  g_monitor.agent_text[0] = 0;
  g_monitor.echo_until_ms = 0;
  g_monitor.last_voice_ms = robot_voice_now_ms();
  g_monitor.chat_activity_ms = g_monitor.last_voice_ms;
  g_monitor.phase = RV_MON_GREETING;
  g_monitor.reply_start_ms = robot_voice_now_ms();
  g_monitor.reply_user_end_ms = 0; /* startup greeting has no human end time */
  pthread_mutex_unlock(&g_monitor.lock);

  if (robot_voice_http_session_begin) {
    ret = robot_voice_http_session_begin();
    if (ret < 0) goto cleanup;
    network_session = true;
  }
  ret = robot_wakeword_init();
  if (ret < 0)
    {
      RV_EVENT("[语音错误] TFLite 唤醒初始化失败 rc=%d，请检查模型导入和 Arena 配置\n", ret);
      goto cleanup;
    }
  wake_initialized = true;
  if (robot_wakeword_window_samples() > RV_MS_BYTES(ROBOT_VOICE_WAKE_CONTEXT_MS) / 2)
    { ret = -ENOTSUP; goto cleanup; }
  ret = rv_thread_start(&acoustic, rv_acoustic_worker, NULL,
                        ROBOT_WAKEWORD_STACK_BYTES, ROBOT_VOICE_CONSUMER_PRIORITY);
  if (ret < 0) goto cleanup;
  acoustic_started = true;

  for (started = 0; started < ROBOT_VOICE_ASR_WORKERS; started++)
    {
      ret = rv_thread_start(&asr[started], rv_asr_worker,
                            (void *)(uintptr_t)started, CONTEST_VOICE_ASR_STACK,
                            ROBOT_VOICE_NETWORK_PRIORITY);
      if (ret < 0)
        {
          goto cleanup;
        }
    }
  ret = rv_thread_start(&capture, rv_capture_worker, NULL,
                        CONTEST_VOICE_RECORD_STACK, ROBOT_VOICE_CAPTURE_PRIORITY);
  if (ret < 0)
    {
      goto cleanup;
    }
  capture_started = true;

  if (g_voice_stop_requested)
    {
      goto cleanup;
    }

  g_voice_reply_pending = false;
  robot_world_state_set_conversation_active(false);
  ret = rv_startup_announce();
  if (ret < 0) {
    if (ret != -ECANCELED) printf("[语音错误] 采集设备未就绪 rc=%d\n", ret);
    goto cleanup;
  }
  RV_VERBOSE("[RV-WAKE] TFLite armed; no ASR upload before acoustic wake\n");

  RV_VERBOSE("[RV-MON] started workers=%u queue=%u; capture never joins ASR\n",
         ROBOT_VOICE_ASR_WORKERS, ROBOT_VOICE_ASR_QUEUE);
  while (!g_voice_stop_requested)
    {
      if (robot_voice_wifi_poll) robot_voice_wifi_poll();
      struct rv_monitor_job_s *result = NULL;
      const char *local_notice = NULL;
      unsigned long notice_run = 0;
      unsigned long long now = robot_voice_now_ms();

      pthread_mutex_lock(&g_monitor.lock);
      if (g_monitor.capture_exited)
        {
          ret = g_monitor.capture_result;
          RV_VERBOSE("[RV-HEALTH] capture_exited rc=%d; stopping controller\n", ret);
          pthread_mutex_unlock(&g_monitor.lock);
          break;
        }
      if (rv_chat_idle_locked(now)) {
        local_notice = ROBOT_PROMPT_CHAT_EXIT;
        notice_run = g_monitor.run_id;
      }
      if ((g_monitor.phase == RV_MON_GREETING ||
           g_monitor.phase == RV_MON_BUSY) && !g_monitor.stream_busy &&
          !g_voice_speaking &&
          now >= g_monitor.reply_start_ms &&
          now - g_monitor.reply_start_ms >= ROBOT_VOICE_REPLY_TIMEOUT_MS)
        {
          rv_agent_timeout_locked();
          local_notice = ROBOT_PROMPT_NETWORK_ERROR;
          notice_run = g_monitor.run_id;
        }
      if (now - stats_at >= 10000)
        {
          if (ROBOT_VOICE_LOG_VERBOSE) rv_afe_diag_locked();
          RV_VERBOSE("[RV-HEALTH] phase=%d frames=%lu during_asr=%lu queued=%u inflight=%u "
                 "dropped=%lu vad_rejected=%lu asr_failures=%u capture_active=%u muted=%u\n",
                 g_monitor.phase, g_monitor.frames, g_monitor.frames_during_asr,
                 g_monitor.queued, g_monitor.in_flight, g_monitor.dropped,
                 g_monitor.vad_rejected,
                 g_monitor.asr_failures, g_monitor.capture_active, g_monitor.muted);
          stats_at = now;
        }
      result = rv_take_result_locked();
      if (result == NULL)
        {
          rv_wait_locked(30);
        }
      pthread_mutex_unlock(&g_monitor.lock);
      if (local_notice)
        (void)robot_voice_tts_submit(local_notice, false, notice_run);
      if (result != NULL)
        {
          rv_process_result(result);
        }
      rv_dispatch_pending();
      if (robot_task_resume_if_idle) robot_task_resume_if_idle();
    }

cleanup:
  g_voice_stop_requested = true;
  pthread_mutex_lock(&g_monitor.lock);
  if (ret == 0 && g_monitor.acoustic_error < 0) ret = g_monitor.acoustic_error;
  g_monitor.stop = true;
  g_monitor.epoch++;
  rv_run_token_advance_locked();
  rv_clear_locked();
  pthread_cond_broadcast(&g_monitor.changed);
  pthread_mutex_unlock(&g_monitor.lock);
  if (capture_started)
    {
      pthread_join(capture, NULL);
    }
  if (acoustic_started) pthread_join(acoustic, NULL);
  if (wake_initialized) robot_wakeword_close();
  /* HTTPS is synchronous: safely finish in-flight calls before freeing jobs.
   * Never pthread_cancel a TLS worker or reuse its snapshot during stop. */
  while (started != 0)
    {
      pthread_join(asr[--started], NULL);
    }
  pthread_mutex_lock(&g_monitor.lock);
  g_monitor.enabled = false;
  g_monitor.reply_locked = false;
  g_monitor.muted = false;
  rv_end_conversation_locked();
  g_monitor.listening_announced = false;
  rv_clear_locked();
  pthread_mutex_unlock(&g_monitor.lock);
  if (network_session && robot_voice_http_session_end) robot_voice_http_session_end();
  g_voice_reply_pending = false;
  robot_world_state_set_conversation_active(false);
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
  pthread_mutex_lock(&g_voice_lock);
  g_voice_running = 0;
  g_voice_worker_exited = true;
  if (!g_voice_explicit_stop && ret < 0)
    {
      g_voice_restart_pending = true;
    }
  contest_voice_set_state(CONTEST_VOICE_IDLE);
  pthread_mutex_unlock(&g_voice_lock);
  RV_VERBOSE("[RV-MON] stopped rc=%d; all workers joined\n", ret);
  RV_EVENT("[语音] 已停止语音监听 rc=%d\n", ret);
  return NULL;
}

static void *voice_record_worker(void *arg)
{
  size_t pcm_cap = (size_t)ROBOT_VOICE_CAPTURE_RATE * 2 *
                   CONTEST_VOICE_CAPTURE_SECONDS;
  uint8_t *pcm = malloc(pcm_cap);
  char text[ROBOT_VOICE_TEXT_CAP];
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
      RV_VERBOSE("[RV-VOICE] capture returned rc=%d bytes=%zu\n", ret, pcm_len);
      if (ret < 0 || g_voice_stop_requested)
        {
          break;
        }
      if (g_voice_speaking)
        {
          continue;
        }
      ret = robot_voice_afe_recording(pcm, pcm_len, g_monitor.afe_mode);
      if (ret < 0) { printf("[RV-AFE] finite preprocessing failed rc=%d\n", ret); break; }
      contest_voice_set_state(CONTEST_VOICE_ASR);
      RV_VERBOSE("[RV-VOICE] invoking ASR bytes=%zu on dedicated worker\n", pcm_len);
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

      ret = contest_voice_route_text(text);
      if (ret < 0)
        {
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

  pthread_mutex_unlock(&g_voice_lock);
  RV_VERBOSE("[contest_voice] worker cleanup\n");
  return NULL;
}

/* Called by Agent through a strong symbol: pull both the voice provider and
 * the existing FFmpeg compatibility object out of the static archive.
 * Link anchors must remain passive: no threads, audio or network work. */
void robot_voice_link_anchor(void)
{
  robot_ffmpeg_amix_compat_link_anchor();
  robot_music_media_link_anchor();
  extern void robot_voice_music_route_tool_anchor(void) __attribute__((weak));
  if(robot_voice_music_route_tool_anchor)robot_voice_music_route_tool_anchor();
}

int voice_channel_init(void)
{
  pthread_mutex_lock(&g_voice_init_lock);
  if (g_voice_initialized)
    {
      /* Re-assert provider registration after a failed/partial startup.
       * The registry is idempotent and this keeps Feishu/Agent actions from
       * observing a stale tool set. */
      (void)robot_expression_register_tool();
      (void)robot_motion_register_tool();
      (void)robot_music_register_tool();
      pthread_mutex_unlock(&g_voice_init_lock);
      return 0;
    }
  robot_mimo_agent_adapter_anchor();
  extern void robot_voice_wifi_link_anchor(void);
  robot_voice_wifi_link_anchor();
  g_voice_running = 0;
  g_voice_speaking = 0;
  g_voice_oneshot = 0;
  g_voice_thread_valid = 0;
  g_voice_reply_pending = false;
  g_voice_stop_requested = false;
  g_voice_worker_exited = true;
  g_voice_restart_pending = false;
  g_voice_explicit_stop = false;
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
  RV_VERBOSE("[contest_voice] RV-NET active repair disabled; official network manager owns wlan0\n");

  if (robot_skill_installer_ensure() < 0)
    {
      printf("[contest_voice] proactive skill unavailable\n");
    }
  (void)robot_expression_register_tool();
  (void)robot_motion_register_tool();
  (void)robot_music_register_tool();
  if (robot_proactive_start() < 0)
    printf("[ROBOT-PROACTIVE] runtime unavailable; music playback remains available\n");
  RV_EVENT("[ROBOT-SKILL] music_ready=%d companion_ready=%d; "
           "music events inline, voice/idle use Agent summary\n",
           robot_skill_installer_music_ready(),
           robot_skill_installer_companion_ready());

  if (!g_cli_tap_registered)
    {
      if (mbus_tap_register(AGENT_CHAN_CLI,
                            robot_voice_cli_outbound_tap, NULL) == OK)
        {
          g_cli_tap_registered = true;
          RV_VERBOSE("[RV-EXPR] CLI outbound tap registered\n");
        }
      else
        {
          printf("[RV-EXPR] CLI outbound tap unavailable\n");
        }
    }
  if (!g_voice_tap_registered)
    {
      if (mbus_tap_register(AGENT_CHAN_VOICE,
                            robot_voice_outbound_tap, NULL) == OK)
        {
          g_voice_tap_registered = true;
        }
      else
        {
          printf("[RV-WAKE] voice outbound tap unavailable\n");
        }
    }
  /* No Media daemon or wlan0 operation is started by voice initialization.
   * TTS uses the contest-local sentence-buffer path. */
  RV_VERBOSE("[contest_voice] no Media/Wi-Fi bootstrap in voice init\n");
  g_voice_initialized = g_voice_tap_registered;
  pthread_mutex_unlock(&g_voice_init_lock);
  printf("[RV-VOICE-PROVIDER] contest TFLite voice ready; ASR=%s TTS=%s tap=%u\n",
         g_voice_asr_backend, g_voice_tts_backend, g_voice_tap_registered);
  return g_voice_tap_registered ? 0 : -EIO;
}

static int voice_channel_start_mode(int oneshot)
{
  pthread_mutex_lock(&g_voice_lock);
  if (g_voice_thread_valid && g_voice_worker_exited)
    {
      /* The monitor can terminate itself after a transient capture error.
       * Reap that completed thread before creating the replacement. */
      pthread_t stale = g_voice_thread;
      if (!pthread_equal(pthread_self(), stale))
        {
          (void)pthread_join(stale, NULL);
        }
      g_voice_thread_valid = 0;
    }
  if (g_voice_thread_valid)
    {
      pthread_mutex_unlock(&g_voice_lock);
      return -EBUSY;
    }
  g_voice_running = 1;
  g_voice_oneshot = oneshot;
  g_voice_reply_pending = false;
  g_voice_stop_requested = false;
  g_voice_restart_pending = false;
  g_voice_explicit_stop = false;
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
                                      oneshot ? voice_record_worker :
                                                voice_monitor_worker, NULL);
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
  RV_VERBOSE("[contest_voice] voice_channel_start mode=%s\n",
         oneshot ? "once" : "continuous");
  return 0;
}

int voice_channel_start(void)
{
  char key[256];
  int ret = voice_channel_init();
  if (ret < 0) return ret;

  if (strcmp(g_voice_asr_backend, "mimo-v2.5-asr") != 0)
    {
      return -ENOSYS;
    }
  if (strcmp(g_voice_tts_backend, "mimo-v2.5-tts") != 0 ||
      !g_voice_tap_registered)
    {
      printf("[RV-WAKE] configure TTS and initialize voice channel first\n");
      return -ENOSYS;
    }
  ret = voice_get_api_key(key, sizeof(key));
  memset(key, 0, sizeof(key));
  if (ret < 0)
    {
      return ret;
    }
  ret = robot_expression_init();
  if (ret < 0)
    printf("[RV-EXPR] voice_start display init failed rc=%d\n", ret);
  ret = voice_channel_start_mode(0);
  if (ret == 0)
    printf("[RV-VOICE-PROVIDER] voice_start accepted; local TFLite controller starting\n");
  return ret;
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
      /* A previous voice session may have left idle Agent/TLS sockets behind.
       * Retire them even when the monitor is already stopped so a later
       * Feishu/CLI music search starts with a fresh transport. */
      robot_voice_http_close_idle();
      vela_tls_pool_cleanup();
      return 0;
    }
  g_voice_explicit_stop = true;
  g_voice_stop_requested = true;
  g_voice_reply_pending = false;
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
  state = g_voice_state;
  contest_voice_set_state(CONTEST_VOICE_STOPPING);
  tid = g_voice_thread;
  join_needed = !pthread_equal(pthread_self(), tid);
  pthread_mutex_unlock(&g_voice_lock);

  pthread_mutex_lock(&g_monitor.lock);
  if (g_monitor.enabled)
    {
      g_monitor.stop = true;
      g_monitor.epoch++;
      rv_clear_locked();
      pthread_cond_broadcast(&g_monitor.changed);
    }
  pthread_mutex_unlock(&g_monitor.lock);

  RV_VERBOSE("[contest_voice] stop requested state=%s\n",
         contest_voice_state_name(state));
  if (join_needed)
    {
      RV_VERBOSE("[contest_voice] waiting capture callbacks and worker cleanup\n");
      pthread_join(tid, NULL);
      /* Finish already-started speech before a new voice_start is allowed. */
      pthread_mutex_lock(&g_voice_speak_lock);
      pthread_mutex_unlock(&g_voice_speak_lock);
    }

  pthread_mutex_lock(&g_voice_lock);
  while (g_tts_jobs != 0)
    {
      pthread_cond_wait(&g_tts_jobs_changed, &g_voice_lock);
    }
  if (g_voice_worker_exited)
    {
      g_voice_thread_valid = 0;
      g_voice_oneshot = 0;
    }
  pthread_mutex_unlock(&g_voice_lock);
  robot_voice_http_close_idle();
  vela_tls_pool_cleanup();
  RV_VERBOSE("[contest_voice] voice_channel_stop state=%s\n",
         contest_voice_state_name(g_voice_state));
  return 0;
}

/* A capture-side EIO must not leave the Agent permanently without wake
 * listening.  Restart only an unexpected monitor exit; an explicit
 * voice_stop remains authoritative. */
static int rv_restart_voice_after_failure(void)
{
  unsigned int attempt;
  int ret = -EIO;

  for (attempt = 1; attempt <= 5; attempt++)
    {
      bool pending;

      pthread_mutex_lock(&g_voice_lock);
      pending = !g_voice_explicit_stop &&
                g_voice_restart_pending;
      pthread_mutex_unlock(&g_voice_lock);
      if (!pending)
        {
          return 0;
        }

      ret = voice_channel_start();
      if (ret == 0)
        {
          RV_EVENT("[RV-RECOVER] voice monitor restarted after capture failure\n");
          return 0;
        }

      if (ret != -EBUSY && ret != -EAGAIN && ret != -EIO)
        {
          break;
        }
      usleep(200 * 1000);
    }

  RV_EVENT("[RV-RECOVER] voice monitor restart failed rc=%d\n", ret);
  return ret;
}

int voice_channel_pause_for_wifi(void)
{
  int token;
  pthread_mutex_lock(&g_voice_lock);
  token = g_voice_thread_valid ? (g_voice_oneshot ? 2 : 1) : 0;
  pthread_mutex_unlock(&g_voice_lock);
  if (token)
    {
      printf("[RV-WIFI] pausing voice workers before Wi-Fi configuration\n");
      int ret = voice_channel_stop();
      if (ret < 0) return ret;
      printf("[RV-WIFI] voice workers stopped for Wi-Fi configuration\n");
    }
  return token;
}

void voice_channel_resume_after_wifi(int token)
{
  /* Resume continuous local wake listening. A one-shot command must never
   * be recorded/executed a second time as a side effect of set_wifi. */
  if (token == 1)
    {
      int ret = voice_channel_start();
      if (ret < 0)
        printf("[RV-WIFI] voice resume failed rc=%d; run voice_start\n", ret);
    }
}

/* openvela schedules the Flash reader, bounded TTS download workers and playback
 * independently. Complete text sentences feed bounded PCM rings; live mode
 * publishes decoded chunks before synthesis EOS. No network/speaker wait
 * holds the text/queue locks. */
#define RV_SENTENCE_PCM_LIMIT       ROBOT_VOICE_TTS_SENTENCE_PCM
#define RV_SENTENCE_PREFETCH_STACK  ROBOT_VOICE_TTS_WORKER_STACK

struct rv_llm_speech_s
{
  pthread_mutex_t lock;
  pthread_cond_t changed;
  pthread_t thread;
  unsigned long run_id;
  char *text;
  char *prior_prefix;
  char *retry_text;
  size_t retry_used;
  size_t used, cursor, played_end;
  size_t phrase_ready_end;
  unsigned long long phrase_wait_since_ms;
  bool eos, cancel, first_logged, final_claimed, hold;
  unsigned int references; /* HTTP reader + detached speech owner. */
  unsigned long run_token;
  unsigned long long started_ms;
  int result;
};

/* Called with monitor lock. Never wait for speech on the Agent/HTTP task. */
static bool rv_stream_accept_final_locked(const char *text, unsigned long run_id)
{
  struct rv_llm_speech_s *s = g_monitor.active_speech;
  bool accepted = false;
  if (!s || s->run_id != run_id) return false;
  pthread_mutex_lock(&s->lock);
  if (s->eos && !s->cancel && s->text && !strcmp(s->text, text)) {
    s->final_claimed = true;
    accepted = true;
  } else s->cancel = true;
  pthread_cond_broadcast(&s->changed);
  pthread_mutex_unlock(&s->lock);
  return accepted;
}

static void rv_llm_speech_put(struct rv_llm_speech_s *s)
{
  bool last;
  pthread_mutex_lock(&s->lock);
  last = --s->references == 0;
  pthread_mutex_unlock(&s->lock);
  if (last) {
    pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->lock);
    free(s->retry_text); free(s->prior_prefix); free(s->text); free(s);
  }
}

static char *rv_llm_source_next(struct rv_llm_speech_s *, size_t *, int *);
static bool rv_llm_source_cancelled(struct rv_llm_speech_s *);

struct rv_tts_pipeline_s;

struct rv_sentence_job_s
{
  char *text;
  uint8_t *pcm;
  size_t pcm_bytes;
  size_t pcm_capacity;
  size_t stream_head, stream_tail, stream_count;
  bool published, producer_done;
  size_t retry_skip_bytes;
  unsigned long long retry_deadline_ms;
  int synthesis_result;
  bool cancel_on_stop;
  unsigned int sequence;
  unsigned long long tts_start_ms, ready_ms, first_pcm_ms;
  unsigned long run_id;
  struct rv_llm_speech_s *source;
  size_t source_end;
  struct rv_tts_pipeline_s *pipeline;
};

struct rv_tts_pipeline_s
{
  pthread_mutex_t lock;
  pthread_mutex_t next_lock; /* Serializes text claims, never held during I/O. */
  pthread_cond_t changed;
  size_t cursor;
  unsigned int sequence, workers_left;
  struct rv_llm_speech_s *source;
  const char *text;
  const char *key;
  unsigned long run_id;
  unsigned long run_token;
  struct rv_sentence_job_s *ready[ROBOT_VOICE_TTS_READY_SLOTS];
  size_t reserved;
  size_t buffered_bytes, buffered_source_end;
  unsigned int buffered_windows;
  unsigned int count;
  bool stop, done;
  int result;
};

static void rv_cancel_active_speech_locked(void)
{
  struct rv_llm_speech_s *s = g_monitor.active_speech;
  if (!s) return;
  pthread_mutex_lock(&s->lock);
  s->cancel = true;
  pthread_cond_broadcast(&s->changed);
  pthread_mutex_unlock(&s->lock);
}

static bool rv_pipeline_cancelled(const struct rv_tts_pipeline_s *p)
{
  return !p || (p->run_id != 0 && g_voice_stop_requested) ||
    (p->run_token && p->run_token != rv_run_token_load()) ||
    rv_llm_source_cancelled(p->source);
}

static bool rv_sentence_is_closer(const char *s, size_t left)
{
  static const char *const utf8_closers[] =
    { "”", "’", "」", "』", "》", "】", "）", "〕", NULL };
  size_t i;

  if (left == 0)
    {
      return false;
    }

  if (s[0] == '"' || s[0] == '\'' || s[0] == ')'
      || s[0] == ']' || s[0] == '}')
    {
      return true;
    }

  for (i = 0; utf8_closers[i] != NULL; i++)
    {
      size_t n = strlen(utf8_closers[i]);
      if (left >= n && memcmp(s, utf8_closers[i], n) == 0)
        {
          return true;
        }
    }

  return false;
}

static size_t rv_sentence_punctuation(const char *s, size_t left,
                                      size_t absolute, size_t begin)
{
  if (left == 0)
    {
      return 0;
    }

  if (s[0] == '\n' || s[0] == '!' || s[0] == '?' || s[0] == ';')
    {
      return 1;
    }

  if (s[0] == '.')
    {
      /* Keep a decimal number such as 3.14 in one spoken segment. */
      if (absolute > begin && left > 1 && s[1] >= '0' && s[1] <= '9' &&
          s[-1] >= '0' && s[-1] <= '9')
        {
          return 0;
        }
      return 1;
    }

  if (left >= 3 &&
      (memcmp(s, "。", 3) == 0 || memcmp(s, "！", 3) == 0 ||
       memcmp(s, "？", 3) == 0 || memcmp(s, "；", 3) == 0))
    {
      return 3;
    }

  return 0;
}

/* Return the next bounded TTS phrase as an owned string. Brief clauses are
 * grouped until the target size; punctuation and UTF-8 boundaries are kept.
 * No text is discarded; only whitespace between separate requests is omitted. */
static char *rv_sentence_next(const char *text, size_t text_len,
                              size_t *cursor)
{
  size_t begin;
  size_t end;
  size_t last_punctuation = 0;
  size_t i;
  char *copy;

  if (text == NULL || cursor == NULL)
    {
      return NULL;
    }

  begin = *cursor;
  while (begin < text_len &&
         (text[begin] == ' ' || text[begin] == '\t' ||
          text[begin] == '\r' || text[begin] == '\n'))
    {
      begin++;
    }

  if (begin >= text_len)
    {
      *cursor = text_len;
      return NULL;
    }

  end = 0;
  for (i = begin; i < text_len; )
    {
      size_t punct = rv_sentence_punctuation(text + i, text_len - i, i,
                                               begin);
      if (punct == 0)
        {
          i++;
          continue;
        }

      i += punct;
      while (i < text_len)
        {
          size_t more = rv_sentence_punctuation(text + i, text_len - i, i,
                                                 begin);
          if (more != 0)
            {
              i += more;
            }
          else if (rv_sentence_is_closer(text + i, text_len - i))
            {
              i += (unsigned char)text[i] < 0x80 ? 1 : 3;
            }
          else
            {
              break;
            }
        }

      if (i - begin <= ROBOT_VOICE_TTS_SENTENCE_BYTES)
        {
          last_punctuation = i;
          if (i - begin >= ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES)
            {
              end = i;
              break;
            }
        }
      else
        {
          break;
        }
    }

  if (end == 0)
    {
      size_t available = text_len - begin;
      if (available <= ROBOT_VOICE_TTS_SENTENCE_BYTES)
        {
          /* At EOS the caller may pass a short trailing phrase with no stop
           * punctuation; include it with the preceding brief clause. */
          end = text_len;
        }
      else if (last_punctuation > begin &&
               last_punctuation - begin >=
                 ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES)
        {
          end = last_punctuation;
        }
      else
        {
          size_t limit = begin + ROBOT_VOICE_TTS_SENTENCE_BYTES;
          while (limit > begin && limit < text_len &&
                 ((unsigned char)text[limit] & 0xc0) == 0x80)
            {
              limit--;
            }
          size_t cut = limit;
          for (size_t k = begin + ROBOT_VOICE_TTS_SENTENCE_BYTES / 2;
               k < limit; k++)
            {
              if (text[k] == ' ' || text[k] == ',')
                {
                  cut = k + 1;
                }
              else if (k + 3 <= limit && !memcmp(text + k, "，", 3))
                {
                  cut = k + 3;
                }
            }
          end = cut;
        }
    }

  i = end;
  if (end < text_len)
    {
      while (end > begin &&
             (text[end - 1] == ' ' || text[end - 1] == '\t' ||
              text[end - 1] == '\r' || text[end - 1] == '\n'))
        {
          end--;
        }
      while (i < text_len &&
             (text[i] == ' ' || text[i] == '\t' ||
              text[i] == '\r' || text[i] == '\n'))
        {
          i++;
        }
    }

  if (end == text_len)
    {
      while (end > begin &&
             (text[end - 1] == ' ' || text[end - 1] == '\t' ||
              text[end - 1] == '\r' || text[end - 1] == '\n'))
        {
          end--;
        }
    }

  if (end <= begin)
    {
      *cursor = text_len;
      return NULL;
    }

  copy = malloc(end - begin + 1);
  if (copy == NULL)
    {
      return NULL;
    }

  memcpy(copy, text + begin, end - begin);
  copy[end - begin] = '\0';
  *cursor = end == text_len ? end : i;
  return copy;
}

/* This wait happens on the TTS prefetch thread, never the Flash reader.
 * Periodic wakeups also make voice_stop independent of another cloud delta. */
static bool rv_llm_phrase_ready_locked(struct rv_llm_speech_s *s,
                                       unsigned long long now_ms)
{
  size_t begin = s->cursor;
  bool short_boundary = false;
  size_t last_boundary = 0;

  while (begin < s->used &&
         (s->text[begin] == ' ' || s->text[begin] == '\t' ||
          s->text[begin] == '\r' || s->text[begin] == '\n'))
    {
      begin++;
    }

  if (s->eos || s->used - begin >= ROBOT_VOICE_TTS_SENTENCE_BYTES)
    {
      s->phrase_ready_end = s->used;
      s->phrase_wait_since_ms = 0;
      return true;
    }

  for (size_t i = begin; i < s->used; )
    {
      size_t punct;
      if (s->text[i] == '.' && i + 1 == s->used && i > begin &&
          s->text[i - 1] >= '0' && s->text[i - 1] <= '9')
        {
          break; /* A period at a delta edge may begin a decimal number. */
        }
      punct = rv_sentence_punctuation(s->text + i, s->used - i, i, begin);
      if (punct == 0)
        {
          i++;
          continue;
        }

      i += punct;
      while (i < s->used)
        {
          size_t more = rv_sentence_punctuation(s->text + i,
            s->used - i, i, begin);
          if (more != 0)
            {
              i += more;
            }
          else if (rv_sentence_is_closer(s->text + i, s->used - i))
            {
              i += (unsigned char)s->text[i] < 0x80 ? 1 : 3;
            }
          else
            {
              break;
            }
        }

      if (i - begin >= ROBOT_VOICE_TTS_CHUNK_TARGET_BYTES)
        {
          s->phrase_ready_end = i;
          s->phrase_wait_since_ms = 0;
          return true;
        }
      short_boundary = true;
      last_boundary = i;
    }

  if (!short_boundary)
    {
      s->phrase_wait_since_ms = 0;
      return false;
    }

  if (!s->phrase_wait_since_ms)
    {
      s->phrase_wait_since_ms = now_ms;
    }
  bool ready = now_ms >= s->phrase_wait_since_ms &&
               now_ms - s->phrase_wait_since_ms >=
                 ROBOT_VOICE_TTS_CHUNK_MERGE_WAIT_MS;
  if (ready) s->phrase_ready_end = last_boundary;
  return ready;
}

static char *rv_llm_source_next(struct rv_llm_speech_s *s, size_t *end, int *rc)
{
  char *text = NULL;
  pthread_mutex_lock(&s->lock);
  for (;;) {
    bool complete;
    bool prefix_pending = false;
    if (s->cancel || (s->run_id != 0 && g_voice_stop_requested) ||
        s->run_token != rv_run_token_load()) { *rc = -ECANCELED; break; }
    if (s->hold) goto wait_for_delta;
    if (s->prior_prefix) {
      size_t n = strlen(s->prior_prefix);
      size_t have = s->used < n ? s->used : n;
      bool match = !memcmp(s->text, s->prior_prefix, have);
      if (match && s->used < n && !s->eos) prefix_pending = true;
      else {
        if (match && s->used >= n) s->cursor = s->played_end = n;
        free(s->prior_prefix); s->prior_prefix = NULL;
      }
    }
    complete = rv_llm_phrase_ready_locked(s, robot_voice_now_ms());
    if (complete && !prefix_pending) {
      /* A timed-out short sentence must not swallow the next incomplete
       * clause just because its text has already arrived in another delta. */
      text = rv_sentence_next(s->text, s->phrase_ready_end, &s->cursor);
      if (text) { *end = s->cursor; s->phrase_wait_since_ms = 0; *rc = 0; break; }
      if (s->cursor < s->used) { *rc = -ENOMEM; break; }
      if (s->eos) { *rc = 1; break; } /* clean source EOF */
    }
wait_for_delta:;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += 50000000;
    if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&s->changed, &s->lock, &t);
  }
  pthread_mutex_unlock(&s->lock);
  return text;
}

static bool rv_llm_source_cancelled(struct rv_llm_speech_s *s)
{
  bool cancel;
  if (!s) return false;
  if (s->run_token != rv_run_token_load()) {
    pthread_mutex_lock(&s->lock); s->cancel = true;
    pthread_cond_broadcast(&s->changed); pthread_mutex_unlock(&s->lock);
    return true;
  }
  pthread_mutex_lock(&s->lock); cancel = s->cancel; pthread_mutex_unlock(&s->lock);
  return cancel;
}

static void rv_pipeline_wait_locked(struct rv_tts_pipeline_s *p)
{
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t); t.tv_nsec += 50000000;
  if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
  pthread_cond_timedwait(&p->changed, &p->lock, &t);
}

static bool rv_sentence_retry_expired(const struct rv_sentence_job_s *job)
{
  return job->retry_deadline_ms &&
         robot_voice_now_ms() >= job->retry_deadline_ms;
}

/* Keep retry backpressure inside the same deadline as the bounded HTTP call.
 * A timed wait is capped by the remaining retry budget, not a fixed 50 ms. */
static void rv_pipeline_wait_job_locked(struct rv_sentence_job_s *job)
{
  if (!job->retry_deadline_ms)
    {
      rv_pipeline_wait_locked(job->pipeline);
    }
  else
    {
      unsigned long long now = robot_voice_now_ms();
      unsigned long long left;
      struct timespec t;
      if (now >= job->retry_deadline_ms) return;
      left = job->retry_deadline_ms - now;
      if (left > 50) left = 50;
      clock_gettime(CLOCK_REALTIME, &t);
      t.tv_sec += (time_t)(left / 1000);
      t.tv_nsec += (long)(left % 1000) * 1000000L;
      if (t.tv_nsec >= 1000000000L)
        { t.tv_sec++; t.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&job->pipeline->changed, &job->pipeline->lock, &t);
    }
}

static int rv_pipeline_reserve(struct rv_sentence_job_s *job, size_t growth)
{
  struct rv_tts_pipeline_s *p = job->pipeline;
  if (!p) return 0;
  if (growth > ROBOT_VOICE_TTS_QUEUE_BYTES ||
      job->pcm_capacity > ROBOT_VOICE_TTS_QUEUE_BYTES - growth) return -EOVERFLOW;
  pthread_mutex_lock(&p->lock);
  while (!p->stop && !rv_pipeline_cancelled(p) &&
         growth > ROBOT_VOICE_TTS_QUEUE_BYTES - p->reserved)
    {
      if (rv_sentence_retry_expired(job))
        { pthread_mutex_unlock(&p->lock); return -ETIMEDOUT; }
      rv_pipeline_wait_job_locked(job);
    }
  if (rv_sentence_retry_expired(job))
    { pthread_mutex_unlock(&p->lock); return -ETIMEDOUT; }
  if (p->stop || rv_pipeline_cancelled(p)) {
    pthread_mutex_unlock(&p->lock); return -ECANCELED;
  }
  p->reserved += growth;
  pthread_mutex_unlock(&p->lock); return 0;
}

static void rv_pipeline_release(struct rv_tts_pipeline_s *p, size_t bytes)
{
  pthread_mutex_lock(&p->lock);
  p->reserved -= bytes;
  pthread_cond_broadcast(&p->changed); pthread_mutex_unlock(&p->lock);
}

/* Fixed-size per-sentence PCM ring. Only the TTS network task can block on
 * this backpressure; Flash continues appending its bounded text separately. */
static int rv_live_sentence_sink(const uint8_t *pcm, size_t len,
                                  struct rv_sentence_job_s *job)
{
  struct rv_tts_pipeline_s *p = job->pipeline;
  size_t offset = 0;
  size_t skip;
  if (rv_sentence_retry_expired(job)) return -ETIMEDOUT;
  /* A retried stream starts at the sentence beginning. Keep the already
   * accepted prefix in the live ring and skip that many regenerated bytes,
   * so a mid-stream retry resumes instead of audibly replaying the prefix. */
  skip = job->retry_skip_bytes < len ? job->retry_skip_bytes : len;
  skip &= ~(size_t)1;
  job->retry_skip_bytes -= skip;
  pcm += skip;
  len -= skip;
  if (!len) return 0;
  if (len > RV_SENTENCE_PCM_LIMIT - job->pcm_bytes) return -EOVERFLOW;
  if (!job->pcm) {
    int ret = rv_pipeline_reserve(job, ROBOT_VOICE_TTS_STREAM_RING_BYTES);
    if (ret < 0) return ret;
    job->pcm = malloc(ROBOT_VOICE_TTS_STREAM_RING_BYTES);
    if (!job->pcm) {
      rv_pipeline_release(p, ROBOT_VOICE_TTS_STREAM_RING_BYTES);
      return -ENOMEM;
    }
    job->pcm_capacity = ROBOT_VOICE_TTS_STREAM_RING_BYTES;
  }
  while (offset < len) {
    pthread_mutex_lock(&p->lock);
    while (job->stream_count == job->pcm_capacity && !p->stop &&
           !rv_pipeline_cancelled(p))
      {
        if (rv_sentence_retry_expired(job))
          { pthread_mutex_unlock(&p->lock); return -ETIMEDOUT; }
        rv_pipeline_wait_job_locked(job);
      }
    if (rv_sentence_retry_expired(job))
      { pthread_mutex_unlock(&p->lock); return -ETIMEDOUT; }
    if (p->stop || rv_pipeline_cancelled(p)) {
      pthread_mutex_unlock(&p->lock); return -ECANCELED;
    }
    size_t n = job->pcm_capacity - job->stream_count;
    if (n > job->pcm_capacity - job->stream_head) n = job->pcm_capacity - job->stream_head;
    if (n > len - offset) n = len - offset;
    memcpy(job->pcm + job->stream_head, pcm + offset, n);
    job->stream_head = (job->stream_head + n) % job->pcm_capacity;
    job->stream_count += n; job->pcm_bytes += n; offset += n;
    if (!job->ready_ms) job->ready_ms = robot_voice_now_ms();
    if (!job->published) {
      job->published = true;
      p->ready[p->count++] = job;
      RV_EVENT("[耗时][TTS首包] run=%lu seq=%u 请求到首PCM=%lldms\n",
        job->run_id, job->sequence, rv_elapsed_ms(job->tts_start_ms, job->ready_ms));
      RV_VERBOSE("[RV-TTS-PIPE] ready mono_ms=%llu run=%lu seq=%u pcm_bytes=%zu queued=%u reserved=%zu\n",
        job->ready_ms, job->run_id, job->sequence, job->pcm_bytes, p->count, p->reserved);
    }
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
  }
  return 0;
}

static int rv_sentence_pcm_sink(const uint8_t *pcm, size_t len, void *arg)
{
  struct rv_sentence_job_s *job = arg;
  size_t required;
  size_t capacity;
  uint8_t *next;

  if (job == NULL || (pcm == NULL && len != 0) || (len & 1u) != 0)
    {
      return -EINVAL;
    }

  if (rv_sentence_retry_expired(job)) return -ETIMEDOUT;

  if ((job->cancel_on_stop && job->run_id != 0 && g_voice_stop_requested) ||
      rv_pipeline_cancelled(job->pipeline))
    {
      return -ECANCELED;
    }

  if (len == 0)
    {
      return 0;
    }
  if (!job->first_pcm_ms) {
    job->first_pcm_ms = robot_voice_now_ms();
    if (!ROBOT_VOICE_TTS_LIVE)
      RV_EVENT("[耗时][TTS首包] run=%lu seq=%u 请求到首PCM=%lldms\n",
        job->run_id, job->sequence, rv_elapsed_ms(job->tts_start_ms, job->first_pcm_ms));
  }
  if (ROBOT_VOICE_TTS_LIVE && job->pipeline)
    return rv_live_sentence_sink(pcm, len, job);

  if (len > RV_SENTENCE_PCM_LIMIT - job->pcm_bytes)
    {
      return -EOVERFLOW;
    }

  required = job->pcm_bytes + len;
  if (required > job->pcm_capacity)
    {
      capacity = job->pcm_capacity == 0 ? 16 * 1024 : job->pcm_capacity;
      while (capacity < required)
        {
          if (capacity > RV_SENTENCE_PCM_LIMIT / 2)
            {
              capacity = RV_SENTENCE_PCM_LIMIT;
              break;
            }
          capacity *= 2;
        }

      int reserve_ret = rv_pipeline_reserve(job, capacity - job->pcm_capacity);
      if (reserve_ret < 0) return reserve_ret;
      next = realloc(job->pcm, capacity);
      if (next == NULL)
        {
          if (job->pipeline) rv_pipeline_release(job->pipeline, capacity - job->pcm_capacity);
          return -ENOMEM;
        }
      job->pcm = next;
      job->pcm_capacity = capacity;
    }

  memcpy(job->pcm + job->pcm_bytes, pcm, len);
  job->pcm_bytes = required;
  return 0;
}

struct rv_tts_retry_s
{
  struct rv_sentence_job_s *job;
};

static bool rv_pipeline_cancel_probe(void *arg);

static int rv_tts_retry_attempt(void *arg, unsigned int timeout_ms)
{
  struct rv_tts_retry_s *retry = arg;
  struct rv_sentence_job_s *job = retry->job;
  if (rv_pipeline_cancelled(job->pipeline)) return -ECANCELED;
  if (ROBOT_VOICE_TTS_LIVE) {
    job->retry_skip_bytes = job->pcm_bytes;
  } else {
    job->pcm_bytes = 0;
    job->retry_skip_bytes = 0;
  }
  if (!ROBOT_VOICE_TTS_LIVE) {
    job->stream_head = job->stream_tail = job->stream_count = 0;
    job->published = false;
  }
  job->producer_done = false;
  job->synthesis_result = 0;
  if (!job->first_pcm_ms) job->ready_ms = 0;
  job->retry_deadline_ms = robot_voice_now_ms() + timeout_ms;
  int ret = mimo_tts_speak_stream_on_worker_timeout(job->pipeline->key,
    job->text, rv_sentence_pcm_sink, job, timeout_ms);
  if (rv_sentence_retry_expired(job) && ret == 0) ret = -ETIMEDOUT;
  job->retry_deadline_ms = 0;
  if (ret == 0 && !job->pcm_bytes) ret = -ENODATA;
  return ret;
}

static int rv_tts_synthesize(struct rv_sentence_job_s *job)
{
  struct rv_tts_retry_s retry = { .job = job };
  unsigned int retries = 0;
  int ret = mimo_tts_speak_stream_on_worker_timeout(job->pipeline->key,
    job->text, rv_sentence_pcm_sink, job, ROBOT_VOICE_HTTP_TTS_MS);
  if (ret == 0 && !job->pcm_bytes) ret = -ENODATA;
  if (ret < 0)
    {
      int last = ret;
      ret = robot_voice_retry_after_failure(rv_tts_retry_attempt, &retry,
        ret, rv_pipeline_cancel_probe, job->pipeline, &retries);
      if (retries)
        RV_EVENT("[RV-RETRY] TTS run=%lu seq=%u retry=%u/%u rc=%d\n",
          job->run_id, job->sequence, retries, ROBOT_VOICE_RETRY_MAX,
          ret ? ret : last);
    }
  return ret;
}

static void rv_sentence_job_clear(struct rv_sentence_job_s *job)
{
  if (job == NULL)
    {
      return;
    }
  free(job->text);
  free(job->pcm);
  memset(job, 0, sizeof(*job));
}

/* The complete-cache backend has not submitted any PCM to DMA yet. Keep
 * capture available while downloading; acquire the echo-protection window
 * only immediately before audible playback. A long reply drains at a whole
 * phrase boundary, never by dropping or splitting already validated PCM.
 */
static int rv_buffered_window_finish(struct rv_tts_pipeline_s *p,
                                     bool *session_open,
                                     bool *playback_window_open)
{
  int ret;
  if (!*session_open) return 0;
  if (!rv_playback_begin_checked(rv_pipeline_cancel_probe, p,
                                p->run_id == 0))
    ret = -ECANCELED;
  else
    {
      *playback_window_open = true;
      RV_EVENT("[RV-CONTINUITY] run=%lu window=%u ready_pcm=%zuB audio_ms=%zu "
               "policy=validated-cache\n",
               p->run_id, ++p->buffered_windows, p->buffered_bytes,
               p->buffered_bytes * 1000 / (ROBOT_VOICE_TTS_RATE * 2));
      ret = robot_audio_playback_finish();
    }
  robot_audio_playback_close();
  *session_open = false;
  if (ret == 0 && p->source && p->buffered_source_end)
    {
      /* Copying into a cache is not proof that this prefix was heard. */
      pthread_mutex_lock(&p->source->lock);
      p->source->played_end = p->buffered_source_end;
      pthread_mutex_unlock(&p->source->lock);
    }
  p->buffered_bytes = p->buffered_source_end = 0;
  if (*playback_window_open)
    {
      rv_playback_end_window();
      *playback_window_open = false;
    }
  return ret;
}

/* A complete sentence owns a finite PCM playback session. Finish it here,
 * while the independent producer downloads the next sentence, rather than
 * waiting for Flash/reply EOS. Finite EOS also releases very short sentences
 * and their last PCM block without a live-ring prebuffer or tail hold.
 * Opt-in whole-reply continuity keeps its existing windowed cache path. */
static int rv_sentence_play_buffered(struct rv_sentence_job_s *job,
                                     unsigned long long commit_ms,
                                     unsigned long long user_end_ms,
                                     bool *session_open,
                                     bool *playback_window_open)
{
  struct rv_tts_pipeline_s *p = job ? job->pipeline : NULL;
  size_t offset = 0;
  int ret = 0;

  if (job == NULL || job->pcm == NULL || job->pcm_bytes == 0)
    {
      return -ENODATA;
    }

  if (ROBOT_VOICE_TTS_CONTINUITY &&
      job->pcm_bytes > ROBOT_VOICE_TTS_CACHE_BYTES)
    return -EOVERFLOW;
  if (ROBOT_VOICE_TTS_CONTINUITY && *session_open &&
      job->pcm_bytes > ROBOT_VOICE_TTS_CACHE_BYTES - p->buffered_bytes)
    {
      ret = rv_buffered_window_finish(p, session_open, playback_window_open);
      if (ret < 0) return ret;
    }

  if (!*session_open)
    {
      ret = robot_audio_playback_open_voice();
      if (ret < 0) return ret;
      *session_open = true;
      robot_audio_playback_set_turn_timing(commit_ms, user_end_ms,
                                           job->tts_start_ms);
    }

  if (!ROBOT_VOICE_TTS_CONTINUITY || p->buffered_bytes == 0)
    robot_audio_playback_set_sentence_timing(job->run_id, job->sequence,
                                             job->ready_ms);

  if (!ROBOT_VOICE_TTS_CONTINUITY)
    RV_EVENT("[TTS播放] run=%lu seq=%u PCM=%zuB 合成完成后开始逐句播放\n",
      job->run_id, job->sequence, job->pcm_bytes);

  while (offset < job->pcm_bytes)
    {
      size_t chunk = job->pcm_bytes - offset;
      if (chunk > 512)
        {
          chunk = 512;
        }
      chunk &= ~(size_t)1;
      if (chunk == 0)
        {
          ret = -EPROTO;
          break;
        }
      if ((job->run_id != 0 && g_voice_stop_requested) ||
          rv_pipeline_cancelled(job->pipeline))
        {
          ret = -ECANCELED;
          break;
        }
      ret = robot_voice_pcm_sink(job->pcm + offset, chunk, NULL);
      if (ret < 0)
        {
          break;
        }
      offset += chunk;
    }

  if (ret < 0)
    {
      return ret;
    }

  if (ROBOT_VOICE_TTS_CONTINUITY)
    {
      p->buffered_bytes += job->pcm_bytes;
      p->buffered_source_end = job->source_end;
    }

  /* The playback ring owns a copy now. Release this phrase before the
   * prefetch thread grows the next phrase buffer. */
  free(job->pcm);
  job->pcm = NULL;
  job->pcm_bytes = 0;
  job->pcm_capacity = 0;

  if (!ROBOT_VOICE_TTS_CONTINUITY)
    {
      /* Do not stop/join the producer here. It can synthesize the following
       * sentence throughout this blocking speaker drain. Each sentence is
       * already complete, so a download gap cannot interrupt its interior. */
      ret = robot_audio_playback_finish();
      robot_audio_playback_close();
      *session_open = false;
      RV_EVENT("[TTS句结束] run=%lu seq=%u rc=%d\n",
        job->run_id, job->sequence, ret);
    }
  return ret;
}

/* Playback starts while this same sentence is still arriving. A failed
 * partial stream can already have been heard; it is never marked complete. */
static int rv_sentence_play_live(struct rv_sentence_job_s *job,
                                 unsigned long long commit_ms,
                                 unsigned long long user_end_ms,
                                 bool *session_open,
                                 bool *playback_window_open,
                                 bool *sink_failed)
{
  struct rv_tts_pipeline_s *p = job->pipeline;
  uint8_t block[512];
  unsigned long long empty_since_ms = 0;
  int ret;
  if (!*session_open)
    {
      ret = robot_audio_playback_open_voice_live();
      if (ret < 0) { *sink_failed = true; return ret; }
      *session_open = true;
      robot_audio_playback_set_turn_timing(commit_ms, user_end_ms,
                                           job->tts_start_ms);
    }
  robot_audio_playback_set_sentence_timing(job->run_id, job->sequence, job->ready_ms);
  for (;;) {
    pthread_mutex_lock(&p->lock);
    while (!job->stream_count && !job->producer_done && !p->stop &&
           !rv_pipeline_cancelled(p))
      {
        if (*session_open && *playback_window_open)
          {
            if (!empty_since_ms) empty_since_ms = robot_voice_now_ms();
            if (rv_playback_gap_listen_due(empty_since_ms))
              {
                pthread_mutex_unlock(&p->lock);
                rv_playback_end_window();
                *playback_window_open = false;
                empty_since_ms = 0;
                pthread_mutex_lock(&p->lock);
                continue;
              }
          }
        else
          {
            empty_since_ms = 0;
          }
        rv_pipeline_wait_locked(p);
      }
    if (!job->stream_count && job->producer_done)
      ret = job->synthesis_result < 0 ? job->synthesis_result : 0;
    else if (p->stop || rv_pipeline_cancelled(p)) ret = -ECANCELED;
    else ret = 0;
    if (ret < 0 || (!job->stream_count && job->producer_done)) {
      pthread_mutex_unlock(&p->lock); break;
    }
    size_t n = job->stream_count;
    if (n > sizeof(block)) n = sizeof(block);
    if (n > job->pcm_capacity - job->stream_tail) n = job->pcm_capacity - job->stream_tail;
    memcpy(block, job->pcm + job->stream_tail, n);
    job->stream_tail = (job->stream_tail + n) % job->pcm_capacity;
    job->stream_count -= n;
    pthread_cond_broadcast(&p->changed); pthread_mutex_unlock(&p->lock);
    empty_since_ms = 0;
    if (!*playback_window_open)
      {
        if (rv_playback_begin_checked(rv_pipeline_cancel_probe, p,
                                      p->run_id == 0))
          {
            *playback_window_open = true;
          }
        else
          {
            ret = -ECANCELED;
            break;
          }
      }
    ret = robot_voice_pcm_sink(block, n, NULL);
    if (ret < 0) { *sink_failed = true; break; }
  }
  return ret;
}

/* Synthesis owns the cloud request, playback owns the speaker. Queue space
 * and PCM memory bound the producer; the Flash callback only appends text. */
static void *rv_pipeline_producer(void *arg)
{
  struct rv_tts_pipeline_s *p = arg;
  struct rv_sentence_job_s *job = NULL;
  size_t cursor = 0;
  size_t text_len = p->text ? strlen(p->text) : 0;
  unsigned int sequence = 0;
  int ret = 0;
  for (;;) {
    pthread_mutex_lock(&p->lock);
    while (!p->stop && p->count == ROBOT_VOICE_TTS_READY_SLOTS &&
           !rv_pipeline_cancelled(p))
      rv_pipeline_wait_locked(p);
    bool cancelled = p->stop || rv_pipeline_cancelled(p);
    pthread_mutex_unlock(&p->lock);
    if (cancelled) { ret = -ECANCELED; break; }
    job = calloc(1, sizeof(*job));
    if (!job) { ret = -ENOMEM; break; }
    job->source = p->source; job->pipeline = p; job->cancel_on_stop = true;
    if (p->source) {
      job->text = rv_llm_source_next(p->source, &job->source_end, &ret);
    } else {
      job->text = rv_sentence_next(p->text, text_len, &cursor);
      ret = job->text ? 0 : (cursor < text_len ? -ENOMEM : 1);
    }
    if (!job->text) { if (ret == 1) ret = 0; break; }
    job->sequence = ++sequence;
    job->run_id = p->run_id;
    job->tts_start_ms = robot_voice_now_ms();
    RV_EVENT("[TTS合成] run=%lu seq=%u 文本=%zuB 开始合成\n",
      job->run_id, job->sequence, strlen(job->text));
    RV_VERBOSE("[RV-TTS-PIPE] request mono_ms=%llu run=%lu seq=%u text_bytes=%zu\n",
           robot_voice_now_ms(), p->run_id, job->sequence, strlen(job->text));
    ret = rv_tts_synthesize(job);
    if (ROBOT_VOICE_TTS_LIVE) {
      pthread_mutex_lock(&p->lock);
      job->producer_done = true; job->synthesis_result = ret;
      /* Published jobs belong to the consumer after this release. Do not
       * touch them again: it may free one before the next request begins. */
      if (job->published) job = NULL;
      pthread_cond_broadcast(&p->changed); pthread_mutex_unlock(&p->lock);
      if (ret < 0) break;
      continue;
    }
    unsigned long long synthesis_done_ms = robot_voice_now_ms();
    RV_EVENT("[TTS就绪] run=%lu seq=%u 合成=%lldms PCM=%zuB rc=%d\n",
      job->run_id, job->sequence,
      rv_elapsed_ms(job->tts_start_ms, synthesis_done_ms), job->pcm_bytes, ret);
    if (ret < 0) break;
    pthread_mutex_lock(&p->lock);
    if (p->stop || rv_pipeline_cancelled(p)) {
      pthread_mutex_unlock(&p->lock); ret = -ECANCELED; break;
    }
    size_t pcm_bytes = job->pcm_bytes;
    unsigned int ready = p->count + 1;
    size_t reserved = p->reserved;
    unsigned long long ready_ms = synthesis_done_ms;
    job->ready_ms = ready_ms;
    p->ready[p->count++] = job; job = NULL;
    pthread_cond_broadcast(&p->changed); pthread_mutex_unlock(&p->lock);
    RV_VERBOSE("[RV-TTS-PIPE] ready mono_ms=%llu run=%lu seq=%u pcm_bytes=%zu queued=%u reserved=%zu\n",
           ready_ms, p->run_id, sequence, pcm_bytes, ready, reserved);
  }
  if (job) {
    rv_pipeline_release(p, job->pcm_capacity);
    rv_sentence_job_clear(job); free(job);
  }
  robot_voice_http_close_idle();
  pthread_mutex_lock(&p->lock); p->done = true; p->result = ret;
  pthread_cond_broadcast(&p->changed); pthread_mutex_unlock(&p->lock);
  return NULL;
}

/* Claim and publish in text order BEFORE downloading. Completion order may
 * differ; the consumer waits only on the head job. No network I/O under a
 * mutex, and no later sentence may bypass a failed head. */
static void *rv_pipeline_parallel_producer(void *arg)
{
  struct rv_tts_pipeline_s *p = arg;
  int ret = 0;
  for (;;) {
    struct rv_sentence_job_s *job = NULL;
    pthread_mutex_lock(&p->next_lock);
    pthread_mutex_lock(&p->lock);
    while (!p->stop && p->count == ROBOT_VOICE_TTS_READY_SLOTS &&
           !rv_pipeline_cancelled(p))
      rv_pipeline_wait_locked(p);
    bool cancelled = p->stop || rv_pipeline_cancelled(p);
    pthread_mutex_unlock(&p->lock);
    if (cancelled) ret = -ECANCELED;
    else {
      job = calloc(1, sizeof(*job));
      if (!job) ret = -ENOMEM;
      else {
        job->pipeline = p; job->source = p->source; job->cancel_on_stop = true;
        job->run_id = p->run_id;
        if (p->source) job->text = rv_llm_source_next(p->source, &job->source_end, &ret);
        else {
          size_t len = strlen(p->text);
          job->text = rv_sentence_next(p->text, len, &p->cursor);
          ret = job->text ? 0 : (p->cursor < len ? -ENOMEM : 1);
        }
      }
    }
    if (ret != 0) {
      pthread_mutex_unlock(&p->next_lock);
      if (job) { rv_sentence_job_clear(job); free(job); }
      if (ret == 1) ret = 0;
      break;
    }
    job->sequence = ++p->sequence;
    pthread_mutex_lock(&p->lock);
    /* The ordered slot is reserved before network I/O. In live mode the
     * consumer starts as soon as this head job has PCM; the PCM sink must not
     * publish the same job into the queue a second time. */
    if (ROBOT_VOICE_TTS_LIVE) job->published = true;
    p->ready[p->count++] = job;
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&p->next_lock);

    job->tts_start_ms = robot_voice_now_ms();
    RV_VERBOSE("[RV-TTS-PIPE] request mono_ms=%llu run=%lu seq=%u text_bytes=%zu\n",
      job->tts_start_ms, p->run_id, job->sequence, strlen(job->text));
    ret = rv_tts_synthesize(job);
    pthread_mutex_lock(&p->lock);
    job->ready_ms = robot_voice_now_ms();
    RV_EVENT("[TTS就绪] run=%lu seq=%u 合成=%lldms PCM=%zuB rc=%d\n",
      job->run_id, job->sequence, rv_elapsed_ms(job->tts_start_ms, job->ready_ms), job->pcm_bytes, ret);
    job->synthesis_result = ret;
    job->producer_done = true; /* Release transfers ownership to consumer. */
    if (ret < 0) { p->stop = true; p->result = ret; }
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
    if (ret < 0) break;
  }
  pthread_mutex_lock(&p->lock);
  if (ret < 0 && !p->result) { p->result = ret; p->stop = true; }
  if (--p->workers_left == 0) p->done = true;
  pthread_cond_broadcast(&p->changed);
  pthread_mutex_unlock(&p->lock);
  return NULL;
}

static bool rv_pipeline_cancel_probe(void *arg)
{
  return rv_pipeline_cancelled(arg);
}

struct rv_prompt_cancel_s { unsigned long token; bool scoped; };
static bool rv_prompt_cancelled(void *arg)
{
  struct rv_prompt_cancel_s *c = arg;
  return c->scoped && (g_voice_stop_requested || c->token != rv_run_token_load());
}

/* g_voice_speak_lock is already held. No API key, DNS or TLS is needed. */
static int rv_play_local_prompt_locked(enum robot_voice_prompt_e id, unsigned long run_id)
{
  struct rv_prompt_cancel_s cancel = {rv_run_token_load(), run_id != 0};
  int ret;
  if (robot_motion_is_busy()) return -EBUSY;
  if (!rv_playback_begin_checked(rv_prompt_cancelled, &cancel, false))
    return -ECANCELED;
  g_voice_speaking = 1;
  robot_world_state_set_tts_active(true);
  contest_voice_set_state(CONTEST_VOICE_SPEAKING);
  (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                              ROBOT_EXPRESSION_SPEAKING, 0);
  ret = robot_voice_prompt_play(id, robot_voice_pcm_sink, NULL,
                                rv_prompt_cancelled, &cancel);
  g_voice_speaking = 0;
  robot_world_state_set_tts_active(false);
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
  rv_playback_end_window();
  if (ret < 0 && ret != -ECANCELED)
    RV_EVENT("[本地提示] 播放失败 name=%s rc=%d\n", robot_voice_prompt_name(id), ret);
  return ret;
}

static int rv_tts_play_pipeline(struct rv_llm_speech_s *source,
                                const char *text, const char *key,
                                unsigned long run_id,
                                unsigned long long commit_ms,
                                unsigned long long user_end_ms,
                                bool recovery_notice)
{
  struct rv_tts_pipeline_s p = {0};
  pthread_t producers[ROBOT_VOICE_TTS_WORKERS];
  unsigned int started = 0;
  bool parallel = ROBOT_VOICE_TTS_WORKERS > 1;
  struct rv_sentence_job_s *aborted = NULL;
  bool first = true;
  bool live_session_open = false;
  bool playback_window_open = false;
  bool live_sink_failed = false;
  size_t live_source_end = 0;
  unsigned long long previous_done_ms = 0;
  unsigned long long playback_idle_since_ms = 0;
  int ret = pthread_mutex_init(&p.lock, NULL);
  if (ret) return -ret;
  ret = pthread_cond_init(&p.changed, NULL);
  if (ret) { pthread_mutex_destroy(&p.lock); return -ret; }
  ret = pthread_mutex_init(&p.next_lock, NULL);
  if (ret) { pthread_cond_destroy(&p.changed); pthread_mutex_destroy(&p.lock); return -ret; }
  p.source = source; p.text = text; p.key = key; p.run_id = run_id;
  p.run_token = rv_run_token_load();
  RV_EVENT("[TTS流水线] run=%lu policy=%s workers=%u input=%s\n",
    run_id, ROBOT_VOICE_TTS_LIVE ? "live-SSE-PCM" :
      (ROBOT_VOICE_TTS_CONTINUITY ? "validated-turn-cache" :
                                   "sentence-cache/prefetch-next"),
    (unsigned int)ROBOT_VOICE_TTS_WORKERS, source ? "flash-deltas" : "complete-text");
  p.workers_left = parallel ? ROBOT_VOICE_TTS_WORKERS : 1;
  for (; started < (parallel ? ROBOT_VOICE_TTS_WORKERS : 1); started++) {
    ret = rv_thread_start(&producers[started], parallel ? rv_pipeline_parallel_producer : rv_pipeline_producer,
                         &p, RV_SENTENCE_PREFETCH_STACK, ROBOT_VOICE_NETWORK_PRIORITY);
    if (ret < 0) break;
  }
  if (ret < 0) goto stop_workers;
  for (;;) {
    struct rv_sentence_job_s *job;
    unsigned long long wait_ms = robot_voice_now_ms();
    pthread_mutex_lock(&p.lock);
    while (!p.count && !p.done && !p.stop && !rv_pipeline_cancelled(&p))
      {
        if (!ROBOT_VOICE_TTS_CONTINUITY && playback_window_open)
          {
            if (!playback_idle_since_ms)
              playback_idle_since_ms = robot_voice_now_ms();
            if (rv_playback_gap_listen_due(playback_idle_since_ms))
              {
                pthread_mutex_unlock(&p.lock);
                rv_playback_end_window();
                playback_window_open = false;
                playback_idle_since_ms = 0;
                pthread_mutex_lock(&p.lock);
                continue;
              }
          }
        else
          {
            playback_idle_since_ms = 0;
          }
        rv_pipeline_wait_locked(&p);
      }
    if (p.count) playback_idle_since_ms = 0;
    if (rv_pipeline_cancelled(&p)) {
      pthread_mutex_unlock(&p.lock); ret = -ECANCELED; break;
    }
    if (!p.count) { ret = p.result; pthread_mutex_unlock(&p.lock); break; }
    job = p.ready[0];
    if (parallel && ROBOT_VOICE_TTS_LIVE) {
      while (!job->stream_count && !job->producer_done && !p.stop &&
             !rv_pipeline_cancelled(&p)) rv_pipeline_wait_locked(&p);
    } else if (parallel) {
      while (!job->producer_done && !p.stop &&
             !rv_pipeline_cancelled(&p)) rv_pipeline_wait_locked(&p);
    }
    if (parallel && ((!ROBOT_VOICE_TTS_LIVE && !job->producer_done) ||
                     (job->producer_done && job->synthesis_result < 0) || p.stop ||
                     rv_pipeline_cancelled(&p))) {
      ret = job->producer_done && job->synthesis_result < 0 ? job->synthesis_result :
            (p.result < 0 ? p.result : -ECANCELED);
      pthread_mutex_unlock(&p.lock); break;
    }
    p.count--;
    memmove(p.ready, p.ready + 1, p.count * sizeof(p.ready[0]));
    pthread_cond_broadcast(&p.changed); pthread_mutex_unlock(&p.lock);
    size_t capacity = job->pcm_capacity;
    unsigned long long gate_ms = robot_voice_now_ms();
    if (ROBOT_VOICE_TTS_CONTINUITY)
      {
        ret = 0; /* Cache writes are inaudible; capture remains available. */
      }
    else if (!playback_window_open)
      {
        ret = rv_playback_begin_checked(rv_pipeline_cancel_probe, &p,
                                       p.run_id == 0) ?
              0 : -ECANCELED;
        if (ret == 0) playback_window_open = true;
      }
    else
      {
        ret = 0;
      }
    if (ret == 0) {
      unsigned long long submit_ms = robot_voice_now_ms();
      if (!ROBOT_VOICE_TTS_CONTINUITY && previous_done_ms)
        RV_EVENT("[耗时][分句衔接] run=%lu seq=%u 上句处理结束到本句交付=%llums\n",
          run_id, job->sequence, submit_ms - previous_done_ms);
      RV_VERBOSE("[RV-TTS-PIPE] play_submit mono_ms=%llu run=%lu seq=%u queue_wait_ms=%llu user_wait_ms=%llu handoff_gap_ms=%llu\n",
             submit_ms, run_id, job->sequence, gate_ms - wait_ms, submit_ms - gate_ms,
             previous_done_ms ? submit_ms - previous_done_ms : 0);
    }
    if (ret == 0) ret = ROBOT_VOICE_TTS_LIVE ?
      rv_sentence_play_live(job, first ? commit_ms : 0, first ? user_end_ms : 0,
                            &live_session_open, &playback_window_open,
                            &live_sink_failed) :
      rv_sentence_play_buffered(job, first ? commit_ms : 0,
                                first ? user_end_ms : 0,
                                &live_session_open, &playback_window_open);
    /* Playback now owns the PCM copy, or clear frees an aborted copy. */
    if (ret < 0) {
      if (ROBOT_VOICE_TTS_LIVE) aborted = job;
      else {
        live_sink_failed = true;
        rv_sentence_job_clear(job); free(job);
        rv_pipeline_release(&p, capacity);
      }
      break;
    }
    if (!ROBOT_VOICE_TTS_LIVE) rv_pipeline_release(&p, capacity);
    if (ROBOT_VOICE_TTS_LIVE && source)
      {
        live_source_end = job->source_end;
      }
    previous_done_ms = robot_voice_now_ms();
    RV_VERBOSE("[RV-TTS-PIPE] play_done mono_ms=%llu run=%lu seq=%u rc=%d\n",
           previous_done_ms, run_id, job->sequence, ret);
    if (!ROBOT_VOICE_TTS_LIVE && !ROBOT_VOICE_TTS_CONTINUITY &&
        ret == 0 && source) {
      pthread_mutex_lock(&source->lock); source->played_end = job->source_end;
      pthread_mutex_unlock(&source->lock);
    }
    rv_sentence_job_clear(job); free(job); first = false;
    if (ROBOT_VOICE_TTS_LIVE) rv_pipeline_release(&p, capacity);
    if (ret < 0) break;
  }
stop_workers:
  if (ret < 0 && source) {
    pthread_mutex_lock(&source->lock); source->cancel = true;
    pthread_cond_broadcast(&source->changed); pthread_mutex_unlock(&source->lock);
  }
  pthread_mutex_lock(&p.lock); p.stop = true;
  pthread_cond_broadcast(&p.changed); pthread_mutex_unlock(&p.lock);
  for (unsigned int i = 0; i < started; i++) pthread_join(producers[i], NULL);
  robot_voice_http_close_idle();
  if (live_session_open)
    {
      bool same_run = !p.run_token || p.run_token == rv_run_token_load();
      bool drain = !live_sink_failed && same_run &&
                   (p.run_id == 0 || !g_voice_stop_requested) &&
                   (!ROBOT_VOICE_TTS_CONTINUITY || ret == 0);
      int finish_ret;
      if (drain && ROBOT_VOICE_TTS_CONTINUITY)
        finish_ret = rv_buffered_window_finish(&p, &live_session_open,
                                                &playback_window_open);
      else
        {
          finish_ret = drain ? robot_audio_playback_finish() : -ECANCELED;
          robot_audio_playback_close();
          live_session_open = false;
        }
      if (drain && finish_ret < 0) live_sink_failed = true;
      if (drain && finish_ret == 0 && source && live_source_end)
        {
          pthread_mutex_lock(&source->lock);
          source->played_end = live_source_end;
          pthread_mutex_unlock(&source->lock);
        }
      if (ret == 0 && finish_ret < 0) ret = finish_ret;
    }
  if (playback_window_open)
    {
      rv_playback_end_window();
    }
  if (aborted) { rv_sentence_job_clear(aborted); free(aborted); }
  for (unsigned int i = 0; i < p.count; i++) {
    rv_sentence_job_clear(p.ready[i]); free(p.ready[i]);
  }
  pthread_mutex_destroy(&p.next_lock);
  pthread_cond_destroy(&p.changed); pthread_mutex_destroy(&p.lock);
  if (ret < 0 && ret != -ECANCELED && live_sink_failed)
    {
      RV_EVENT("[RV-AUDIO-ERROR] playback failed rc=%d; check RV-DMA/RV-PB\n", ret);
    }
  if (ret < 0 && ret != -ECANCELED && !live_sink_failed && !recovery_notice &&
      !g_voice_stop_requested &&
      (!p.run_token || p.run_token == rv_run_token_load()))
    {
      RV_EVENT("[RV-TTS] retries exhausted; playing local notice\n");
      (void)rv_play_local_prompt_locked(RV_PROMPT_NETWORK_ERROR, run_id);
    }
  return ret;
}

static int voice_channel_speak_source_owned(const char *text, bool agent_reply,
                                      unsigned long run_id,
                                      struct rv_llm_speech_s *source)
{
  char key[256];
  int ret;
  unsigned long long commit_ms = 0;
  unsigned long long user_end_ms = 0;

  if (run_id && robot_music_media_owns_reply && robot_music_media_owns_reply(run_id)) {
    if (agent_reply) rv_reply_done_for_run(true,run_id);
    return 0; /* Never open a second direct I2S owner for music acknowledgement. */
  }
  if (!source && (!text || text[0] == '\0'))
    {
      if (agent_reply)
        {
          rv_reply_done_for_run(true, run_id);
        }
      return -EINVAL;
    }
  int prompt = source ? -1 : robot_voice_prompt_id(text);
  if (prompt >= 0) {
    pthread_mutex_lock(&g_voice_speak_lock);
    pthread_mutex_lock(&g_monitor.lock);
    bool valid = !run_id || (g_monitor.enabled && !g_voice_stop_requested &&
      g_monitor.run_id == run_id &&
      (prompt != RV_PROMPT_CHAT_EXIT || g_monitor.phase == RV_MON_WAKE));
    pthread_mutex_unlock(&g_monitor.lock);
    if (!valid) ret = -ECANCELED;
    else {
      ret = rv_play_local_prompt_locked((enum robot_voice_prompt_e)prompt, run_id);
      if (ret != -ECANCELED) {
        rv_reply_done_for_run(agent_reply, run_id);
      }
    }
    pthread_mutex_unlock(&g_voice_speak_lock);
    return ret;
  }
  if (strcmp(g_voice_tts_backend, "mimo-v2.5-tts") != 0)
    {
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      rv_reply_done_for_run(agent_reply, run_id);
      return -ENOSYS;
    }

  /* A non-streamed final can race a cancelled stream. Its worker waits here
   * for the old owner to publish its precisely completed prefix, not on the
   * Agent task. Do this BEFORE taking the speaker lock: the old owner may
   * still need that lock to start and observe its cancellation. */
  if (!source && agent_reply && run_id) {
    pthread_mutex_lock(&g_monitor.lock);
    while (g_monitor.stream_busy && g_monitor.run_id == run_id &&
           !g_voice_stop_requested) rv_wait_locked(50);
    pthread_mutex_unlock(&g_monitor.lock);
  }
  pthread_mutex_lock(&g_voice_speak_lock);
  pthread_mutex_lock(&g_monitor.lock);
  if (run_id != 0 && (!g_monitor.enabled ||
      g_monitor.run_id != run_id || g_voice_stop_requested))
    {
      pthread_mutex_unlock(&g_monitor.lock);
      pthread_mutex_unlock(&g_voice_speak_lock);
      return -ECANCELED;
    }
  if (!source && agent_reply && run_id && g_monitor.spoken_run == run_id &&
      g_monitor.spoken_prefix) {
    size_t prefix = strlen(g_monitor.spoken_prefix);
    if (!strncmp(text, g_monitor.spoken_prefix, prefix)) text += prefix;
    free(g_monitor.spoken_prefix); g_monitor.spoken_prefix = NULL;
  }
  pthread_mutex_unlock(&g_monitor.lock);
  if (!source && !text[0]) {
    rv_reply_done_for_run(agent_reply, run_id);
    pthread_mutex_unlock(&g_voice_speak_lock);
    return 0;
  }
  ret = voice_get_api_key(key, sizeof(key));
  if (ret < 0)
    {
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      memset(key, 0, sizeof(key));
      rv_reply_done_for_run(agent_reply, run_id);
      pthread_mutex_unlock(&g_voice_speak_lock);
      return ret;
    }
  if (robot_motion_is_busy())
    {
      (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
      printf("[POWER-SAFE] mono_ms=%llu TTS_REJECT motion_active\n",
             robot_voice_now_ms());
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
      memset(key, 0, sizeof(key));
      rv_reply_done_for_run(agent_reply, run_id);
      pthread_mutex_unlock(&g_voice_speak_lock);
      return -EBUSY;
    }

  RV_VERBOSE("[RV-TTS] playback path=%s input=%s; Media TTS bypassed\n",
         ROBOT_VOICE_TTS_LIVE ? "live-SSE-PCM" :
           (ROBOT_VOICE_TTS_CONTINUITY ? "validated-turn-cache" : "sentence-cache/prefetch-next"),
         source ? "flash-deltas" : "complete-text");
  if ((agent_reply || source) && run_id != 0)
    {
      pthread_mutex_lock(&g_monitor.lock);
      if (run_id == g_monitor.run_id)
        {
          commit_ms = g_monitor.reply_start_ms;
          user_end_ms = g_monitor.reply_user_end_ms;
        }
      pthread_mutex_unlock(&g_monitor.lock);
    }
  g_voice_speaking = 1;
  robot_world_state_set_tts_active(true);
  contest_voice_set_state(CONTEST_VOICE_SPEAKING);
  RV_VERBOSE("[POWER-SAFE] mono_ms=%llu TTS_START\n", robot_voice_now_ms());
  (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_VOICE,
                              ROBOT_EXPRESSION_SPEAKING, 0);
  ret = rv_tts_play_pipeline(source, text, key, run_id, commit_ms, user_end_ms,
                             false);

  if (source) {
    pthread_mutex_lock(&source->lock);
    source->cancel = true;
    pthread_cond_broadcast(&source->changed);
    pthread_mutex_unlock(&source->lock);
  }
  g_voice_speaking = 0;
  robot_world_state_set_tts_active(false);
  if (!source) robot_world_state_set_conversation_active(false);
  RV_VERBOSE("[POWER-SAFE] mono_ms=%llu TTS_DONE rc=%d\n",
         robot_voice_now_ms(), ret);
  (void)robot_expression_clear(ROBOT_EXPRESSION_SOURCE_VOICE);
  if (ret < 0 && !(source && ret == -ECANCELED))
    {
      if (ret != -ECANCELED) printf("[语音错误] TTS 合成或播放失败 rc=%d\n", ret);
      (void)robot_expression_set(ROBOT_EXPRESSION_SOURCE_SYSTEM,
                                  ROBOT_EXPRESSION_ERROR, 1500);
    }
  memset(key, 0, sizeof(key));
  rv_reply_done_for_run(agent_reply, run_id);
  if (g_voice_running && !g_voice_stop_requested)
    {
      pthread_mutex_lock(&g_monitor.lock);
      if (!g_monitor.enabled)
        {
          contest_voice_set_state(CONTEST_VOICE_RECORDING);
        }
      pthread_mutex_unlock(&g_monitor.lock);
    }
  pthread_mutex_unlock(&g_voice_speak_lock);
  return ret;
}

static int voice_channel_speak_source(const char *text, bool agent_reply,
                                      unsigned long run_id,
                                      struct rv_llm_speech_s *source)
{
  int ret;
  pthread_mutex_lock(&g_monitor.lock);
  bool valid = !run_id || (g_monitor.enabled && !g_voice_stop_requested &&
                           g_monitor.run_id == run_id);
  pthread_mutex_unlock(&g_monitor.lock);
  if (!valid) return -ECANCELED;
  if (run_id && robot_music_media_owns_reply && robot_music_media_owns_reply(run_id))
    {
      if (agent_reply) rv_reply_done_for_run(true, run_id);
      return 0;
    }
  ret = robot_task_begin ? robot_task_begin() : 0;
  if (ret < 0)
    {
      if (agent_reply) rv_reply_done_for_run(true, run_id);
      return ret;
    }
  ret = voice_channel_speak_source_owned(text, agent_reply, run_id, source);
  if (robot_task_end) robot_task_end();
  return ret;
}

static void *rv_llm_speech_worker(void *arg)
{
  struct rv_llm_speech_s *s = arg;
  bool final_claimed;
  s->result = voice_channel_speak_source(NULL, false, s->run_id, s);
  robot_voice_http_close_idle();
  pthread_mutex_lock(&g_monitor.lock);
  pthread_mutex_lock(&s->lock);
  if (g_monitor.run_id == s->run_id && g_monitor.enabled &&
      !g_voice_stop_requested && s->played_end) {
    s->text[s->played_end] = 0;
    free(g_monitor.spoken_prefix); g_monitor.spoken_prefix = s->text;
    g_monitor.spoken_run = s->run_id; s->text = NULL;
  }
  final_claimed = s->final_claimed && g_monitor.run_id == s->run_id;
  pthread_mutex_unlock(&s->lock);
  if (g_monitor.active_speech == s) {
    g_monitor.active_speech = NULL;
    g_monitor.stream_busy = false;
  }
  pthread_cond_broadcast(&g_monitor.changed);
  pthread_mutex_unlock(&g_monitor.lock);
  if (final_claimed) rv_reply_done_for_run(true, s->run_id);
  RV_VERBOSE("[RV-LLM] detached speech complete run=%lu played=%zu rc=%d\n",
             s->run_id, s->played_end, s->result);
  rv_llm_speech_put(s);
  pthread_mutex_lock(&g_voice_lock);
  g_tts_jobs--; pthread_cond_broadcast(&g_tts_jobs_changed);
  pthread_mutex_unlock(&g_voice_lock);
  if (robot_task_resume_if_idle) robot_task_resume_if_idle();
  return NULL;
}

bool robot_voice_task_can_resume(void)
{
  bool idle;
  pthread_mutex_lock(&g_voice_lock);
  idle = g_tts_jobs == 0 && !g_voice_speaking;
  pthread_mutex_unlock(&g_voice_lock);
  if (!idle) return false;
  pthread_mutex_lock(&g_monitor.lock);
  idle = !g_monitor.enabled ||
    (g_monitor.phase == RV_MON_WAKE && !g_monitor.capture_active &&
     !g_monitor.wake_handoff_pending && !g_monitor.queued && !g_monitor.in_flight &&
     !g_monitor.pending_text[0] && !g_monitor.pending_failed &&
     !g_monitor.stream_busy && !g_voice_reply_pending);
  pthread_mutex_unlock(&g_monitor.lock);
  return idle;
}

void robot_voice_music_started(void)
{
  pthread_mutex_lock(&g_monitor.lock);
  if (g_monitor.enabled && g_monitor.phase != RV_MON_WAKE &&
      !g_monitor.capture_active && !g_monitor.wake_handoff_pending)
    rv_return_to_wake_locked();
  pthread_mutex_unlock(&g_monitor.lock);
}

unsigned long robot_voice_music_run_id(void)
{
  pthread_mutex_lock(&g_monitor.lock);
  unsigned long id=g_monitor.enabled?g_monitor.run_id:0;
  pthread_mutex_unlock(&g_monitor.lock);return id;
}

unsigned long robot_voice_llm_request_token(const char *body)
{
  cJSON *root = cJSON_Parse(body);
  cJSON *messages = cJSON_GetObjectItem(root, "messages");
  const char *user = NULL;
  unsigned long token = 0;
  for (int i = cJSON_GetArraySize(messages) - 1; i >= 0; i--) {
    cJSON *m = cJSON_GetArrayItem(messages, i);
    cJSON *role = cJSON_GetObjectItem(m, "role");
    cJSON *text = cJSON_GetObjectItem(m, "content");
    if (cJSON_IsString(role) && !strcmp(role->valuestring, "user")) {
      if (cJSON_IsString(text)) user = text->valuestring;
      break;
    }
  }
  if (user && strncmp(user, "[ROBOT_INTERNAL_EVENT]", 22) == 0)
    {
      cJSON_Delete(root);
      return 0; /* Internal events do not own a user voice run. */
    }
  pthread_mutex_lock(&g_monitor.lock);
  if (user && g_monitor.enabled && !g_monitor.stop && !g_voice_stop_requested &&
      g_monitor.agent_text[0] && !strcmp(user, g_monitor.agent_text) &&
      (g_monitor.phase == RV_MON_BUSY || g_monitor.phase == RV_MON_GREETING))
    token = rv_run_token_load();
  pthread_mutex_unlock(&g_monitor.lock);
  cJSON_Delete(root);
  return token;
}

bool robot_voice_llm_request_current(unsigned long token)
{ return !token || (!g_voice_stop_requested && token == rv_run_token_load()); }

void robot_voice_llm_reply_started(unsigned long token)
{
  if (!token) return;
  pthread_mutex_lock(&g_monitor.lock);
  if (token == rv_run_token_load()) rv_lock_reply_locked(g_monitor.run_id);
  pthread_mutex_unlock(&g_monitor.lock);
}

void *robot_voice_llm_stream_begin(const char *body)
{
  cJSON *root = cJSON_Parse(body);
  cJSON *messages = cJSON_GetObjectItem(root, "messages");
  const char *user = NULL;
  struct rv_llm_speech_s *s = NULL;
  unsigned long run_id = 0;
  int ret;
  for (int i = cJSON_GetArraySize(messages) - 1; i >= 0; i--) {
    cJSON *m = cJSON_GetArrayItem(messages, i);
    cJSON *role = cJSON_GetObjectItem(m, "role");
    cJSON *text = cJSON_GetObjectItem(m, "content");
    if (cJSON_IsString(role) && !strcmp(role->valuestring, "user")) {
      if (cJSON_IsString(text)) user = text->valuestring;
      break;
    }
  }
  if (user && strncmp(user, "[ROBOT_INTERNAL_EVENT]", 22) == 0)
    {
      cJSON_Delete(root);
      return NULL; /* Companion turns never arm streaming TTS. */
    }
  pthread_mutex_lock(&g_monitor.lock);
  if (user && g_monitor.enabled && !g_monitor.stop &&
      !g_voice_stop_requested && !g_monitor.reply_claimed &&
      !g_monitor.stream_busy && g_monitor.agent_text[0] &&
      !strcmp(user, g_monitor.agent_text) &&
      (g_monitor.phase == RV_MON_BUSY || g_monitor.phase == RV_MON_GREETING) &&
      !strcmp(g_voice_tts_backend, "mimo-v2.5-tts")) {
    run_id = g_monitor.run_id;
    g_monitor.stream_busy = true;
  }
  pthread_mutex_unlock(&g_monitor.lock);
  cJSON_Delete(root);
  if (!run_id) return NULL;
  s = calloc(1, sizeof(*s));
  if (!s) goto failed;
  s->text = calloc(1, ROBOT_VOICE_TEXT_CAP);
  if (!s->text) { free(s); s = NULL; goto failed; }
  pthread_mutex_lock(&g_monitor.lock);
  bool prior_needed = g_monitor.spoken_run == run_id && g_monitor.spoken_prefix;
  if (prior_needed) s->prior_prefix = strdup(g_monitor.spoken_prefix);
  pthread_mutex_unlock(&g_monitor.lock);
  if (prior_needed && !s->prior_prefix) {
    free(s->prior_prefix); free(s->text); free(s); s = NULL; goto failed;
  }
  ret = pthread_mutex_init(&s->lock, NULL);
  if (ret) { free(s->prior_prefix); free(s->text); free(s); s = NULL; goto failed; }
  ret = pthread_cond_init(&s->changed, NULL);
  if (ret) { pthread_mutex_destroy(&s->lock); free(s->prior_prefix); free(s->text); free(s); s = NULL; goto failed; }
  s->run_id = run_id;
  /* run_id is revalidated under the monitor lock before publication. */
  s->hold = false; /* Ordinary deltas immediately feed TTS prefetch. */
  s->started_ms = robot_voice_now_ms();
  s->references = 2;
  pthread_mutex_lock(&g_monitor.lock);
  if (g_monitor.run_id != run_id || !g_monitor.enabled || g_monitor.stop ||
      g_voice_stop_requested) {
    pthread_mutex_unlock(&g_monitor.lock);
    pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->lock);
    free(s->prior_prefix); free(s->text); free(s); s = NULL; goto failed;
  }
  s->run_token = rv_run_token_load();
  g_monitor.active_speech = s;
  pthread_mutex_unlock(&g_monitor.lock);
  pthread_mutex_lock(&g_voice_lock);
  if (g_voice_stop_requested || g_tts_jobs >= 2) ret = -EBUSY;
  else {
    g_tts_jobs++;
    pthread_attr_t attr;
    ret = pthread_attr_init(&attr);
    if (ret == 0) {
      ret = rv_thread_attributes(&attr, CONTEST_CLI_TTS_STACK,
                                 ROBOT_VOICE_CONSUMER_PRIORITY);
      if (ret == 0) ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      if (ret == 0) ret = pthread_create(&s->thread, &attr, rv_llm_speech_worker, s);
      pthread_attr_destroy(&attr);
    }
    if (ret > 0) ret = -ret;
    if (ret < 0) { g_tts_jobs--; pthread_cond_broadcast(&g_tts_jobs_changed); }
  }
  pthread_mutex_unlock(&g_voice_lock);
  if (ret < 0) {
    pthread_mutex_lock(&g_monitor.lock);
    if (g_monitor.active_speech == s) g_monitor.active_speech = NULL;
    pthread_mutex_unlock(&g_monitor.lock);
    pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->lock);
    free(s->prior_prefix); free(s->text); free(s); s = NULL; goto failed;
  }
  RV_VERBOSE("[RV-LLM-TTS] streaming sentence playback armed run=%lu\n", run_id);
  return s;
failed:
  pthread_mutex_lock(&g_monitor.lock);
  if (g_monitor.run_id == run_id && !g_monitor.active_speech)
    g_monitor.stream_busy = false;
  pthread_mutex_unlock(&g_monitor.lock);
  return NULL; /* The complete Agent reply still follows the normal TTS path. */
}

void robot_voice_llm_stream_hold(void *arg)
{
  struct rv_llm_speech_s *s = arg;
  if (!s) return;
  pthread_mutex_lock(&s->lock);
  s->hold = true;
  if (!s->retry_text) s->retry_text = calloc(1, ROBOT_VOICE_TEXT_CAP);
  if (!s->retry_text) s->cancel = true;
  pthread_cond_broadcast(&s->changed);
  pthread_mutex_unlock(&s->lock);
}

void robot_voice_llm_stream_retry_reset(void *arg)
{
  struct rv_llm_speech_s *s = arg;
  if (!s) return;
  pthread_mutex_lock(&s->lock);
  if (s->hold && !s->cancel) {
    /* Retain original phrases already copied to TTS producers. */
    s->retry_used = 0;
    s->retry_text[0] = '\0';
  }
  pthread_cond_broadcast(&s->changed);
  pthread_mutex_unlock(&s->lock);
}

void robot_voice_llm_stream_commit(void *arg)
{
  struct rv_llm_speech_s *s = arg;
  if (!s) return;
  pthread_mutex_lock(&s->lock);
  if (!s->cancel && s->hold && s->retry_text) {
    /* cursor includes queued audio. Matching retries resume after it once.
     * If the retry changes that text, cancel this stream and let the final
     * Agent reply use the existing fallback rather than splice two answers. */
    if (s->retry_used < s->cursor ||
        memcmp(s->text, s->retry_text, s->cursor)) {
      s->cancel = true;
      RV_EVENT("[RV-LLM-TTS] retry changed submitted text; using final reply\n");
    } else {
      memcpy(s->text, s->retry_text, s->retry_used + 1);
      s->used = s->retry_used;
      s->phrase_wait_since_ms = 0;
    }
    free(s->retry_text); s->retry_text = NULL; s->retry_used = 0;
  }
  if (!s->cancel) s->hold = false;
  pthread_cond_broadcast(&s->changed);
  pthread_mutex_unlock(&s->lock);
}

bool robot_voice_llm_stream_cancelled(void *arg)
{
  struct rv_llm_speech_s *s = arg;
  bool cancel;
  if (!s) return false;
  if (s->run_token != rv_run_token_load()) {
    pthread_mutex_lock(&s->lock);
    s->cancel = true;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
    return true;
  }
  pthread_mutex_lock(&s->lock);
  cancel = s->cancel || g_voice_stop_requested;
  pthread_mutex_unlock(&s->lock);
  return cancel;
}

void robot_voice_llm_stream_delta(void *arg, const char *text, size_t n, bool tools)
{
  struct rv_llm_speech_s *s = arg;
  if (!s) return;
  if (tools || (text && n)) {
    pthread_mutex_lock(&g_monitor.lock);
    if (s->run_token == rv_run_token_load()) rv_lock_reply_locked(s->run_id);
    pthread_mutex_unlock(&g_monitor.lock);
  }
  pthread_mutex_lock(&s->lock);
  if (tools) s->cancel = true;
  if (!s->cancel && s->hold && text && n) {
    if (!s->retry_text || n >= ROBOT_VOICE_TEXT_CAP - s->retry_used)
      s->cancel = true;
    else {
      memcpy(s->retry_text + s->retry_used, text, n);
      s->retry_used += n; s->retry_text[s->retry_used] = 0;
    }
  } else if (!s->cancel && text && n) {
    if (n >= ROBOT_VOICE_TEXT_CAP - s->used) s->cancel = true;
    else {
      memcpy(s->text + s->used, text, n); s->used += n; s->text[s->used] = 0;
      if (!s->first_logged) {
        for (size_t i = 0; i < s->used; i++) {
          if (s->text[i] == '.' && i + 1 == s->used && i > 0 &&
              s->text[i - 1] >= '0' && s->text[i - 1] <= '9') continue;
          if (rv_sentence_punctuation(s->text + i, s->used - i, i, 0)) {
            s->first_logged = true;
            RV_EVENT("[耗时][Flash分句] 首个完整句子=%lldms\n",
              rv_elapsed_ms(s->started_ms, robot_voice_now_ms()));
            RV_VERBOSE("[RV-LLM-TTS] first_complete_sentence mono_ms=%llu run=%lu\n",
                   robot_voice_now_ms(), s->run_id);
            break;
          }
        }
      }
    }
  }
  pthread_cond_broadcast(&s->changed); pthread_mutex_unlock(&s->lock);
}

void robot_voice_llm_stream_end(void *arg, int status)
{
  struct rv_llm_speech_s *s = arg;
  if (!s) return;
  pthread_mutex_lock(&s->lock);
  if (status < 200 || status >= 300) s->cancel = true;
  if (!s->cancel && s->used && !s->first_logged)
    RV_EVENT("[耗时][Flash分句] 流结束后提交尾句=%lldms\n",
      rv_elapsed_ms(s->started_ms, robot_voice_now_ms()));
  s->eos = true;
  pthread_cond_broadcast(&s->changed); pthread_mutex_unlock(&s->lock);
  /* The Agent watchdog measures HTTP/SSE work only. The independent speech
   * owner drains audio and retires this context once both references end. */
  rv_llm_speech_put(s);
}

static int voice_channel_speak_internal(const char *text, bool agent_reply,
                                        unsigned long run_id)
{
  return voice_channel_speak_source(text, agent_reply, run_id, NULL);
}

int voice_channel_speak(const char *text)
{
  int ret = voice_channel_speak_internal(text, true, 0);

  /* Feishu replies use run_id=0 and must remain speakable even if the
   * previous capture worker failed.  Restore continuous wake listening only
   * after the speaker has been fully drained. */
  if (g_voice_restart_pending)
    {
      (void)rv_restart_voice_after_failure();
    }
  return ret;
}

/* Synchronous system notice used by non-Agent activities such as music
 * loading. It deliberately does not complete or mutate an Agent reply. */
int robot_voice_speak_notice(const char *text)
{
  return voice_channel_speak_internal(text, false, 0);
}


struct contest_cli_tts_job_s
{
  char *text;
  bool agent_reply;
  unsigned long run_id;
};

static void *robot_voice_cli_tts_worker(void *arg)
{
  struct contest_cli_tts_job_s *job = arg;
  unsigned int waited_ms = 0;
  int ret = -EBUSY;

  /* Retain the existing power policy: finish servo motion before audio. */
  while (robot_motion_is_busy() &&
         waited_ms < CONTEST_CLI_TTS_MOTION_WAIT_MS &&
         !(job->run_id != 0 && g_voice_stop_requested))
    {
      usleep(100 * 1000);
      waited_ms += 100;
    }
  if (!robot_motion_is_busy())
    {
      ret = voice_channel_speak_internal(job->text, job->agent_reply,
                                         job->run_id);
    }
  else if (job->agent_reply)
    {
      g_voice_reply_pending = false;
      rv_reply_done(true);
    }
  RV_VERBOSE("[RV-TTS] queued speech done rc=%d\n", ret);
  free(job->text);
  free(job);
  pthread_mutex_lock(&g_voice_lock);
  g_tts_jobs--;
  pthread_cond_broadcast(&g_tts_jobs_changed);
  pthread_mutex_unlock(&g_voice_lock);
  if (robot_task_resume_if_idle) robot_task_resume_if_idle();
  return NULL;
}

static int robot_voice_tts_submit(const char *text, bool agent_reply,
                                   unsigned long run_id)
{
  struct contest_cli_tts_job_s *job;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }
  if (robot_voice_prompt_id(text) < 0 &&
      strcmp(g_voice_tts_backend, "mimo-v2.5-tts") != 0)
    {
      return -ENOSYS;
    }
  job = calloc(1, sizeof(*job));
  if (job == NULL)
    {
      return -ENOMEM;
    }
  job->text = strdup(text);
  job->agent_reply = agent_reply;
  job->run_id = run_id;
  if (job->text == NULL)
    {
      free(job);
      return -ENOMEM;
    }
  pthread_mutex_lock(&g_voice_lock);
  if (g_tts_jobs >= 2 || (run_id != 0 && g_voice_stop_requested))
    {
      pthread_mutex_unlock(&g_voice_lock);
      free(job->text);
      free(job);
      return -EBUSY;
    }
  g_tts_jobs++;
  pthread_mutex_unlock(&g_voice_lock);
  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = rv_thread_attributes(&attr, CONTEST_CLI_TTS_STACK,
                                  ROBOT_VOICE_CONSUMER_PRIORITY);
      if (ret == 0)
        {
          /* Detached at creation: a fast worker cannot leak join resources. */
          ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        }
      if (ret == 0)
        {
          ret = pthread_create(&thread, &attr, robot_voice_cli_tts_worker, job);
        }
      pthread_attr_destroy(&attr);
    }
  if (ret != 0)
    {
      pthread_mutex_lock(&g_voice_lock);
      g_tts_jobs--;
      pthread_cond_broadcast(&g_tts_jobs_changed);
      pthread_mutex_unlock(&g_voice_lock);
      free(job->text);
      free(job);
      return -ret;
    }
  RV_VERBOSE("[RV-TTS] speech queued; outbound bus stays available\n");
  return 0;
}

static int robot_voice_cli_tts_submit(const char *text)
{
  return robot_voice_tts_submit(text, false, 0);
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

  ret = ROBOT_VOICE_TTS_LIVE ? robot_audio_playback_open_voice_live() :
                              robot_audio_playback_open_voice();
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
  char text[ROBOT_VOICE_TEXT_CAP];
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

  /* The Agent music fast-path can run before any normal reply is emitted. */
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

  /* Whichever voice backend is selected first owns the one-shot startup. */
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
  char text[ROBOT_VOICE_TEXT_CAP];
  size_t len = 0;
  int ret;
  if (!pcm) return -ENOMEM;
  ret = robot_voice_capture_record(pcm, cap, &len,
                                   CONTEST_VOICE_CAPTURE_SECONDS * 1000);
  if (ret == 0) ret = robot_voice_afe_recording(pcm, len, g_monitor.afe_mode);
  if (ret == 0) ret = contest_voice_run_asr(pcm, len, text, sizeof(text));
  if (ret == 0) printf("ASR: %s\n", text);
  free(pcm);
  return ret;
}


static int robot_voice_tts_test(const char *text)
{
  int ret = ROBOT_VOICE_TTS_LIVE ? robot_audio_playback_open_voice_live() :
                                  robot_audio_playback_open_voice();
  if (ret < 0) return ret;
  ret = mimo_tts_speak_stream(g_api_key, text, robot_voice_pcm_sink, NULL);
  if (ret == 0) ret = robot_audio_playback_finish();
  robot_audio_playback_close();
  return ret;
}

struct rv_tts_net_probe_s
{
  size_t bytes;
  unsigned int chunks;
  unsigned long long started, first, last, max_gap;
};

static int rv_tts_net_probe_sink(const uint8_t *pcm, size_t bytes, void *arg)
{
  struct rv_tts_net_probe_s *probe = arg;
  unsigned long long now = robot_voice_now_ms();
  if (!probe) return -EINVAL;
  if ((!pcm && bytes) || (bytes & 1)) return -EPROTO;
  if (bytes)
    {
      if (!probe->first) probe->first = now;
      if (probe->last && now - probe->last > probe->max_gap)
        probe->max_gap = now - probe->last;
      probe->last = now;
      probe->bytes += bytes;
      probe->chunks++;
    }
  return 0; /* Network + TLS + SSE only; no I2S, scaling or playback pacing. */
}

static int robot_voice_tts_net_test(const char *text)
{
  struct rv_tts_net_probe_s probe = {0};
  if (g_voice_speaking || g_tts_jobs) return -EBUSY;
  if (!g_api_key[0] || !text || !text[0]) return -EINVAL;
  probe.started = robot_voice_now_ms();
  int ret = mimo_tts_speak_stream_on_worker_timeout(g_api_key, text,
    rv_tts_net_probe_sink, &probe, ROBOT_VOICE_HTTP_TTS_MS);
  unsigned long long elapsed = robot_voice_now_ms() - probe.started;
  RV_EVENT("[RV-TTS-NET-TEST] text_bytes=%zu chunks=%u pcm=%zu "
           "audio_ms=%llu first_pcm_ms=%lld total_ms=%llu max_gap_ms=%llu "
           "realtime_milli=%llu rc=%d\n",
    strlen(text), probe.chunks, probe.bytes,
    (unsigned long long)probe.bytes * 1000 / (ROBOT_VOICE_TTS_RATE * 2),
    rv_elapsed_ms(probe.started, probe.first), elapsed, probe.max_gap,
    elapsed ? (unsigned long long)probe.bytes * 1000000 /
      ((ROBOT_VOICE_TTS_RATE * 2ULL) * elapsed) : 0, ret);
  return ret;
}

int main(int argc, char *argv[])
{
  if (g_api_key[0] == '\0')
    {
      voice_get_api_key(g_api_key, sizeof(g_api_key));
    }

  if (argc >= 3 && strcmp(argv[1], "tts_net_test") == 0)
    return robot_voice_tts_net_test(argv[2]);

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
  if (argc >= 2 && strcmp(argv[1], "prompt_list") == 0) {
    for (int i = 0; i < RV_PROMPT_COUNT; i++) {
      struct robot_voice_prompt_asset_s asset = robot_voice_prompt_asset(i);
      printf("%s.wav: %s PCM=%zuB%s\n", robot_voice_prompt_name(i),
             robot_voice_prompt_text(i), asset.size, asset.size ? "" : " (tone fallback)");
    }
    return 0;
  }
  if (argc >= 3 && strcmp(argv[1], "prompt_test") == 0) {
    for (int i = 0; i < RV_PROMPT_COUNT; i++)
      if (!strcmp(argv[2], robot_voice_prompt_name(i))) {
        pthread_mutex_lock(&g_voice_speak_lock);
        int ret = rv_play_local_prompt_locked(i, 0);
        rv_reply_done(false);
        pthread_mutex_unlock(&g_voice_speak_lock);
        return ret;
      }
    return -EINVAL;
  }
  if (argc >= 2 && strcmp(argv[1], "test_wakeword") == 0)
    {
      if (g_voice_thread_valid) return -EBUSY;
      return robot_wakeword_selftest();
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
  if (argc == 3 && strcmp(argv[1], "afe_mode") == 0)
    {
      enum robot_voice_afe_mode_e mode;
      if (!strcmp(argv[2], "clean")) mode = ROBOT_AFE_CLEAN;
      else if (!strcmp(argv[2], "raw")) mode = ROBOT_AFE_RAW;
      else if (!strcmp(argv[2], "kws_raw")) mode = ROBOT_AFE_KWS_RAW;
      else { printf("afe_mode: clean | raw | kws_raw\n"); return 1; }
      pthread_mutex_lock(&g_monitor.lock);
      if (g_monitor.enabled || g_voice_running)
        {
          pthread_mutex_unlock(&g_monitor.lock);
          printf("afe_mode: stop voice capture before switching\n");
          return 1;
        }
      g_monitor.afe_mode = mode;
      pthread_mutex_unlock(&g_monitor.lock);
      printf("[RV-AFE] mode=%u; applied to next voice_start\n", (unsigned int)mode);
      return 0;
    }
  if (argc >= 2 && strcmp(argv[1], "diag") == 0)
    {
      robot_voice_clock_diag();
      printf("robot_voice: wake=TFLiteMicro playback=voice-%s(direct-I2S) interrupt=%s log_verbose=%u host=%s asr=%s llm=%s tts=%s capture=%dHz tts=%dHz\n",
             ROBOT_VOICE_TTS_LIVE ? "live-SSE" :
               (ROBOT_VOICE_TTS_CONTINUITY ? "turn-cache" : "sentence-cache/prefetch-next"),
             "asr-before-flash-v4",
             (unsigned int)ROBOT_VOICE_LOG_VERBOSE,
             ROBOT_VOICE_HOST, ROBOT_VOICE_ASR_MODEL, ROBOT_VOICE_LLM_MODEL,
             ROBOT_VOICE_TTS_MODEL, ROBOT_VOICE_CAPTURE_RATE, ROBOT_VOICE_TTS_RATE);
      pthread_mutex_lock(&g_monitor.lock);
      printf("robot_voice: monitor=%d state=%s workers=%u queue=%u "
             "inflight=%u frames=%lu during_asr=%lu dropped=%lu\n",
             g_monitor.enabled, contest_voice_state_name(g_voice_state),
             ROBOT_VOICE_ASR_WORKERS, g_monitor.queued, g_monitor.in_flight,
             g_monitor.frames, g_monitor.frames_during_asr, g_monitor.dropped);
      rv_afe_diag_locked();
      RV_VERBOSE("[RV-HEALTH] phase=%d running=%d stop=%u capture_exited=%u "
             "capture_rc=%d muted=%u asr_failures=%u pending_bytes=%zu\n",
             g_monitor.phase, g_voice_running, g_voice_stop_requested,
             g_monitor.capture_exited, g_monitor.capture_result, g_monitor.muted,
             g_monitor.asr_failures, strlen(g_monitor.pending_text));
      pthread_mutex_unlock(&g_monitor.lock);
      return 0;
    }
  if (argc == 2 && strcmp(argv[1], "clock_ready") == 0)
    {
      int ret = robot_voice_clock_confirm();
      if (ret < 0) printf("[RV-CLOCK] confirm rejected rc=%d; synchronize board clock first\n", ret);
      return ret;
    }
  if (argc == 2 && strcmp(argv[1], "clock_clear") == 0)
    {
      robot_voice_clock_clear();
      return 0;
    }
  printf("Usage: robot_voice set_key <api_key> | once | test_capture | "
         "test_asr | test_wakeword | test_llm <text> | music_play <path> | "
         "test_wav <path> | test_tts <text> | tts_net_test <text> | prompt_list | prompt_test <name> | afe_mode <clean|raw|kws_raw> | diag | clock_ready | clock_clear\n");
  return 0;
}
