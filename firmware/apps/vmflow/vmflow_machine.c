/****************************************************************************
 * firmware/apps/vmflow/vmflow_machine.c
 *
 * The orchestrator's view of the vending machine, built from the MDB
 * engine's events (mdb.h), and what each event means upstream, as in
 * mdb_cashless_task() of the ESP-IDF firmware:
 *
 *   vend_request / vend_success / vend_failure / session_end
 *                   BLE notification to the phone app (19 bytes, signed)
 *   vend_success    "<price>:<item>:<ts>" signed on .../vend_success, for
 *                   sessions funded by a credit; the backend attaches the
 *                   item to the sale it recorded when it sent the credit.
 *                   App sessions (no fixed funds) are reported by the app.
 *   vend_failure    "<price>,<item>:<ts>" signed on .../vend_fail
 *   cash_sale       "<price>:<item>:<ts>" signed on .../sale
 *
 * Prices are in cents (the 1/100 units of the backend). The engine may
 * not be running, or may restart and forget its subscribers; the thread
 * renews its subscription whenever the queue has been quiet for a while,
 * and the engine answers each renewal with a snapshot of its state.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "mdb.h"
#include "vmflow.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RENEW_S        5        /* quiet queue: subscribe again */
#define RETRY_S        5        /* no engine: try again */

/* BLE notification commands of the ESP-IDF firmware */

#define BLE_VEND_REQUEST     0x0a
#define BLE_VEND_SUCCESS     0x0b
#define BLE_VEND_FAILURE     0x0c
#define BLE_SESSION_COMPLETE 0x0d

/****************************************************************************
 * Public Data
 ****************************************************************************/

struct vmflow_machine_s g_machine;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The open session was funded by a credit (MQTT RPC or "mdb credit") */

static bool g_credit_session;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t uptime(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static void handle_event(FAR const struct mdb_evt_s *e)
{
  char msg[48];

  g_machine.engine = true;
  g_machine.state  = e->state;

  switch (e->evt)
    {
      case MDB_EVT_SNAPSHOT:
      case MDB_EVT_SESSION_BEGIN:
        g_credit_session = e->state >= MDB_STATE_IDLE &&
                           e->price != MDB_FUNDS_UNLIMITED;
        break;

      case MDB_EVT_VEND_REQUEST:
        vmflow_ble_notify(BLE_VEND_REQUEST, e->price, e->item);
        break;

      case MDB_EVT_VEND_SUCCESS:
        g_machine.last_sale_price        = e->price;
        g_machine.last_sale_item         = e->item;
        g_machine.last_vend_success_time = time(NULL);
        vmflow_ble_notify(BLE_VEND_SUCCESS, e->price, e->item);

        if (g_credit_session)
          {
            snprintf(msg, sizeof(msg), "%lu:%u:%lld",
                     (unsigned long)e->price, e->item,
                     (long long)time(NULL));
            vmflow_publish_signed("vend_success", msg);
          }
        break;

      case MDB_EVT_VEND_FAILURE:
        g_machine.vend_fails++;
        vmflow_ble_notify(BLE_VEND_FAILURE, e->price, e->item);

        snprintf(msg, sizeof(msg), "%lu,%u:%lld", (unsigned long)e->price,
                 e->item, (long long)time(NULL));
        vmflow_publish_signed("vend_fail", msg);
        break;

      case MDB_EVT_SESSION_END:
        g_credit_session = false;
        vmflow_ble_notify(BLE_SESSION_COMPLETE, 0, 0);
        break;

      case MDB_EVT_CASH_SALE:
        snprintf(msg, sizeof(msg), "%lu:%u:%lld", (unsigned long)e->price,
                 e->item, (long long)time(NULL));
        vmflow_publish_signed("sale", msg);
        break;

      default:
        break;
    }

  vmflow_led_update();
}

static FAR void *machine_thread(FAR void *arg)
{
  struct mdb_evt_s evt;
  struct timespec deadline;
  time_t heard;
  mqd_t mq;
  ssize_t n;

  for (; ; )
    {
      mq = mdb_subscribe(0);
      if (mq == (mqd_t)-1)
        {
          if (g_machine.engine)
            {
              g_machine.engine = false;
              vmflow_led_update();
            }

          sleep(RETRY_S);
          continue;
        }

      heard = uptime();

      for (; ; )
        {
          clock_gettime(CLOCK_REALTIME, &deadline);
          deadline.tv_sec += RENEW_S;

          n = mq_timedreceive(mq, (FAR char *)&evt, sizeof(evt), NULL,
                              &deadline);
          if (n == sizeof(evt))
            {
              heard = uptime();
              handle_event(&evt);
              continue;
            }

          /* Every renewal is answered with a snapshot, so two silent ones
           * mean the engine is gone (its command queue outlives it).
           */

          if (g_machine.engine && uptime() - heard > 2 * RENEW_S)
            {
              g_machine.engine = false;
              vmflow_led_update();
            }

          /* Quiet: renew. A restarted engine answers with a snapshot. */

          if (mdb_request(MDB_CMD_SUBSCRIBE, (uint32_t)getpid()) < 0)
            {
              break;
            }
        }

      mdb_unsubscribe(mq);
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vmflow_machine_start(void)
{
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_VMFLOW_MACHINE_STACKSIZE);
  ret = pthread_create(&tid, &attr, machine_thread, NULL);
  pthread_attr_destroy(&attr);

  if (ret == 0)
    {
      pthread_setname_np(tid, "vmflow_mdb");
    }

  return -ret;
}
