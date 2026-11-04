if(CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE)
  if(NOT CONFIG_TFLITEMICRO)
    message(FATAL_ERROR "robot_voice requires CONFIG_TFLITEMICRO and its math/FlatBuffers dependencies")
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  file(GLOB VOICE_PROMPT_WAVS CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_LIST_DIR}/assets/voice/*.wav)
  set(VOICE_PROMPT_DATA ${CMAKE_CURRENT_BINARY_DIR}/robot_voice_prompt_data.c)
  add_custom_target(robot_voice_prompt_assets
    BYPRODUCTS ${VOICE_PROMPT_DATA}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/generate_voice_prompts.py
            --input ${CMAKE_CURRENT_LIST_DIR}/assets/voice --output ${VOICE_PROMPT_DATA}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/scripts/generate_voice_prompts.py ${VOICE_PROMPT_WAVS}
    VERBATIM)
  file(GLOB CONTEST_SKILL_FILES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_LIST_DIR}/../../skills/*.md)
  set(CONTEST_SKILL_MANIFEST
      ${CMAKE_CURRENT_LIST_DIR}/../../skills/manifest.json)
  set(CONTEST_SKILL_BUNDLE
      ${CMAKE_CURRENT_BINARY_DIR}/contest_skill_bundle.c)
  add_custom_command(
    OUTPUT ${CONTEST_SKILL_BUNDLE}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate_skill_bundle.py
            --input ${CMAKE_CURRENT_LIST_DIR}/../../skills
            --manifest ${CONTEST_SKILL_MANIFEST}
            --output ${CONTEST_SKILL_BUNDLE}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate_skill_bundle.py
            ${CONTEST_SKILL_MANIFEST} ${CONTEST_SKILL_FILES}
    VERBATIM)

  include(${CMAKE_CURRENT_LIST_DIR}/robot_voice_afe_sources.cmake)

  nuttx_add_application(
    NAME robot_voice
    DEPENDS tflite_micro
    DEFINITIONS TF_LITE_STATIC_MEMORY TF_LITE_DISABLE_X86_NEON
                TFLITE_WITH_STABLE_ABI=0 TFLITE_USE_OPAQUE_DELEGATE=0
                TFLITE_SINGLE_ROUNDING=0
    SRCS ${ROBOT_AFE_SRCS} robot_voice_prompts.c ${VOICE_PROMPT_DATA}
         robot_wakeword.c robot_wakeword_frontend.c
         robot_wakeword_tflm.cc robot_wakeword_model_data.cc
         robot_voice_main.c robot_voice_capture.c robot_audio_playback.c robot_music_player.c robot_music_media.c robot_media_player.c robot_voice_media_stream.c robot_http_transport.c mimo_client.c mimo_asr.c mimo_llm.c mimo_llm_stream.c mimo_tts.c mimo_tts_stream.c robot_voice_http.c robot_voice_http_parser.c robot_voice_weather.c robot_voice_skill_guard.c robot_voice_clock.c robot_voice_wifi.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_expression/robot_expression.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_expression/robot_expression_tool.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_expression/robot_oled.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_expression/robot_oled_expr.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_core/robot_action_guard.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_core/robot_network_adapter.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_core/robot_ffmpeg_amix_compat.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_motion/robot_motion_tool.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive/robot_event.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive/robot_music_observer.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive/robot_skill_installer.c
         ${CONTEST_SKILL_BUNDLE}
         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive/robot_world_state.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive/robot_proactive.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}
                         ${CMAKE_CURRENT_LIST_DIR}/../../../../packages/ai_agent/include
                         ${CMAKE_CURRENT_LIST_DIR}/../../../../packages/ai_agent/src
                         ${CMAKE_CURRENT_LIST_DIR}/../../../../apps/netutils/cjson/cJSON
                         ${CMAKE_CURRENT_LIST_DIR}/../../../../apps/crypto/mbedtls/mbedtls/include
                         ${CMAKE_CURRENT_LIST_DIR}/../../../../external/libhelix-mp3/libhelix-mp3/pub
                         ${CMAKE_CURRENT_LIST_DIR}/../../../../external/ffmpeg/ffmpeg
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_expression
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_core
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_motion
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive
                         ${CMAKE_CURRENT_LIST_DIR}/../robotctl
         STACKSIZE 32768)

  add_dependencies(apps_robot_voice robot_voice_prompt_assets)

  target_compile_definitions(apps_robot_voice PRIVATE ROBOT_VOICE_OWNED_BUILD=1
                                                    ROBOT_VOICE_AGENT_INTERPOSE=1)
  if(NOT CONFIG_TFLITEMICRO_DEBUG)
    target_compile_definitions(apps_robot_voice PRIVATE TF_LITE_STRIP_ERROR_STRINGS)
  endif()
  function(rv_apply_official_agent_hooks target hook)
    if(NOT TARGET ${target})
      message(FATAL_ERROR "robot_voice: official Agent target ${target} is unavailable")
    endif()
    target_compile_definitions(${target} PRIVATE ROBOT_VOICE_AGENT_INTERPOSE=1)
    target_compile_options(${target} PRIVATE -include "${hook}")
    get_target_property(agent_sources ${target} SOURCES)
    foreach(agent_source IN LISTS agent_sources)
      if(agent_source MATCHES "(^|/)tool_media[.]c$")
        get_filename_component(redirect "${hook}" DIRECTORY)
        set_property(SOURCE "${agent_source}" TARGET_DIRECTORY ${target} APPEND
                     PROPERTY COMPILE_OPTIONS -include "${redirect}/robot_music_media_redirect.h")
      endif()
    endforeach()
  endfunction()
  # Defer until all application targets exist; never patch official CMake.
  if(CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR "robot_voice: use the board Make entry or CMake >= 3.19")
  endif()
  cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" CALL rv_apply_official_agent_hooks \"apps_${CONFIG_EXAMPLES_AI_AGENT_VELA_PROGNAME}\" \"${CMAKE_CURRENT_LIST_DIR}/robot_voice_agent_hooks.h\")")
endif()
