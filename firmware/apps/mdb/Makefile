############################################################################
# firmware/apps/mdb/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_VMFLOW_MDB_PROGNAME)
PRIORITY  = $(CONFIG_VMFLOW_MDB_PRIORITY)
STACKSIZE = $(CONFIG_VMFLOW_MDB_STACKSIZE)
MODULE    = $(CONFIG_VMFLOW_MDB)

MAINSRC = mdb_main.c
CSRCS   = mdb_engine.c

# Shared header-only helpers (daemon_status.h)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/include

include $(APPDIR)/Application.mk
