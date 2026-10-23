/****************************************************************************
 * firmware/apps/rgbled/rgbled_main.c
 *
 * Drives the board's addressable RGB LED through the WS2812 driver
 * (/dev/leds0, RMT backend). Every pixel is written as 0x00RRGGBB; the
 * driver reorders it to GRB on the wire. The LED latches the last color,
 * so the device is closed after each update.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RGBLED_DEFAULT_PERIOD_MS  500
#define RGBLED_DEFAULT_BLINKS     10
#define RGBLED_DEFAULT_RAINBOW_S  5
#define RGBLED_RAINBOW_STEP_MS    20

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rgbled_color_s
{
  FAR const char *name;
  uint32_t rgb;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rgbled_color_s g_colors[] =
{
  { "off",     0x000000 },
  { "red",     0xff0000 },
  { "green",   0x00ff00 },
  { "blue",    0x0000ff },
  { "yellow",  0xffff00 },
  { "cyan",    0x00ffff },
  { "magenta", 0xff00ff },
  { "white",   0xffffff },
  { "orange",  0xff4000 },
  { "purple",  0x8000ff },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int rgbled_write(uint32_t rgb)
{
  int fd;
  int ret = OK;

  fd = open(CONFIG_VMFLOW_RGBLED_DEVPATH, O_WRONLY);
  if (fd < 0)
    {
      fprintf(stderr, "rgbled: open %s failed: %d\n",
              CONFIG_VMFLOW_RGBLED_DEVPATH, errno);
      return -errno;
    }

  if (write(fd, &rgb, sizeof(rgb)) != sizeof(rgb))
    {
      fprintf(stderr, "rgbled: write failed: %d\n", errno);
      ret = -errno;
    }

  close(fd);
  return ret;
}

/* Accepts a color name, RRGGBB or #RRGGBB. */

static int rgbled_parse_color(FAR const char *arg, FAR uint32_t *rgb)
{
  FAR char *end;
  unsigned long value;
  size_t i;

  for (i = 0; i < sizeof(g_colors) / sizeof(g_colors[0]); i++)
    {
      if (strcasecmp(arg, g_colors[i].name) == 0)
        {
          *rgb = g_colors[i].rgb;
          return OK;
        }
    }

  if (arg[0] == '#')
    {
      arg++;
    }

  if (strlen(arg) != 6 || !isxdigit((unsigned char)arg[0]))
    {
      return -EINVAL;
    }

  value = strtoul(arg, &end, 16);
  if (*end != '\0')
    {
      return -EINVAL;
    }

  *rgb = (uint32_t)value;
  return OK;
}

static uint32_t rgbled_scale(uint32_t rgb, unsigned int percent)
{
  uint32_t r = ((rgb >> 16) & 0xff) * percent / 100;
  uint32_t g = ((rgb >> 8) & 0xff) * percent / 100;
  uint32_t b = (rgb & 0xff) * percent / 100;

  return (r << 16) | (g << 8) | b;
}

/* Color wheel: 0..255 walks red -> green -> blue -> red. */

static uint32_t rgbled_wheel(uint8_t pos)
{
  if (pos < 85)
    {
      return ((uint32_t)(255 - pos * 3) << 16) | ((uint32_t)(pos * 3) << 8);
    }

  if (pos < 170)
    {
      pos -= 85;
      return ((uint32_t)(255 - pos * 3) << 8) | (uint32_t)(pos * 3);
    }

  pos -= 170;
  return ((uint32_t)(pos * 3) << 16) | (uint32_t)(255 - pos * 3);
}

static int rgbled_blink(uint32_t rgb, unsigned int period_ms,
                        unsigned int count)
{
  unsigned int half_us = period_ms * 1000 / 2;
  unsigned int i;
  int ret;

  for (i = 0; i < count; i++)
    {
      ret = rgbled_write(rgb);
      if (ret < 0)
        {
          return ret;
        }

      usleep(half_us);

      ret = rgbled_write(0);
      if (ret < 0)
        {
          return ret;
        }

      usleep(half_us);
    }

  return OK;
}

static int rgbled_rainbow(unsigned int seconds, unsigned int percent)
{
  unsigned int steps = seconds * 1000 / RGBLED_RAINBOW_STEP_MS;
  unsigned int i;
  int ret;

  for (i = 0; i < steps; i++)
    {
      ret = rgbled_write(rgbled_scale(rgbled_wheel(i & 0xff), percent));
      if (ret < 0)
        {
          return ret;
        }

      usleep(RGBLED_RAINBOW_STEP_MS * 1000);
    }

  return rgbled_write(0);
}

static void rgbled_usage(void)
{
  size_t i;

  printf("Usage:\n"
         "  rgbled <color> [brightness%%]\n"
         "  rgbled blink <color> [period_ms] [count]\n"
         "  rgbled rainbow [seconds] [brightness%%]\n"
         "\n"
         "color: RRGGBB, #RRGGBB or one of:");

  for (i = 0; i < sizeof(g_colors) / sizeof(g_colors[0]); i++)
    {
      printf(" %s", g_colors[i].name);
    }

  printf("\n");
}

static unsigned int rgbled_uint(FAR const char *arg, unsigned int def,
                                unsigned int max)
{
  unsigned long value;

  if (arg == NULL)
    {
      return def;
    }

  value = strtoul(arg, NULL, 10);
  return value > max ? max : (unsigned int)value;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t rgb;
  int ret;

  if (argc < 2)
    {
      rgbled_usage();
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "blink") == 0)
    {
      if (argc < 3 || rgbled_parse_color(argv[2], &rgb) < 0)
        {
          rgbled_usage();
          return EXIT_FAILURE;
        }

      ret = rgbled_blink(rgb,
                         rgbled_uint(argc > 3 ? argv[3] : NULL,
                                     RGBLED_DEFAULT_PERIOD_MS, 10000),
                         rgbled_uint(argc > 4 ? argv[4] : NULL,
                                     RGBLED_DEFAULT_BLINKS, 1000));
    }
  else if (strcmp(argv[1], "rainbow") == 0)
    {
      ret = rgbled_rainbow(rgbled_uint(argc > 2 ? argv[2] : NULL,
                                       RGBLED_DEFAULT_RAINBOW_S, 600),
                           rgbled_uint(argc > 3 ? argv[3] : NULL, 100, 100));
    }
  else if (rgbled_parse_color(argv[1], &rgb) == OK)
    {
      ret = rgbled_write(rgbled_scale(rgb,
                                      rgbled_uint(argc > 2 ? argv[2] : NULL,
                                                  100, 100)));
    }
  else
    {
      rgbled_usage();
      return EXIT_FAILURE;
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
