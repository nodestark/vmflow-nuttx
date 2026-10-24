/****************************************************************************
 * firmware/boards/vmflow-esp32s3/src/esp32s3_gpio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board GPIOs exposed through the NuttX GPIO driver: the buzzer as
 * /dev/gpio0 (nsh> gpio -o 1 /dev/gpio0) and the SIM7080G PWRKEY by name.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <assert.h>
#include <debug.h>
#include <sys/param.h>

#include <nuttx/ioexpander/gpio.h>
#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "hardware/esp32s3_gpio_sigmap.h"

#include "vmflow-esp32s3.h"

#ifdef CONFIG_DEV_GPIO

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vmflow_gpout_s
{
  struct gpio_dev_s gpio;
  FAR const char *name;         /* NULL: /dev/gpio<minor> */
  uint8_t minor;
  uint8_t pin;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int gpout_read(FAR struct gpio_dev_s *dev, FAR bool *value);
static int gpout_write(FAR struct gpio_dev_s *dev, bool value);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gpio_operations_s g_gpout_ops =
{
  .go_read   = gpout_read,
  .go_write  = gpout_write,
  .go_attach = NULL,
  .go_enable = NULL,
};

static struct vmflow_gpout_s g_gpout[] =
{
  {
    .minor = 0,
    .pin   = CONFIG_VMFLOW_BUZZER_GPIO,
  },
  {
    .name = "sim7080g_pwrkey",
    .pin  = CONFIG_VMFLOW_SIM7080G_PWRKEY_GPIO,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int gpout_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  FAR struct vmflow_gpout_s *out = (FAR struct vmflow_gpout_s *)dev;

  DEBUGASSERT(out != NULL && value != NULL);
  *value = esp_gpioread(out->pin);
  return OK;
}

static int gpout_write(FAR struct gpio_dev_s *dev, bool value)
{
  FAR struct vmflow_gpout_s *out = (FAR struct vmflow_gpout_s *)dev;

  DEBUGASSERT(out != NULL);
  esp_gpiowrite(out->pin, value);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_gpio_init
 ****************************************************************************/

int esp32s3_gpio_init(void)
{
  int ret;
  int i;

  for (i = 0; i < nitems(g_gpout); i++)
    {
      /* Drive low (buzzer off, key released) before exposing the pin */

      esp_gpio_matrix_out(g_gpout[i].pin, SIG_GPIO_OUT_IDX, 0, 0);
      esp_configgpio(g_gpout[i].pin, OUTPUT_FUNCTION_2 | INPUT_FUNCTION_2);
      esp_gpiowrite(g_gpout[i].pin, false);

      g_gpout[i].gpio.gp_pintype = GPIO_OUTPUT_PIN;
      g_gpout[i].gpio.gp_ops     = &g_gpout_ops;

      if (g_gpout[i].name != NULL)
        {
          ret = gpio_pin_register_byname(&g_gpout[i].gpio, g_gpout[i].name);
        }
      else
        {
          ret = gpio_pin_register(&g_gpout[i].gpio, g_gpout[i].minor);
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

#endif /* CONFIG_DEV_GPIO */
