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
 *   vmflow status           what the daemon wrote to /tmp/vmflow.status
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "daemon_status.h"
#include "mdb.h"
#include "vmflow.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define STATUS_NAME       "vmflow"    /* /tmp/vmflow.status */
#define STATUS_PERIOD_S   1
#define STATUS_STACKSIZE  4096

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void print_status(FAR FILE *out)
{
  char sub[VMFLOW_SUB_LEN];
  struct utsname name;
  bool installed;

  uname(&name);
  fprintf(out, "vmflow  : %s %s, build %s\n", name.sysname, name.release,
          name.version);

  installed = vmflow_identity(sub, NULL);
  fprintf(out, "device  : %s\n",
          installed ? sub : "not installed (" CONFIG_VMFLOW_DATADIR ")");

  if (g_machine.engine)
    {
      fprintf(out, "machine : %s, last sale item %u price %lu, "
              "%u failed vends\n", mdb_state_name(g_machine.state),
              g_machine.last_sale_item,
              (unsigned long)g_machine.last_sale_price,
              g_machine.vend_fails);
    }
  else
    {
      fprintf(out, "machine : no MDB engine\n");
    }

  vmflow_mqtt_status(out);
  vmflow_ble_status(out);
}

/* The daemon publishes its own status; "vmflow status" only reads it */

static FAR void *status_thread(FAR void *arg)
{
  FAR FILE *out;

  for (; ; )
    {
      out = daemon_status_begin(STATUS_NAME);
      if (out != NULL)
        {
          print_status(out);
          daemon_status_commit(STATUS_NAME, out);
        }

      sleep(STATUS_PERIOD_S);
    }

  return NULL;
}

static int status_start(void)
{
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, STATUS_STACKSIZE);
  ret = pthread_create(&tid, &attr, status_thread, NULL);
  pthread_attr_destroy(&attr);

  if (ret == 0)
    {
      pthread_setname_np(tid, "vmflow_status");
    }

  return -ret;
}

static int vmflow_daemon(int argc, FAR char *argv[])
{
  vmflow_led_update();

  if (status_start() < 0)
    {
      fprintf(stderr, "vmflow: cannot start the status thread\n");
    }

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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  pid_t pid;

  if (argc < 2 || strcmp(argv[1], "start") == 0)
    {
      pid = daemon_status_pid(STATUS_NAME);
      if (pid > 0)
        {
          printf("vmflow: already running (pid %d)\n", pid);
          return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
        }

      if (argc < 2)
        {
          return vmflow_daemon(argc, argv);
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
      if (daemon_status_show(STATUS_NAME) < 0)
        {
          printf("vmflow: not running\n");
        }

      return EXIT_SUCCESS;
    }

  fprintf(stderr, "usage: %s [start|status]\n", argv[0]);
  return EXIT_FAILURE;
}
