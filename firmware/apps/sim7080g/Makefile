############################################################################
# firmware/apps/sim7080g/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_VMFLOW_SIM7080G_PROGNAME)
PRIORITY  = $(CONFIG_VMFLOW_SIM7080G_PRIORITY)
STACKSIZE = $(CONFIG_VMFLOW_SIM7080G_STACKSIZE)
MODULE    = $(CONFIG_VMFLOW_SIM7080G)

MAINSRC = sim7080g_main.c

# Shared header-only helpers (daemon_status.h)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/include

include $(APPDIR)/Application.mk
