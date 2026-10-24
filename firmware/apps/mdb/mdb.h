/****************************************************************************
 * firmware/apps/mdb/mdb.h
 *
 * MDB cashless engine, client side. The engine (the "mdb" service) is the
 * only owner of the bus; any task asks it for a session, a vend decision
 * and so on through a POSIX message queue, and can follow what happens on
 * the bus through the event queue. Amounts are in 1/100 currency units
 * (cents); the engine converts them to the MDB scale factor.
 *
 * Other apps include this header with
 *   CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/mdb
 * and call mdb_request(), which is in libapps through mdb_client.c.
 *
 ****************************************************************************/

#ifndef __VMFLOW_APPS_MDB_MDB_H
#define __VMFLOW_APPS_MDB_MDB_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MDB_CMD_MQ           "/mdb_cmd"   /* clients -> engine */
#define MDB_EVT_MQ           "/mdb_evt"   /* engine -> one reader */

/* MDB_CMD_SESSION_OPEN amount for a session with no fixed funds: the VMC
 * sees unlimited funds and every vend waits for MDB_CMD_VEND_APPROVE or
 * MDB_CMD_VEND_DENY (the phone-app flow of the ESP-IDF firmware).
 */

#define MDB_FUNDS_UNLIMITED  UINT32_MAX

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum mdb_cmd_e
{
  MDB_CMD_SESSION_OPEN = 0, /* begin session; amount = funds or UNLIMITED */
  MDB_CMD_SESSION_CLOSE,    /* cancel the session (or a pending credit) */
  MDB_CMD_VEND_APPROVE,     /* approve the vend being requested */
  MDB_CMD_VEND_DENY,        /* deny it */
  MDB_CMD_OUT_OF_SEQUENCE   /* answer the next poll with "out of sequence" */
};

struct mdb_cmd_s
{
  uint32_t amount;          /* cents, MDB_CMD_SESSION_OPEN only */
  uint8_t cmd;              /* enum mdb_cmd_e */
};

/* Reader state, same steps as machine_state_t in the ESP-IDF firmware */

enum mdb_state_e
{
  MDB_STATE_INACTIVE = 0,   /* after RESET, before SETUP */
  MDB_STATE_DISABLED,       /* set up, VMC has not enabled the reader */
  MDB_STATE_ENABLED,        /* ready, no session */
  MDB_STATE_IDLE,           /* session open */
  MDB_STATE_VEND            /* vend requested, waiting for its outcome */
};

enum mdb_evt_e
{
  MDB_EVT_STATE = 0,        /* state changed */
  MDB_EVT_SESSION_BEGIN,    /* price = funds (UNLIMITED for app sessions) */
  MDB_EVT_VEND_REQUEST,     /* price, item */
  MDB_EVT_VEND_APPROVED,    /* price, item */
  MDB_EVT_VEND_DENIED,      /* price, item */
  MDB_EVT_VEND_SUCCESS,     /* price, item */
  MDB_EVT_VEND_FAILURE,     /* price, item */
  MDB_EVT_SESSION_END,      /* price = funds left unspent */
  MDB_EVT_CASH_SALE         /* price, item: paid in cash, not by us */
};

struct mdb_evt_s
{
  uint32_t price;           /* cents */
  uint16_t item;
  uint8_t evt;              /* enum mdb_evt_e */
  uint8_t state;            /* enum mdb_state_e after the event */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: mdb_request
 *
 * Description:
 *   Queue a command for the engine. Does not block; the engine acts on it
 *   at the next poll from the VMC, if the reader state allows it.
 *
 * Returned Value:
 *   OK, -ENOENT if the engine is not running, -EAGAIN if its queue is full.
 *
 ****************************************************************************/

int mdb_request(uint8_t cmd, uint32_t amount);

FAR const char *mdb_state_name(uint8_t state);
FAR const char *mdb_event_name(uint8_t evt);

#endif /* __VMFLOW_APPS_MDB_MDB_H */
