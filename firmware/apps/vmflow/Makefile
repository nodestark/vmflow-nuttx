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

# Firmware version: git describe of this repository, written to
# vmflow_version.h only when it changes so that only then vmflow_rpc.c and
# vmflow_main.c are rebuilt.

VMFLOW_VERSION := $(shell git -C $(APPDIR)/external describe --always --dirty --tags 2>/dev/null || echo unknown)
$(shell echo '#define VMFLOW_VERSION "$(VMFLOW_VERSION)"' > vmflow_version.h.tmp; \
        cmp -s vmflow_version.h.tmp vmflow_version.h || \
        mv vmflow_version.h.tmp vmflow_version.h; rm -f vmflow_version.h.tmp)

# The MDB engine's client side is its header alone (mdb.h)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/mdb

# Shared header-only helpers (daemon_status.h)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/include

include $(APPDIR)/Application.mk
