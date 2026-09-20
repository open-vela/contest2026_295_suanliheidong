if(CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOTCTL)
  nuttx_add_application(
    NAME robotctl
    SRCS robotctl_main.c robotctl_servo.c
         ${CMAKE_CURRENT_LIST_DIR}/../robot_motion/robot_motion.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/../robot_motion
    STACKSIZE 4096)
endif()
