# ESP32-S3-WROOM-1-N16R8 contest board sources

target_sources(board PRIVATE board_boot.c board_bringup.c)

if(CONFIG_ESP32S3_I2S)
  target_sources(board PRIVATE board_voice_i2s.c)
endif()

if(CONFIG_CONTEST_BOARD_I2S1_TX AND CONFIG_DRIVERS_AUDIO AND CONFIG_AUDIO_I2S)
  target_sources(board PRIVATE board_voice_audio.c)
endif()

if(CONFIG_ETC_ROMFS)
  add_board_rcsrcs(${CMAKE_CURRENT_SOURCE_DIR}/etc/init.d/rcS
                   ${CMAKE_CURRENT_SOURCE_DIR}/etc/init.d/rc.sysinit)
endif()

if(CONFIG_MEDIA_SERVER)
  add_board_rcsrcs(${CMAKE_CURRENT_SOURCE_DIR}/etc/media/graph.conf)
  add_board_rcraws(${CMAKE_CURRENT_SOURCE_DIR}/etc/media/criteria.txt
                   ${CMAKE_CURRENT_SOURCE_DIR}/etc/media/settings.pfw
                   ${CMAKE_CURRENT_SOURCE_DIR}/etc/media/test.wav)
endif()

if(CONFIG_CONTEST_BOARD_I2S0_RX OR CONFIG_CONTEST_BOARD_I2S1_TX)
  target_sources(board PRIVATE contest_i2s.c)
endif()

if(CONFIG_LCD_SSD1306_I2C)
  target_sources(board PRIVATE board_oled.c)
endif()

if(CONFIG_ARCH_BUTTONS)
  target_sources(board PRIVATE board_buttons.c)
endif()

if(CONFIG_BOARDCTL)
  target_sources(board PRIVATE board_appinit.c)
endif()
