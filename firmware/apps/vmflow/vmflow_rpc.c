/****************************************************************************
 * firmware/apps/vmflow/vmflow_rpc.c
 *
 * Signed RPC from the backend, as in the ESP-IDF firmware:
 *
 *   topic    <subdomain>.vmflow.xyz/rpc
 *   payload  <cmd>:<args>:<ts>:<hmac>
 *
 * hmac is HMAC-SHA256(passkey, everything before the last ':') in lowercase
 * hex; ts is Unix time and must be within RPC_FRESHNESS_S of the device
 * clock (NTP). Commands without an argument send "-".
 *
 *   credit:<cents>  begin an MDB session with that credit, beep
 *   oos:-           answer the VMC's next poll with "out of sequence"
 *   dex:-           read the EVA-DTS audit, publish it on .../rpc/dex
 *   info:-          device snapshot JSON on .../rpc/info
 *   echo:-          reply <ts> on .../rpc/echo
 *   buzzer:-        1 s beep
 *   restart:-       reboot
 *
 * Every command but info and echo is acknowledged with "ok" on
 * domain.vmflow.xyz/<subdomain>/rpc/confirm.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <arpa/inet.h>
#include <sys/utsname.h>

#include "netutils/netlib.h"

#include "mdb.h"
#include "vmflow.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RPC_FRESHNESS_S  10

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void reply(FAR const char *sub, FAR const char *what,
                  FAR const char *payload, int qos)
{
  char topic[VMFLOW_TOPIC_LEN];

  snprintf(topic, sizeof(topic), "domain." VMFLOW_DOMAIN "/%s/rpc/%s", sub,
           what);
  vmflow_mqtt_publish(topic, payload, strlen(payload), qos, false);
}

static void publish_info(FAR const char *sub)
{
  struct utsname name;
  struct mallinfo mem;
  struct timespec ts;
  struct in_addr ppp;
  char json[384];

  uname(&name);
  mem = mallinfo();
  clock_gettime(CLOCK_MONOTONIC, &ts);

  ppp.s_addr = 0;
  netlib_get_ipv4addr("ppp0", &ppp);

  /* Same fields as rpc_publish_info() in the ESP-IDF firmware. There is no
   * minimum free heap here: the allocator keeps the largest amount ever
   * used, so arena - usmblks is the low-water mark.
   */

  snprintf(json, sizeof(json),
           "{\"version\":\"%s\",\"uptime_s\":%lld,"
           "\"free_heap\":%d,\"min_free_heap\":%d,\"machine_state\":%d,"
           "\"last_sale_price\":%lu,\"last_sale_item\":%u,"
           "\"last_vend_success_time\":%lld,"
           "\"ip_wifi\":\"\",\"ip_ppp\":\"%s\"}",
           name.version, (long long)ts.tv_sec,
           mem.fordblks, mem.arena - mem.usmblks,
           g_machine.engine ? g_machine.state : -1,
           (unsigned long)g_machine.last_sale_price,
           g_machine.last_sale_item,
           (long long)g_machine.last_vend_success_time,
           ppp.s_addr != 0 ? inet_ntoa(ppp) : "");

  reply(sub, "info", json, 1);
}

/* Copy the next ':'-terminated field of [*p, end) into out */

static bool field(FAR const char **p, FAR const char *end, FAR char *out,
                  size_t outlen)
{
  FAR const char *colon = memchr(*p, ':', end - *p);
  size_t n;

  if (colon == NULL || (size_t)(colon - *p) >= outlen)
    {
      return false;
    }

  n = colon - *p;
  memcpy(out, *p, n);
  out[n] = '\0';
  *p = colon + 1;
  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void vmflow_rpc(FAR const char *msg, size_t len)
{
  char sub[VMFLOW_SUB_LEN];
  char key[VMFLOW_KEY_LEN];
  char cmd[32];
  char args[64];
  char buf[24];
  FAR const char *colon;
  FAR const char *p;
  unsigned int ts;
  long dt;
  bool has_args;
  int ret;

  if (!vmflow_identity(sub, key))
    {
      return;
    }

  /* HMAC over everything before the last ':' */

  colon = memrchr(msg, ':', len);
  if (colon == NULL || !vmflow_verify(key, msg, colon - msg, colon + 1))
    {
      syslog(LOG_WARNING, "rpc: rejected, bad hmac\n");
      return;
    }

  /* <cmd>:<args>:<ts>, then the hmac after the last ':' */

  p = msg;
  if (!field(&p, colon + 1, cmd, sizeof(cmd)) ||
      !field(&p, colon + 1, args, sizeof(args)) ||
      !field(&p, colon + 1, buf, sizeof(buf)) || p != colon + 1)
    {
      syslog(LOG_WARNING, "rpc: rejected, malformed\n");
      return;
    }

  ts = strtoul(buf, NULL, 10);

  /* Freshness needs the wall clock: until NTP set it, refuse everything
   * rather than accept replays.
   */

  dt = (long)(time(NULL) - (time_t)ts);
  if (time(NULL) < VMFLOW_TIME_VALID || labs(dt) > RPC_FRESHNESS_S)
    {
      syslog(LOG_WARNING, "rpc: rejected %s, stale ts (dt=%ld)\n", cmd, dt);
      return;
    }

  has_args = args[0] != '\0' && strcmp(args, "-") != 0;
  syslog(LOG_INFO, "rpc: %s %s\n", cmd, has_args ? args : "");

  if (strcmp(cmd, "credit") == 0 && has_args)
    {
      ret = mdb_request(MDB_CMD_SESSION_OPEN, strtoul(args, NULL, 10));
      if (ret < 0)
        {
          syslog(LOG_WARNING, "rpc: credit not queued: %d\n", ret);
          return;
        }

      vmflow_beep();
      reply(sub, "confirm", "ok", 1);
    }
  else if (strcmp(cmd, "oos") == 0)
    {
      if (mdb_request(MDB_CMD_OUT_OF_SEQUENCE, 0) == 0)
        {
          reply(sub, "confirm", "ok", 1);
        }
    }
  else if (strcmp(cmd, "dex") == 0)
    {
      if (vmflow_dex_start() == 0)
        {
          reply(sub, "confirm", "ok", 1);
        }
    }
  else if (strcmp(cmd, "info") == 0)
    {
      publish_info(sub);
    }
  else if (strcmp(cmd, "echo") == 0)
    {
      snprintf(buf, sizeof(buf), "%u", ts);
      reply(sub, "echo", buf, 0);
    }
  else if (strcmp(cmd, "buzzer") == 0)
    {
      vmflow_beep();
      reply(sub, "confirm", "ok", 1);
    }
  else if (strcmp(cmd, "restart") == 0)
    {
      reply(sub, "confirm", "ok", 1);
      vmflow_mqtt_restart();
    }
  else
    {
      syslog(LOG_WARNING, "rpc: unknown command %s\n", cmd);
    }
}
