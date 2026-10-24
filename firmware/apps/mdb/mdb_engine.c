/****************************************************************************
 * firmware/apps/mdb/mdb_engine.c
 *
 * MDB cashless engine: answers the VMC on /dev/mdb0 as a level 1 cashless
 * device. Port of mdb_cashless_task() from the ESP-IDF firmware
 * (mdb-esp32-cashless), with the session queue and the *_todo flags fed by
 * the MDB_CMD_MQ message queue instead of BLE/MQTT callbacks, and what
 * happens on the bus published on MDB_EVT_MQ.
 *
 * The VMC expects an answer within 5 ms of its command, so the reply goes
 * out first; queued commands and events run after it, while the driver
 * keeps buffering the next words.
 *
 * No syslog once the engine is up: the default syslog channel writes the
 * USB-Serial/JTAG console with up_putc(), which spins until the host reads
 * the port. With the port closed that would stop the engine (and, at its
 * priority, everything below it). What happens goes to MDB_EVT_MQ and
 * "mdb status" instead.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "mdb.h"
#include "mdb_engine.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ACK            0x00
#define RET            0xaa
#define NAK            0xff

#define BIT_MODE       0x100
#define BIT_ADDR       0x0f8
#define BIT_CMD        0x007

#define RX_ERROR       0x200        /* never a bus word: read failed */

#define FUNDS_UNLIMITED_WIRE 0xffff
#define FUNDS_MAX_WIRE       0xfffe

#define CMDQ_DEPTH     8
#define EVTQ_DEPTH     16
#define MAX_EVENTS     4            /* per frame */

/* VMC -> reader commands (low three bits of the address byte) */

enum
{
  MDB_RESET     = 0x00,
  MDB_SETUP     = 0x01,
  MDB_POLL      = 0x02,
  MDB_VEND      = 0x03,
  MDB_READER    = 0x04,
  MDB_EXPANSION = 0x07
};

enum
{
  SETUP_CONFIG_DATA    = 0x00,
  SETUP_MAX_MIN_PRICES = 0x01
};

enum
{
  VEND_REQUEST     = 0x00,
  VEND_CANCEL      = 0x01,
  VEND_SUCCESS     = 0x02,
  VEND_FAILURE     = 0x03,
  SESSION_COMPLETE = 0x04,
  CASH_SALE        = 0x05
};

enum
{
  READER_DISABLE = 0x00,
  READER_ENABLE  = 0x01,
  READER_CANCEL  = 0x02
};

enum
{
  EXPANSION_REQUEST_ID = 0x00
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct mdb_engine_s
{
  pid_t pid;
  int fd;
  mqd_t cmdq;
  mqd_t evtq;
  bool ioerr;

  uint8_t state;                /* enum mdb_state_e */
  uint8_t vmc_level;

  /* Replies owed to the VMC, sent one per POLL (the *_todo flags) */

  bool reset_todo;
  bool cancel_todo;
  bool approved_todo;
  bool denied_todo;
  bool end_todo;
  bool oos_todo;

  uint16_t funds;               /* session funds, scaled; 0xffff = app */
  uint16_t pending;             /* credit waiting for a session, scaled */
  uint16_t item_price;          /* scaled */
  uint16_t item_number;
  time_t session_since;

  struct mdb_evt_s events[MAX_EVENTS];
  int nevents;

  /* Counters for "mdb status" */

  time_t since;
  unsigned int sessions;
  unsigned int vends;
  unsigned int vend_fails;
  unsigned int cash_sales;
  unsigned int bad_frames;
  unsigned long words;          /* every word on the bus */
  unsigned long frames;         /* frames addressed to us */
  uint32_t last_price;          /* cents */
  uint16_t last_item;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct mdb_engine_s g_mdb =
{
  .fd   = -1,
  .cmdq = (mqd_t)-1,
  .evtq = (mqd_t)-1,
};

static const uint16_t g_pow10[] =
{
  1, 10, 100, 1000
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t uptime(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static bool task_alive(pid_t pid)
{
  return pid > 0 && kill(pid, 0) == 0;
}

/* Cents <-> MDB scaled units: value = scaled * scale factor / 10^decimals,
 * TO_SCALE_FACTOR/FROM_SCALE_FACTOR in the ESP-IDF firmware.
 */

static uint16_t to_wire(uint32_t cents)
{
  uint64_t v;

  v = (uint64_t)cents * g_pow10[CONFIG_VMFLOW_MDB_DECIMAL_PLACES] /
      (100 * CONFIG_VMFLOW_MDB_SCALE_FACTOR);

  return v > FUNDS_MAX_WIRE ? FUNDS_MAX_WIRE : v;
}

static uint32_t from_wire(uint16_t scaled)
{
  if (scaled == FUNDS_UNLIMITED_WIRE)
    {
      return MDB_FUNDS_UNLIMITED;
    }

  return (uint32_t)scaled * CONFIG_VMFLOW_MDB_SCALE_FACTOR * 100 /
         g_pow10[CONFIG_VMFLOW_MDB_DECIMAL_PLACES];
}

/* One bus word, added to the running checksum. A failed read returns a
 * value no checksum matches, so the frame is dropped and the main loop
 * sees ioerr.
 */

static uint16_t rx(FAR uint8_t *checksum)
{
  uint16_t word;
  ssize_t n;

  do
    {
      n = read(g_mdb.fd, &word, sizeof(word));
    }
  while (n < 0 && errno == EINTR);

  if (n != sizeof(word))
    {
      g_mdb.ioerr = true;
      return RX_ERROR;
    }

  g_mdb.words++;
  if (checksum)
    {
      *checksum += word;
    }

  return word;
}

/* Reply: data words, then the checksum with the mode bit. An empty reply
 * is the checksum alone, 0x100 = ACK.
 */

static void tx(FAR const uint8_t *payload, size_t len)
{
  uint16_t words[40];
  uint8_t checksum = 0;
  size_t i;

  for (i = 0; i < len; i++)
    {
      checksum += payload[i];
      words[i] = payload[i];
    }

  words[len] = BIT_MODE | checksum;
  write(g_mdb.fd, words, (len + 1) * sizeof(uint16_t));
}

static void add_event(uint8_t evt, uint32_t price, uint16_t item)
{
  FAR struct mdb_evt_s *e;

  if (g_mdb.nevents == MAX_EVENTS)
    {
      return;
    }

  e = &g_mdb.events[g_mdb.nevents++];
  e->evt   = evt;
  e->price = price;
  e->item  = item;
  e->state = g_mdb.state;
}

static void set_state(uint8_t state)
{
  if (g_mdb.state != state)
    {
      g_mdb.state = state;
      add_event(MDB_EVT_STATE, 0, 0);
    }
}

/* After the reply: hand the frame's events to the reader of MDB_EVT_MQ.
 * Nobody may be reading, so a full queue drops its oldest event.
 */

static void flush_events(void)
{
  FAR struct mdb_evt_s *e;
  struct mdb_evt_s old;
  int i;

  for (i = 0; i < g_mdb.nevents; i++)
    {
      e = &g_mdb.events[i];

      if (mq_send(g_mdb.evtq, (FAR const char *)e, sizeof(*e), 0) < 0 &&
          errno == EAGAIN)
        {
          mq_receive(g_mdb.evtq, (FAR char *)&old, sizeof(old), NULL);
          mq_send(g_mdb.evtq, (FAR const char *)e, sizeof(*e), 0);
        }
    }

  g_mdb.nevents = 0;
}

static void apply_command(FAR const struct mdb_cmd_s *cmd)
{
  uint16_t scaled;

  switch (cmd->cmd)
    {
      case MDB_CMD_SESSION_OPEN:

        /* Credits add up until the VMC lets a session begin; an app
         * session (unlimited) takes over whatever was pending.
         */

        if (cmd->amount == MDB_FUNDS_UNLIMITED)
          {
            g_mdb.pending = FUNDS_UNLIMITED_WIRE;
          }
        else if (g_mdb.pending != FUNDS_UNLIMITED_WIRE)
          {
            scaled = to_wire(cmd->amount);
            g_mdb.pending = g_mdb.pending + scaled > FUNDS_MAX_WIRE ?
                            FUNDS_MAX_WIRE : g_mdb.pending + scaled;
          }
        break;

      case MDB_CMD_SESSION_CLOSE:
        if (g_mdb.state == MDB_STATE_VEND)
          {
            g_mdb.denied_todo = true;
          }
        else if (g_mdb.state == MDB_STATE_IDLE)
          {
            g_mdb.cancel_todo = true;
          }
        else
          {
            g_mdb.pending = 0;
          }
        break;

      case MDB_CMD_VEND_APPROVE:
      case MDB_CMD_VEND_DENY:
        if (g_mdb.state != MDB_STATE_VEND)
          {
            break;                              /* no vend to answer */
          }

        if (cmd->cmd == MDB_CMD_VEND_APPROVE)
          {
            g_mdb.approved_todo = true;
          }
        else
          {
            g_mdb.denied_todo = true;
          }
        break;

      case MDB_CMD_OUT_OF_SEQUENCE:
        g_mdb.oos_todo = true;
        break;

      default:
        break;
    }
}

static void drain_commands(void)
{
  struct mdb_cmd_s cmd;

  while (mq_receive(g_mdb.cmdq, (FAR char *)&cmd, sizeof(cmd), NULL) ==
         sizeof(cmd))
    {
      apply_command(&cmd);
    }
}

/****************************************************************************
 * Name: handle_poll
 *
 * Description:
 *   One owed reply per POLL, in the order of the ESP-IDF firmware.
 *
 ****************************************************************************/

static size_t handle_poll(FAR uint8_t *out)
{
  time_t now = uptime();

  if (g_mdb.reset_todo)
    {
      g_mdb.reset_todo = false;
      out[0] = 0x00;                            /* JUST RESET */
      return 1;
    }

  if (g_mdb.state == MDB_STATE_ENABLED && g_mdb.pending != 0)
    {
      g_mdb.funds   = g_mdb.pending;
      g_mdb.pending = 0;
      g_mdb.session_since = now;
      g_mdb.sessions++;
      set_state(MDB_STATE_IDLE);
      add_event(MDB_EVT_SESSION_BEGIN, from_wire(g_mdb.funds), 0);

      out[0] = 0x03;                            /* BEGIN SESSION */
      out[1] = g_mdb.funds >> 8;
      out[2] = g_mdb.funds;
      return 3;
    }

  if (g_mdb.cancel_todo)
    {
      g_mdb.cancel_todo = false;
      out[0] = 0x04;                            /* SESSION CANCEL REQUEST */
      return 1;
    }

  if (g_mdb.approved_todo)
    {
      g_mdb.approved_todo = false;
      add_event(MDB_EVT_VEND_APPROVED, from_wire(g_mdb.item_price),
                g_mdb.item_number);

      out[0] = 0x05;                            /* VEND APPROVED */
      out[1] = g_mdb.item_price >> 8;
      out[2] = g_mdb.item_price;
      return 3;
    }

  if (g_mdb.denied_todo)
    {
      g_mdb.denied_todo = false;
      set_state(MDB_STATE_IDLE);
      add_event(MDB_EVT_VEND_DENIED, from_wire(g_mdb.item_price),
                g_mdb.item_number);

      out[0] = 0x06;                            /* VEND DENIED */
      return 1;
    }

  if (g_mdb.end_todo)
    {
      g_mdb.end_todo = false;
      set_state(MDB_STATE_ENABLED);
      add_event(MDB_EVT_SESSION_END, from_wire(g_mdb.funds), 0);
      g_mdb.funds = 0;

      out[0] = 0x07;                            /* END SESSION */
      return 1;
    }

  if (g_mdb.oos_todo)
    {
      g_mdb.oos_todo = false;
      out[0] = 0x0b;                            /* CMD OUT OF SEQUENCE */
      return 1;
    }

  /* Nobody picked a product: give the session back. A vend still waiting
   * for an app decision is denied instead. Retried every timeout until
   * the VMC closes the session.
   */

  if (g_mdb.state >= MDB_STATE_IDLE &&
      now - g_mdb.session_since > CONFIG_VMFLOW_MDB_SESSION_TIMEOUT)
    {
      g_mdb.session_since = now;
      if (g_mdb.state == MDB_STATE_VEND)
        {
          g_mdb.denied_todo = true;
        }
      else
        {
          g_mdb.cancel_todo = true;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: handle_frame
 *
 * Description:
 *   Reads the rest of a frame addressed to us and builds the reply.
 *
 * Returned Value:
 *   Reply length, or -1 to stay silent (bad checksum).
 *
 ****************************************************************************/

static int handle_frame(uint8_t command, uint8_t checksum,
                        FAR uint8_t *out)
{
  uint16_t price;
  uint16_t item;
  int i;

  switch (command)
    {
      case MDB_RESET:
        if (rx(NULL) != checksum)
          {
            return -1;
          }

        g_mdb.reset_todo    = true;
        g_mdb.cancel_todo   = false;
        g_mdb.approved_todo = false;
        g_mdb.denied_todo   = false;
        g_mdb.end_todo      = false;
        g_mdb.funds         = 0;
        set_state(MDB_STATE_INACTIVE);
        return 0;

      case MDB_SETUP:
        switch (rx(&checksum))
          {
            case SETUP_CONFIG_DATA:
              g_mdb.vmc_level = rx(&checksum);
              rx(&checksum);                    /* columns on display */
              rx(&checksum);                    /* rows on display */
              rx(&checksum);                    /* display info */
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              set_state(MDB_STATE_DISABLED);

              out[0] = 0x01;                    /* READER CONFIG DATA */
              out[1] = 1;                       /* feature level */
              out[2] = CONFIG_VMFLOW_MDB_CURRENCY_CODE >> 8;
              out[3] = CONFIG_VMFLOW_MDB_CURRENCY_CODE & 0xff;
              out[4] = CONFIG_VMFLOW_MDB_SCALE_FACTOR;
              out[5] = CONFIG_VMFLOW_MDB_DECIMAL_PLACES;
              out[6] = 3;                       /* max response time, s */
              out[7] = 0x09;                    /* misc options */
              return 8;

            case SETUP_MAX_MIN_PRICES:
              for (i = 0; i < 4; i++)
                {
                  rx(&checksum);
                }

              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              return 0;
          }

        return 0;

      case MDB_POLL:
        if (rx(NULL) != checksum)
          {
            return -1;
          }

        return handle_poll(out);

      case MDB_VEND:
        switch (rx(&checksum))
          {
            case VEND_REQUEST:
              price  = rx(&checksum) << 8;
              price |= rx(&checksum) & 0xff;
              item   = rx(&checksum) << 8;
              item  |= rx(&checksum) & 0xff;
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              g_mdb.item_price  = price;
              g_mdb.item_number = item;
              set_state(MDB_STATE_VEND);
              add_event(MDB_EVT_VEND_REQUEST, from_wire(price), item);

              /* Fixed funds decide here; an app session waits for
               * MDB_CMD_VEND_APPROVE/DENY.
               */

              if (g_mdb.funds != FUNDS_UNLIMITED_WIRE)
                {
                  if (price <= g_mdb.funds)
                    {
                      g_mdb.funds -= price;
                      g_mdb.approved_todo = true;
                    }
                  else
                    {
                      g_mdb.denied_todo = true;
                    }
                }

              return 0;

            case VEND_CANCEL:
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              g_mdb.denied_todo = true;
              return 0;

            case VEND_SUCCESS:
              item  = rx(&checksum) << 8;
              item |= rx(&checksum) & 0xff;
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              g_mdb.vends++;
              g_mdb.last_price = from_wire(g_mdb.item_price);
              g_mdb.last_item  = item;
              set_state(MDB_STATE_IDLE);
              add_event(MDB_EVT_VEND_SUCCESS, g_mdb.last_price, item);
              return 0;

            case VEND_FAILURE:
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              /* Product not delivered: the price goes back to the
               * session funds.
               */

              if (g_mdb.funds != FUNDS_UNLIMITED_WIRE)
                {
                  g_mdb.funds = g_mdb.funds + g_mdb.item_price >
                                FUNDS_MAX_WIRE ? FUNDS_MAX_WIRE :
                                g_mdb.funds + g_mdb.item_price;
                }

              g_mdb.vend_fails++;
              set_state(MDB_STATE_IDLE);
              add_event(MDB_EVT_VEND_FAILURE, from_wire(g_mdb.item_price),
                        g_mdb.item_number);
              return 0;

            case SESSION_COMPLETE:
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              g_mdb.end_todo = true;
              return 0;

            case CASH_SALE:
              price  = rx(&checksum) << 8;
              price |= rx(&checksum) & 0xff;
              item   = rx(&checksum) << 8;
              item  |= rx(&checksum) & 0xff;
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              g_mdb.cash_sales++;
              add_event(MDB_EVT_CASH_SALE, from_wire(price), item);
              return 0;
          }

        return 0;

      case MDB_READER:
        switch (rx(&checksum))
          {
            case READER_DISABLE:
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              set_state(MDB_STATE_DISABLED);
              return 0;

            case READER_ENABLE:
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              set_state(MDB_STATE_ENABLED);
              return 0;

            case READER_CANCEL:
              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              out[0] = 0x08;                    /* CANCELLED */
              return 1;
          }

        return 0;

      case MDB_EXPANSION:
        switch (rx(&checksum))
          {
            case EXPANSION_REQUEST_ID:
              for (i = 0; i < 29; i++)
                {
                  rx(&checksum);
                }

              if (rx(NULL) != checksum)
                {
                  return -1;
                }

              out[0] = 0x09;                    /* PERIPHERAL ID */
              memcpy(&out[1], "VMF", 3);        /* manufacturer */
              memset(&out[4], ' ', 12);         /* serial number */
              memset(&out[16], ' ', 12);        /* model number */
              out[28] = 0x00;                   /* software version */
              out[29] = 0x03;
              return 30;
          }

        return 0;
    }

  return 0;
}

static void engine_close(void)
{
  if (g_mdb.fd >= 0)
    {
      close(g_mdb.fd);
      g_mdb.fd = -1;
    }

  if (g_mdb.cmdq != (mqd_t)-1)
    {
      mq_close(g_mdb.cmdq);
      g_mdb.cmdq = (mqd_t)-1;
    }

  if (g_mdb.evtq != (mqd_t)-1)
    {
      mq_close(g_mdb.evtq);
      g_mdb.evtq = (mqd_t)-1;
    }
}

static int engine_open(void)
{
  struct mq_attr attr;

  g_mdb.fd = open(CONFIG_VMFLOW_MDB_DEVPATH, O_RDWR);
  if (g_mdb.fd < 0)
    {
      syslog(LOG_ERR, "mdb: open %s failed: %d\n",
             CONFIG_VMFLOW_MDB_DEVPATH, errno);
      return -1;
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg  = CMDQ_DEPTH;
  attr.mq_msgsize = sizeof(struct mdb_cmd_s);

  g_mdb.cmdq = mq_open(MDB_CMD_MQ, O_RDONLY | O_NONBLOCK | O_CREAT, 0666,
                       &attr);

  attr.mq_maxmsg  = EVTQ_DEPTH;
  attr.mq_msgsize = sizeof(struct mdb_evt_s);

  g_mdb.evtq = mq_open(MDB_EVT_MQ, O_RDWR | O_NONBLOCK | O_CREAT, 0666,
                       &attr);

  if (g_mdb.cmdq == (mqd_t)-1 || g_mdb.evtq == (mqd_t)-1)
    {
      syslog(LOG_ERR, "mdb: mq_open failed: %d\n", errno);
      engine_close();
      return -1;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int mdb_engine_run(int argc, FAR char *argv[])
{
  uint8_t out[32];
  uint8_t checksum;
  uint16_t word;
  int len;

  if (engine_open() < 0)
    {
      return 1;
    }

  g_mdb.pid   = getpid();
  g_mdb.since = uptime();
  g_mdb.ioerr = false;

  /* A VMC that set the reader up before this (re)start keeps polling it
   * as enabled. JUST RESET on the first poll makes it run SETUP and READER
   * ENABLE again, so both sides agree on the state.
   */

  g_mdb.reset_todo = true;
  g_mdb.state      = MDB_STATE_INACTIVE;

  while (!g_mdb.ioerr)
    {
      checksum = 0;
      word = rx(&checksum);

      /* Only an address byte (mode bit set) starts a frame; the VMC's
       * ACK/RET/NAK and other peripherals' traffic are skipped.
       */

      if ((word & BIT_MODE) == 0 || word == RX_ERROR)
        {
          continue;
        }

      if ((uint8_t)word == ACK || (uint8_t)word == RET ||
          (uint8_t)word == NAK)
        {
          continue;
        }

      if ((word & BIT_ADDR) != CONFIG_VMFLOW_MDB_ADDRESS)
        {
          continue;
        }

      g_mdb.frames++;
      len = handle_frame(word & BIT_CMD, checksum, out);
      if (len < 0)
        {
          g_mdb.bad_frames++;
          continue;
        }

      tx(out, len);

      /* Off the 5 ms path: what the clients asked for (acted on at the
       * next poll) and what this frame did.
       */

      drain_commands();
      flush_events();
    }

  syslog(LOG_ERR, "mdb: bus read failed: %d\n", errno);
  engine_close();
  g_mdb.pid = 0;
  return 1;
}

int mdb_engine_start(void)
{
  pid_t pid;

  if (task_alive(g_mdb.pid))
    {
      printf("mdb: already running (pid %d)\n", g_mdb.pid);
      return 0;
    }

  pid = task_create("mdbd", CONFIG_VMFLOW_MDB_PRIORITY,
                    CONFIG_VMFLOW_MDB_STACKSIZE, mdb_engine_run, NULL);
  if (pid < 0)
    {
      printf("mdb: failed to start: %d\n", errno);
      return -1;
    }

  printf("mdb: started (pid %d)\n", pid);
  return 0;
}

bool mdb_engine_running(void)
{
  return task_alive(g_mdb.pid);
}

void mdb_engine_status(void)
{
  if (!task_alive(g_mdb.pid))
    {
      printf("mdb: not running\n");
      return;
    }

  printf("engine  : pid %d, up %lld s, %s, address 0x%02x\n", g_mdb.pid,
         (long long)(uptime() - g_mdb.since), CONFIG_VMFLOW_MDB_DEVPATH,
         CONFIG_VMFLOW_MDB_ADDRESS);
  printf("state   : %s, vmc level %u\n", mdb_state_name(g_mdb.state),
         g_mdb.vmc_level);

  if (g_mdb.state >= MDB_STATE_IDLE)
    {
      if (g_mdb.funds == FUNDS_UNLIMITED_WIRE)
        {
          printf("session : app (vends wait for approve/deny), %lld s\n",
                 (long long)(uptime() - g_mdb.session_since));
        }
      else
        {
          printf("session : funds %lu, %lld s\n",
                 (unsigned long)from_wire(g_mdb.funds),
                 (long long)(uptime() - g_mdb.session_since));
        }
    }

  if (g_mdb.pending == FUNDS_UNLIMITED_WIRE)
    {
      printf("pending : app session\n");
    }
  else if (g_mdb.pending != 0)
    {
      printf("pending : credit %lu\n",
             (unsigned long)from_wire(g_mdb.pending));
    }

  printf("bus     : %lu words, %lu frames for us, %u bad\n",
         g_mdb.words, g_mdb.frames, g_mdb.bad_frames);
  printf("totals  : sessions %u, vends %u, failed %u, cash %u\n",
         g_mdb.sessions, g_mdb.vends, g_mdb.vend_fails, g_mdb.cash_sales);

  if (g_mdb.vends > 0)
    {
      printf("last    : item %u, price %lu\n", g_mdb.last_item,
             (unsigned long)g_mdb.last_price);
    }

  printf("currency: 0x%04x, scale %d, decimals %d (amounts in cents)\n",
         CONFIG_VMFLOW_MDB_CURRENCY_CODE, CONFIG_VMFLOW_MDB_SCALE_FACTOR,
         CONFIG_VMFLOW_MDB_DECIMAL_PLACES);
}
