/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8127Hw.h -- RTL8127 hardware bring-up context for the dext.
 *
 * Mirrors the member names of the kext RTL8125 class so that the methods
 * ported from RTL812xLucyHardware.cpp compile with minimal changes. The
 * heavy lifting (rtl812xx.cpp, rtl8127_fiber.cpp) is compiled verbatim on
 * top of compat/RTL8127HwCompat.h.
 */

#ifndef RTL8127Hw_h
#define RTL8127Hw_h

#include "../compat/RTL8127HwCompat.h"
#include "rtl8127_fiber.h"

#include <PCIDriverKit/PCIDriverKit.h>

struct IOEthernetAddress {
    UInt8 bytes[6];
};
#define kIOEthernetAddressSize 6

/* PCIe capability register offsets used by the ported code. */
#define kIOPCIELinkCapability   0x0c
#define kIOPCIELinkCapASPMCompl 0x00400000
#define kIOPCIELinkCapL0sSup    0x00000400
#define kIOPCIELinkCapL1Sup     0x00000800

/* State flag bits, identical to the kext. */
enum {
    __LINK_UP = 1,
    __PROMISC = 2,
    __M_CAST = 3,
    __POLL_MODE = 4,
    __POLLING = 5,
};

enum {
    __LINK_UP_M = (1 << __LINK_UP),
    __PROMISC_M = (1 << __PROMISC),
    __M_CAST_M = (1 << __M_CAST),
    __POLL_MODE_M = (1 << __POLL_MODE),
    __POLLING_M = (1 << __POLLING),
};

void random_buf(void *buf, size_t len);

/* Ring geometry, identical to the kext. */
#define kNumTxDesc 1024
#define kNumRxDesc 1024
#define kTxLastDesc (kNumTxDesc - 1)
#define kRxLastDesc (kNumRxDesc - 1)
#define kTxDescMask (kNumTxDesc - 1)
#define kRxDescMask (kNumRxDesc - 1)

/*
 * Adapter exposing the kext-style PCI config-space API on top of
 * PCIDriverKit, so ported code keeps its pciDevice-> call sites.
 */
class DextPciConfig
{
public:
    IOPCIDevice *dev;
    IOService   *owner;

    UInt8 extendedConfigRead8(UInt32 off)
    {
        uint8_t v = 0;
        dev->ConfigurationRead8(off, &v);
        return v;
    }

    UInt16 extendedConfigRead16(UInt32 off)
    {
        uint16_t v = 0;
        dev->ConfigurationRead16(off, &v);
        return v;
    }

    UInt32 extendedConfigRead32(UInt32 off)
    {
        uint32_t v = 0;
        dev->ConfigurationRead32(off, &v);
        return v;
    }

    void extendedConfigWrite8(UInt32 off, UInt8 v)  { dev->ConfigurationWrite8(off, v); }
    void extendedConfigWrite16(UInt32 off, UInt16 v) { dev->ConfigurationWrite16(off, v); }
    void extendedConfigWrite32(UInt32 off, UInt32 v) { dev->ConfigurationWrite32(off, v); }

    UInt16 configRead16(UInt32 off) { return extendedConfigRead16(off); }
    void configWrite16(UInt32 off, UInt16 v) { extendedConfigWrite16(off, v); }
};

class RTL8127Hw
{
public:
    struct rtl8125_private linuxData = {};
    DextPciConfig *pciDevice = nullptr;

    struct {
        UInt16 vendor;
        UInt16 device;
        UInt16 subsystem_vendor;
        UInt16 subsystem_device;
    } pciDeviceData = {};

    UInt32 mtu = 1500;
    UInt8 pcieCapOffset = 0;

    UInt32 intrMask = 0;
    UInt32 intrMaskRxTx = 0;
    UInt32 intrMaskTimer = 0;
    UInt32 intrMaskPoll = 0;

    UInt64 statPhyAddr = 0;
    UInt64 txPhyAddr = 0;
    UInt64 rxPhyAddr = 0;
    UInt32 txTailPtr0 = 0;
    UInt32 txClosePtr0 = 0;
    UInt32 txNextDescIndex = 0;
    UInt32 txDirtyDescIndex = 0;
    SInt32 txNumFreeDesc = 0;
    UInt32 rxNextDescIndex = 0;

    volatile unsigned int stateFlags = 0;
    UInt64 multicastFilter = 0;

    bool wolCapable = false;
    bool hasSensor = false;

    IOEthernetAddress currMacAddr = {};
    IOEthernetAddress origMacAddr = {};
    IOEthernetAddress fallBackMacAddr = {};

    /* Ported from RTL812xLucyHardware.cpp. */
    void setupASPM(bool allowL0s, bool allowL1);
    bool rtl812xIdentifyChip(struct rtl8125_private *tp);
    void initMacAddr(struct rtl8125_private *tp);
    bool rtl812xInit();
    void rtl812xEnable();
    void rtl812xDisable();
    void rtl812xHwInit(struct rtl8125_private *tp);
    void rtl812xHwConfig(struct rtl8125_private *tp);
    void rtl812xSetPhyMedium(struct rtl8125_private *tp, UInt8 autoneg, UInt32 speed, UInt8 duplex, UInt64 adv);
    void rtl812xLinkOnPatch(struct rtl8125_private *tp);
    void rtl812xLinkDownPatch(struct rtl8125_private *tp);
    void rtl812xGetEEEMode(struct rtl8125_private *tp);
    UInt32 rtl812xGetHwCloPtr(struct rtl8125_private *tp);
    void rtl812xDoorbell(struct rtl8125_private *tp, UInt32 txTailPtr);
    void rtl812xSetMrrs(struct rtl8125_private *tp, UInt8 setting);

    /* Dext-side helper replacing the kext's setMulticastMode(). */
    void applyRxMode();
};

#endif /* RTL8127Hw_h */
