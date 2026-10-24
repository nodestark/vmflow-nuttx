############################################################################
# firmware/apps/vmflow/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_VMFLOW_PROGNAME)
PRIORITY  = $(CONFIG_VMFLOW_PRIORITY)
STACKSIZE = $(CONFIG_VMFLOW_STACKSIZE)
MODULE    = $(CONFIG_VMFLOW)

MAINSRC = vmflow_main.c
CSRCS   = vmflow_config.c vmflow_hmac.c vmflow_io.c vmflow_machine.c
CSRCS  += vmflow_mqtt.c vmflow_rpc.c

ifneq ($(CONFIG_VMFLOW_BLE),)
CSRCS  += vmflow_ble.c
include $(APPDIR)/wireless/bluetooth/nimble/Makefile.nimble
endif

# The MDB engine's client side is its header alone (mdb.h)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/mdb

# Shared header-only helpers (daemon_status.h)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/include

include $(APPDIR)/Application.mk
