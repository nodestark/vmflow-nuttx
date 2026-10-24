/****************************************************************************
 * firmware/apps/vmflow/vmflow.h
 *
 * VMflow orchestrator, internal interfaces. The orchestrator owns no
 * hardware: it reaches the MDB engine through mdb.h, the audit reader by
 * running the dex command, the buzzer and the LED through their device
 * files, and keeps the device identity in plain files.
 *
 ****************************************************************************/

#ifndef __VMFLOW_APPS_VMFLOW_VMFLOW_H
#define __VMFLOW_APPS_VMFLOW_VMFLOW_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VMFLOW_DOMAIN      "vmflow.xyz"
#define VMFLOW_HMAC_LEN    32
#define VMFLOW_SUB_LEN     32
#define VMFLOW_KEY_LEN     64
#define VMFLOW_TOPIC_LEN   64

/* Wall clock below this is the unset 1970 clock, not NTP time */

#define VMFLOW_TIME_VALID  1700000000

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* What the orchestrator knows about the machine, from the engine events.
 * Read by "info", the LED and "vmflow status".
 */

struct vmflow_machine_s
{
  bool engine;                  /* subscribed to a running engine */
  uint8_t state;                /* enum mdb_state_e */
  uint32_t last_sale_price;     /* cents */
  uint16_t last_sale_item;
  time_t last_vend_success_time;
  unsigned int vend_fails;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern struct vmflow_machine_s g_machine;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* vmflow_config.c: <CONFIG_VMFLOW_DATADIR>/subdomain and /passkey */

bool vmflow_identity(FAR char *subdomain, FAR char *passkey);
int  vmflow_identity_set(FAR const char *name, FAR const char *value);

/* vmflow_hmac.c */

void vmflow_hmac(FAR const char *key, FAR const void *msg, size_t len,
                 FAR uint8_t *out);
void vmflow_sign(FAR const char *key, FAR const char *msg, FAR char *out,
                 size_t outlen);
bool vmflow_verify(FAR const char *key, FAR const char *msg, size_t len,
                   FAR const char *hex);

/* vmflow_mqtt.c */

int  vmflow_mqtt_run(void);
bool vmflow_mqtt_connected(void);
int  vmflow_mqtt_publish(FAR const char *topic, FAR const void *payload,
                         size_t len, int qos, bool retain);
int  vmflow_publish_signed(FAR const char *what, FAR const char *msg);
void vmflow_mqtt_restart(void);
void vmflow_mqtt_status(void);

/* vmflow_rpc.c */

void vmflow_rpc(FAR const char *msg, size_t len);

/* vmflow_machine.c: engine events -> MQTT, BLE, LED */

int  vmflow_machine_start(void);

/* vmflow_io.c: buzzer, LED, dex */

void vmflow_beep(void);
void vmflow_led_update(void);
int  vmflow_dex_start(void);

/* vmflow_ble.c */

#ifdef CONFIG_VMFLOW_BLE
int  vmflow_ble_start(void);
void vmflow_ble_notify(uint8_t cmd, uint32_t price, uint16_t item);
void vmflow_ble_set_name(FAR const char *subdomain);
void vmflow_ble_status(void);
#else
#  define vmflow_ble_start()          (0)
#  define vmflow_ble_notify(c, p, i)
#  define vmflow_ble_set_name(s)
#  define vmflow_ble_status()
#endif

#endif /* __VMFLOW_APPS_VMFLOW_VMFLOW_H */
