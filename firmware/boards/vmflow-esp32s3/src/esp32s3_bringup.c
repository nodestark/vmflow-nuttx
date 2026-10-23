/****************************************************************************
 * firmware/boards/vmflow-esp32s3/src/esp32s3_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from boards/xtensa/esp32s3/esp32s3-devkit (Apache NuttX).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>

#include <nuttx/fs/fs.h>
#include <arch/board/board.h>

#ifdef CONFIG_ESPRESSIF_HR_TIMER
#  include "espressif/esp_hr_timer.h"
#endif

#ifdef CONFIG_ESPRESSIF_EFUSE
#  include "espressif/esp_efuse.h"
#endif

#ifdef CONFIG_ESP32S3_PARTITION_TABLE
#  include "esp32s3_partition.h"
#endif

#ifdef CONFIG_RTC_DRIVER
#  include "espressif/esp_rtc.h"
#endif

#ifdef CONFIG_WATCHDOG
#  include "esp32s3_board_wdt.h"
#endif

#ifdef CONFIG_ESPRESSIF_WIFI
#  include "esp32s3_board_wlan.h"
#endif

#include "vmflow-esp32s3.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_bringup
 ****************************************************************************/

int esp32s3_bringup(void)
{
  int ret;

#ifdef CONFIG_ESPRESSIF_HR_TIMER
  ret = esp_hr_timer_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_hr_timer_init() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_EFUSE
  ret = esp_efuse_initialize("/dev/efuse");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init EFUSE: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at %s: %d\n",
             CONFIG_LIBC_TMPDIR, ret);
    }
#endif

#ifdef CONFIG_ESP32S3_SPIFLASH
  ret = board_spiflash_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SPI Flash\n");
    }
#endif

#ifdef CONFIG_ESP32S3_PARTITION_TABLE
  ret = esp32s3_partition_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize partition error=%d\n",
             ret);
    }
#endif

#ifdef CONFIG_RTC_DRIVER
  ret = esp_rtc_driverinit();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to Instantiate the RTC driver: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_WATCHDOG
  ret = board_wdt_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize watchdog timer: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_WIFI
  ret = board_wlan_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize wlan subsystem=%d\n",
             ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  ret = esp32s3_gpio_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize GPIO driver: %d\n", ret);
    }
#endif

  UNUSED(ret);
  return OK;
}
