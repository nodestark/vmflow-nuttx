/****************************************************************************
 * firmware/apps/dex/dex_main.c
 *
 * EVA-DTS audit reader: port of eva-dts.c from the ESP-IDF firmware
 * (mdb-esp32-cashless). Talks to the vending machine's DEX port, first
 * with DDCMP at 2400 baud and, if that yields nothing, with DEX/UCS at
 * 9600 baud. The audit text is printed and, when the vmflow MQTT session
 * is up, published on domain.vmflow.xyz/<subdomain>/rpc/dex.
 *
 * Timeouts follow the ESP-IDF uart_read_bytes() calls they replace: the
 * wait restarts whenever a byte arrives, so they bound the gap between
 * bytes rather than a whole message.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_VMFLOW_MQTT
#  include "vmflow_mqtt.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DEX/UCS control characters */

#define SOH   0x01
#define STX   0x02
#define ETX   0x03
#define EOT   0x04
#define ENQ   0x05
#define DLE   0x10
#define ETB   0x17

/* DDCMP message types and the station address used throughout */

#define DDCMP_CONTROL   0x05
#define DDCMP_DATA      0x81
#define DDCMP_ACK       0x01
#define DDCMP_STACK     0x07
#define DDCMP_SADD      0x01
#define DDCMP_RXSIZE    1024

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_fd = -1;

static uint8_t g_audit[CONFIG_VMFLOW_DEX_BUFSIZE];
static size_t g_len;
static bool g_truncated;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void audit_put(FAR const uint8_t *data, size_t len)
{
  if (len > sizeof(g_audit) - g_len)
    {
      len = sizeof(g_audit) - g_len;
      g_truncated = true;
    }

  memcpy(g_audit + g_len, data, len);
  g_len += len;
}

static uint16_t crc16(uint16_t crc, uint8_t data)
{
  int i;

  for (i = 0; i < 8; i++, data >>= 1)
    {
      if ((data ^ crc) & 0x01)
        {
          crc = (crc >> 1) ^ 0xa001;
        }
      else
        {
          crc >>= 1;
        }
    }

  return crc;
}

static int tty_baud(speed_t speed)
{
  struct termios tio;

  if (tcgetattr(g_fd, &tio) < 0)
    {
      return -errno;
    }

  cfmakeraw(&tio);
  cfsetspeed(&tio, speed);

  if (tcsetattr(g_fd, TCSANOW, &tio) < 0)
    {
      return -errno;
    }

  /* Drop bytes left over from the previous protocol attempt */

  tcflush(g_fd, TCIFLUSH);
  return OK;
}

static void tty_write(FAR const void *buf, size_t len)
{
  FAR const uint8_t *p = buf;
  ssize_t n;

  while (len > 0)
    {
      n = write(g_fd, p, len);
      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          return;
        }

      p   += n;
      len -= n;
    }
}

/* Read up to len bytes; returns how many arrived before timeout_ms passed
 * without a new byte.
 */

static size_t tty_read(FAR uint8_t *buf, size_t len, int timeout_ms)
{
  struct pollfd pfd;
  size_t got = 0;
  ssize_t n;

  pfd.fd     = g_fd;
  pfd.events = POLLIN;

  while (got < len)
    {
      if (poll(&pfd, 1, timeout_ms) <= 0)
        {
          break;
        }

      n = read(g_fd, buf + got, len - got);
      if (n <= 0)
        {
          break;
        }

      got += n;
    }

  return got;
}

static bool tty_expect(FAR const uint8_t *expected, size_t len,
                       int timeout_ms)
{
  uint8_t buf[2];

  return len <= sizeof(buf) && tty_read(buf, len, timeout_ms) == len &&
         memcmp(buf, expected, len) == 0;
}

/****************************************************************************
 * DDCMP
 ****************************************************************************/

/* Send a DDCMP block followed by its CRC-16 (low byte first). */

static void ddcmp_send(FAR const uint8_t *msg, size_t len)
{
  uint16_t crc = 0;
  uint8_t tail[2];
  size_t i;

  for (i = 0; i < len; i++)
    {
      crc = crc16(crc, msg[i]);
    }

  tail[0] = crc & 0xff;
  tail[1] = crc >> 8;

  tty_write(msg, len);
  tty_write(tail, 2);
}

static void ddcmp_send_ack(uint8_t rr)
{
  const uint8_t ack[] =
  {
    DDCMP_CONTROL, DDCMP_ACK, 0x40, rr, 0x00, DDCMP_SADD
  };

  ddcmp_send(ack, sizeof(ack));
}

static bool ddcmp_expect_ack(void)
{
  uint8_t rx[8];

  return tty_read(rx, 8, 200) == 8 &&
         rx[0] == DDCMP_CONTROL && rx[1] == DDCMP_ACK;
}

/* Receive one data message: 8-byte header, then <count> bytes plus CRC.
 * Returns the message length without the CRC, or -1.
 */

static int ddcmp_recv_data(FAR uint8_t *rx, FAR uint8_t *rr,
                           FAR bool *last)
{
  uint8_t hdr[8];
  size_t len;

  if (tty_read(hdr, 8, 200) != 8 || hdr[0] != DDCMP_DATA)
    {
      return -1;
    }

  *rr   = hdr[4];
  *last = (hdr[2] & 0x80) != 0;
  len   = ((size_t)(hdr[2] & 0x3f) << 8) | hdr[1];

  if (len + 2 > DDCMP_RXSIZE || tty_read(rx, len + 2, 200) != len + 2)
    {
      return -1;
    }

  return (int)len;
}

static bool ddcmp_read(void)
{
  static uint8_t rx[DDCMP_RXSIZE];
  const size_t start = g_len;
  uint8_t xx = 0;
  uint8_t rr;
  bool last;
  int len;

  const uint8_t start_msg[] =
  {
    DDCMP_CONTROL, 0x06, 0x40, 0x00, 0x00, DDCMP_SADD
  };

  /* Who-are-you: security code 0000, pass code 0000, date 01/01/70,
   * time 00:00:00, user 0000, 0x0c = route person.
   */

  const uint8_t who_are_you[] =
  {
    0x77, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c
  };

  /* Read data, audit collection list, read-only (no interim reset) */

  const uint8_t read_list[] =
  {
    0x77, 0xe2, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00
  };

  /* FINIS, with its CRC as sent by the ESP-IDF firmware */

  const uint8_t finis[] =
  {
    0x77, 0xff, 0x67, 0xb0
  };

  if (tty_baud(B2400) < 0)
    {
      return false;
    }

  /* START -> STACK */

  ddcmp_send(start_msg, sizeof(start_msg));
  if (tty_read(rx, 8, 200) != 8 ||
      rx[0] != DDCMP_CONTROL || rx[1] != DDCMP_STACK)
    {
      return false;
    }

  /* Who-are-you -> ACK, then the machine's identification */

  {
    const uint8_t hdr[] =
    {
      DDCMP_DATA, sizeof(who_are_you), 0x40, 0x00, ++xx, DDCMP_SADD
    };

    ddcmp_send(hdr, sizeof(hdr));
    ddcmp_send(who_are_you, sizeof(who_are_you));
  }

  if (!ddcmp_expect_ack() || ddcmp_recv_data(rx, &rr, &last) < 0)
    {
      return false;
    }

  ddcmp_send_ack(rr);

  /* Audit collection request -> ACK, accepted response */

  {
    const uint8_t hdr[] =
    {
      DDCMP_DATA, sizeof(read_list), 0x40, rr, ++xx, DDCMP_SADD
    };

    ddcmp_send(hdr, sizeof(hdr));
    ddcmp_send(read_list, sizeof(read_list));
  }

  if (!ddcmp_expect_ack())
    {
      return false;
    }

  len = ddcmp_recv_data(rx, &rr, &last);
  if (len < 3 || rx[2] != 0x01)
    {
      return false;
    }

  ddcmp_send_ack(rr);

  /* Audit blocks: 0x99 <block#> <audit data>, ACK each, FINIS after the
   * last one.
   */

  for (; ; )
    {
      len = ddcmp_recv_data(rx, &rr, &last);
      if (len < 0)
        {
          break;
        }

      if (len > 2)
        {
          audit_put(rx + 2, len - 2);
        }

      ddcmp_send_ack(rr);

      if (last)
        {
          const uint8_t hdr[] =
          {
            DDCMP_DATA, 0x02, 0x40, rr, 0x03, DDCMP_SADD
          };

          ddcmp_send(hdr, sizeof(hdr));
          tty_write(finis, sizeof(finis));
          ddcmp_expect_ack();
          break;
        }

      usleep(10 * 1000);
    }

  return g_len > start;
}

/****************************************************************************
 * DEX/UCS
 ****************************************************************************/

static bool dex_read(void)
{
  const size_t start = g_len;
  const char request[] = "1234567890" "R" "R00L06";
  uint8_t frame[2 + sizeof(request) - 1 + 4];
  uint8_t ack[2];
  uint8_t block = 0;
  uint8_t buf[18];
  uint8_t c;
  uint16_t crc = 0;
  size_t n = 0;
  size_t i;

  if (tty_baud(B9600) < 0)
    {
      return false;
    }

  /* First handshake: we are master. ENQ -> DLE 0, then
   * DLE SOH <communication id> <R> <revision/level> DLE ETX <crc> -> DLE 1
   */

  tty_write((uint8_t[]){ ENQ }, 1);
  if (!tty_expect((uint8_t[]){ DLE, '0' }, 2, 100))
    {
      return false;
    }

  frame[n++] = DLE;
  frame[n++] = SOH;
  for (i = 0; i < sizeof(request) - 1; i++)
    {
      frame[n++] = request[i];
      crc = crc16(crc, request[i]);
    }

  frame[n++] = DLE;
  frame[n++] = ETX;
  crc = crc16(crc, ETX);
  frame[n++] = crc & 0xff;
  frame[n++] = crc >> 8;
  tty_write(frame, n);

  if (!tty_expect((uint8_t[]){ DLE, '1' }, 2, 100))
    {
      return false;
    }

  tty_write((uint8_t[]){ EOT }, 1);

  /* Second handshake: the machine is master and answers with its
   * response code, communication id and revision/level.
   */

  if (!tty_expect((uint8_t[]){ ENQ }, 1, 1000))
    {
      return false;
    }

  tty_write((uint8_t[]){ DLE, '0' }, 2);
  if (!tty_expect((uint8_t[]){ DLE, SOH }, 2, 100) ||
      tty_read(buf, 2 + 10 + 6, 100) != 2 + 10 + 6 ||
      !tty_expect((uint8_t[]){ DLE, ETX }, 2, 100) ||
      tty_read(buf, 2, 100) != 2)
    {
      return false;
    }

  tty_write((uint8_t[]){ DLE, '1' }, 2);
  if (!tty_expect((uint8_t[]){ EOT }, 1, 100))
    {
      return false;
    }

  /* Data transfer: the machine may take a while to assemble the audit */

  if (!tty_expect((uint8_t[]){ ENQ }, 1, 3000))
    {
      return false;
    }

  for (; ; )
    {
      ack[0] = DLE;
      ack[1] = '0' + (block++ & 1);
      tty_write(ack, 2);

      if (!tty_expect((uint8_t[]){ DLE, STX }, 2, 1000))
        {
          break;
        }

      for (; ; )
        {
          if (tty_read(&c, 1, 200) != 1)
            {
              return g_len > start;
            }

          if (c == DLE)
            {
              if (tty_read(&c, 1, 200) != 1)
                {
                  return g_len > start;
                }

              if (c == ETB)
                {
                  /* End of block: CRC, then acknowledge the next one */

                  tty_read(buf, 2, 200);
                  break;
                }

              if (c == ETX)
                {
                  /* End of data: CRC, acknowledge, EOT */

                  tty_read(buf, 2, 200);
                  ack[0] = DLE;
                  ack[1] = '0' + (block++ & 1);
                  tty_write(ack, 2);
                  tty_read(buf, 1, 200);
                  return g_len > start;
                }

              /* DLE-escaped data byte: keep it */
            }

          audit_put(&c, 1);
        }
    }

  return g_len > start;
}

/****************************************************************************
 * Publishing
 ****************************************************************************/

static void audit_publish(void)
{
#ifdef CONFIG_VMFLOW_MQTT
  char topic[64];
  int ret;

  if (CONFIG_VMFLOW_SUBDOMAIN[0] == '\0' || !vmflow_mqtt_running())
    {
      return;
    }

  snprintf(topic, sizeof(topic), "domain.vmflow.xyz/%s/rpc/dex",
           CONFIG_VMFLOW_SUBDOMAIN);

  ret = vmflow_mqtt_publish(topic, g_audit, g_len, 0, false);
  if (ret < 0)
    {
      fprintf(stderr, "dex: publish on %s failed: %d\n", topic, ret);
    }
  else
    {
      fprintf(stderr, "dex: published on %s\n", topic);
    }
#endif
}

static void usage(FAR const char *progname)
{
  fprintf(stderr, "usage: %s [ddcmp|ucs]\n"
          "  no argument: DDCMP, then DEX/UCS if DDCMP got nothing\n",
          progname);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *protocol = NULL;
  bool try_ddcmp = true;
  bool try_ucs = true;
  bool ok = false;

  if (argc > 2)
    {
      usage(argv[0]);
      return EXIT_FAILURE;
    }

  if (argc == 2)
    {
      if (strcmp(argv[1], "ddcmp") == 0)
        {
          try_ucs = false;
        }
      else if (strcmp(argv[1], "ucs") == 0)
        {
          try_ddcmp = false;
        }
      else
        {
          usage(argv[0]);
          return EXIT_FAILURE;
        }
    }

  g_fd = open(CONFIG_VMFLOW_DEX_TTY, O_RDWR | O_NOCTTY);
  if (g_fd < 0)
    {
      fprintf(stderr, "dex: open %s failed: %d\n", CONFIG_VMFLOW_DEX_TTY,
              errno);
      return EXIT_FAILURE;
    }

  g_len       = 0;
  g_truncated = false;

  if (try_ddcmp && ddcmp_read())
    {
      protocol = "DDCMP";
      ok = true;
    }
  else if (try_ucs && dex_read())
    {
      protocol = "DEX/UCS";
      ok = true;
    }

  close(g_fd);
  g_fd = -1;

  if (!ok)
    {
      fprintf(stderr, "dex: no answer from the machine\n");
      return EXIT_FAILURE;
    }

  fwrite(g_audit, 1, g_len, stdout);
  if (g_len > 0 && g_audit[g_len - 1] != '\n')
    {
      putchar('\n');
    }

  fflush(stdout);
  fprintf(stderr, "dex: %zu bytes over %s%s\n", g_len, protocol,
          g_truncated ? " (truncated)" : "");

  audit_publish();
  return EXIT_SUCCESS;
}
