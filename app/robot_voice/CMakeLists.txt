if(CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  file(GLOB CONTEST_SKILL_FILES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_LIST_DIR}/../../skills/*.md)
  set(CONTEST_SKILL_BUNDLE
      ${CMAKE_CURRENT_BINARY_DIR}/contest_skill_bundle.c)
  add_custom_command(
    OUTPUT ${CONTEST_SKILL_BUNDLE}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate_skill_bundle.py
            --input ${CMAKE_CURRENT_LIST_DIR}/../../skills
            --output ${CONTEST_SKILL_BUNDLE}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate_skill_bundle.py
            ${CONTEST_SKILL_FILES}
    VERBATIM)

  nuttx_add_application(
    NAME robot_voice
    SRCS robot_voice_main.c robot_voice_capture.c robot_audio_playback.c robot_music_player.c mimo_client.c mimo_asr.c mimo_llm.c mimo_tts.c
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
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/../../../packages/ai_agent/include
                         ${CMAKE_CURRENT_LIST_DIR}/../../../packages/ai_agent/src
                         ${CMAKE_CURRENT_LIST_DIR}/../../../apps/netutils/cjson/cJSON
                         ${CMAKE_CURRENT_LIST_DIR}/../../../external/libhelix-mp3/libhelix-mp3/pub
                         ${CMAKE_CURRENT_LIST_DIR}/../../../external/ffmpeg/ffmpeg
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_expression
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_core
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_motion
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive
                         ${CMAKE_CURRENT_BINARY_DIR}
                         ${CMAKE_CURRENT_LIST_DIR}/../robotctl
         STACKSIZE 32768)

  if(NOT CONFIG_MEDIA)
    target_sources(apps_robot_voice PRIVATE robot_media_player.c)
  endif()
endif()
