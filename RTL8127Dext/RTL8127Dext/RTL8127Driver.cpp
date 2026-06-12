/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8127Driver.cpp -- DriverKit (dext) driver for the Realtek RTL8127
 * 10GbE PCIe family, including the RTL8127ATF fiber (SFP+) variant.
 *
 * Skeleton scope (milestone D1): match the device, open it through
 * PCIDriverKit, locate the MMIO BAR, identify the chip from TxConfig,
 * detect the ATF fiber mode (MAC-OCP 0xD006) and read the MAC address.
 * The NetworkingDriverKit datapath (packet pool, queues, interrupts)
 * comes in the next milestones -- see docs/DEXT-PORT.md.
 */

#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSCollections.h>
#include <PCIDriverKit/PCIDriverKit.h>

#include "RTL8127Driver.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "RTL8127Dext: " fmt "\n", ##__VA_ARGS__)

/* Register offsets shared with the kext (see RTL812xLucy/rtl812xx.h). */
enum {
    kRegMAC0       = 0x0000,
    kRegTxConfig   = 0x0040,
    kRegMACOCPData = 0x00B0,    /* MACOCP: 32-bit, addr in upper half */
};

/* TxConfig & 0x7C800000 signature for the RTL8127 family. */
enum {
    kTxConfigVersionMask = 0x7C800000,
    kTxConfigICVerIDMask = 0x00700000,
    kSigRTL8127          = 0x6C800000,
};

struct RTL8127Driver_IVars {
    IOPCIDevice *pciDevice;
    uint8_t      barIndex;      /* memory index of the MMIO BAR */
    bool         opened;
    bool         fiberMode;     /* RTL8127ATF detected */
    uint8_t      macAddr[6];
};

bool RTL8127Driver::init()
{
    if (!super::init())
        return false;

    ivars = IONewZero(RTL8127Driver_IVars, 1);

    return (ivars != nullptr);
}

void RTL8127Driver::free()
{
    IOSafeDeleteNULL(ivars, RTL8127Driver_IVars, 1);
    super::free();
}

/*
 * MAC-OCP read, same protocol as rtl8125_mac_ocp_read() in the kext:
 * write the (word) address to the high half of the MACOCP register,
 * read back the data in the low half.
 */
static uint16_t macOCPRead(RTL8127Driver_IVars *ivars, uint16_t addr)
{
    uint32_t value = 0;

    ivars->pciDevice->MemoryWrite32(ivars->barIndex, kRegMACOCPData,
                                    ((uint32_t)(addr / 2)) << 16);
    ivars->pciDevice->MemoryRead32(ivars->barIndex, kRegMACOCPData, &value);

    return (uint16_t)(value & 0xFFFF);
}

kern_return_t IMPL(RTL8127Driver, Start)
{
    kern_return_t ret;
    uint32_t txConfig = 0;
    uint16_t fiberReg;
    uint16_t cmd;
    int bar;

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("super::Start failed: 0x%x", ret);
        return ret;
    }

    ivars->pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!ivars->pciDevice) {
        Log("provider is not an IOPCIDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    ret = ivars->pciDevice->Open(this, 0);
    if (ret != kIOReturnSuccess) {
        Log("failed to open PCI device: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }
    ivars->opened = true;

    /* Enable memory space and bus mastering. */
    ivars->pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    cmd |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
    ivars->pciDevice->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, cmd);

    /*
     * Locate the MMIO BAR. On the RTL812x family BAR0 is I/O space and the
     * 64-bit MMIO window is BAR2, but the DriverKit memory index depends on
     * how the kernel enumerated the ranges, so probe each index for the
     * RTL8127 TxConfig signature instead of hardcoding one.
     */
    ivars->barIndex = 0xFF;
    for (bar = 0; bar < 6; bar++) {
        txConfig = 0;
        ivars->pciDevice->MemoryRead32((uint8_t)bar, kRegTxConfig, &txConfig);
        if ((txConfig & kTxConfigVersionMask) == kSigRTL8127) {
            ivars->barIndex = (uint8_t)bar;
            break;
        }
    }
    if (ivars->barIndex == 0xFF) {
        Log("no BAR with an RTL8127 TxConfig signature found");
        goto fail;
    }
    Log("RTL8127 found: BAR index %u, TxConfig 0x%08x, ICVerID 0x%x",
        ivars->barIndex, txConfig, (txConfig & kTxConfigICVerIDMask) >> 20);

    /*
     * ATF fiber detection, same check as rtl8127_check_fiber_mode_support():
     * production chips (ICVerID 0x1) report 0x07 in MAC-OCP 0xD006.
     */
    fiberReg = macOCPRead(ivars, 0xD006);
    ivars->fiberMode = ((fiberReg & 0xFF) == 0x07);
    Log("MAC-OCP 0xD006 = 0x%04x -> %s", fiberReg,
        ivars->fiberMode ? "RTL8127ATF fiber mode" : "copper/NIC mode");

    /* Read the MAC address straight from the MAC0 registers. */
    for (int i = 0; i < 6; i += 4) {
        uint32_t v = 0;
        ivars->pciDevice->MemoryRead32(ivars->barIndex, kRegMAC0 + i, &v);
        for (int j = 0; j < 4 && (i + j) < 6; j++)
            ivars->macAddr[i + j] = (uint8_t)(v >> (8 * j));
    }
    Log("MAC address %02x:%02x:%02x:%02x:%02x:%02x",
        ivars->macAddr[0], ivars->macAddr[1], ivars->macAddr[2],
        ivars->macAddr[3], ivars->macAddr[4], ivars->macAddr[5]);

    /*
     * TODO milestone D2 (datapath bring-up; port from the kext):
     *  - hw_init / exit_oob / hw_config         -> rtl812xx.cpp CFG_METHOD_41/42
     *  - PHY MCU ram code + EPHY config         -> rtl812xx.cpp (tables 8127a_*)
     *  - fiber SerDes link (10G/1G forced)      -> rtl8127_fiber.cpp
     *  - descriptor rings via IOBufferMemoryDescriptor + PCI DMA
     *  - MSI interrupt via IOInterruptDispatchSource
     *  - IOUserNetworkPacketBufferPool::CreateWithOptions + Tx/Rx
     *    submission/completion queues, then RegisterEthernetInterface()
     *    with ivars->macAddr and reportLinkStatus() from the SerDes state.
     *
     * Until then the driver only probes and logs, and does not register a
     * network interface.
     */

    RegisterService();

    return kIOReturnSuccess;

fail:
    if (ivars->opened) {
        ivars->pciDevice->Close(this, 0);
        ivars->opened = false;
    }
    Stop(provider, SUPERDISPATCH);
    return kIOReturnNoDevice;
}

kern_return_t IMPL(RTL8127Driver, Stop)
{
    if (ivars && ivars->opened && ivars->pciDevice) {
        ivars->pciDevice->Close(this, 0);
        ivars->opened = false;
    }
    return Stop(provider, SUPERDISPATCH);
}

#pragma mark - IOUserNetworkEthernet abstract interface

kern_return_t IMPL(RTL8127Driver, SetInterfaceEnable)
{
    Log("SetInterfaceEnable(%d)", isEnable);
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, SetPromiscuousModeEnable)
{
    /* TODO D2: RxConfig AcceptAllPhys, see kext setMulticastMode(). */
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, SetMulticastAddresses)
{
    /* TODO D2: MAR0/MAR4 hash filter, see kext setMulticastList(). */
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, SetAllMulticastModeEnable)
{
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, SelectMediaType)
{
    /* TODO D3: map to the fiber 10G/1G forced modes (rtl8127_fiber). */
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, SetWakeOnMagicPacketEnable)
{
    return kIOReturnUnsupported;
}

kern_return_t IMPL(RTL8127Driver, SetMTU)
{
    /* v1: standard frames only, like the kext default. */
    return (mtu <= 1500) ? kIOReturnSuccess : kIOReturnUnsupported;
}

kern_return_t IMPL(RTL8127Driver, GetMaxTransferUnit)
{
    if (!mtu)
        return kIOReturnBadArgument;
    *mtu = 1500;
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, SetHardwareAssists)
{
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, GetHardwareAssists)
{
    if (!hardwareAssists)
        return kIOReturnBadArgument;
    *hardwareAssists = 0;
    return kIOReturnSuccess;
}
