/****************************************************************************
 * firmware/apps/mdb/mdb_client.c
 *
 * Client side of the mdb engine: what any task links to send it commands.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>

#include "mdb.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int mdb_request(uint8_t cmd, uint32_t amount)
{
  struct mdb_cmd_s msg;
  mqd_t mq;
  int ret = OK;

  msg.amount = amount;
  msg.cmd    = cmd;

  /* No O_CREAT: the engine creates the queue, so a missing queue means it
   * never ran.
   */

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

FAR const char *mdb_state_name(uint8_t state)
{
  static FAR const char *const names[] =
  {
    "inactive", "disabled", "enabled", "idle", "vend"
  };

  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

FAR const char *mdb_event_name(uint8_t evt)
{
  static FAR const char *const names[] =
  {
    "state", "session_begin", "vend_request", "vend_approved",
    "vend_denied", "vend_success", "vend_failure", "session_end",
    "cash_sale"
  };

  return evt < sizeof(names) / sizeof(names[0]) ? names[evt] : "?";
}
