/****************************************************************************
 * firmware/apps/vmflow/vmflow_main.c
 *
 * VMflow cashless application entry point (NuttX port of
 * mdb-esp32-cashless). MDB, uplink and BLE are not ported yet.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <sys/utsname.h>

int main(int argc, FAR char *argv[])
{
  struct utsname name;

  uname(&name);

  printf("VMflow on %s %s (%s)\n", name.sysname, name.release, name.machine);
  printf("build: %s\n", name.version);

  return 0;
}
