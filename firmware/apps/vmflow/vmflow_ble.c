/****************************************************************************
 * firmware/apps/vmflow/vmflow_ble.c
 *
 * BLE peripheral for the phone app (NimBLE over the HCI socket), port of
 * nimble.c and ble_event_handler() of the ESP-IDF firmware. One service
 * with one read/write/notify characteristic, same UUIDs; the device
 * advertises as "<subdomain>.vmflow.xyz" ("0.vmflow.xyz" until installed).
 *
 * Writes from the app, first byte = command:
 *
 *   0x00 <subdomain>  provisioning, only while none is set
 *   0x01 <passkey>    provisioning, only while none is set
 *   0x02              begin an MDB session with no fixed funds
 *   0x03 <payload>    approve the vend (signed payload, below)
 *   0x04              cancel the session
 *
 * Notifications to the app, 19 bytes:
 *
 *   [0] cmd  [1-4] price u32  [5-6] item u16  [7-10] time u32
 *   [11-14] 0  [15-18] HMAC-SHA256(passkey, bytes 0-14)[:4]
 *
 * with cmd 0x0a vend request, 0x0b vend success, 0x0c vend failure, 0x0d
 * session complete; big endian, price in cents. An approval (0x03) carries
 * the same layout and must be signed and no older than BLE_FRESHNESS_S.
 * Wi-Fi provisioning (0x06/0x07) and the PAX counter are not ported.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "netutils/netlib.h"

#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "mdb.h"
#include "vmflow.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BLE_PAYLOAD_LEN   19
#define BLE_SIGNED_LEN    15
#define BLE_FRESHNESS_S   60

#define BLE_SET_SUBDOMAIN 0x00
#define BLE_SET_PASSKEY   0x01
#define BLE_SESSION_OPEN  0x02
#define BLE_VEND_APPROVE  0x03
#define BLE_SESSION_CLOSE 0x04

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

void ble_hci_sock_ack_handler(FAR void *param);

static int chr_access(uint16_t conn, uint16_t attr,
                      FAR struct ble_gatt_access_ctxt *ctxt, FAR void *arg);
static int gap_event(FAR struct ble_gap_event *event, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const ble_uuid128_t g_svc_uuid =
  BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x78, 0xb8,
                   0xed, 0x11, 0xda, 0x46, 0x42, 0xc6, 0xbb, 0xb2);

static const ble_uuid128_t g_chr_uuid =
  BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x78, 0xb8,
                   0xed, 0x11, 0xde, 0x46, 0x76, 0x9c, 0xaf, 0xc9);

static uint16_t g_chr_handle;

static const struct ble_gatt_svc_def g_svcs[] =
{
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &g_svc_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[])
    {
      {
        .uuid       = &g_chr_uuid.u,
        .access_cb  = chr_access,
        .val_handle = &g_chr_handle,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_NOTIFY,
      },
      {
        0
      }
    }
  },
  {
    0
  }
};

/* What a read returns, as in the ESP-IDF firmware (the app ignores it) */

static const char g_read_value[50] = "I am characteristic value";

static uint8_t g_own_addr_type;
static uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
static bool g_notify;
static bool g_synced;
static int g_adv_rc = -1;
static char g_name[VMFLOW_SUB_LEN + sizeof("." VMFLOW_DOMAIN)];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void put_u32(FAR uint8_t *p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static uint32_t get_u32(FAR const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | p[3];
}

/* ble_decode_with_passkey() of the ESP-IDF firmware */

static bool payload_valid(FAR const uint8_t *p, size_t len)
{
  char key[VMFLOW_KEY_LEN];
  uint8_t mac[VMFLOW_HMAC_LEN];
  uint8_t diff = 0;
  int i;

  if (len < BLE_PAYLOAD_LEN || !vmflow_identity(NULL, key))
    {
      return false;
    }

  vmflow_hmac(key, p, BLE_SIGNED_LEN, mac);
  for (i = 0; i < 4; i++)
    {
      diff |= mac[i] ^ p[BLE_SIGNED_LEN + i];
    }

  return diff == 0 &&
         labs((long)(time(NULL) - (time_t)get_u32(&p[7]))) <=
         BLE_FRESHNESS_S;
}

static void advertise(void)
{
  struct ble_gap_adv_params params;
  struct ble_hs_adv_fields fields;
  FAR const char *name = ble_svc_gap_device_name();

  memset(&fields, 0, sizeof(fields));
  fields.flags                 = BLE_HS_ADV_F_DISC_GEN |
                                 BLE_HS_ADV_F_BREDR_UNSUP;
  fields.tx_pwr_lvl_is_present = 1;
  fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  fields.name                  = (FAR uint8_t *)name;
  fields.name_len              = strlen(name);
  fields.name_is_complete      = 1;

  g_adv_rc = ble_gap_adv_set_fields(&fields);
  if (g_adv_rc != 0)
    {
      syslog(LOG_ERR, "ble: adv fields: %d\n", g_adv_rc);
      return;
    }

  memset(&params, 0, sizeof(params));
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  g_adv_rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, gap_event, NULL);
  if (g_adv_rc != 0)
    {
      syslog(LOG_ERR, "ble: adv start: %d\n", g_adv_rc);
    }
}

static void handle_write(FAR const uint8_t *buf, size_t len)
{
  FAR const char *value = (FAR const char *)&buf[1];

  switch (buf[0])
    {
      case BLE_SET_SUBDOMAIN:
        if (vmflow_identity_set("subdomain", value) == 0)
          {
            syslog(LOG_INFO, "ble: subdomain %s\n", value);
            vmflow_ble_set_name(value);
            vmflow_led_update();
          }
        break;

      case BLE_SET_PASSKEY:
        if (vmflow_identity_set("passkey", value) == 0)
          {
            syslog(LOG_INFO, "ble: passkey set\n");
            vmflow_led_update();
          }
        break;

      case BLE_SESSION_OPEN:
        mdb_request(MDB_CMD_SESSION_OPEN, MDB_FUNDS_UNLIMITED);
        break;

      case BLE_VEND_APPROVE:
        if (payload_valid(buf, len))
          {
            mdb_request(MDB_CMD_VEND_APPROVE, 0);
          }
        else
          {
            syslog(LOG_WARNING, "ble: approval rejected\n");
          }
        break;

      case BLE_SESSION_CLOSE:
        mdb_request(MDB_CMD_SESSION_CLOSE, 0);
        break;

      default:
        break;
    }
}

static int chr_access(uint16_t conn, uint16_t attr,
                      FAR struct ble_gatt_access_ctxt *ctxt, FAR void *arg)
{
  uint8_t buf[128];
  uint16_t len;
  int rc;

  switch (ctxt->op)
    {
      case BLE_GATT_ACCESS_OP_READ_CHR:
        rc = os_mbuf_append(ctxt->om, g_read_value, sizeof(g_read_value));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

      case BLE_GATT_ACCESS_OP_WRITE_CHR:
        len = OS_MBUF_PKTLEN(ctxt->om);
        if (len < 1 || len > sizeof(buf) - 1)
          {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
          }

        /* Zero-filled, so string values (provisioning) end in '\0' */

        memset(buf, 0, sizeof(buf));
        rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0)
          {
            return BLE_ATT_ERR_UNLIKELY;
          }

        handle_write(buf, len);
        return 0;

      default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int gap_event(FAR struct ble_gap_event *event, FAR void *arg)
{
  switch (event->type)
    {
      case BLE_GAP_EVENT_CONNECT:
        syslog(LOG_INFO, "ble: connect status %d\n", event->connect.status);
        if (event->connect.status != 0)
          {
            advertise();
          }
        else
          {
            g_conn = event->connect.conn_handle;
          }
        break;

      case BLE_GAP_EVENT_DISCONNECT:
        syslog(LOG_INFO, "ble: disconnect reason 0x%x\n",
               event->disconnect.reason);
        g_conn   = BLE_HS_CONN_HANDLE_NONE;
        g_notify = false;
        advertise();
        break;

      case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_chr_handle)
          {
            g_notify = event->subscribe.cur_notify;
          }
        break;

      default:
        break;
    }

  return 0;
}

static void on_sync(void)
{
  ble_hs_id_infer_auto(0, &g_own_addr_type);
  g_synced = true;
  syslog(LOG_INFO, "ble: synced, advertising %s\n", g_name);
  advertise();
}

static void on_reset(int reason)
{
  g_synced = false;
  syslog(LOG_WARNING, "ble: host reset: %d\n", reason);
}

static FAR void *hci_task(FAR void *param)
{
  ble_hci_sock_ack_handler(param);
  return NULL;
}

static FAR void *host_task(FAR void *param)
{
  nimble_port_run();
  return NULL;
}

/* Own threads rather than ble_npl_task_init(), whose NuttX port ignores
 * the stack size: the host's GATT and HMAC work needs more than the
 * default pthread stack.
 */

static int spawn(FAR const char *name, FAR void *(*entry)(FAR void *))
{
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_VMFLOW_BLE_STACKSIZE);
  ret = pthread_create(&tid, &attr, entry, NULL);
  pthread_attr_destroy(&attr);

  if (ret == 0)
    {
      pthread_setname_np(tid, name);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vmflow_ble_start(void)
{
  char sub[VMFLOW_SUB_LEN];
  int ret;

  if (!vmflow_identity(sub, NULL) && sub[0] == '\0')
    {
      strcpy(sub, "0");
    }

  snprintf(g_name, sizeof(g_name), "%s." VMFLOW_DOMAIN, sub);

  /* The HCI socket only carries traffic while the controller's network
   * interface is up (netinit_bringup() in the NimBLE example).
   */

  ret = netlib_ifup(CONFIG_VMFLOW_BLE_IFNAME);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ble: ifup %s failed: %d\n", CONFIG_VMFLOW_BLE_IFNAME,
             ret);
    }

  nimble_port_init();

  ble_hs_cfg.sync_cb    = on_sync;
  ble_hs_cfg.reset_cb   = on_reset;
  ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_sc      = 0;

  ble_svc_gap_init();
  ble_svc_gatt_init();

  ret = ble_gatts_count_cfg(g_svcs);
  if (ret == 0)
    {
      ret = ble_gatts_add_svcs(g_svcs);
    }

  if (ret != 0)
    {
      return -EINVAL;
    }

  ble_svc_gap_device_name_set(g_name);

  ret = spawn("vmflow_hci", hci_task);
  if (ret == 0)
    {
      ret = spawn("vmflow_ble", host_task);
    }

  return ret == 0 ? 0 : -ENOMEM;
}

/* ble_encode_with_passkey() + ble_notify_send() of the ESP-IDF firmware */

void vmflow_ble_notify(uint8_t cmd, uint32_t price, uint16_t item)
{
  char key[VMFLOW_KEY_LEN];
  uint8_t p[BLE_PAYLOAD_LEN];
  uint8_t mac[VMFLOW_HMAC_LEN];
  FAR struct os_mbuf *om;

  if (!g_notify || g_conn == BLE_HS_CONN_HANDLE_NONE)
    {
      return;
    }

  vmflow_identity(NULL, key);

  p[0] = cmd;
  put_u32(&p[1], price);
  p[5] = item >> 8;
  p[6] = item;
  put_u32(&p[7], (uint32_t)time(NULL));
  put_u32(&p[11], 0);

  vmflow_hmac(key, p, BLE_SIGNED_LEN, mac);
  memcpy(&p[BLE_SIGNED_LEN], mac, 4);

  om = ble_hs_mbuf_from_flat(p, sizeof(p));
  if (om != NULL)
    {
      ble_gattc_notify_custom(g_conn, g_chr_handle, om);
    }
}

void vmflow_ble_status(FAR FILE *out)
{
  fprintf(out, "ble     : %s, name %s, %s%s\n",
          !g_synced ? "not synced with the controller" :
          g_adv_rc != 0 ? "advertising failed" : "advertising",
          g_name,
          g_conn != BLE_HS_CONN_HANDLE_NONE ? "connected" : "no connection",
          g_notify ? ", notifying" : "");
}

void vmflow_ble_set_name(FAR const char *subdomain)
{
  snprintf(g_name, sizeof(g_name), "%s." VMFLOW_DOMAIN, subdomain);
  ble_svc_gap_device_name_set(g_name);

  /* While connected the peripheral is not advertising; the new name goes
   * out when the disconnect restarts it.
   */

  if (g_synced && g_conn == BLE_HS_CONN_HANDLE_NONE)
    {
      ble_gap_adv_stop();
      advertise();
    }
}
