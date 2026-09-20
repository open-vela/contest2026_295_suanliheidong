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
SKILL_FILES := $(wildcard $(CONTEST_ROOT)/skills/*.md)
$(shell mkdir -p "$(SKILL_BUNDLE_DIR)" && python3 \
  $(CONTEST_ROOT)/scripts/generate_skill_bundle.py \
  --input $(CONTEST_ROOT)/skills --output "$(SKILL_BUNDLE)" >/dev/null)
VPATH += $(SKILL_BUNDLE_DIR)

MAINSRC = robot_voice_main.c

CSRCS += robot_voice_capture.c robot_audio_playback.c robot_music_player.c
ifeq ($(CONFIG_MEDIA),)
CSRCS += robot_media_player.c
endif
CSRCS += mimo_client.c mimo_asr.c mimo_llm.c mimo_tts.c
CSRCS += robot_expression.c robot_expression_tool.c robot_oled.c robot_oled_expr.c
CSRCS += robot_action_guard.c robot_network_adapter.c robot_ffmpeg_amix_compat.c
CSRCS += robot_motion_tool.c
CSRCS += robot_event.c robot_music_observer.c robot_skill_installer.c robot_world_state.c robot_proactive.c
CSRCS += contest_skill_bundle.c

include $(APPDIR)/Application.mk
