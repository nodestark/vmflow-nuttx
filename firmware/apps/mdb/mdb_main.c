/****************************************************************************
 * firmware/apps/mdb/mdb_main.c
 *
 * "mdb" command: runs the MDB cashless engine (nxinit service) and, from any
 * shell or script, sends it commands through the client API in mdb.h.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mdb.h"
#include "mdb_engine.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void usage(FAR const char *progname)
{
  fprintf(stderr,
          "usage: %s                 run the engine (what init does)\n"
          "       %s start           run it in the background\n"
          "       %s status\n"
          "       %s credit <amount> session with funds, e.g. 1.00\n"
          "       %s open            session where each vend waits for "
          "approve/deny\n"
          "       %s close           cancel the session or pending "
          "credit\n"
          "       %s approve|deny    answer the vend being requested\n"
          "       %s oos             reply \"out of sequence\" to the VMC\n"
          "       %s events [s]      print engine events for s seconds "
          "(60)\n",
          progname, progname, progname, progname, progname, progname,
          progname, progname, progname);
}

/* "1", "1.5", "1.50" -> cents */

static int parse_amount(FAR const char *str, FAR uint32_t *cents)
{
  uint32_t units = 0;
  uint32_t frac = 0;
  int digits = 0;

  if (!isdigit((unsigned char)*str))
    {
      return -EINVAL;
    }

  while (isdigit((unsigned char)*str))
    {
      units = units * 10 + (*str++ - '0');
      if (units > 100000)
        {
          return -ERANGE;
        }
    }

  if (*str == '.' || *str == ',')
    {
      str++;
      while (isdigit((unsigned char)*str) && digits < 2)
        {
          frac = frac * 10 + (*str++ - '0');
          digits++;
        }

      if (digits == 1)
        {
          frac *= 10;
        }
    }

  if (*str != '\0')
    {
      return -EINVAL;
    }

  *cents = units * 100 + frac;
  return *cents > 0 ? OK : -EINVAL;
}

static int request(uint8_t cmd, uint32_t amount)
{
  int ret = mdb_request(cmd, amount);

  if (ret == -ENOENT)
    {
      fprintf(stderr, "mdb: engine not running\n");
    }
  else if (ret < 0)
    {
      fprintf(stderr, "mdb: request failed: %d\n", ret);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int show_events(int seconds)
{
  struct mdb_evt_s evt;
  struct timespec deadline;
  mqd_t mq;

  mq = mq_open(MDB_EVT_MQ, O_RDONLY);
  if (mq == (mqd_t)-1)
    {
      fprintf(stderr, "mdb: engine not running\n");
      return EXIT_FAILURE;
    }

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += seconds;

  while (mq_timedreceive(mq, (FAR char *)&evt, sizeof(evt), NULL,
                         &deadline) == sizeof(evt))
    {
      if (evt.evt == MDB_EVT_STATE)
        {
          printf("state %s\n", mdb_state_name(evt.state));
        }
      else if (evt.price == MDB_FUNDS_UNLIMITED)
        {
          printf("%s app\n", mdb_event_name(evt.evt));
        }
      else
        {
          printf("%s price %lu.%02lu item %u\n", mdb_event_name(evt.evt),
                 (unsigned long)evt.price / 100,
                 (unsigned long)evt.price % 100, evt.item);
        }
    }

  mq_close(mq);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t cents;

  if (argc < 2)
    {
      if (mdb_engine_running())
        {
          printf("mdb: already running\n");
          return EXIT_FAILURE;
        }

      return mdb_engine_run(argc, argv);
    }

  if (strcmp(argv[1], "start") == 0)
    {
      return mdb_engine_start() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      mdb_engine_status();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "credit") == 0 && argc == 3)
    {
      if (parse_amount(argv[2], &cents) < 0)
        {
          fprintf(stderr, "mdb: bad amount \"%s\"\n", argv[2]);
          return EXIT_FAILURE;
        }

      return request(MDB_CMD_SESSION_OPEN, cents);
    }

  if (strcmp(argv[1], "open") == 0)
    {
      return request(MDB_CMD_SESSION_OPEN, MDB_FUNDS_UNLIMITED);
    }

  if (strcmp(argv[1], "close") == 0)
    {
      return request(MDB_CMD_SESSION_CLOSE, 0);
    }

  if (strcmp(argv[1], "approve") == 0)
    {
      return request(MDB_CMD_VEND_APPROVE, 0);
    }

  if (strcmp(argv[1], "deny") == 0)
    {
      return request(MDB_CMD_VEND_DENY, 0);
    }

  if (strcmp(argv[1], "oos") == 0)
    {
      return request(MDB_CMD_OUT_OF_SEQUENCE, 0);
    }

  if (strcmp(argv[1], "events") == 0)
    {
      return show_events(argc > 2 ? atoi(argv[2]) : 60);
    }

  usage(argv[0]);
  return EXIT_FAILURE;
}
