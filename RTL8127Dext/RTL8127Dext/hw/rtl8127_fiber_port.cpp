/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtl8127_fiber_port.cpp -- compiles the kext fiber/SerDes layer unchanged
 * into the dext, on top of the DriverKit compat layer.
 */

#include "../compat/RTL8127HwCompat.h"

#include "../../../RTL812xLucy/rtl8127_fiber.cpp"

/*
 * Dext entry point to the static SerDes reset: on the RTL8127ATF it brings the
 * fiber link down (Linux r8169 does the same, r8127_sfp_sds_phy_reset(), when
 * the interface goes down). No-op on copper parts.
 */
void rtl8127DextSdsPhyReset(struct rtl8125_private *tp)
{
    rtl8127_sds_phy_reset(tp);
}
