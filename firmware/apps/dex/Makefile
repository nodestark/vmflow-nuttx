############################################################################
# firmware/apps/dex/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_VMFLOW_DEX_PROGNAME)
PRIORITY  = $(CONFIG_VMFLOW_DEX_PRIORITY)
STACKSIZE = $(CONFIG_VMFLOW_DEX_STACKSIZE)
MODULE    = $(CONFIG_VMFLOW_DEX)

MAINSRC = dex_main.c

# Publishing goes through the vmflow application's MQTT session.

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/vmflow

include $(APPDIR)/Application.mk
