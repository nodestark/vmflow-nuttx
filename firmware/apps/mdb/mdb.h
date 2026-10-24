/****************************************************************************
 * firmware/apps/mdb/mdb.h
 *
 * MDB cashless engine, client side. The engine (the "mdb" service) is the
 * only owner of the bus; any task asks it for a session, a vend decision
 * and so on through a POSIX message queue, and follows what happens on the
 * bus through an event queue of its own. Amounts are in 1/100 currency
 * units (cents); the engine converts them to the MDB scale factor.
 *
 * The queues and these structs are the whole contract, so the client side
 * is this header alone: other apps add
 *   CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/mdb
 * and build whether the engine is configured or not. Without a running
 * engine every call fails with ENOENT.
 *
 ****************************************************************************/

#ifndef __VMFLOW_APPS_MDB_MDB_H
#define __VMFLOW_APPS_MDB_MDB_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MDB_CMD_MQ           "/mdb_cmd"     /* clients -> engine */
#define MDB_EVT_MQ_FMT       "/mdb_evt.%d"  /* engine -> one subscriber */
#define MDB_EVT_MQ_DEPTH     16

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
  MDB_CMD_OUT_OF_SEQUENCE,  /* answer the next poll with "out of sequence" */
  MDB_CMD_SUBSCRIBE,        /* amount = pid owning MDB_EVT_MQ_FMT queue */
  MDB_CMD_UNSUBSCRIBE       /* amount = pid */
};

struct mdb_cmd_s
{
  uint32_t amount;          /* cents, or a pid */
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
  MDB_EVT_CASH_SALE,        /* price, item: paid in cash, not by us */
  MDB_EVT_BUS_LOST,         /* the VMC stopped polling; reader inactive */
  MDB_EVT_CREDIT_EXPIRED,   /* price = pending credit no session took */
  MDB_EVT_SNAPSHOT          /* first event after subscribing: state, and
                             * price = session funds */
};

struct mdb_evt_s
{
  uint32_t price;           /* cents */
  uint16_t item;
  uint8_t evt;              /* enum mdb_evt_e */
  uint8_t state;            /* enum mdb_state_e after the event */
};

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mdb_request
 *
 * Description:
 *   Queue a command for the engine. Does not block; the engine acts on it
 *   at the next poll from the VMC, if the reader state allows it.
 *
 * Returned Value:
 *   OK, -ENOENT if the engine never ran, -EAGAIN if its queue is full.
 *
 ****************************************************************************/

static inline int mdb_request(uint8_t cmd, uint32_t amount)
{
  struct mdb_cmd_s msg;
  mqd_t mq;
  int ret = 0;

  msg.amount = amount;
  msg.cmd    = cmd;

  /* No O_CREAT: the engine creates the queue */

  mq = mq_open(MDB_CMD_MQ, O_WRONLY | O_NONBLOCK);
  if (mq == (mqd_t)-1)
    {
      return -errno;
    }

  if (mq_send(mq, (FAR const char *)&msg, sizeof(msg), 0) < 0)
    {
      ret = -errno;
    }

  mq_close(mq);
  return ret;
}

/****************************************************************************
 * Name: mdb_subscribe
 *
 * Description:
 *   Create this process's event queue and ask the engine to copy every
 *   event into it. The engine answers with MDB_EVT_SNAPSHOT, then the
 *   events as they happen; a subscriber that falls behind loses events
 *   (the engine never blocks on it). One subscription per process.
 *
 * Returned Value:
 *   The queue to mq_receive() struct mdb_evt_s from, or (mqd_t)-1 with
 *   errno set.
 *
 ****************************************************************************/

static inline mqd_t mdb_subscribe(int oflags)
{
  struct mq_attr attr;
  char name[24];
  mqd_t mq;
  int ret;

  snprintf(name, sizeof(name), MDB_EVT_MQ_FMT, (int)getpid());

  attr.mq_flags   = 0;
  attr.mq_maxmsg  = MDB_EVT_MQ_DEPTH;
  attr.mq_msgsize = sizeof(struct mdb_evt_s);
  attr.mq_curmsgs = 0;

  mq = mq_open(name, O_RDONLY | O_CREAT | oflags, 0666, &attr);
  if (mq == (mqd_t)-1)
    {
      return mq;
    }

  ret = mdb_request(MDB_CMD_SUBSCRIBE, (uint32_t)getpid());
  if (ret < 0)
    {
      mq_close(mq);
      mq_unlink(name);
      errno = -ret;
      return (mqd_t)-1;
    }

  return mq;
}

static inline void mdb_unsubscribe(mqd_t mq)
{
  char name[24];

  snprintf(name, sizeof(name), MDB_EVT_MQ_FMT, (int)getpid());
  mdb_request(MDB_CMD_UNSUBSCRIBE, (uint32_t)getpid());
  mq_close(mq);
  mq_unlink(name);
}

static inline FAR const char *mdb_state_name(uint8_t state)
{
  static FAR const char *const names[] =
  {
    "inactive", "disabled", "enabled", "idle", "vend"
  };

  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

static inline FAR const char *mdb_event_name(uint8_t evt)
{
  static FAR const char *const names[] =
  {
    "state", "session_begin", "vend_request", "vend_approved",
    "vend_denied", "vend_success", "vend_failure", "session_end",
    "cash_sale", "bus_lost", "credit_expired", "snapshot"
  };

  return evt < sizeof(names) / sizeof(names[0]) ? names[evt] : "?";
}

#endif /* __VMFLOW_APPS_MDB_MDB_H */
