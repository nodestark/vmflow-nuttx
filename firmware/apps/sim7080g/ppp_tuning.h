/****************************************************************************
 * firmware/apps/sim7080g/ppp_tuning.h
 *
 * Force-included into the apps/netutils/pppd build right after its
 * ppp_conf.h (see Make.defs), so pppd constants can be changed without
 * patching the submodule.
 *
 ****************************************************************************/

#ifndef __FIRMWARE_APPS_SIM7080G_PPP_TUNING_H
#define __FIRMWARE_APPS_SIM7080G_PPP_TUNING_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* pppd drops the link once it has sent this many frames in a row without
 * receiving one. Upstream uses 5, which a few back-to-back TCP segments
 * hit long before the reply crosses an LTE-M/NB-IoT round trip.
 */

#undef  AHDLC_TX_OFFLINE
#define AHDLC_TX_OFFLINE  CONFIG_VMFLOW_SIM7080G_PPP_TX_OFFLINE

#endif /* __FIRMWARE_APPS_SIM7080G_PPP_TUNING_H */
