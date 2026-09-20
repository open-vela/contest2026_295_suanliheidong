# Contest 2026 raw PCM voice echo application

if(CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO)
  nuttx_add_application(
    NAME voice_echo
    SRCS voice_echo_main.c
    STACKSIZE 6144
    INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/../packages/ai_agent/include
    ${CMAKE_SOURCE_DIR}/../packages/ai_agent/src)
endif()
