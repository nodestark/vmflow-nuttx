/****************************************************************************
 * firmware/boards/vmflow-esp32s3/src/vmflow-esp32s3.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from boards/xtensa/esp32s3/esp32s3-devkit (Apache NuttX).
 *
 ****************************************************************************/

#ifndef __VMFLOW_ESP32S3_SRC_VMFLOW_ESP32S3_H
#define __VMFLOW_ESP32S3_SRC_VMFLOW_ESP32S3_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Name: esp32s3_bringup
 *
 * Description:
 *   Perform architecture-specific initialization. Called from
 *   board_late_initialize() (CONFIG_BOARD_LATE_INITIALIZE=y).
 *
 ****************************************************************************/

int esp32s3_bringup(void);

/****************************************************************************
 * Name: esp32s3_gpio_init
 *
 * Description:
 *   Register the board GPIOs (SIM7080G PWRKEY) with the GPIO driver.
 *
 ****************************************************************************/

#ifdef CONFIG_DEV_GPIO
int esp32s3_gpio_init(void);
#endif

/****************************************************************************
 * Name: board_mdb_initialize
 *
 * Description:
 *   Set up the bit-banged MDB bus pins and register /dev/mdb0.
 *
 ****************************************************************************/

#ifdef CONFIG_VMFLOW_MDB_DEVICE
int board_mdb_initialize(void);
#endif

/****************************************************************************
 * Name: board_spiflash_init
 *
 * Description:
 *   Initialize the SPI flash and register the MTD device.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPIFLASH
int board_spiflash_init(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VMFLOW_ESP32S3_SRC_VMFLOW_ESP32S3_H */
