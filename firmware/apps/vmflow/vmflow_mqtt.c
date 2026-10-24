/****************************************************************************
 * firmware/apps/vmflow/vmflow_mqtt.c
 *
 * MQTT uplink (MQTT-C over a plain TCP socket). Mirrors the esp-mqtt setup
 * of the ESP-IDF firmware: broker mqtt.vmflow.xyz:1883, keepalive 120 s,
 * retained last will "offline" on domain.vmflow.xyz/<subdomain>/status,
 * "online" published there on connect and a subscription to
 * <subdomain>.vmflow.xyz/#. The client waits for the network by itself:
 * while ppp0 is down, DNS or connect fail and it retries every
 * CONFIG_VMFLOW_MQTT_RECONNECT seconds. Until the device has an identity
 * (vmflow_config.c) it does not connect at all.
 *
 * Messages on .../rpc go to vmflow_rpc(). MQTT-C calls back with its lock
 * held and the handler publishes, so the callback only copies them; they
 * are handled between two mqtt_sync() calls.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/boardctl.h>
#include <sys/socket.h>

#include <mqtt.h>

#include "vmflow.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MQTT_SYNC_MS      100
#define RPC_MAXLEN        127           /* the ESP-IDF firmware's limit */
#define RPC_SLOTS         4
#define RESTART_DELAY_MS  1000          /* let the "ok" reach the broker */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vmflow_mqtt_state_s
{
  bool connected;
  time_t since;               /* last connect/disconnect */
  unsigned int connects;
  unsigned int failures;
  unsigned int rx;
  unsigned int tx;
  unsigned int late_acks;
  enum MQTTErrors last_error;
  bool restart;
};

struct rpc_slot_s
{
  size_t len;
  char msg[RPC_MAXLEN + 1];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct vmflow_mqtt_state_s g_mqtt;
static struct mqtt_client g_client;

/* Guards g_client and g_sub against the other vmflow threads while the
 * client swaps the socket on reconnect.
 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static char g_sub[VMFLOW_SUB_LEN];
static char g_key[VMFLOW_KEY_LEN];

static uint8_t g_sendbuf[CONFIG_VMFLOW_MQTT_SENDBUF];
static uint8_t g_recvbuf[CONFIG_VMFLOW_MQTT_RECVBUF];

/* RPC messages copied by the callback, handled after mqtt_sync() */

static struct rpc_slot_s g_rpc[RPC_SLOTS];
static unsigned int g_rpc_head;
static unsigned int g_rpc_tail;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t uptime(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static void status_topic(FAR char *topic, size_t len)
{
  snprintf(topic, len, "domain." VMFLOW_DOMAIN "/%s/status", g_sub);
}

static void publish_callback(FAR void **state,
                             FAR struct mqtt_response_publish *publish)
{
  FAR const char *topic = publish->topic_name;
  size_t tlen = publish->topic_name_size;
  FAR struct rpc_slot_s *slot;
  size_t len;

  g_mqtt.rx++;

  if (tlen < 4 || memcmp(topic + tlen - 4, "/rpc", 4) != 0)
    {
      return;
    }

  if (g_rpc_head - g_rpc_tail == RPC_SLOTS)
    {
      syslog(LOG_WARNING, "vmflow: mqtt: rpc dropped, %d pending\n",
             RPC_SLOTS);
      return;
    }

  len = publish->application_message_size;
  if (len > RPC_MAXLEN)
    {
      len = RPC_MAXLEN;
    }

  slot = &g_rpc[g_rpc_head % RPC_SLOTS];
  memcpy(slot->msg, publish->application_message, len);
  slot->msg[len] = '\0';
  slot->len = len;
  g_rpc_head++;
}

/* Resolve the broker and open a TCP connection. The socket is switched to
 * non-blocking afterwards, as MQTT-C's POSIX PAL expects.
 */

static int broker_open(void)
{
  struct addrinfo hints;
  FAR struct addrinfo *res;
  FAR struct addrinfo *ai;
  char port[8];
  int fd = -1;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d", CONFIG_VMFLOW_MQTT_PORT);

  ret = getaddrinfo(CONFIG_VMFLOW_MQTT_HOST, port, &hints, &res);
  if (ret != 0)
    {
      syslog(LOG_WARNING, "vmflow: mqtt: resolve %s failed: %d\n",
             CONFIG_VMFLOW_MQTT_HOST, ret);
      return -EHOSTUNREACH;
    }

  for (ai = res; ai != NULL; ai = ai->ai_next)
    {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
        {
          continue;
        }

      if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        {
          break;
        }

      close(fd);
      fd = -1;
    }

  freeaddrinfo(res);

  if (fd < 0)
    {
      syslog(LOG_WARNING, "vmflow: mqtt: connect %s:%d failed: %d\n",
             CONFIG_VMFLOW_MQTT_HOST, CONFIG_VMFLOW_MQTT_PORT, errno);
      return -ECONNREFUSED;
    }

  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

static int session_start(int fd)
{
  char client_id[VMFLOW_TOPIC_LEN];
  char topic[VMFLOW_TOPIC_LEN];
  enum MQTTErrors err;

  pthread_mutex_lock(&g_lock);
  snprintf(client_id, sizeof(client_id), "vmflow-%s", g_sub);
  status_topic(topic, sizeof(topic));

  mqtt_init(&g_client, fd, g_sendbuf, sizeof(g_sendbuf),
            g_recvbuf, sizeof(g_recvbuf), publish_callback);
  err = mqtt_connect(&g_client, client_id, topic, "offline", 7,
                     NULL, NULL,
                     MQTT_CONNECT_CLEAN_SESSION | MQTT_CONNECT_WILL_QOS_1 |
                     MQTT_CONNECT_WILL_RETAIN,
                     CONFIG_VMFLOW_MQTT_KEEPALIVE);
  pthread_mutex_unlock(&g_lock);

  if (err != MQTT_OK)
    {
      syslog(LOG_ERR, "vmflow: mqtt: connect: %s\n", mqtt_error_str(err));
      return -EIO;
    }

  return OK;
}

/* First CONNACK: announce the device and subscribe, as the ESP-IDF
 * firmware does on MQTT_EVENT_CONNECTED. The reset reason that firmware
 * appends ("online,<n>") is not available to applications here.
 */

static void session_connected(void)
{
  char topic[VMFLOW_TOPIC_LEN];

  pthread_mutex_lock(&g_lock);
  status_topic(topic, sizeof(topic));
  mqtt_publish(&g_client, topic, "online", 6,
               MQTT_PUBLISH_QOS_1 | MQTT_PUBLISH_RETAIN);

  snprintf(topic, sizeof(topic), "%s." VMFLOW_DOMAIN "/#", g_sub);
  mqtt_subscribe(&g_client, topic, 0);
  g_mqtt.connected = true;
  pthread_mutex_unlock(&g_lock);

  g_mqtt.connects++;
  g_mqtt.since = uptime();
  vmflow_led_update();
  syslog(LOG_INFO, "vmflow: mqtt: connected to %s, subscribed %s\n",
         CONFIG_VMFLOW_MQTT_HOST, topic);
}

static void handle_rpcs(void)
{
  FAR struct rpc_slot_s *slot;

  while (g_rpc_tail != g_rpc_head)
    {
      slot = &g_rpc[g_rpc_tail % RPC_SLOTS];
      vmflow_rpc(slot->msg, slot->len);
      g_rpc_tail++;
    }
}

/* Run MQTT-C until the connection fails. */

static void session_run(void)
{
  enum MQTTErrors err;
  time_t restart_at = 0;

  for (; ; )
    {
      pthread_mutex_lock(&g_lock);
      err = mqtt_sync(&g_client);

      /* MQTT-C resends a request whose ack is late by more than
       * response_timeout (30 s), which LTE-M stalls exceed. Both acks then
       * arrive and the second matches nothing. MQTT-C has consumed it, but
       * the error sticks and would end the session: drop it instead.
       */

      if (err == MQTT_ERROR_ACK_OF_UNKNOWN)
        {
          g_client.error = MQTT_OK;
          g_mqtt.late_acks++;
          err = MQTT_OK;
        }

      pthread_mutex_unlock(&g_lock);

      if (err != MQTT_OK)
        {
          g_mqtt.last_error = err;
          syslog(LOG_WARNING, "vmflow: mqtt: %s\n", mqtt_error_str(err));
          return;
        }

      if (!g_mqtt.connected && g_client.event_connect)
        {
          session_connected();
        }

      handle_rpcs();

      if (g_mqtt.restart)
        {
          if (restart_at == 0)
            {
              restart_at = uptime() + RESTART_DELAY_MS / 1000;
            }
          else if (uptime() >= restart_at)
            {
              boardctl(BOARDIOC_RESET, 0);
            }
        }

      usleep(MQTT_SYNC_MS * 1000);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vmflow_mqtt_run(void)
{
  char sub[VMFLOW_SUB_LEN];
  char key[VMFLOW_KEY_LEN];
  int fd;

  g_mqtt.since = uptime();

  for (; ; )
    {
      bool was_connected = false;

      /* Identity first: without it there is no topic to use. Read again
       * on every attempt, so provisioning needs no restart.
       */

      if (!vmflow_identity(sub, key))
        {
          sleep(CONFIG_VMFLOW_MQTT_RECONNECT);
          continue;
        }

      pthread_mutex_lock(&g_lock);
      strcpy(g_sub, sub);
      strcpy(g_key, key);
      pthread_mutex_unlock(&g_lock);

      fd = broker_open();
      if (fd >= 0)
        {
          if (session_start(fd) == OK)
            {
              session_run();
            }

          pthread_mutex_lock(&g_lock);
          was_connected = g_mqtt.connected;
          if (was_connected)
            {
              g_mqtt.connected = false;
              g_mqtt.since     = uptime();
            }

          close(fd);
          pthread_mutex_unlock(&g_lock);
          vmflow_led_update();
        }

      if (!was_connected)
        {
          g_mqtt.failures++;
        }

      sleep(CONFIG_VMFLOW_MQTT_RECONNECT);
    }

  return 0;
}

bool vmflow_mqtt_connected(void)
{
  return g_mqtt.connected;
}

int vmflow_mqtt_publish(FAR const char *topic, FAR const void *payload,
                        size_t len, int qos, bool retain)
{
  enum MQTTErrors err;
  uint8_t flags;

  flags = (qos == 2 ? MQTT_PUBLISH_QOS_2 :
           qos == 1 ? MQTT_PUBLISH_QOS_1 : MQTT_PUBLISH_QOS_0) |
          (retain ? MQTT_PUBLISH_RETAIN : 0);

  pthread_mutex_lock(&g_lock);
  if (!g_mqtt.connected)
    {
      pthread_mutex_unlock(&g_lock);
      return -ENOTCONN;
    }

  err = mqtt_publish(&g_client, topic, payload, len, flags);
  pthread_mutex_unlock(&g_lock);

  if (err != MQTT_OK)
    {
      return -EIO;
    }

  g_mqtt.tx++;
  return OK;
}

/* "<msg>:<hmac>" on domain.vmflow.xyz/<subdomain>/<what>, QoS 1: how the
 * ESP-IDF firmware reports sales and failed vends.
 */

int vmflow_publish_signed(FAR const char *what, FAR const char *msg)
{
  char topic[VMFLOW_TOPIC_LEN];
  char line[160];

  pthread_mutex_lock(&g_lock);
  snprintf(topic, sizeof(topic), "domain." VMFLOW_DOMAIN "/%s/%s", g_sub,
           what);
  vmflow_sign(g_key, msg, line, sizeof(line));
  pthread_mutex_unlock(&g_lock);

  return vmflow_mqtt_publish(topic, line, strlen(line), 1, false);
}

void vmflow_mqtt_restart(void)
{
  g_mqtt.restart = true;
}

void vmflow_mqtt_status(FAR FILE *out)
{
  fprintf(out, "mqtt    : %s for %lld s, broker %s:%d\n",
          g_mqtt.connected ? "connected" : "disconnected",
          (long long)(uptime() - g_mqtt.since), CONFIG_VMFLOW_MQTT_HOST,
          CONFIG_VMFLOW_MQTT_PORT);
  fprintf(out, "sessions: %u, failed attempts: %u\n",
          g_mqtt.connects, g_mqtt.failures);
  fprintf(out, "messages: rx %u, tx %u, late acks %u\n", g_mqtt.rx,
          g_mqtt.tx, g_mqtt.late_acks);

  if (g_mqtt.last_error < 0)
    {
      fprintf(out, "last err: %s\n", mqtt_error_str(g_mqtt.last_error));
    }
}
