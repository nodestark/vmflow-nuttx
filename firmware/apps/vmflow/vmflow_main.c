/****************************************************************************
 * firmware/apps/vmflow/vmflow_main.c
 *
 * VMflow cashless application entry point (NuttX port of
 * mdb-esp32-cashless). RPC and BLE are not ported yet; MDB is the mdb app.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#ifdef CONFIG_VMFLOW_MQTT
#  include "vmflow_mqtt.h"
#endif

static void show_version(void)
{
  struct utsname name;

  uname(&name);

  printf("VMflow on %s %s (%s)\n", name.sysname, name.release, name.machine);
  printf("build: %s\n", name.version);
}

#ifdef CONFIG_VMFLOW_MQTT
/* vmflow mqtt                    run the client in the foreground (init)
 * vmflow mqtt start              spawn it in the background
 * vmflow mqtt status
 * vmflow mqtt pub <topic> <msg> [qos] [retain]
 */

static int mqtt_command(int argc, FAR char *argv[])
{
  int ret;

  if (CONFIG_VMFLOW_SUBDOMAIN[0] == '\0')
    {
      fprintf(stderr, "mqtt: CONFIG_VMFLOW_SUBDOMAIN is not set\n");
      return EXIT_FAILURE;
    }

  if (argc < 3)
    {
      if (vmflow_mqtt_running())
        {
          printf("mqtt: already running\n");
          return EXIT_FAILURE;
        }

      return vmflow_mqtt_run(argc, argv);
    }

  if (strcmp(argv[2], "start") == 0)
    {
      return vmflow_mqtt_start() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[2], "status") == 0)
    {
      vmflow_mqtt_status();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[2], "pub") == 0 && argc >= 5)
    {
      ret = vmflow_mqtt_publish(argv[3], argv[4], strlen(argv[4]),
                                argc > 5 ? atoi(argv[5]) : 0,
                                argc > 6 && atoi(argv[6]) != 0);
      if (ret < 0)
        {
          fprintf(stderr, "mqtt: publish failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      return EXIT_SUCCESS;
    }

  fprintf(stderr, "usage: %s mqtt [start|status|pub <topic> <msg> "
          "[qos] [retain]]\n", argv[0]);
  return EXIT_FAILURE;
}
#endif

int main(int argc, FAR char *argv[])
{
#ifdef CONFIG_VMFLOW_MQTT
  if (argc > 1 && strcmp(argv[1], "mqtt") == 0)
    {
      return mqtt_command(argc, argv);
    }
#endif

  show_version();
  return 0;
}
