############################################################################
# Contest-local proactive runtime diagnostic command
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = robot_agentctl
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_AGENTCTL)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robot_proactive
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/include
MAINSRC = robot_agentctl.c

include $(APPDIR)/Application.mk
