/****************************************************************************
 * firmware/apps/vmflow/vmflow_mqtt.h
 ****************************************************************************/

#ifndef __FIRMWARE_APPS_VMFLOW_VMFLOW_MQTT_H
#define __FIRMWARE_APPS_VMFLOW_VMFLOW_MQTT_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>

/* Client loop: connects, keeps the session and reconnects. Never returns. */

int vmflow_mqtt_run(int argc, FAR char *argv[]);

/* Spawn vmflow_mqtt_run() as a background task. */

int vmflow_mqtt_start(void);

bool vmflow_mqtt_running(void);

/* Queue a message; -ENOTCONN while the broker session is down. */

int vmflow_mqtt_publish(FAR const char *topic, FAR const void *payload,
                        size_t len, int qos, bool retain);

void vmflow_mqtt_status(void);

#endif /* __FIRMWARE_APPS_VMFLOW_VMFLOW_MQTT_H */
