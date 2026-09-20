############################################################################
# Contest 2026 raw PCM voice echo application
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = voice_echo
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 6144
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO)

ifeq ($(CONFIG_EXAMPLES_AI_AGENT_VELA),y)
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/../packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/../packages/ai_agent/src
endif

MAINSRC = voice_echo_main.c

include $(APPDIR)/Application.mk
