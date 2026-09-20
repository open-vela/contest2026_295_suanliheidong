############################################################################
# Contest 2026 EDA Robot control application
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = robotctl
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOTCTL)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robot_motion
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_295_suanliheidong/app/robotctl

VPATH += ../robot_motion

CSRCS   = robotctl_servo.c robot_motion.c
MAINSRC = robotctl_main.c

include $(APPDIR)/Application.mk
