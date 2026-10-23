/****************************************************************************
 * firmware/apps/sim7080g/sim7080g_main.c
 *
 * SIM7080G modem daemon: powers the modem, runs pppd on its UART and
 * supervises the ppp0 link. Port of sim7080g_task() from the ESP-IDF
 * firmware (mdb-esp32-cashless), with pppd + TUN standing in for
 * esp_modem + esp_netif PPP.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <nuttx/ioexpander/gpio.h>

#include "netutils/netlib.h"
#include "netutils/pppd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PPP_IFNAME            "ppp0"
#define SUPERVISE_PERIOD_S    5

/* PWRKEY timing (SIM7080G hardware design: >1 s on, >1.2 s off) */

#define PWRKEY_PULSE_MS       1200
#define MODEM_BOOT_S          5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sim7080g_state_s
{
  pid_t daemon_pid;
  pid_t ppp_pid;
  bool link_up;
  time_t link_since;          /* last link up/down transition */
  unsigned int link_drops;
  unsigned int modem_resets;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct sim7080g_state_s g_state;

/* Runs on every (re)connect, so a modem that was power-cycled or lost its
 * registration is configured again. ATH drops a data call left behind by
 * "+++" (otherwise ATD answers ERROR). "+CGATT: 1" (attached to the packet
 * domain) stands in for the CEREG home/roaming check: until the modem is
 * attached the expect times out and pppd retries the script.
 */

static char g_connect_script[384];

static const char g_disconnect_script[] =
  "TIMEOUT 3 "
  "\"\" AT "
  "OK ATH "
  "OK \\c";

static struct pppd_settings_s g_pppd_settings;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t uptime(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static int64_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int pwrkey_pulse(void)
{
  int fd;
  int ret;

  fd = open(CONFIG_SIM7080G_PWRKEY, O_RDWR);
  if (fd < 0)
    {
      syslog(LOG_ERR, "sim7080g: open %s failed: %d\n",
             CONFIG_SIM7080G_PWRKEY, errno);
      return -errno;
    }

  ret = ioctl(fd, GPIOC_WRITE, (unsigned long)true);
  if (ret == 0)
    {
      usleep(PWRKEY_PULSE_MS * 1000);
      ret = ioctl(fd, GPIOC_WRITE, (unsigned long)false);
    }

  close(fd);
  return ret;
}

/* Send a command and wait for "OK" (or "ERROR") within timeout_ms. */

static bool modem_at(int fd, FAR const char *cmd, int timeout_ms)
{
  char buf[64];
  size_t len = 0;
  struct pollfd pfd;
  int64_t deadline;

  tcflush(fd, TCIFLUSH);
  write(fd, cmd, strlen(cmd));
  write(fd, "\r", 1);

  deadline = now_ms() + timeout_ms;
  pfd.fd = fd;
  pfd.events = POLLIN;

  while (now_ms() < deadline)
    {
      if (poll(&pfd, 1, 100) <= 0)
        {
          continue;
        }

      ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
      if (n <= 0)
        {
          continue;
        }

      len += n;
      buf[len] = '\0';

      if (strstr(buf, "OK") != NULL)
        {
          return true;
        }

      if (strstr(buf, "ERROR") != NULL)
        {
          return false;
        }

      if (len >= sizeof(buf) - 1)
        {
          /* Keep the tail so a split "OK" still matches */

          memmove(buf, buf + len - 4, 4);
          len = 4;
        }
    }

  return false;
}

static bool modem_sync(int fd)
{
  int i;

  for (i = 0; i < 5; i++)
    {
      if (modem_at(fd, "AT", 1000))
        {
          return true;
        }
    }

  return false;
}

/* Make sure the modem answers AT before pppd takes over the UART. A modem
 * left in data mode by a previous run is brought back with "+++"; a modem
 * that stays silent is powered on with PWRKEY.
 */

static void modem_power_on(void)
{
  int fd;

  fd = open(CONFIG_SIM7080G_TTY, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
    {
      syslog(LOG_ERR, "sim7080g: open %s failed: %d\n",
             CONFIG_SIM7080G_TTY, errno);
      return;
    }

  sleep(1);
  write(fd, "+++", 3);
  sleep(1);

  if (modem_sync(fd))
    {
      syslog(LOG_INFO, "sim7080g: modem already on\n");
    }
  else
    {
      syslog(LOG_INFO, "sim7080g: powering on\n");
      pwrkey_pulse();
      sleep(MODEM_BOOT_S);

      if (modem_sync(fd))
        {
          syslog(LOG_INFO, "sim7080g: modem ok\n");
        }
      else
        {
          /* Leave it to pppd retries and the link supervisor */

          syslog(LOG_ERR, "sim7080g: modem not answering\n");
        }
    }

  close(fd);
}

static bool link_is_up(FAR struct in_addr *addr)
{
  uint8_t flags = 0;

  addr->s_addr = INADDR_ANY;

  if (netlib_getifstatus(PPP_IFNAME, &flags) < 0 ||
      (flags & IFF_UP) == 0)
    {
      return false;
    }

  return netlib_get_ipv4addr(PPP_IFNAME, addr) == 0 &&
         addr->s_addr != INADDR_ANY;
}

static int ppp_task(int argc, FAR char *argv[])
{
  return pppd(&g_pppd_settings);
}

static void ppp_settings_init(void)
{
  snprintf(g_connect_script, sizeof(g_connect_script),
           "TIMEOUT 5 "
           "\"\" AT "
           "OK ATE0 "
           "OK ATH "
           "OK AT+CNMP=38 "
           "OK AT+CMNB=%d "
           "OK AT+CGDCONT=1,\\\"IP\\\",\\\"%s\\\" "
           "OK AT+CGATT? "
           "\"+CGATT: 1\" ATD*99# "
           "TIMEOUT 30 "
           "CONNECT \\c",
           CONFIG_SIM7080G_CMNB, CONFIG_SIM7080G_APN);

  strlcpy(g_pppd_settings.ttyname, CONFIG_SIM7080G_TTY,
          sizeof(g_pppd_settings.ttyname));
  g_pppd_settings.connect_script    = g_connect_script;
  g_pppd_settings.disconnect_script = g_disconnect_script;
#ifdef CONFIG_NETUTILS_PPPD_PAP
  strlcpy(g_pppd_settings.pap_username, CONFIG_SIM7080G_PAP_USERNAME,
          sizeof(g_pppd_settings.pap_username));
  strlcpy(g_pppd_settings.pap_password, CONFIG_SIM7080G_PAP_PASSWORD,
          sizeof(g_pppd_settings.pap_password));
#endif
}

static bool task_alive(pid_t pid)
{
  return pid > 0 && kill(pid, 0) == 0;
}

static int sim7080g_daemon(int argc, FAR char *argv[])
{
  struct in_addr addr;
  bool up;

  g_state.daemon_pid = getpid();

  /* A daemon restarted by init finds pppd still running: keep it */

  if (task_alive(g_state.ppp_pid))
    {
      syslog(LOG_INFO, "sim7080g: reusing pppd (pid %d)\n", g_state.ppp_pid);
    }
  else
    {
      ppp_settings_init();
      modem_power_on();

      g_state.ppp_pid = task_create("pppd", CONFIG_SIM7080G_PPP_PRIORITY,
                                    CONFIG_SIM7080G_PPP_STACKSIZE, ppp_task,
                                    NULL);
      if (g_state.ppp_pid < 0)
        {
          syslog(LOG_ERR, "sim7080g: pppd task failed: %d\n", errno);
          g_state.ppp_pid    = 0;
          g_state.daemon_pid = 0;
          return 1;
        }
    }

  g_state.link_since = uptime();

  for (; ; )
    {
      sleep(SUPERVISE_PERIOD_S);

      up = link_is_up(&addr);
      if (up != g_state.link_up)
        {
          g_state.link_up    = up;
          g_state.link_since = uptime();

          if (up)
            {
              syslog(LOG_INFO, "sim7080g: ppp got IP: %s\n",
                     inet_ntoa(addr));
            }
          else
            {
              g_state.link_drops++;
              syslog(LOG_WARNING, "sim7080g: ppp lost IP\n");
            }
        }

      /* pppd keeps retrying the connect script by itself. A modem that
       * hung or powered off never answers it, so toggle PWRKEY. The key
       * toggles power: if the modem was on, the next window turns it back
       * on.
       */

      if (!up &&
          uptime() - g_state.link_since >= CONFIG_SIM7080G_LINK_TIMEOUT)
        {
          syslog(LOG_WARNING, "sim7080g: no link for %d s, toggling "
                 "PWRKEY\n", CONFIG_SIM7080G_LINK_TIMEOUT);
          pwrkey_pulse();
          g_state.modem_resets++;
          g_state.link_since = uptime();
        }
    }

  return 0;
}

static void show_status(void)
{
  struct in_addr addr;
  struct in_addr dr;
  bool up;

  if (!task_alive(g_state.daemon_pid))
    {
      printf("sim7080g: not running\n");
      return;
    }

  up = link_is_up(&addr);
  printf("daemon  : pid %d, pppd pid %d\n",
         g_state.daemon_pid, g_state.ppp_pid);
  printf("link    : %s for %lld s\n", up ? "up" : "down",
         (long long)(uptime() - g_state.link_since));

  if (up)
    {
      printf("ip      : %s\n", inet_ntoa(addr));
      if (netlib_get_dripv4addr(PPP_IFNAME, &dr) == 0)
        {
          printf("gateway : %s\n", inet_ntoa(dr));
        }
    }

  printf("drops   : %u, modem resets: %u\n",
         g_state.link_drops, g_state.modem_resets);
  printf("apn     : %s, cmnb %d, tty %s\n", CONFIG_SIM7080G_APN,
         CONFIG_SIM7080G_CMNB, CONFIG_SIM7080G_TTY);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  /* No argument: run the daemon in the foreground (nxinit service).
   * "start": spawn it in the background (autostart disabled).
   */

  if (argc < 2 || strcmp(argv[1], "start") == 0)
    {
      if (task_alive(g_state.daemon_pid))
        {
          printf("sim7080g: already running (pid %d)\n", g_state.daemon_pid);
          return argc < 2 ? 1 : 0;
        }

      if (argc < 2)
        {
          return sim7080g_daemon(argc, argv);
        }

      g_state.daemon_pid = task_create("sim7080gd", CONFIG_SIM7080G_PRIORITY,
                                       CONFIG_SIM7080G_STACKSIZE,
                                       sim7080g_daemon, NULL);
      if (g_state.daemon_pid < 0)
        {
          printf("sim7080g: failed to start: %d\n", errno);
          g_state.daemon_pid = 0;
          return 1;
        }

      printf("sim7080g: started (pid %d)\n", g_state.daemon_pid);
      return 0;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      show_status();
      return 0;
    }

  fprintf(stderr, "usage: %s [start|status]\n", argv[0]);
  return 1;
}
