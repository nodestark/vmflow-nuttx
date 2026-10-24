/****************************************************************************
 * firmware/apps/vmflow/vmflow_io.c
 *
 * The orchestrator's outputs, each through what the system already offers:
 *
 *   buzzer  the GPIO driver (/dev/gpio0), like "gpio -o 1 /dev/gpio0"
 *   LED     the WS2812 driver (/dev/leds0), one uint32_t 0x00RRGGBB
 *   audit   the dex command, its stdout in a file, published on .../rpc/dex
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/ioexpander/gpio.h>

#include "builtin/builtin.h"
#include "nshlib/nshlib.h"

#include "mdb.h"
#include "vmflow.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BEEP_MS        1000
#define LED_LVL        30           /* per-channel brightness, as ESP-IDF */
#define DEX_FILE       CONFIG_LIBC_TMPDIR "/vmflow-dex.txt"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_led_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_led_color = UINT32_MAX;
static volatile bool g_dex_busy;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int gpio_set(FAR const char *path, bool on)
{
  int fd;
  int ret;

  fd = open(path, O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  ret = ioctl(fd, GPIOC_WRITE, (unsigned long)on);
  close(fd);
  return ret;
}

static FAR void *beep_thread(FAR void *arg)
{
  if (gpio_set(CONFIG_VMFLOW_BUZZER_DEVPATH, true) == 0)
    {
      usleep(BEEP_MS * 1000);
      gpio_set(CONFIG_VMFLOW_BUZZER_DEVPATH, false);
    }

  return NULL;
}

static FAR void *dex_thread(FAR void *arg)
{
  FAR char *argv[] =
  {
    CONFIG_VMFLOW_DEX_COMMAND, NULL
  };

  struct nsh_param_s param;
  char sub[VMFLOW_SUB_LEN];
  char topic[VMFLOW_TOPIC_LEN];
  FAR char *audit = NULL;
  ssize_t len = 0;
  pid_t pid;
  int status;
  int fd;

  memset(&param, 0, sizeof(param));
  param.fd_in      = -1;
  param.fd_out     = -1;
  param.fd_err     = -1;
  param.file_out   = DEX_FILE;
  param.oflags_out = O_WRONLY | O_CREAT | O_TRUNC;

  pid = exec_builtin(CONFIG_VMFLOW_DEX_COMMAND, argv, &param);
  if (pid < 0)
    {
      syslog(LOG_ERR, "vmflow: dex: cannot run %s: %d\n",
             CONFIG_VMFLOW_DEX_COMMAND,
             errno);
      goto out;
    }

  waitpid(pid, &status, 0);

  fd = open(DEX_FILE, O_RDONLY);
  if (fd >= 0)
    {
      /* MQTT-C keeps the whole message in its send buffer until acked */

      audit = malloc(CONFIG_VMFLOW_MQTT_SENDBUF);
      if (audit != NULL)
        {
          len = read(fd, audit, CONFIG_VMFLOW_MQTT_SENDBUF - 256);
        }

      close(fd);
      unlink(DEX_FILE);
    }

  if (len > 0 && vmflow_identity(sub, NULL))
    {
      snprintf(topic, sizeof(topic), "domain." VMFLOW_DOMAIN "/%s/rpc/dex",
               sub);
      vmflow_mqtt_publish(topic, audit, len, 0, false);
    }

  syslog(LOG_INFO, "vmflow: dex: %zd bytes published\n", len);
  free(audit);

out:
  g_dex_busy = false;
  return NULL;
}

static int detach(FAR void *(*entry)(FAR void *), size_t stacksize)
{
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stacksize);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&tid, &attr, entry, NULL);
  pthread_attr_destroy(&attr);
  return -ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 1 s beep without holding up the caller */

void vmflow_beep(void)
{
  detach(beep_thread, 1024);
}

/* Status colors of the ESP-IDF firmware:
 *   white    not installed (no subdomain/passkey)
 *   green    MQTT up and the VMC has the reader enabled
 *   magenta  MQTT up, MDB not
 *   blue     MDB up, MQTT not
 *   red      neither
 */

void vmflow_led_update(void)
{
  uint32_t color;
  bool net;
  bool mdb;
  int fd;

  net = vmflow_mqtt_connected();
  mdb = g_machine.engine && g_machine.state >= MDB_STATE_ENABLED;

  if (!vmflow_identity(NULL, NULL))
    {
      color = LED_LVL << 16 | LED_LVL << 8 | LED_LVL;
    }
  else if (net && mdb)
    {
      color = LED_LVL << 8;
    }
  else if (net)
    {
      color = LED_LVL << 16 | LED_LVL;
    }
  else if (mdb)
    {
      color = LED_LVL;
    }
  else
    {
      color = LED_LVL << 16;
    }

  pthread_mutex_lock(&g_led_lock);
  if (color != g_led_color)
    {
      fd = open(CONFIG_VMFLOW_LED_DEVPATH, O_WRONLY);
      if (fd >= 0)
        {
          write(fd, &color, sizeof(color));
          close(fd);
          g_led_color = color;
        }
    }

  pthread_mutex_unlock(&g_led_lock);
}

/* One audit read at a time; dex itself also refuses to run twice */

int vmflow_dex_start(void)
{
  int ret;

  if (g_dex_busy)
    {
      return -EBUSY;
    }

  g_dex_busy = true;
  ret = detach(dex_thread, 3072);
  if (ret < 0)
    {
      g_dex_busy = false;
    }

  return ret;
}
