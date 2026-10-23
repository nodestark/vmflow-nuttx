############################################################################
# firmware/apps/vmflow/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_VMFLOW_PROGNAME)
PRIORITY  = $(CONFIG_VMFLOW_PRIORITY)
STACKSIZE = $(CONFIG_VMFLOW_STACKSIZE)
MODULE    = $(CONFIG_VMFLOW)

MAINSRC = vmflow_main.c

ifneq ($(CONFIG_VMFLOW_MQTT),)
CSRCS += vmflow_mqtt.c
endif

include $(APPDIR)/Application.mk
