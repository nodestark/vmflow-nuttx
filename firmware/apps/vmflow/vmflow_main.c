/****************************************************************************
 * firmware/apps/vmflow/vmflow_main.c
 *
 * VMflow orchestrator (NuttX port of mdb-esp32-cashless): ties the MQTT
 * backend and the phone app (BLE) to the MDB engine, the audit reader, the
 * buzzer and the status LED. Optional: the engine and the other services
 * run without it.
 *
 *   vmflow                  run it in the foreground (what init does)
 *   vmflow start            spawn it in the background
 *   vmflow status
 *   vmflow pub <topic> <msg> [qos] [retain]
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "mdb.h"
#include "vmflow.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pid_t g_pid;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool running(void)
{
  return g_pid > 0 && kill(g_pid, 0) == 0;
}

static int vmflow_daemon(int argc, FAR char *argv[])
{
  g_pid = getpid();

  vmflow_led_update();

  if (vmflow_machine_start() < 0)
    {
      fprintf(stderr, "vmflow: cannot start the MDB bridge\n");
    }

  if (vmflow_ble_start() < 0)
    {
      fprintf(stderr, "vmflow: cannot start BLE\n");
    }

  return vmflow_mqtt_run();
}

static void show_status(void)
{
  char sub[VMFLOW_SUB_LEN];
  struct utsname name;
  bool installed;

  uname(&name);
  printf("vmflow  : %s %s, build %s\n", name.sysname, name.release,
         name.version);

  if (!running())
    {
      printf("daemon  : not running\n");
      return;
    }

  installed = vmflow_identity(sub, NULL);
  printf("daemon  : pid %d, device %s\n", g_pid,
         installed ? sub : "not installed (" CONFIG_VMFLOW_DATADIR ")");

  if (g_machine.engine)
    {
      printf("machine : %s, last sale item %u price %lu, %u failed vends\n",
             mdb_state_name(g_machine.state), g_machine.last_sale_item,
             (unsigned long)g_machine.last_sale_price, g_machine.vend_fails);
    }
  else
    {
      printf("machine : no MDB engine\n");
    }

  vmflow_mqtt_status();
  vmflow_ble_status();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  pid_t pid;
  int ret;

  if (argc < 2)
    {
      if (running())
        {
          printf("vmflow: already running (pid %d)\n", g_pid);
          return EXIT_FAILURE;
        }

      return vmflow_daemon(argc, argv);
    }

  if (strcmp(argv[1], "start") == 0)
    {
      if (running())
        {
          printf("vmflow: already running (pid %d)\n", g_pid);
          return EXIT_SUCCESS;
        }

      pid = task_create("vmflow", CONFIG_VMFLOW_PRIORITY,
                        CONFIG_VMFLOW_STACKSIZE, vmflow_daemon, NULL);
      if (pid < 0)
        {
          fprintf(stderr, "vmflow: failed to start: %d\n", errno);
          return EXIT_FAILURE;
        }

      printf("vmflow: started (pid %d)\n", pid);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      show_status();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "pub") == 0 && argc >= 4)
    {
      ret = vmflow_mqtt_publish(argv[2], argv[3], strlen(argv[3]),
                                argc > 4 ? atoi(argv[4]) : 0,
                                argc > 5 && atoi(argv[5]) != 0);
      if (ret < 0)
        {
          fprintf(stderr, "vmflow: publish failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      return EXIT_SUCCESS;
    }

  fprintf(stderr, "usage: %s [start|status|pub <topic> <msg> [qos] "
          "[retain]]\n", argv[0]);
  return EXIT_FAILURE;
}
