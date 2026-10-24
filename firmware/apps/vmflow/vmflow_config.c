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
 * The same directory keeps last_seq, the highest remote-credit sequence
 * accepted, so a replayed credit line is refused across reboots.
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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vmflow.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int write_value(FAR const char *name, FAR const char *value)
{
  char path[64];
  char tmp[64];
  int fd;

  mkdir(CONFIG_VMFLOW_DATADIR, 0755);
  snprintf(path, sizeof(path), CONFIG_VMFLOW_DATADIR "/%s", name);
  snprintf(tmp, sizeof(tmp), CONFIG_VMFLOW_DATADIR "/.%s", name);

  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  if (write(fd, value, strlen(value)) < 0 || write(fd, "\n", 1) < 0)
    {
      close(fd);
      unlink(tmp);
      return -errno;
    }

  close(fd);
  return rename(tmp, path) < 0 ? -errno : 0;
}

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

  if (read_value(name, cur, sizeof(cur)))
    {
      return -EEXIST;
    }

  return write_value(name, value);
}

/* Highest remote-credit seq accepted; 0 when none was */

uint32_t vmflow_last_seq(void)
{
  char buf[16];

  return read_value("last_seq", buf, sizeof(buf)) ?
         strtoul(buf, NULL, 10) : 0;
}

int vmflow_last_seq_set(uint32_t seq)
{
  char buf[16];

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)seq);
  return write_value("last_seq", buf);
}
