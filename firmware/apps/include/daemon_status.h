/****************************************************************************
 * firmware/apps/include/daemon_status.h
 *
 * Status file of a daemon (mdb, sim7080g, vmflow): what "<name> status"
 * prints, written by the daemon itself, so the command never reads the
 * daemon's memory (which only works in a flat build). The first line is
 * the daemon's pid, so the file is also its pidfile, as in /run on Linux:
 *
 *   /tmp/<name>.status
 *     pid 7
 *     <status lines>
 *
 * /tmp is RAM (tmpfs). The daemon rewrites the file whole, through a
 * temporary file and rename(), so a reader never sees half of it. Apps
 * add CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/include.
 *
 ****************************************************************************/

#ifndef __VMFLOW_APPS_INCLUDE_DAEMON_STATUS_H
#define __VMFLOW_APPS_INCLUDE_DAEMON_STATUS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DAEMON_STATUS_PATH_MAX  48

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

static inline void daemon_status_path(FAR const char *name, FAR char *path,
                                      FAR const char *suffix)
{
  snprintf(path, DAEMON_STATUS_PATH_MAX, CONFIG_LIBC_TMPDIR "/%s.status%s",
           name, suffix);
}

/* Start a new status file: returns the stream to print the status lines
 * to, then daemon_status_commit(). NULL if /tmp cannot be written.
 */

static inline FAR FILE *daemon_status_begin(FAR const char *name)
{
  char path[DAEMON_STATUS_PATH_MAX];
  FAR FILE *out;

  daemon_status_path(name, path, ".new");
  out = fopen(path, "w");
  if (out != NULL)
    {
      fprintf(out, "pid %d\n", (int)getpid());
    }

  return out;
}

static inline void daemon_status_commit(FAR const char *name,
                                        FAR FILE *out)
{
  char tmp[DAEMON_STATUS_PATH_MAX];
  char path[DAEMON_STATUS_PATH_MAX];

  fclose(out);
  daemon_status_path(name, tmp, ".new");
  daemon_status_path(name, path, "");
  rename(tmp, path);
}

static inline void daemon_status_remove(FAR const char *name)
{
  char path[DAEMON_STATUS_PATH_MAX];

  daemon_status_path(name, path, "");
  unlink(path);
}

/* pid of the running daemon, 0 if it is not running (no file, or a file
 * left behind by one that died).
 */

static inline pid_t daemon_status_pid(FAR const char *name)
{
  char path[DAEMON_STATUS_PATH_MAX];
  char line[24];
  FAR FILE *in;
  pid_t pid = 0;

  daemon_status_path(name, path, "");
  in = fopen(path, "r");
  if (in == NULL)
    {
      return 0;
    }

  if (fgets(line, sizeof(line), in) != NULL &&
      strncmp(line, "pid ", 4) == 0)
    {
      pid = atoi(line + 4);
    }

  fclose(in);
  return pid > 0 && kill(pid, 0) == 0 ? pid : 0;
}

/* Print the status lines (all but the pid). -ESRCH if not running. */

static inline int daemon_status_show(FAR const char *name)
{
  char path[DAEMON_STATUS_PATH_MAX];
  char line[128];
  FAR FILE *in;

  if (daemon_status_pid(name) == 0)
    {
      return -ESRCH;
    }

  daemon_status_path(name, path, "");
  in = fopen(path, "r");
  if (in == NULL)
    {
      return -ESRCH;
    }

  fgets(line, sizeof(line), in);
  while (fgets(line, sizeof(line), in) != NULL)
    {
      fputs(line, stdout);
    }

  fclose(in);
  return 0;
}

#endif /* __VMFLOW_APPS_INCLUDE_DAEMON_STATUS_H */
