/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8127Driver.cpp -- DriverKit (dext) driver for the Realtek RTL8127
 * 10GbE PCIe family, including the RTL8127ATF fiber (SFP+) variant.
 *
 * Milestones D2-D5 (see docs/DEXT-PORT.md): full hardware bring-up through
 * the verbatim-compiled kext hardware layer (hw/RTL8127Hw.*), fiber SerDes
 * link with media reporting, NetworkingDriverKit datapath (packet pool +
 * submission/completion queues over the legacy descriptor rings), MSI
 * interrupt, checksum offloads.
 *
 * Datapath layout: all hardware mutation runs on the default dispatch
 * queue (interrupt handler, enable/disable). The NDK queue actions run on
 * framework threads and exchange packets with the interrupt side through
 * single-producer/single-consumer rings (txDoneFifo, rxDoneFifo,
 * rxFreeFifo) plus atomic counters, mirroring the lock layout that the
 * kext validated on real hardware.
 */

#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/OSCollections.h>
#include <PCIDriverKit/PCIDriverKit.h>
#include <NetworkingDriverKit/IOUserNetworkEthernet.h>
#include <NetworkingDriverKit/IOUserNetworkPacket.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkPacketQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>

/*
 * rtl812xx.h's LinkStatus enumerator collides with the NDK LinkStatus
 * type; rename it for this translation unit only.
 */
#define LinkStatus RtlHwLinkStatus
#include "hw/RTL8127Hw.h"
#undef LinkStatus

#include "RTL8127Driver.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "RTL8127Dext: " fmt "\n", ##__VA_ARGS__)

/* Descriptors, identical to the kext layouts (USE_NEW_TX_DESC). */
struct RtlDextTxDesc {
    UInt32 opts1;
    UInt32 opts2;
    UInt64 addr;
    UInt32 reserved[4];
};

struct RtlDextRxDesc {
    UInt32 opts1;
    UInt32 opts2;
    UInt64 addr;
};

#define kTxDescSize (kNumTxDesc * sizeof(struct RtlDextTxDesc))
#define kRxDescSize (kNumRxDesc * sizeof(struct RtlDextRxDesc))
#define kStatSize   PAGE_SIZE

#define kRxBufferSize   2048
#define kPoolPackets    (kNumTxDesc + kNumRxDesc + 512)
#define kFifoCapacity   2048    /* power of two, > ring size */
#define kFifoMask       (kFifoCapacity - 1)

/* Lock-free single-producer/single-consumer packet FIFO. */
struct PacketFifo {
    IOUserNetworkPacket * volatile slots[kFifoCapacity];
    volatile UInt32 head;   /* consumer index */
    volatile UInt32 tail;   /* producer index */
};

static inline bool fifoPush(PacketFifo *f, IOUserNetworkPacket *p)
{
    UInt32 tail = f->tail;
    UInt32 next = (tail + 1) & kFifoMask;

    if (next == __atomic_load_n(&f->head, __ATOMIC_ACQUIRE))
        return false;

    f->slots[tail] = p;
    __atomic_store_n(&f->tail, next, __ATOMIC_RELEASE);
    return true;
}

static inline IOUserNetworkPacket *fifoPop(PacketFifo *f)
{
    UInt32 head = f->head;

    if (head == __atomic_load_n(&f->tail, __ATOMIC_ACQUIRE))
        return nullptr;

    IOUserNetworkPacket *p = f->slots[head];
    __atomic_store_n(&f->head, (head + 1) & kFifoMask, __ATOMIC_RELEASE);
    return p;
}

struct RTL8127Driver_IVars {
    IOPCIDevice *pciDevice;
    DextPciConfig cfg;
    RTL8127Hw *hw;

    bool opened;
    bool fiberMode;
    bool interfaceEnabled;
    bool linkUp;

    IOMemoryDescriptor *barMd;
    IOMemoryMap *barMap;
    uint8_t barIndex;

    IODispatchQueue *workQueue;
    IOInterruptDispatchSource *intSource;
    OSAction *intAction;

    /* Descriptor rings and stats block. */
    IOBufferMemoryDescriptor *txRingMd;
    IOBufferMemoryDescriptor *rxRingMd;
    IOBufferMemoryDescriptor *statMd;
    IODMACommand *txRingDma;
    IODMACommand *rxRingDma;
    IODMACommand *statDma;
    volatile RtlDextTxDesc *txRing;
    volatile RtlDextRxDesc *rxRing;

    /* NDK objects. */
    IOUserNetworkPacketBufferPool *pool;
    IOUserNetworkTxSubmissionQueue *txSubQueue;
    IOUserNetworkTxCompletionQueue *txCompQueue;
    IOUserNetworkRxSubmissionQueue *rxSubQueue;
    IOUserNetworkRxCompletionQueue *rxCompQueue;

    /* Per-slot packets owned by the hardware rings. */
    IOUserNetworkPacket *txPkt[kNumTxDesc];
    IOUserNetworkPacket *rxPkt[kNumRxDesc];

    /* Producer/consumer hand-off between interrupt and queue actions. */
    PacketFifo txDoneFifo;
    PacketFifo rxDoneFifo;
    PacketFifo rxFreeFifo;

    uint32_t mediaArray[8];
    uint32_t mediaCount;
    uint32_t currentMedia;
    uint32_t mtu;
};

bool RTL8127Driver::init()
{
    if (!super::init())
        return false;

    ivars = IONewZero(RTL8127Driver_IVars, 1);
    if (!ivars)
        return false;

    ivars->hw = IONewZero(RTL8127Hw, 1);
    if (!ivars->hw)
        return false;

    ivars->mtu = 1500;
    return true;
}

void RTL8127Driver::free()
{
    if (ivars)
        IOSafeDeleteNULL(ivars->hw, RTL8127Hw, 1);

    IOSafeDeleteNULL(ivars, RTL8127Driver_IVars, 1);
    super::free();
}

#pragma mark - DMA helpers

/*
 * Allocate a host buffer and map it for device DMA, returning the device
 * (DART) address. All device-visible memory must be mapped this way --
 * lesson learned the hard way in the kext bring-up.
 */
static kern_return_t allocDmaBuffer(RTL8127Driver *owner,
                                    IOPCIDevice *pci,
                                    uint64_t size,
                                    IOBufferMemoryDescriptor **outMd,
                                    IODMACommand **outCmd,
                                    void **outVirt,
                                    uint64_t *outIova)
{
    IOBufferMemoryDescriptor *md = nullptr;
    IODMACommand *cmd = nullptr;
    IOAddressSegment seg = {};
    IODMACommandSpecification spec = {};
    uint64_t flags = 0;
    uint32_t segCount = 1;
    kern_return_t ret;

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, size, 256, &md);
    if (ret != kIOReturnSuccess)
        return ret;

    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;

    ret = IODMACommand::Create(pci, kIODMACommandCreateNoOptions, &spec, &cmd);
    if (ret != kIOReturnSuccess) {
        md->release();
        return ret;
    }

    ret = cmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, md, 0, size,
                             &flags, &segCount, &seg);
    if (ret != kIOReturnSuccess || segCount != 1) {
        cmd->release();
        md->release();
        return (ret != kIOReturnSuccess) ? ret : kIOReturnNoResources;
    }

    IOAddressSegment range = {};
    md->GetAddressRange(&range);

    *outMd = md;
    *outCmd = cmd;
    *outVirt = (void *)range.address;
    *outIova = seg.address;
    return kIOReturnSuccess;
}

static void freeDmaBuffer(IOBufferMemoryDescriptor **md, IODMACommand **cmd)
{
    if (*cmd) {
        (*cmd)->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        (*cmd)->release();
        *cmd = nullptr;
    }
    if (*md) {
        (*md)->release();
        *md = nullptr;
    }
}

#pragma mark - NDK queue actions

/*
 * TX submission: the framework hands us packets to transmit. Runs on a
 * framework thread; only touches producer-side TX state.
 */
static uint32_t txDequeueAction(OSObject *target,
                                IOUserNetworkPacketQueue *queue,
                                IOUserNetworkPacket **packets,
                                uint32_t packetCount,
                                void *refCon)
{
    RTL8127Driver *driver = (RTL8127Driver *)target;
    RTL8127Driver_IVars *iv = driver->ivars;
    RTL8127Hw *hw = iv->hw;
    struct rtl8125_private *tp = &hw->linuxData;
    uint32_t accepted = 0;

    if (!iv->linkUp)
        return 0;

    for (uint32_t i = 0; i < packetCount; i++) {
        IOUserNetworkPacket *pkt = packets[i];
        uint32_t len = pkt->getDataLength();
        uint64_t iova = pkt->getDataIOVirtualAddress();
        uint32_t opts1, opts2 = 0;
        uint32_t index;

        if (__atomic_load_n(&hw->txNumFreeDesc, __ATOMIC_ACQUIRE) <= 2)
            break;

        /* Checksum offload, same opts2 bits as the kext outputStart(). */
        IOUserNetworkPacketTxChecksumFlags csum = 0;
        uint16_t start = 0, stuff = 0;
        pkt->getTxChecksumInfo(&csum, &start, &stuff);

        if (csum & kIOUserNetworkPacketTxCsumTCPIPV4)
            opts2 = (TxIPCS_C | TxTCPCS_C);
        else if (csum & kIOUserNetworkPacketTxCsumUDPIPV4)
            opts2 = (TxIPCS_C | TxUDPCS_C);
        else if (csum & kIOUserNetworkPacketTxCsumTCPIPV6)
            opts2 = (TxTCPCS_C | TxIPV6F_C | ((14 + 40) << TCPHO_SHIFT));
        else if (csum & kIOUserNetworkPacketTxCsumUDPIPV6)
            opts2 = (TxUDPCS_C | TxIPV6F_C | ((14 + 40) << TCPHO_SHIFT));
        else if (csum & kIOUserNetworkPacketTxCsumIPHdr)
            opts2 = TxIPCS_C;

        index = hw->txNextDescIndex;
        volatile RtlDextTxDesc *desc = &iv->txRing[index];

        opts1 = (len | FirstFrag | LastFrag);
        if (index == kTxLastDesc)
            opts1 |= RingEnd;

        iv->txPkt[index] = pkt;
        desc->addr = OSSwapHostToLittleInt64(iova);
        desc->opts2 = OSSwapHostToLittleInt32(opts2);
        desc->opts1 = OSSwapHostToLittleInt32(opts1 | DescOwn);

        hw->txNextDescIndex = (index + 1) & kTxDescMask;
        hw->txTailPtr0++;
        OSAddAtomic(-1, &hw->txNumFreeDesc);
        accepted++;
    }

    if (accepted) {
        /* Order descriptor stores before the doorbell MMIO write. */
        wmb();
        hw->rtl812xDoorbell(tp, hw->txTailPtr0);
    }
    return accepted;
}

static uint32_t txQueryFreeSpace(OSObject *target,
                                 IOUserNetworkPacketQueue *queue,
                                 uint32_t *freeSpaceBytes)
{
    RTL8127Driver *driver = (RTL8127Driver *)target;
    RTL8127Hw *hw = driver->ivars->hw;
    SInt32 freeDesc = __atomic_load_n(&hw->txNumFreeDesc, __ATOMIC_ACQUIRE);

    if (freeDesc < 0)
        freeDesc = 0;

    if (freeSpaceBytes)
        *freeSpaceBytes = (uint32_t)freeDesc * kRxBufferSize;

    return (uint32_t)freeDesc;
}

/* TX completion: the framework collects packets we have transmitted. */
static uint32_t txEnqueueAction(OSObject *target,
                                IOUserNetworkPacketQueue *queue,
                                IOUserNetworkPacket *packets[],
                                uint32_t arrayCapacity,
                                void *refCon)
{
    RTL8127Driver *driver = (RTL8127Driver *)target;
    RTL8127Driver_IVars *iv = driver->ivars;
    uint32_t count = 0;

    while (count < arrayCapacity) {
        IOUserNetworkPacket *pkt = fifoPop(&iv->txDoneFifo);
        if (!pkt)
            break;
        packets[count++] = pkt;
    }
    return count;
}

/* RX submission: the framework hands us empty packets for the RX ring. */
static uint32_t rxDequeueAction(OSObject *target,
                                IOUserNetworkPacketQueue *queue,
                                IOUserNetworkPacket **packets,
                                uint32_t packetCount,
                                void *refCon)
{
    RTL8127Driver *driver = (RTL8127Driver *)target;
    RTL8127Driver_IVars *iv = driver->ivars;
    uint32_t accepted = 0;

    while (accepted < packetCount) {
        if (!fifoPush(&iv->rxFreeFifo, packets[accepted]))
            break;
        accepted++;
    }
    return accepted;
}

/* RX completion: the framework collects packets we have received. */
static uint32_t rxEnqueueAction(OSObject *target,
                                IOUserNetworkPacketQueue *queue,
                                IOUserNetworkPacket *packets[],
                                uint32_t arrayCapacity,
                                void *refCon)
{
    RTL8127Driver *driver = (RTL8127Driver *)target;
    RTL8127Driver_IVars *iv = driver->ivars;
    uint32_t count = 0;

    while (count < arrayCapacity) {
        IOUserNetworkPacket *pkt = fifoPop(&iv->rxDoneFifo);
        if (!pkt)
            break;
        packets[count++] = pkt;
    }
    return count;
}

#pragma mark - ring management (work queue context)

static bool rxRingRefill(RTL8127Driver_IVars *iv)
{
    RTL8127Hw *hw = iv->hw;
    bool complete = true;

    for (uint32_t i = 0; i < kNumRxDesc; i++) {
        uint64_t word1;

        if (iv->rxPkt[i])
            continue;

        IOUserNetworkPacket *pkt = fifoPop(&iv->rxFreeFifo);
        if (!pkt && iv->pool) {
            if (iv->pool->allocatePacket(&pkt) != kIOReturnSuccess)
                pkt = nullptr;
        }
        if (!pkt) {
            complete = false;
            break;
        }
        pkt->prepareWithQueue(iv->rxSubQueue, kIOUserNetworkPacketDirectionRx);
        iv->rxPkt[i] = pkt;

        word1 = (kRxBufferSize | DescOwn);
        if (i == kRxLastDesc)
            word1 |= RingEnd;

        iv->rxRing[i].addr = OSSwapHostToLittleInt64(pkt->getDataIOVirtualAddress());
        iv->rxRing[i].opts2 = 0;
        iv->rxRing[i].opts1 = OSSwapHostToLittleInt32((uint32_t)word1);
    }
    wmb();
    return complete;
}

static void txRingReclaim(RTL8127Driver *driver, bool abort)
{
    RTL8127Driver_IVars *iv = driver->ivars;
    RTL8127Hw *hw = iv->hw;
    struct rtl8125_private *tp = &hw->linuxData;
    uint32_t nextClose = abort ? hw->txTailPtr0 : hw->rtl812xGetHwCloPtr(tp);
    uint32_t numDone = (nextClose - hw->txClosePtr0) & tp->MaxTxDescPtrMask;
    bool didWork = false;

    hw->txClosePtr0 = nextClose;

    while (numDone-- > 0) {
        uint32_t index = hw->txDirtyDescIndex;
        IOUserNetworkPacket *pkt = iv->txPkt[index];

        iv->txPkt[index] = nullptr;
        if (pkt) {
            pkt->setCompletionStatus(abort ? kIOReturnAborted : kIOReturnSuccess);
            if (!fifoPush(&iv->txDoneFifo, pkt))
                iv->pool->deallocatePacket(pkt);
            didWork = true;
        }
        OSAddAtomic(1, &hw->txNumFreeDesc);
        hw->txDirtyDescIndex = (index + 1) & kTxDescMask;
    }
    if (didWork && iv->txCompQueue) {
        iv->txCompQueue->requestEnqueue();
        if (iv->txSubQueue)
            iv->txSubQueue->requestDequeue();
    }
}

static void rxRingService(RTL8127Driver *driver)
{
    RTL8127Driver_IVars *iv = driver->ivars;
    RTL8127Hw *hw = iv->hw;
    bool delivered = false;

    for (uint32_t budget = 0; budget < kNumRxDesc; budget++) {
        uint32_t index = hw->rxNextDescIndex;
        volatile RtlDextRxDesc *desc = &iv->rxRing[index];
        uint32_t status = OSSwapLittleToHostInt32(desc->opts1);
        IOUserNetworkPacket *pkt;
        int32_t length;

        if (status & DescOwn)
            break;

        pkt = iv->rxPkt[index];
        if (!pkt)
            break;

        length = (int32_t)(status & 0x1fff) - 4 /* FCS */;

        if ((status & RxRES) || length <= 0 ||
            !(status & FirstFrag) || !(status & LastFrag)) {
            /* Error or fragmented jumbo: recycle the buffer in place. */
            uint64_t word1 = (kRxBufferSize | DescOwn);
            if (index == kRxLastDesc)
                word1 |= RingEnd;
            wmb();
            desc->opts1 = OSSwapHostToLittleInt32((uint32_t)word1);
        } else {
            uint32_t opts2 = OSSwapLittleToHostInt32(desc->opts2);

            iv->rxPkt[index] = nullptr;
            pkt->setDataLength((uint32_t)length);

            /* RX checksum offload result, same bits as the kext. */
            IOUserNetworkPacketRxChecksumFlags csum = 0;
            if (opts2 & RxV4F) {
                csum |= kIOUserNetworkPacketRxCsumIPChecked;
                if (!(opts2 & RxIPF))
                    csum |= kIOUserNetworkPacketRxCsumIPValid;
            }
            if (((opts2 & RxTCPT) && !(opts2 & RxTCPF)) ||
                ((opts2 & RxUDPT) && !(opts2 & RxUDPF)))
                csum |= (kIOUserNetworkPacketRxCsumDataValid | kIOUserNetworkPacketRxCsumPseudoHdr);
            if (csum)
                pkt->setRxChecksumInfo(csum, 0xffff);

            if (!fifoPush(&iv->rxDoneFifo, pkt))
                iv->pool->deallocatePacket(pkt);
            delivered = true;
        }
        hw->rxNextDescIndex = (index + 1) & kRxDescMask;
    }

    rxRingRefill(iv);

    if (delivered && iv->rxCompQueue) {
        iv->rxCompQueue->requestEnqueue();
        if (iv->rxSubQueue)
            iv->rxSubQueue->requestDequeue();
    }
}

#pragma mark - link management (work queue context)

static void updateLinkStatus(RTL8127Driver *driver)
{
    RTL8127Driver_IVars *iv = driver->ivars;
    RTL8127Hw *hw = iv->hw;
    struct rtl8125_private *tp = &hw->linuxData;
    UInt32 status = RTL_R32(tp, PHYstatus);

    if ((status != 0xffffffff) && (status & RtlHwLinkStatus)) {
        uint32_t media;

        if (status & _10000bpsF)
            media = iv->fiberMode ? kIOUserNetworkMediaEthernet10GBaseSR
                                  : kIOUserNetworkMediaEthernet10GBaseT;
        else if (status & (_5000bpsF | _10000bpsL))
            media = kIOUserNetworkMediaEthernet5000BaseT;
        else if (status & (_2500bpsF | _5000bpsL))
            media = kIOUserNetworkMediaEthernet2500BaseT;
        else if (status & (_1000bpsF | _2500bpsL | _1000bpsL))
            media = iv->fiberMode ? kIOUserNetworkMediaEthernet1000BaseSX
                                  : kIOUserNetworkMediaEthernet1000BaseT;
        else if (status & _100bps)
            media = kIOUserNetworkMediaEthernet100BaseTX;
        else
            media = kIOUserNetworkMediaEthernet10BaseT;

        hw->rtl812xLinkOnPatch(tp);

        /* Restart the datapath. */
        RTL_W8(tp, ChipCmd, CmdTxEnb | CmdRxEnb);
        set_bit(__LINK_UP, &hw->stateFlags);

        if (!iv->linkUp || media != iv->currentMedia) {
            iv->linkUp = true;
            iv->currentMedia = media;
            driver->reportLinkStatus(kIOUserNetworkLinkStatusActive, media);
            Log("link up, media 0x%08x", media);
        }
    } else if (iv->linkUp) {
        iv->linkUp = false;
        clear_bit(__LINK_UP, &hw->stateFlags);

        hw->rtl812xLinkDownPatch(tp);

        /* Return all in-flight tx packets and reset the rings. */
        txRingReclaim(driver, true);
        hw->txTailPtr0 = hw->txClosePtr0 = 0;
        hw->txNextDescIndex = hw->txDirtyDescIndex = 0;
        hw->txNumFreeDesc = kNumTxDesc;

        driver->reportLinkStatus(kIOUserNetworkLinkStatusInactive, iv->currentMedia);
        Log("link down");

        hw->rtl812xSetPhyMedium(tp, tp->autoneg, tp->speed, tp->duplex, tp->advertising);
    }
}

#pragma mark - interrupt

void IMPL(RTL8127Driver, InterruptOccurred)
{
    RTL8127Driver_IVars *iv = ivars;
    RTL8127Hw *hw = iv->hw;
    struct rtl8125_private *tp = &hw->linuxData;
    UInt32 status = RTL_R32(tp, ISR0_8125);

    if ((status == 0xFFFFFFFF) || !status)
        return;

    /* Ack. */
    RTL_W32(tp, ISR0_8125, status);

    if (iv->interfaceEnabled) {
        if (status & (RxOK | RxDescUnavail))
            rxRingService(this);

        if (status & (TxOK | RxOK | PCSTimeout))
            txRingReclaim(this, false);
    }

    if (status & LinkChg)
        updateLinkStatus(this);

    RTL_W32(tp, IMR0_8125, hw->intrMask);
}

#pragma mark - Start / Stop

kern_return_t IMPL(RTL8127Driver, Start)
{
    kern_return_t ret;
    uint32_t txConfig = 0;
    uint16_t fiberReg;
    uint16_t cmd;
    uint64_t barVirt = 0;
    uint64_t iova = 0;
    void *virt = nullptr;
    RTL8127Hw *hw = ivars->hw;
    struct rtl8125_private *tp = &hw->linuxData;
    int bar;

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("super::Start failed: 0x%x", ret);
        return ret;
    }

    ivars->pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!ivars->pciDevice) {
        Log("provider is not an IOPCIDevice");
        goto fail;
    }

    ret = ivars->pciDevice->Open(this, 0);
    if (ret != kIOReturnSuccess) {
        Log("failed to open PCI device: 0x%x", ret);
        goto fail;
    }
    ivars->opened = true;

    ivars->cfg.dev = ivars->pciDevice;
    ivars->cfg.owner = this;
    hw->pciDevice = &ivars->cfg;

    /* Enable memory space and bus mastering. */
    ivars->pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    cmd |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
    ivars->pciDevice->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, cmd);

    hw->pciDeviceData.vendor = ivars->cfg.extendedConfigRead16(kIOPCIConfigurationOffsetVendorID);
    hw->pciDeviceData.device = ivars->cfg.extendedConfigRead16(kIOPCIConfigurationOffsetDeviceID);
    hw->pciDeviceData.subsystem_vendor = ivars->cfg.extendedConfigRead16(kIOPCIConfigurationOffsetSubSystemVendorID);
    hw->pciDeviceData.subsystem_device = ivars->cfg.extendedConfigRead16(kIOPCIConfigurationOffsetSubSystemID);

    /* Locate the PCIe capability for the ASPM setup. */
    {
        uint8_t capPtr = ivars->cfg.extendedConfigRead8(kIOPCIConfigurationOffsetCapabilitiesPtr);
        int guard = 0;

        while (capPtr && guard++ < 16) {
            uint8_t capId = ivars->cfg.extendedConfigRead8(capPtr);
            if (capId == 0x10) {        /* PCI Express capability */
                hw->pcieCapOffset = capPtr;
                break;
            }
            capPtr = ivars->cfg.extendedConfigRead8(capPtr + 1);
        }
    }

    /*
     * Locate the MMIO BAR, then map it into this process: the ported
     * hardware layer accesses registers through tp->mmio_addr. We don't
     * pin a specific chip signature here (this dext is generic across the
     * RTL812x family) -- any BAR that reads back a plausible TxConfig is
     * accepted, and rtl812xIdentifyChip() decides the exact chip below.
     */
    ivars->barIndex = 0xFF;
    for (bar = 0; bar < 6; bar++) {
        txConfig = 0;
        ivars->pciDevice->MemoryRead32((uint8_t)bar, 0x40 /* TxConfig */, &txConfig);
        if (txConfig != 0 && txConfig != 0xFFFFFFFF) {
            ivars->barIndex = (uint8_t)bar;
            break;
        }
    }
    if (ivars->barIndex == 0xFF) {
        Log("no MMIO BAR with a readable TxConfig found");
        goto fail;
    }

    ret = ivars->pciDevice->_CopyDeviceMemoryWithIndex(ivars->barIndex, &ivars->barMd, this);
    if (ret != kIOReturnSuccess) {
        Log("_CopyDeviceMemoryWithIndex failed: 0x%x", ret);
        goto fail;
    }
    ret = ivars->barMd->CreateMapping(0, 0, 0, 0, 0, &ivars->barMap);
    if (ret != kIOReturnSuccess) {
        Log("BAR CreateMapping failed: 0x%x", ret);
        goto fail;
    }
    barVirt = ivars->barMap->GetAddress();
    tp->mmio_addr = (volatile void *)barVirt;

    Log("RTL8127: BAR index %u mapped, TxConfig 0x%08x", ivars->barIndex, txConfig);

    /* Full chip identification + bring-up, same path as the kext. */
    if (!hw->rtl812xInit()) {
        Log("rtl812xInit() failed (unknown chip?)");
        goto fail;
    }

    /* Fiber (SFP+) only exists on the RTL8127ATF; the 0xD006 == 0x07 probe
     * is meaningful only for CFG_METHOD_41/42. Other chips are copper. */
    if (tp->mcfg == CFG_METHOD_41 || tp->mcfg == CFG_METHOD_42) {
        fiberReg = rtl8125_mac_ocp_read(tp, 0xD006);
        ivars->fiberMode = ((fiberReg & 0xFF) == 0x07);
    } else {
        fiberReg = 0;
        ivars->fiberMode = false;
    }
    Log("chip mcfg %u, maxSpeed %u, MAC-OCP 0xD006=0x%04x -> %s", tp->mcfg,
        (unsigned int)tp->HwSuppMaxPhyLinkSpeed, fiberReg,
        ivars->fiberMode ? "RTL8127ATF fiber mode" : "copper/NIC mode");
    Log("MAC address %02x:%02x:%02x:%02x:%02x:%02x",
        hw->currMacAddr.bytes[0], hw->currMacAddr.bytes[1], hw->currMacAddr.bytes[2],
        hw->currMacAddr.bytes[3], hw->currMacAddr.bytes[4], hw->currMacAddr.bytes[5]);

    /* Descriptor rings + tally block, DMA-mapped for the device. */
    ret = allocDmaBuffer(this, ivars->pciDevice, kTxDescSize,
                         &ivars->txRingMd, &ivars->txRingDma, &virt, &iova);
    if (ret != kIOReturnSuccess) {
        Log("tx ring alloc failed: 0x%x", ret);
        goto fail;
    }
    ivars->txRing = (volatile RtlDextTxDesc *)virt;
    hw->txPhyAddr = iova;

    ret = allocDmaBuffer(this, ivars->pciDevice, kRxDescSize,
                         &ivars->rxRingMd, &ivars->rxRingDma, &virt, &iova);
    if (ret != kIOReturnSuccess) {
        Log("rx ring alloc failed: 0x%x", ret);
        goto fail;
    }
    ivars->rxRing = (volatile RtlDextRxDesc *)virt;
    hw->rxPhyAddr = iova;

    ret = allocDmaBuffer(this, ivars->pciDevice, kStatSize,
                         &ivars->statMd, &ivars->statDma, &virt, &iova);
    if (ret != kIOReturnSuccess) {
        Log("stats alloc failed: 0x%x", ret);
        goto fail;
    }
    hw->statPhyAddr = iova;

    memset((void *)ivars->txRing, 0, kTxDescSize);
    memset((void *)ivars->rxRing, 0, kRxDescSize);

    /* Dispatch queue + MSI interrupt. */
    ret = IODispatchQueue::Create("rtl8127-work", 0, 0, &ivars->workQueue);
    if (ret != kIOReturnSuccess) {
        Log("work queue creation failed: 0x%x", ret);
        goto fail;
    }

    ret = ivars->pciDevice->ConfigureInterrupts(kIOInterruptTypePCIMessaged, 1, 1, 0);
    if (ret != kIOReturnSuccess) {
        Log("MSI configuration failed: 0x%x", ret);
        goto fail;
    }

    ret = IOInterruptDispatchSource::Create(ivars->pciDevice, 0, ivars->workQueue,
                                            &ivars->intSource);
    if (ret != kIOReturnSuccess) {
        Log("interrupt source creation failed: 0x%x", ret);
        goto fail;
    }

    ret = CreateActionInterruptOccurred(sizeof(void *), &ivars->intAction);
    if (ret != kIOReturnSuccess) {
        Log("interrupt action creation failed: 0x%x", ret);
        goto fail;
    }
    ivars->intSource->SetHandler(ivars->intAction);
    ivars->intSource->SetEnable(true);

    /* NDK pool + queues. */
    {
        IOUserNetworkPacketBufferPoolOptions opts = {};
        opts.packetCount = kPoolPackets;
        opts.bufferCount = kPoolPackets;
        opts.bufferSize = kRxBufferSize;
        opts.maxBuffersPerPacket = 1;
        opts.memorySegmentSize = 0;
        opts.poolFlags = 0;
        opts.dmaSpecification.maxAddressBits = 64;

        ret = IOUserNetworkPacketBufferPool::CreateWithOptions(ivars->pciDevice,
                                                               "RTL8127Pool",
                                                               &opts, &ivars->pool);
        if (ret != kIOReturnSuccess) {
            Log("packet pool creation failed: 0x%x", ret);
            goto fail;
        }
    }

    ivars->txSubQueue = IOUserNetworkTxSubmissionQueue::withPool(ivars->pool,
        kNumTxDesc, 0, this, txQueryFreeSpace, txDequeueAction);
    ivars->txCompQueue = IOUserNetworkTxCompletionQueue::withPool(ivars->pool,
        kNumTxDesc, 0, this, txEnqueueAction);
    ivars->rxSubQueue = IOUserNetworkRxSubmissionQueue::withPool(ivars->pool,
        kNumRxDesc, kNumRxDesc, 1, this, rxDequeueAction);
    ivars->rxCompQueue = IOUserNetworkRxCompletionQueue::withPool(ivars->pool,
        kNumRxDesc, 1, this, rxEnqueueAction);

    if (!ivars->txSubQueue || !ivars->txCompQueue ||
        !ivars->rxSubQueue || !ivars->rxCompQueue) {
        Log("NDK queue creation failed");
        goto fail;
    }

    {
        IOUserNetworkPacketQueue *queues[4] = {
            ivars->txSubQueue, ivars->txCompQueue,
            ivars->rxSubQueue, ivars->rxCompQueue,
        };
        ether_addr_t mac = {};
        memcpy(mac.octet, hw->currMacAddr.bytes, 6);

        ret = RegisterEthernetInterface(mac, ivars->pool, queues, 4);
        if (ret != kIOReturnSuccess) {
            Log("RegisterEthernetInterface failed: 0x%x", ret);
            goto fail;
        }
    }

    /*
     * Media list: derived from the chip's max PHY speed so 2.5G (8125) and
     * 5G (8126) parts don't advertise rates they can't reach. Fiber
     * (8127ATF) exposes the forced 10G/1G SerDes modes.
     */
    ivars->mediaCount = 0;
    ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernetAuto;
    if (ivars->fiberMode) {
        ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet10GBaseSR;
        ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet1000BaseSX;
    } else {
        UInt32 maxSpeed = tp->HwSuppMaxPhyLinkSpeed;

        if (maxSpeed >= SPEED_10000)
            ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet10GBaseT;
        if (maxSpeed >= SPEED_5000)
            ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet5000BaseT;
        if (maxSpeed >= SPEED_2500)
            ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet2500BaseT;
        ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet1000BaseT;
        ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet100BaseTX;
        ivars->mediaArray[ivars->mediaCount++] = kIOUserNetworkMediaEthernet10BaseT;
    }
    ivars->currentMedia = kIOUserNetworkMediaEthernetAuto;

    Log("started: D2-D5 datapath ready, waiting for interface enable");

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
    RTL8127Driver_IVars *iv = ivars;

    if (iv) {
        if (iv->intSource) {
            iv->intSource->SetEnable(false);
            iv->intSource->Cancel(^{});
            OSSafeReleaseNULL(iv->intSource);
        }
        OSSafeReleaseNULL(iv->intAction);

        freeDmaBuffer(&iv->txRingMd, &iv->txRingDma);
        freeDmaBuffer(&iv->rxRingMd, &iv->rxRingDma);
        freeDmaBuffer(&iv->statMd, &iv->statDma);

        OSSafeReleaseNULL(iv->barMap);
        OSSafeReleaseNULL(iv->barMd);
        OSSafeReleaseNULL(iv->workQueue);

        /*
         * Return any packets the rings still hold, then release the NDK
         * queues and the pool (each created +1 in Start). The queues
         * reference the pool, so drop them first. Releasing the pool
         * reclaims every packet buffer it backs.
         */
        if (iv->pool) {
            for (uint32_t i = 0; i < kNumRxDesc; i++) {
                if (iv->rxPkt[i]) {
                    iv->pool->deallocatePacket(iv->rxPkt[i]);
                    iv->rxPkt[i] = nullptr;
                }
            }
            for (uint32_t i = 0; i < kNumTxDesc; i++) {
                if (iv->txPkt[i]) {
                    iv->pool->deallocatePacket(iv->txPkt[i]);
                    iv->txPkt[i] = nullptr;
                }
            }
        }
        OSSafeReleaseNULL(iv->txSubQueue);
        OSSafeReleaseNULL(iv->txCompQueue);
        OSSafeReleaseNULL(iv->rxSubQueue);
        OSSafeReleaseNULL(iv->rxCompQueue);
        OSSafeReleaseNULL(iv->pool);

        if (iv->opened && iv->pciDevice) {
            iv->pciDevice->Close(this, 0);
            iv->opened = false;
        }
    }
    return Stop(provider, SUPERDISPATCH);
}

#pragma mark - NDK 2.1 LOCALONLY interface

IOReturn RTL8127Driver::setInterfaceEnable(bool enable)
{
    RTL8127Driver_IVars *iv = ivars;
    RTL8127Hw *hw = iv->hw;
    struct rtl8125_private *tp = &hw->linuxData;

    Log("setInterfaceEnable(%d)", enable);

    if (enable) {
        /* Full enable sequence, same as the kext rtl812xEnable(). */
        hw->rtl812xEnable();

        /* Hardware is configured: arm the RX ring and start the engines. */
        rxRingRefill(iv);
        if (iv->rxSubQueue)
            iv->rxSubQueue->requestDequeue();

        iv->interfaceEnabled = true;

        RTL_W8(tp, ChipCmd, CmdTxEnb | CmdRxEnb);
        RTL_W32(tp, IMR0_8125, hw->intrMask);

        updateLinkStatus(this);
    } else {
        iv->interfaceEnabled = false;
        iv->linkUp = false;

        hw->rtl812xDisable();
        txRingReclaim(this, true);

        /* Drop ring-held rx packets back into the pool. */
        for (uint32_t i = 0; i < kNumRxDesc; i++) {
            if (iv->rxPkt[i]) {
                iv->pool->deallocatePacket(iv->rxPkt[i]);
                iv->rxPkt[i] = nullptr;
            }
        }
        reportLinkStatus(kIOUserNetworkLinkStatusInactive, iv->currentMedia);
    }
    return kIOReturnSuccess;
}

IOReturn RTL8127Driver::setPromiscuousModeEnable(bool enable)
{
    RTL8127Hw *hw = ivars->hw;

    if (enable)
        set_bit(__PROMISC, &hw->stateFlags);
    else
        clear_bit(__PROMISC, &hw->stateFlags);

    hw->applyRxMode();
    return kIOReturnSuccess;
}

IOReturn RTL8127Driver::setAllMulticastModeEnable(bool enable)
{
    RTL8127Hw *hw = ivars->hw;

    hw->multicastFilter = enable ? 0xffffffffffffffffULL : 0;
    if (enable)
        set_bit(__M_CAST, &hw->stateFlags);
    else
        clear_bit(__M_CAST, &hw->stateFlags);

    hw->applyRxMode();
    return kIOReturnSuccess;
}

IOReturn RTL8127Driver::getSupportedMediaArray(MediaWord *mediaArray, uint32_t *mediaCount)
{
    if (!mediaCount)
        return kIOReturnBadArgument;

    if (mediaArray) {
        uint32_t n = (*mediaCount < ivars->mediaCount) ? *mediaCount : ivars->mediaCount;
        for (uint32_t i = 0; i < n; i++)
            mediaArray[i] = ivars->mediaArray[i];
        *mediaCount = n;
    } else {
        *mediaCount = ivars->mediaCount;
    }
    return kIOReturnSuccess;
}

IOReturn RTL8127Driver::handleChosenMedia(MediaWord chosenMedia)
{
    RTL8127Hw *hw = ivars->hw;
    struct rtl8125_private *tp = &hw->linuxData;
    uint32_t speed;

    switch (chosenMedia) {
        case kIOUserNetworkMediaEthernet10GBaseSR:
        case kIOUserNetworkMediaEthernet10GBaseT:
            speed = SPEED_10000;
            break;
        case kIOUserNetworkMediaEthernet1000BaseSX:
        case kIOUserNetworkMediaEthernet1000BaseT:
            speed = SPEED_1000;
            break;
        case kIOUserNetworkMediaEthernet5000BaseT:
            speed = SPEED_5000;
            break;
        case kIOUserNetworkMediaEthernet2500BaseT:
            speed = SPEED_2500;
            break;
        default:
            speed = SPEED_10000;
            break;
    }
    ivars->currentMedia = chosenMedia;

    if (ivars->interfaceEnabled)
        hw->rtl812xSetPhyMedium(tp, AUTONEG_ENABLE, speed, DUPLEX_FULL, tp->advertising);

    return kIOReturnSuccess;
}

IOReturn RTL8127Driver::setMaxTransferUnit(uint32_t mtu)
{
    if (mtu > 1500)
        return kIOReturnUnsupported;

    ivars->mtu = mtu;
    ivars->hw->mtu = mtu;
    return kIOReturnSuccess;
}

uint32_t RTL8127Driver::getMaxTransferUnit()
{
    return ivars->mtu;
}

uint32_t RTL8127Driver::getHardwareAssists()
{
    return (kIOUserNetworkHWAssistTxChecksumIPHdr |
            kIOUserNetworkHWAssistTxChecksumTCP |
            kIOUserNetworkHWAssistTxChecksumUDP |
            kIOUserNetworkHWAssistRxChecksum);
}

IOReturn RTL8127Driver::getHardwareAddress(ether_addr_t *addr)
{
    if (!addr)
        return kIOReturnBadArgument;

    memcpy(addr->octet, ivars->hw->currMacAddr.bytes, 6);
    return kIOReturnSuccess;
}

#pragma mark - legacy RPC interface (still pure virtual upstream)

kern_return_t IMPL(RTL8127Driver, SetInterfaceEnable)
{
    return setInterfaceEnable(isEnable);
}

kern_return_t IMPL(RTL8127Driver, SetPromiscuousModeEnable)
{
    return setPromiscuousModeEnable(enable);
}

kern_return_t IMPL(RTL8127Driver, SetMulticastAddresses)
{
    /* Hash filtering: accept-all-multicast keeps semantics safe. */
    ivars->hw->multicastFilter = 0xffffffffffffffffULL;
    set_bit(__M_CAST, &ivars->hw->stateFlags);
    ivars->hw->applyRxMode();
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8127Driver, SetAllMulticastModeEnable)
{
    return setAllMulticastModeEnable(enable);
}

kern_return_t IMPL(RTL8127Driver, SelectMediaType)
{
    return handleChosenMedia(mediaType);
}

kern_return_t IMPL(RTL8127Driver, SetWakeOnMagicPacketEnable)
{
    return kIOReturnUnsupported;
}

kern_return_t IMPL(RTL8127Driver, SetMTU)
{
    return setMaxTransferUnit(mtu);
}

kern_return_t IMPL(RTL8127Driver, GetMaxTransferUnit)
{
    if (!mtu)
        return kIOReturnBadArgument;
    *mtu = ivars->mtu;
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
    *hardwareAssists = getHardwareAssists();
    return kIOReturnSuccess;
}
