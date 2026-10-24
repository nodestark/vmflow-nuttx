/****************************************************************************
 * firmware/apps/mdb/mdb_engine.c
 *
 * MDB cashless engine: answers the VMC on /dev/mdb0 as a level 1 cashless
 * device. Port of mdb_cashless_task() from the ESP-IDF firmware
 * (mdb-esp32-cashless), with the session queue and the *_todo flags fed by
 * the MDB_CMD_MQ message queue instead of BLE/MQTT callbacks, and what
 * happens on the bus copied to every subscriber's event queue.
 *
 * The VMC expects an answer within 5 ms of its command, so the reply goes
 * out first; queued commands and events run after it, while the driver
 * keeps buffering the next words.
 *
 * Nothing waits forever, so neither the machine nor the engine can get
 * stuck:
 *
 *   - a session with no activity is cancelled (SESSION_TIMEOUT)
 *   - a vend nobody approves is denied (APPROVE_TIMEOUT)
 *   - a VMC that stops polling drops the reader to inactive (BUS_TIMEOUT)
 *   - a credit that never finds a session expires (CREDIT_TIMEOUT)
 *   - a word missing in the middle of a frame drops the frame
 *   - the engine feeds a hardware watchdog, which resets the board if the
 *     engine itself stops
 *
 * No logging on the bus path: formatting a log line costs stack and time
 * the 5 ms window does not have. What happens goes to the event queues and
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
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>

#ifdef CONFIG_VMFLOW_MDB_WATCHDOG
#  include <nuttx/timers/watchdog.h>
#endif

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
#define RX_TIMEOUT     0x400        /* never a bus word: nothing came */

/* Between frames the engine wakes up this often to run its timers and feed
 * the watchdog. Within a frame the VMC leaves at most 1 ms between words
 * (MDB 4.2, inter-byte time); WORD_GAP_MS gives the interrupt and the
 * scheduler room.
 */

#define IDLE_WAIT_MS   1000
#define WORD_GAP_MS    20

#define SEC_MS(s)      ((int64_t)(s) * 1000)

#define FUNDS_UNLIMITED_WIRE 0xffff
#define FUNDS_MAX_WIRE       0xfffe

#define CMDQ_DEPTH     8
#define MAX_SUBSCRIBERS 4
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

  struct
  {
    pid_t pid;
    mqd_t mq;
    unsigned int dropped;       /* events its full queue did not take */
  } subs[MAX_SUBSCRIBERS];
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
  bool vend_answered;           /* VEND APPROVED sent for this request */

  /* Timers (uptime_ms()) */

  int64_t session_since;        /* session begin */
  int64_t activity_ms;          /* session begin or last vend step */
  int64_t vend_since;           /* VEND REQUEST */
  int64_t pending_since;        /* last credit added to pending */
  int64_t frame_ms;             /* last frame addressed to us */

  bool frame_broken;            /* a word timed out in this frame */
  int wdog;                     /* watchdog fd, -1 without */

  struct mdb_evt_s events[MAX_EVENTS];
  int nevents;

  /* Counters for "mdb status" */

  int64_t since;
  unsigned int sessions;
  unsigned int vends;
  unsigned int vend_fails;
  unsigned int cash_sales;
  unsigned int bad_frames;
  unsigned long words;          /* every word on the bus */
  unsigned long frames;         /* frames addressed to us */
  unsigned int bus_losses;
  unsigned int credits_expired;
  unsigned int approve_timeouts;
  uint32_t last_price;          /* cents */
  uint16_t last_item;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct mdb_engine_s g_mdb =
{
  .fd   = -1,
  .wdog = -1,
  .cmdq = (mqd_t)-1,
};

static const uint16_t g_pow10[] =
{
  1, 10, 100, 1000
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int64_t uptime_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long secs_since(int64_t ms)
{
  return (uptime_ms() - ms) / 1000;
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

/* Waits up to timeout_ms for a word. RX_TIMEOUT if none came, RX_ERROR
 * (and ioerr) if the driver failed.
 */

static uint16_t rx_wait(int timeout_ms)
{
  struct pollfd pfd;
  uint16_t word;
  ssize_t n;
  int ret;

  pfd.fd     = g_mdb.fd;
  pfd.events = POLLIN;

  do
    {
      ret = poll(&pfd, 1, timeout_ms);
    }
  while (ret < 0 && errno == EINTR);

  if (ret == 0)
    {
      return RX_TIMEOUT;
    }

  n = ret < 0 ? -1 : read(g_mdb.fd, &word, sizeof(word));
  if (n != sizeof(word))
    {
      g_mdb.ioerr = true;
      return RX_ERROR;
    }

  g_mdb.words++;
  return word;
}

/* The next word of a frame, added to the running checksum. A word that
 * does not come (or a failed read) returns a value no checksum matches,
 * so the frame is dropped; the rest of a broken frame is not waited for.
 */

static uint16_t rx(FAR uint8_t *checksum)
{
  uint16_t word;

  if (g_mdb.frame_broken)
    {
      return RX_TIMEOUT;
    }

  word = rx_wait(WORD_GAP_MS);
  if (word == RX_TIMEOUT || word == RX_ERROR)
    {
      g_mdb.frame_broken = true;
      return word;
    }

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

static void send_event(int sub, FAR const struct mdb_evt_s *e)
{
  if (mq_send(g_mdb.subs[sub].mq, (FAR const char *)e, sizeof(*e), 0) < 0)
    {
      g_mdb.subs[sub].dropped++;
    }
}

/* After the reply: copy the frame's events to every subscriber. Never
 * blocks; a subscriber whose queue is full misses the event.
 */

static void flush_events(void)
{
  int i;
  int j;

  for (i = 0; i < g_mdb.nevents; i++)
    {
      for (j = 0; j < MAX_SUBSCRIBERS; j++)
        {
          if (g_mdb.subs[j].pid > 0)
            {
              send_event(j, &g_mdb.events[i]);
            }
        }
    }

  g_mdb.nevents = 0;
}

static void unsubscribe(int sub)
{
  mq_close(g_mdb.subs[sub].mq);
  g_mdb.subs[sub].pid = 0;
}

static void subscribe(pid_t pid)
{
  struct mdb_evt_s snap;
  char name[24];
  int slot = -1;
  int i;

  for (i = 0; i < MAX_SUBSCRIBERS; i++)
    {
      if (g_mdb.subs[i].pid == pid)
        {
          unsubscribe(i);               /* resubscribe: reopen its queue */
        }

      if (g_mdb.subs[i].pid <= 0 && slot < 0)
        {
          slot = i;
        }
    }

  snprintf(name, sizeof(name), MDB_EVT_MQ_FMT, (int)pid);
  if (slot < 0 || !task_alive(pid))
    {
      return;
    }

  g_mdb.subs[slot].mq = mq_open(name, O_WRONLY | O_NONBLOCK);
  if (g_mdb.subs[slot].mq == (mqd_t)-1)
    {
      return;
    }

  g_mdb.subs[slot].pid     = pid;
  g_mdb.subs[slot].dropped = 0;

  /* Where things stand, so a client that starts late (or restarts) does
   * not need the history.
   */

  snap.evt   = MDB_EVT_SNAPSHOT;
  snap.state = g_mdb.state;
  snap.price = g_mdb.state >= MDB_STATE_IDLE ? from_wire(g_mdb.funds) : 0;
  snap.item  = 0;
  send_event(slot, &snap);
}

static void apply_command(FAR const struct mdb_cmd_s *cmd)
{
  uint16_t scaled;
  int i;

  switch (cmd->cmd)
    {
      case MDB_CMD_SESSION_OPEN:

        /* Credits add up until the VMC lets a session begin; an app
         * session (unlimited) takes over whatever was pending.
         */

        g_mdb.pending_since = uptime_ms();

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

      case MDB_CMD_SUBSCRIBE:
        subscribe((pid_t)cmd->amount);
        break;

      case MDB_CMD_UNSUBSCRIBE:
        for (i = 0; i < MAX_SUBSCRIBERS; i++)
          {
            if (g_mdb.subs[i].pid == (pid_t)cmd->amount)
              {
                unsubscribe(i);
              }
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
  int64_t now = uptime_ms();

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
      g_mdb.activity_ms   = now;
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
      g_mdb.vend_answered = true;
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
      g_mdb.activity_ms = now;
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

  /* Nobody picked a product: give the session back. Retried every timeout
   * until the VMC closes the session.
   */

  if (g_mdb.state == MDB_STATE_IDLE &&
      now - g_mdb.activity_ms > SEC_MS(CONFIG_VMFLOW_MDB_SESSION_TIMEOUT))
    {
      g_mdb.activity_ms = now;
      g_mdb.cancel_todo = true;
    }

  /* A vend waiting for MDB_CMD_VEND_APPROVE/DENY that nobody answered.
   * Once approved, the VMC owns the vend until it reports the outcome.
   */

  if (g_mdb.state == MDB_STATE_VEND && !g_mdb.vend_answered &&
      !g_mdb.approved_todo && !g_mdb.denied_todo &&
      now - g_mdb.vend_since > SEC_MS(CONFIG_VMFLOW_MDB_APPROVE_TIMEOUT))
    {
      g_mdb.approve_timeouts++;
      g_mdb.denied_todo = true;
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
              out[6] = CONFIG_VMFLOW_MDB_RESPONSE_TIME; /* max response, s */
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

              g_mdb.item_price    = price;
              g_mdb.item_number   = item;
              g_mdb.vend_answered = false;
              g_mdb.vend_since    = uptime_ms();
              g_mdb.activity_ms   = g_mdb.vend_since;
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
              g_mdb.activity_ms = uptime_ms();
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
              g_mdb.activity_ms = uptime_ms();
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
  int i;

  /* The watchdog keeps running: if init cannot bring the engine back
   * within its timeout, the board resets.
   */

  if (g_mdb.wdog >= 0)
    {
      close(g_mdb.wdog);
      g_mdb.wdog = -1;
    }

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

  for (i = 0; i < MAX_SUBSCRIBERS; i++)
    {
      if (g_mdb.subs[i].pid > 0)
        {
          unsubscribe(i);
        }
    }
}

static int engine_open(void)
{
  struct mq_attr attr;

  g_mdb.fd = open(CONFIG_VMFLOW_MDB_DEVPATH, O_RDWR);
  if (g_mdb.fd < 0)
    {
      fprintf(stderr, "mdb: open %s failed: %d\n",
              CONFIG_VMFLOW_MDB_DEVPATH, errno);
      return -1;
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg  = CMDQ_DEPTH;
  attr.mq_msgsize = sizeof(struct mdb_cmd_s);

  g_mdb.cmdq = mq_open(MDB_CMD_MQ, O_RDONLY | O_NONBLOCK | O_CREAT, 0666,
                       &attr);

  if (g_mdb.cmdq == (mqd_t)-1)
    {
      fprintf(stderr, "mdb: mq_open failed: %d\n", errno);
      engine_close();
      return -1;
    }

#ifdef CONFIG_VMFLOW_MDB_WATCHDOG
  g_mdb.wdog = open(CONFIG_VMFLOW_MDB_WATCHDOG_DEVPATH, O_RDONLY);
  if (g_mdb.wdog < 0 ||
      ioctl(g_mdb.wdog, WDIOC_SETTIMEOUT,
            (unsigned long)CONFIG_VMFLOW_MDB_WATCHDOG_TIMEOUT * 1000) < 0 ||
      ioctl(g_mdb.wdog, WDIOC_START, 0) < 0)
    {
      fprintf(stderr, "mdb: watchdog %s unavailable: %d\n",
              CONFIG_VMFLOW_MDB_WATCHDOG_DEVPATH, errno);
      if (g_mdb.wdog >= 0)
        {
          close(g_mdb.wdog);
          g_mdb.wdog = -1;
        }
    }
#endif

  return 0;
}

/****************************************************************************
 * Name: housekeeping
 *
 * Description:
 *   Runs after every frame and at least every IDLE_WAIT_MS: the timers that
 *   do not need a poll to act on, and the watchdog.
 *
 ****************************************************************************/

static void housekeeping(void)
{
  int64_t now = uptime_ms();
  int i;

#ifdef CONFIG_VMFLOW_MDB_WATCHDOG
  if (g_mdb.wdog >= 0)
    {
      ioctl(g_mdb.wdog, WDIOC_KEEPALIVE, 0);
    }
#endif

  /* The VMC stopped talking to us (machine off, cable out, VMC reset
   * without RESET). The session is gone; JUST RESET resyncs a VMC that
   * comes back.
   */

  if (g_mdb.state != MDB_STATE_INACTIVE &&
      now - g_mdb.frame_ms > SEC_MS(CONFIG_VMFLOW_MDB_BUS_TIMEOUT))
    {
      if (g_mdb.state >= MDB_STATE_IDLE)
        {
          add_event(MDB_EVT_SESSION_END, from_wire(g_mdb.funds), 0);
        }

      g_mdb.bus_losses++;
      g_mdb.cancel_todo   = false;
      g_mdb.approved_todo = false;
      g_mdb.denied_todo   = false;
      g_mdb.end_todo      = false;
      g_mdb.funds         = 0;
      g_mdb.reset_todo    = true;
      set_state(MDB_STATE_INACTIVE);
      add_event(MDB_EVT_BUS_LOST, 0, 0);
    }

  /* Subscribers that exited without unsubscribing */

  for (i = 0; i < MAX_SUBSCRIBERS; i++)
    {
      if (g_mdb.subs[i].pid > 0 && !task_alive(g_mdb.subs[i].pid))
        {
          char name[24];

          snprintf(name, sizeof(name), MDB_EVT_MQ_FMT,
                   (int)g_mdb.subs[i].pid);
          unsubscribe(i);
          mq_unlink(name);
        }
    }

  /* A credit no session took: hand it back to whoever sent it. */

  if (g_mdb.pending != 0 &&
      now - g_mdb.pending_since > SEC_MS(CONFIG_VMFLOW_MDB_CREDIT_TIMEOUT))
    {
      g_mdb.credits_expired++;
      add_event(MDB_EVT_CREDIT_EXPIRED, from_wire(g_mdb.pending), 0);
      g_mdb.pending = 0;
    }

  flush_events();
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

  g_mdb.pid      = getpid();
  g_mdb.since    = uptime_ms();
  g_mdb.frame_ms = g_mdb.since;
  g_mdb.ioerr    = false;

  /* A VMC that set the reader up before this (re)start keeps polling it
   * as enabled. JUST RESET on the first poll makes it run SETUP and READER
   * ENABLE again, so both sides agree on the state.
   */

  g_mdb.reset_todo = true;
  g_mdb.state      = MDB_STATE_INACTIVE;

  while (!g_mdb.ioerr)
    {
      word = rx_wait(IDLE_WAIT_MS);

      /* Only an address byte (mode bit set) starts a frame; the VMC's
       * ACK/RET/NAK and other peripherals' traffic are skipped.
       */

      if ((word & BIT_MODE) != 0 && (word & (RX_ERROR | RX_TIMEOUT)) == 0 &&
          (uint8_t)word != ACK && (uint8_t)word != RET &&
          (uint8_t)word != NAK &&
          (word & BIT_ADDR) == CONFIG_VMFLOW_MDB_ADDRESS)
        {
          g_mdb.frames++;
          g_mdb.frame_ms     = uptime_ms();
          g_mdb.frame_broken = false;
          checksum = word;

          len = handle_frame(word & BIT_CMD, checksum, out);
          if (len < 0)
            {
              g_mdb.bad_frames++;
            }
          else
            {
              tx(out, len);
            }

          /* Off the 5 ms path: what the clients asked for (acted on at
           * the next poll) and what this frame did.
           */

          drain_commands();
        }

      housekeeping();
    }

  fprintf(stderr, "mdb: bus read failed: %d\n", errno);
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
  int i;

  if (!task_alive(g_mdb.pid))
    {
      printf("mdb: not running\n");
      return;
    }

  printf("engine  : pid %d, up %lld s, %s, address 0x%02x, watchdog %s\n",
         g_mdb.pid, secs_since(g_mdb.since), CONFIG_VMFLOW_MDB_DEVPATH,
         CONFIG_VMFLOW_MDB_ADDRESS, g_mdb.wdog >= 0 ? "on" : "off");
  printf("state   : %s, vmc level %u\n", mdb_state_name(g_mdb.state),
         g_mdb.vmc_level);

  if (g_mdb.state >= MDB_STATE_IDLE)
    {
      if (g_mdb.funds == FUNDS_UNLIMITED_WIRE)
        {
          printf("session : app (vends wait for approve/deny), %lld s, "
                 "idle %lld s\n", secs_since(g_mdb.session_since),
                 secs_since(g_mdb.activity_ms));
        }
      else
        {
          printf("session : funds %lu, %lld s, idle %lld s\n",
                 (unsigned long)from_wire(g_mdb.funds),
                 secs_since(g_mdb.session_since),
                 secs_since(g_mdb.activity_ms));
        }
    }

  if (g_mdb.state == MDB_STATE_VEND)
    {
      printf("vend    : item %u, price %lu, %lld s%s\n", g_mdb.item_number,
             (unsigned long)from_wire(g_mdb.item_price),
             secs_since(g_mdb.vend_since),
             g_mdb.vend_answered || g_mdb.approved_todo ||
             g_mdb.denied_todo ? "" : " (waiting for approve/deny)");
    }

  if (g_mdb.pending == FUNDS_UNLIMITED_WIRE)
    {
      printf("pending : app session, %lld s\n",
             secs_since(g_mdb.pending_since));
    }
  else if (g_mdb.pending != 0)
    {
      printf("pending : credit %lu, %lld s\n",
             (unsigned long)from_wire(g_mdb.pending),
             secs_since(g_mdb.pending_since));
    }

  printf("bus     : %lu words, %lu frames for us, %u bad, last %lld s "
         "ago, lost %u\n", g_mdb.words, g_mdb.frames, g_mdb.bad_frames,
         secs_since(g_mdb.frame_ms), g_mdb.bus_losses);
  printf("totals  : sessions %u, vends %u, failed %u, cash %u\n",
         g_mdb.sessions, g_mdb.vends, g_mdb.vend_fails, g_mdb.cash_sales);
  printf("timeouts: approve %u, credits expired %u (session %d s, approve "
         "%d s, bus %d s, credit %d s)\n", g_mdb.approve_timeouts,
         g_mdb.credits_expired, CONFIG_VMFLOW_MDB_SESSION_TIMEOUT,
         CONFIG_VMFLOW_MDB_APPROVE_TIMEOUT, CONFIG_VMFLOW_MDB_BUS_TIMEOUT,
         CONFIG_VMFLOW_MDB_CREDIT_TIMEOUT);

  if (g_mdb.vends > 0)
    {
      printf("last    : item %u, price %lu\n", g_mdb.last_item,
             (unsigned long)g_mdb.last_price);
    }

  for (i = 0; i < MAX_SUBSCRIBERS; i++)
    {
      if (g_mdb.subs[i].pid > 0)
        {
          printf("listener: pid %d, %u events dropped\n",
                 g_mdb.subs[i].pid, g_mdb.subs[i].dropped);
        }
    }

  printf("currency: 0x%04x, scale %d, decimals %d (amounts in cents)\n",
         CONFIG_VMFLOW_MDB_CURRENCY_CODE, CONFIG_VMFLOW_MDB_SCALE_FACTOR,
         CONFIG_VMFLOW_MDB_DECIMAL_PLACES);
}
