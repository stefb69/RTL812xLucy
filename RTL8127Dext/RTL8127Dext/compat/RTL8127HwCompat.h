/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8127HwCompat.h -- compat prelude for compiling the kext hardware layer
 * (rtl812xx.cpp, rtl8127_fiber.cpp) unchanged in DriverKit userspace.
 *
 *  - compat/IOKit/IOLib.h (via HEADER_SEARCH_PATHS) maps the kernel APIs
 *    pulled by linux/linux.h onto DriverKit equivalents,
 *  - the RTL812xEthernet_hpp guard neutralises the kext class header that
 *    rtl812xx.cpp includes (only the C hardware layer is used),
 *  - DebugLog/feature defines mirror the kext prefix header.
 */

#ifndef RTL8127HwCompat_h
#define RTL8127HwCompat_h

#include "linux/linux.h"

#ifdef DEBUG
#define DebugLog(args...) IOLog(args)
#else
#define DebugLog(args...)
#endif

#define ENABLE_TX_NO_CLOSE

/*
 * 32-byte TX descriptors, as the kext prefix header enables and as the
 * RTL8127 requires (r8127 sets 0xEB58 BIT_0 unconditionally). The dext
 * writes RtlDextTxDesc in that layout; without this define the hardware
 * layer configures the chip for 16-byte descriptors, it consumes the first
 * descriptor and then reads the next one at the wrong offset (DescOwn
 * clear) and stalls -- observed on hardware as "tx done 1, hwclo 1".
 */
#define USE_NEW_TX_DESC

/* Neutralise the kext-only class header included by rtl812xx.cpp. */
#define RTL812xEthernet_hpp

#include "rtl812x_hw.h"
#include "rtl812xx.h"

#endif /* RTL8127HwCompat_h */
