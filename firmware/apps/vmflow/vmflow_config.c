/****************************************************************************
 * firmware/apps/vmflow/vmflow_config.c
 *
 * Device identity as plain files, in place of the ESP-IDF firmware's NVS:
 *
 *   nsh> mkdir /data/vmflow
 *   nsh> echo 123 > /data/vmflow/subdomain
 *   nsh> echo <passkey> > /data/vmflow/passkey
 *
 * Read again whenever needed, so provisioning takes effect without a
 * restart. Trailing whitespace (echo's newline) is dropped.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vmflow.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool read_value(FAR const char *name, FAR char *buf, size_t len)
{
  char path[64];
  ssize_t n;
  int fd;

  snprintf(path, sizeof(path), CONFIG_VMFLOW_DATADIR "/%s", name);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      buf[0] = '\0';
      return false;
    }

  n = read(fd, buf, len - 1);
  close(fd);

  if (n < 0)
    {
      n = 0;
    }

  while (n > 0 && isspace((unsigned char)buf[n - 1]))
    {
      n--;
    }

  buf[n] = '\0';
  return n > 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Both values present: the device is installed. Either buffer may be NULL;
 * subdomain holds VMFLOW_SUB_LEN bytes, passkey VMFLOW_KEY_LEN.
 */

bool vmflow_identity(FAR char *subdomain, FAR char *passkey)
{
  char sub[VMFLOW_SUB_LEN];
  char key[VMFLOW_KEY_LEN];
  bool ok;

  ok  = read_value("subdomain", sub, sizeof(sub));
  ok &= read_value("passkey", key, sizeof(key));

  if (subdomain != NULL)
    {
      strcpy(subdomain, sub);
    }

  if (passkey != NULL)
    {
      strcpy(passkey, key);
    }

  return ok;
}

/* BLE provisioning: like the ESP-IDF firmware, a value already set is kept
 * (-EEXIST); only an empty or missing file is written.
 */

int vmflow_identity_set(FAR const char *name, FAR const char *value)
{
  char cur[VMFLOW_KEY_LEN];
  char path[64];
  int fd;

  if (read_value(name, cur, sizeof(cur)))
    {
      return -EEXIST;
    }

  mkdir(CONFIG_VMFLOW_DATADIR, 0755);
  snprintf(path, sizeof(path), CONFIG_VMFLOW_DATADIR "/%s", name);

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  write(fd, value, strlen(value));
  write(fd, "\n", 1);
  close(fd);
  return 0;
}
