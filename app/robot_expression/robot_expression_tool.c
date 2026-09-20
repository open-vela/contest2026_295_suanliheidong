/*
 * Contest-local robot_set_expression Tool Provider.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netutils/cJSON.h>

#include "agent_compat.h"
#include "tools/tool_registry.h"
#include "robot_action_guard.h"
#include "robot_expression.h"
#include "robot_oled.h"

#define ROBOT_EXPRESSION_TOOL_NAME "robot_set_expression"
#define ROBOT_REACTION_TOOL_NAME   "robot_react"
#define ROBOT_REACTION_DEFAULT_MS  1800
#define ROBOT_REACTION_MIN_MS      500
#define ROBOT_REACTION_MAX_MS      6000


static bool g_robot_expression_tool_registered;

static enum robot_expression_e robot_expression_from_name(const char *name)
{
  enum robot_expression_e expression;

  if (name == NULL)
    {
      return ROBOT_EXPRESSION_COUNT;
    }
  for (expression = 0; expression < ROBOT_EXPRESSION_COUNT; expression++)
    {
      if (strcmp(name, robot_expression_name(expression)) == 0)
        {
          return expression;
        }
    }
  return ROBOT_EXPRESSION_COUNT;
}

static char *robot_expression_tool_get_tools(void)
{
  static const char tools_json[] =
    "[{\"name\":\"robot_set_expression\","
    "\"description\":\"" ROBOT_SIDE_EFFECT_TOOL_POLICY
    "Set the robot dog's persistent OLED facial expression. Use this when the "
    "user explicitly asks for a lasting face. For short proactive emotion "
    "during music, tail wagging, or a spoken reply, prefer robot_react.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"expression\":{\"type\":\"string\","
    "\"enum\":[\"idle\",\"listening\",\"thinking\",\"happy\","
    "\"speaking\",\"error\",\"excited\",\"wink\",\"love\",\"surprised\","
    "\"cool\",\"playful\",\"sleepy\",\"singing\",\"curious\",\"proud\"]}},"
    "\"required\":[\"expression\"]}},"
    "{\"name\":\"robot_react\","
    "\"description\":\"" ROBOT_SIDE_EFFECT_TOOL_POLICY
    "Show a short-lived expressive OLED reaction. This is designed for "
    "proactive personality around music playback, tail wagging, tool success, "
    "and spoken/TTS replies. The reaction has priority above normal voice "
    "speaking only while its timer is active; after expiry the underlying "
    "speaking/music/behavior expression automatically becomes visible again. "
    "Use one meaningful reaction, not rapid frame spam.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"expression\":{\"type\":\"string\","
    "\"enum\":[\"happy\",\"excited\",\"wink\",\"love\",\"surprised\","
    "\"cool\",\"playful\",\"sleepy\",\"singing\",\"curious\",\"proud\"]},"
    "\"duration_ms\":{\"type\":\"integer\",\"minimum\":500,\"maximum\":6000,"
    "\"default\":1800}},\"required\":[\"expression\"]}}]";

  printf("[RV-EXPR-TOOL] tools_json requested\n");
  return strdup(tools_json);
}

static int robot_expression_tool_parse_uint(cJSON *item,
                                            unsigned int minimum,
                                            unsigned int maximum,
                                            unsigned int *value)
{
  double number;

  if (item == NULL || value == NULL || !cJSON_IsNumber(item))
    {
      return -EINVAL;
    }

  number = item->valuedouble;
  if (number < minimum || number > maximum ||
      number != (double)(unsigned int)number)
    {
      return -EINVAL;
    }

  *value = (unsigned int)number;
  return 0;
}

static int robot_reaction_tool_execute(const char *input_json,
                                       char *output, size_t output_size)
{
  cJSON *root;
  cJSON *expression_item;
  cJSON *duration_item;
  const char *expression_name;
  enum robot_expression_e expression;
  unsigned int duration_ms = ROBOT_REACTION_DEFAULT_MS;
  int ret;

  if (input_json == NULL || output == NULL || output_size == 0)
    {
      return ERROR;
    }

  printf("[RV-REACT-TOOL] execute input=%s\n", input_json);
  root = cJSON_Parse(input_json);
  if (root == NULL)
    {
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"invalid json\"}");
      return ERROR;
    }

  expression_item = cJSON_GetObjectItem(root, "expression");
  expression_name = cJSON_GetStringValue(expression_item);
  expression = robot_expression_from_name(expression_name);
  if (expression == ROBOT_EXPRESSION_COUNT ||
      expression == ROBOT_EXPRESSION_IDLE ||
      expression == ROBOT_EXPRESSION_LISTENING ||
      expression == ROBOT_EXPRESSION_THINKING ||
      expression == ROBOT_EXPRESSION_SPEAKING ||
      expression == ROBOT_EXPRESSION_ERROR)
    {
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"unsupported reaction\"}");
      return ERROR;
    }

  duration_item = cJSON_GetObjectItem(root, "duration_ms");
  if (duration_item != NULL &&
      robot_expression_tool_parse_uint(duration_item,
                                       ROBOT_REACTION_MIN_MS,
                                       ROBOT_REACTION_MAX_MS,
                                       &duration_ms) < 0)
    {
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"duration_ms must be 500..6000\"}");
      return ERROR;
    }

  cJSON_Delete(root);

  robot_action_guard_begin(ROBOT_ACTION_EXPRESSION,
                           ROBOT_REACTION_TOOL_NAME);
  ret = robot_expression_set(ROBOT_EXPRESSION_SOURCE_REACTION,
                             expression, duration_ms);
  robot_action_guard_end(ROBOT_ACTION_EXPRESSION,
                         ROBOT_REACTION_TOOL_NAME, ret);

  printf("[RV-REACT-TOOL] expression=%s duration_ms=%u rc=%d\n",
         robot_expression_name(expression), duration_ms, ret);
  if (ret < 0)
    {
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"reaction display unavailable\"}");
      return ERROR;
    }

  snprintf(output, output_size,
           "{\"ok\":true,\"expression\":\"%s\",\"duration_ms\":%u,"
           "\"temporary\":true}",
           robot_expression_name(expression), duration_ms);
  return OK;
}

static int robot_expression_tool_execute(const char *name,
                                         const char *input_json,
                                         char *output,
                                         size_t output_size)
{
  cJSON *root;
  cJSON *expression_item;
  const char *expression_name;
  enum robot_expression_e expression;
  int ret;

  if (name != NULL && strcmp(name, ROBOT_REACTION_TOOL_NAME) == 0)
    {
      return robot_reaction_tool_execute(input_json, output, output_size);
    }

  if (name == NULL || input_json == NULL || output == NULL ||
      output_size == 0 || strcmp(name, ROBOT_EXPRESSION_TOOL_NAME) != 0)
    {
      printf("[RV-EXPR-TOOL] execute rejected: invalid arguments/name\n");
      return ERROR;
    }

  printf("[RV-EXPR-TOOL] execute name=%s input=%s\n", name, input_json);

  root = cJSON_Parse(input_json);
  if (root == NULL)
    {
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"invalid json\"}");
      return ERROR;
    }

  expression_item = cJSON_GetObjectItem(root, "expression");
  expression_name = cJSON_GetStringValue(expression_item);
  expression = robot_expression_from_name(expression_name);
  if (expression == ROBOT_EXPRESSION_COUNT)
    {
      cJSON_Delete(root);
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"unknown expression\"}");
      return ERROR;
    }

  /*
   * Agent-selected expressions are persistent.  duration_ms == 0 means this
   * source stays active until the next Agent expression replaces it.
   */
  printf("[RV-EXPR-TOOL] parsed expression=%s persistent=1\n",
         robot_expression_name(expression));

  robot_action_guard_begin(ROBOT_ACTION_EXPRESSION,
                           ROBOT_EXPRESSION_TOOL_NAME);
  ret = robot_expression_set(ROBOT_EXPRESSION_SOURCE_AGENT, expression, 0);
  robot_action_guard_end(ROBOT_ACTION_EXPRESSION,
                         ROBOT_EXPRESSION_TOOL_NAME, ret);
  cJSON_Delete(root);

  printf("[RV-EXPR-TOOL] expression_set rc=%d\n", ret);
  if (ret < 0)
    {
      snprintf(output, output_size,
               "{\"ok\":false,\"error\":\"expression display unavailable\"}");
      return ERROR;
    }

  snprintf(output, output_size,
           "{\"ok\":true,\"expression\":\"%s\",\"persistent\":true}",
           robot_expression_name(expression));
  return OK;
}

#define ROBOT_BOOT_DISPLAY_READY_RETRIES  100
#define ROBOT_BOOT_DISPLAY_POLL_US        (10 * 1000)
#define ROBOT_BOOT_DISPLAY_SETTLE_US      (250 * 1000)

static void robot_expression_boot_latch_idle(void)
{
  int ret;
  int i;
  bool ready = false;

  /*
   * Startup display and runtime expression management are deliberately
   * separated.
   *
   * We briefly start the OLED worker, draw IDLE, wait for the frame to reach
   * the SSD1306, then stop the worker.  The panel keeps the last framebuffer,
   * so ai_agent reaches its prompt with a naturally lit idle face but no OLED
   * or expression background thread remains active while Wi-Fi is brought up.
   *
   * The first real Agent/voice expression later starts the normal persistent
   * runtime workers through robot_expression_init().
   */
  printf("[RV-EXPR] boot idle display begin\n");

  ret = robot_oled_init();
  if (ret < 0)
    {
      printf("[RV-EXPR] boot idle display: oled init failed rc=%d\n", ret);
      return;
    }

  ret = robot_oled_submit(ROBOT_EXPRESSION_IDLE);
  if (ret < 0)
    {
      printf("[RV-EXPR] boot idle display: submit failed rc=%d\n", ret);
      (void)robot_oled_stop();
      return;
    }

  for (i = 0; i < ROBOT_BOOT_DISPLAY_READY_RETRIES; i++)
    {
      if (robot_oled_is_available())
        {
          ready = true;
          break;
        }

      usleep(ROBOT_BOOT_DISPLAY_POLL_US);
    }

  if (ready)
    {
      /*
       * Availability means board/plane setup has completed.  Give the worker
       * enough time to drain the already queued 128x64 idle frame before
       * asking it to exit.
       */
      usleep(ROBOT_BOOT_DISPLAY_SETTLE_US);
      printf("[RV-EXPR] boot idle display latched\n");
    }
  else
    {
      printf("[RV-EXPR] boot idle display: worker not ready\n");
    }

  /*
   * robot_oled_stop() does not power the SSD1306 off or clear it, so the last
   * idle frame remains visible while the background worker is gone.
   */
  (void)robot_oled_stop();
  printf("[RV-EXPR] boot OLED worker stopped; panel remains latched\n");
}

int robot_expression_register_tool(void)
{
  if (g_robot_expression_tool_registered)
    {
      return 0;
    }

  /*
   * Register the semantic Tool first.  Do NOT call robot_expression_init()
   * here: that would leave the long-lived expression/OLED workers running
   * during Wi-Fi bring-up, which previously correlated with the CPU1 panic.
   */
  tool_registry_register_provider("robot_expression",
                                  robot_expression_tool_get_tools,
                                  robot_expression_tool_execute);
  tool_registry_invalidate();
  g_robot_expression_tool_registered = true;
  printf("[RV-EXPR] tool provider registered (runtime lazy init)\n");

  /*
   * Light a stable IDLE face for the completed ai_agent startup, but tear the
   * temporary OLED worker back down before returning to the rest of startup.
   */
  robot_expression_boot_latch_idle();
  return 0;
}
