############################################################################
# Independent Contest MiMo robot voice application
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = robot_voice
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 32768
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/src
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../external/libhelix-mp3/libhelix-mp3/pub
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../external/ffmpeg/ffmpeg
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robot_expression
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robot_core
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robot_motion
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robot_proactive
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robotctl

VPATH += ../robot_expression
VPATH += ../robot_core
VPATH += ../robot_motion
VPATH += ../robot_proactive

# Generate the contest-local Skill resource table during Makefile parsing.
# The generator copies raw Markdown bytes only; official skill_loader remains
# responsible for parsing and using the installed Skill files.
CONTEST_ROOT := $(APPDIR)/../contest2026_295_suanliheidong
SKILL_BUNDLE_DIR := $(TOPDIR)/.generated/contest_robot_voice
SKILL_BUNDLE := $(SKILL_BUNDLE_DIR)/contest_skill_bundle.c
SKILL_MANIFEST := $(CONTEST_ROOT)/skills/manifest.json
SKILL_FILES := $(wildcard $(CONTEST_ROOT)/skills/*.md)
$(shell mkdir -p "$(SKILL_BUNDLE_DIR)" && python3 \
  $(CONTEST_ROOT)/scripts/generate_skill_bundle.py \
  --input $(CONTEST_ROOT)/skills --manifest "$(SKILL_MANIFEST)" \
  --output "$(SKILL_BUNDLE)" >/dev/null)
VPATH += $(SKILL_BUNDLE_DIR)

# WAVs are host build inputs, not runtime files. Empty slots use a local tone.
VOICE_PROMPT_GEN := $(TOPDIR)/.generated/robot_voice_prompts
VOICE_PROMPT_BUNDLE := $(VOICE_PROMPT_GEN)/robot_voice_prompt_data.c
VOICE_PROMPT_STATUS := $(shell mkdir -p "$(VOICE_PROMPT_GEN)" && python3 \
  $(CONTEST_ROOT)/app/robot_voice/scripts/generate_voice_prompts.py \
  --input $(CONTEST_ROOT)/app/robot_voice/assets/voice \
  --output "$(VOICE_PROMPT_BUNDLE)"; echo $$?)
ifneq ($(VOICE_PROMPT_STATUS),0)
$(error Local voice prompt generation failed; check assets/voice WAV format)
endif
VPATH += $(VOICE_PROMPT_GEN)
CFLAGS += ${INCDIR_PREFIX}$(CONTEST_ROOT)/app/robot_voice

# The board entry supplies the interposition header to all build objects.
# Contest definitions stay strong; no official file is modified.
CFLAGS += -DROBOT_VOICE_OWNED_BUILD=1
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include

MAINSRC = robot_voice_main.c

include $(CONTEST_ROOT)/app/robot_voice/robot_voice_afe_sources.mk
CSRCS += $(ROBOT_AFE_SRCS)
CSRCS += robot_voice_capture.c robot_audio_playback.c robot_music_player.c
CSRCS += robot_media_player.c
CSRCS += robot_voice_media_stream.c
CSRCS += robot_music_media.c
CSRCS += robot_http_transport.c
CSRCS += mimo_client.c mimo_asr.c mimo_llm.c mimo_llm_stream.c mimo_tts.c mimo_tts_stream.c
CSRCS += robot_voice_http.c robot_voice_http_parser.c robot_voice_weather.c
CSRCS += robot_voice_clock.c robot_voice_wifi.c
CSRCS += robot_voice_prompts.c robot_voice_prompt_data.c
CSRCS += robot_voice_skill_guard.c
CSRCS += robot_expression.c robot_expression_tool.c robot_oled.c robot_oled_expr.c
CSRCS += robot_action_guard.c robot_network_adapter.c robot_ffmpeg_amix_compat.c
CSRCS += robot_motion_tool.c
CSRCS += robot_event.c robot_music_observer.c robot_skill_installer.c robot_world_state.c robot_proactive.c
CSRCS += contest_skill_bundle.c

# Acoustic wakeword. The imported model and frontend constants live in RODATA.
# This is an ESP32-S3: use portable kernels, not ARM CMSIS-NN or HiFi4 kernels.
CSRCS += robot_wakeword.c robot_wakeword_frontend.c
CXXEXT = .cc
CXXSRCS += robot_wakeword_tflm.cc robot_wakeword_model_data.cc
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/mlearning/tflite-micro/tflite-micro
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/system/flatbuffers/flatbuffers/include
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/gemmlowp/gemmlowp
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/ruy/ruy
# Match the Make build's TFLM public header layout/rounding configuration.
CXXFLAGS += -DTFLITE_EMULATE_FLOAT -DTF_LITE_DISABLE_X86_NEON
CXXFLAGS += -Wno-shadow -Wno-sign-compare -Wno-undef
ifeq ($(CONFIG_TFLITEMICRO_DEBUG),)
CXXFLAGS += -DTF_LITE_STRIP_ERROR_STRINGS
endif

include $(APPDIR)/Application.mk
