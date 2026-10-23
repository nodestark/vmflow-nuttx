/****************************************************************************
 * firmware/apps/vmflow/vmflow_mqtt.c
 *
 * MQTT uplink (MQTT-C over a plain TCP socket). Mirrors the esp-mqtt setup
 * of the ESP-IDF firmware: broker mqtt.vmflow.xyz:1883, keepalive 120 s,
 * retained last will "offline" on domain.vmflow.xyz/<subdomain>/status,
 * "online" published there on connect and a subscription to
 * <subdomain>.vmflow.xyz/#. The client waits for the network by itself:
 * while ppp0 is down, DNS or connect fail and it retries every
 * CONFIG_VMFLOW_MQTT_RECONNECT seconds.
 *
 * Incoming messages are only logged; the signed RPC handler is not ported
 * yet.
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
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>

#include <mqtt.h>

#include "vmflow_mqtt.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MQTT_DOMAIN       "vmflow.xyz"
#define MQTT_SYNC_MS      100
#define MQTT_TOPIC_LEN    64

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vmflow_mqtt_state_s
{
  pid_t pid;
  bool connected;
  time_t since;               /* last connect/disconnect */
  unsigned int connects;
  unsigned int failures;
  unsigned int rx;
  unsigned int tx;
  unsigned int late_acks;
  enum MQTTErrors last_error;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct vmflow_mqtt_state_s g_mqtt;
static struct mqtt_client g_client;

/* Guards g_client against "vmflow mqtt pub" while the client task swaps
 * the socket on reconnect.
 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t g_sendbuf[CONFIG_VMFLOW_MQTT_SENDBUF];
static uint8_t g_recvbuf[CONFIG_VMFLOW_MQTT_RECVBUF];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t uptime(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static bool task_alive(pid_t pid)
{
  return pid > 0 && kill(pid, 0) == 0;
}

static void status_topic(FAR char *topic, size_t len)
{
  snprintf(topic, len, "domain." MQTT_DOMAIN "/%s/status",
           CONFIG_VMFLOW_SUBDOMAIN);
}

static void publish_callback(FAR void **state,
                             FAR struct mqtt_response_publish *publish)
{
  g_mqtt.rx++;
  syslog(LOG_INFO, "mqtt: rx %.*s (%zu bytes): %.*s\n",
         publish->topic_name_size, (FAR const char *)publish->topic_name,
         publish->application_message_size,
         (int)publish->application_message_size,
         (FAR const char *)publish->application_message);
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
      syslog(LOG_WARNING, "mqtt: resolve %s failed: %d\n",
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
      syslog(LOG_WARNING, "mqtt: connect %s:%d failed: %d\n",
             CONFIG_VMFLOW_MQTT_HOST, CONFIG_VMFLOW_MQTT_PORT, errno);
      return -ECONNREFUSED;
    }

  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

static int session_start(int fd)
{
  char client_id[MQTT_TOPIC_LEN];
  char topic[MQTT_TOPIC_LEN];
  enum MQTTErrors err;

  snprintf(client_id, sizeof(client_id), "vmflow-%s",
           CONFIG_VMFLOW_SUBDOMAIN);
  status_topic(topic, sizeof(topic));

  pthread_mutex_lock(&g_lock);
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
      syslog(LOG_ERR, "mqtt: connect: %s\n", mqtt_error_str(err));
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
  char topic[MQTT_TOPIC_LEN];

  pthread_mutex_lock(&g_lock);
  status_topic(topic, sizeof(topic));
  mqtt_publish(&g_client, topic, "online", 6,
               MQTT_PUBLISH_QOS_1 | MQTT_PUBLISH_RETAIN);

  snprintf(topic, sizeof(topic), "%s." MQTT_DOMAIN "/#",
           CONFIG_VMFLOW_SUBDOMAIN);
  mqtt_subscribe(&g_client, topic, 0);
  g_mqtt.connected = true;
  pthread_mutex_unlock(&g_lock);

  g_mqtt.connects++;
  g_mqtt.since = uptime();
  syslog(LOG_INFO, "mqtt: connected to %s, subscribed %s\n",
         CONFIG_VMFLOW_MQTT_HOST, topic);
}

/* Run MQTT-C until the connection fails. */

static void session_run(void)
{
  enum MQTTErrors err;

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
          syslog(LOG_WARNING, "mqtt: %s\n", mqtt_error_str(err));
          return;
        }

      if (!g_mqtt.connected && g_client.event_connect)
        {
          session_connected();
        }

      usleep(MQTT_SYNC_MS * 1000);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vmflow_mqtt_run(int argc, FAR char *argv[])
{
  int fd;

  g_mqtt.pid   = getpid();
  g_mqtt.since = uptime();

  syslog(LOG_INFO, "mqtt: device %s, broker %s:%d\n",
         CONFIG_VMFLOW_SUBDOMAIN, CONFIG_VMFLOW_MQTT_HOST,
         CONFIG_VMFLOW_MQTT_PORT);

  for (; ; )
    {
      bool was_connected = false;

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
        }

      if (!was_connected)
        {
          g_mqtt.failures++;
        }

      sleep(CONFIG_VMFLOW_MQTT_RECONNECT);
    }

  return 0;
}

int vmflow_mqtt_start(void)
{
  pid_t pid;

  if (task_alive(g_mqtt.pid))
    {
      printf("mqtt: already running (pid %d)\n", g_mqtt.pid);
      return OK;
    }

  pid = task_create("vmflow_mqtt", CONFIG_VMFLOW_PRIORITY,
                    CONFIG_VMFLOW_MQTT_STACKSIZE, vmflow_mqtt_run, NULL);
  if (pid < 0)
    {
      printf("mqtt: failed to start: %d\n", errno);
      return -errno;
    }

  printf("mqtt: started (pid %d)\n", pid);
  return OK;
}

bool vmflow_mqtt_running(void)
{
  return task_alive(g_mqtt.pid);
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

void vmflow_mqtt_status(void)
{
  if (!task_alive(g_mqtt.pid))
    {
      printf("mqtt: not running\n");
      return;
    }

  printf("client  : pid %d, device %s\n", g_mqtt.pid,
         CONFIG_VMFLOW_SUBDOMAIN);
  printf("broker  : %s:%d, keepalive %d s\n", CONFIG_VMFLOW_MQTT_HOST,
         CONFIG_VMFLOW_MQTT_PORT, CONFIG_VMFLOW_MQTT_KEEPALIVE);
  printf("state   : %s for %lld s\n",
         g_mqtt.connected ? "connected" : "disconnected",
         (long long)(uptime() - g_mqtt.since));
  printf("sessions: %u, failed attempts: %u\n",
         g_mqtt.connects, g_mqtt.failures);
  printf("messages: rx %u, tx %u, late acks %u\n", g_mqtt.rx, g_mqtt.tx,
         g_mqtt.late_acks);

  if (g_mqtt.last_error < 0)
    {
      printf("last err: %s\n", mqtt_error_str(g_mqtt.last_error));
    }
}
