#ifndef ROBOT_VOICE_AGENT_HOOKS_H
#define ROBOT_VOICE_AGENT_HOOKS_H
#include "robot_voice_platform_log.h"
/* Force-included by the contest board's existing EXTRAFLAGS. No official
 * source/build file is rewritten. The contest app defines OWNED_BUILD, so its
 * implementations stay strong. Other objects resolve these public symbols to
 * the contest implementations, including the checked Wi-Fi connect path.
 * The main application's strong link anchor ensures archive extraction. */
#if defined(ROBOT_VOICE_AGENT_INTERPOSE) && !defined(ROBOT_VOICE_OWNED_BUILD)
#  if defined(__has_include)
#    if __has_include("agent_config.h")
#      include "agent_config.h"
/* An interactive voice request must not sleep through 2+4+8 seconds of
 * automatic 429 retries. Report the real failure and let the user retry. */
#      undef AGENT_LLM_MAX_RETRIES
#      define AGENT_LLM_MAX_RETRIES 0
#    endif
#  endif
#  pragma weak vela_https_post_json
#  pragma weak tool_get_weather_execute
#  pragma weak skill_loader_build_summary
#  pragma weak voice_channel_init
#  pragma weak voice_channel_start
#  pragma weak voice_channel_stop
#  pragma weak voice_channel_speak
#  pragma weak voice_channel_test_tts
#  pragma weak voice_channel_test_asr
#  pragma weak voice_tts_set_backend
#  pragma weak voice_asr_set_backend
#  pragma weak voice_tts_get_backend
#  pragma weak voice_asr_get_backend
#endif
#endif
