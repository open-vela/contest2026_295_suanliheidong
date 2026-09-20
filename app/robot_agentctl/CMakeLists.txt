if(CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_AGENTCTL)
  nuttx_add_application(
    NAME robot_agentctl
    SRCS robot_agentctl.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/../../../packages/ai_agent/include
                         ${CMAKE_CURRENT_LIST_DIR}/../robot_proactive
    STACKSIZE 4096)
endif()
