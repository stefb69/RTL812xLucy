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
#include <time.h>
#include <stdio.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOTimerDispatchSource.h>
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

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <net/bpf.h>

/*
 * rtl812xx.h's LinkStatus enumerator collides with the NDK LinkStatus
 * type; rename it for this translation unit only.
 */
#define LinkStatus RtlHwLinkStatus
#include "hw/RTL8127Hw.h"
#undef LinkStatus

#include "RTL8127Driver.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "RTL8127Dext: " fmt, ##__VA_ARGS__)

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

/*
 * Buffer sizing. One buffer per packet on both sides (the NDK packet API
 * exposes a single data pointer per packet), so:
 *  - RX buffers hold the largest frame the chip is allowed to receive:
 *    9000-byte MTU + VLAN Ethernet header + FCS, with headroom;
 *  - TX buffers bound the size of a TSO packet the stack may hand us,
 *    advertised through getTSOOptions().
 * The RX descriptor length field is 14 bits, so both fit.
 */
#define kMaxMtu         9000
#define kRxBufferSize   9216
#define kTxBufferSize   32768
#define kTsoMaxPacket   (kTxBufferSize - 256)
#define kRxPoolPackets  (kNumRxDesc + 512)
/* TX packets in flight are bounded by the pool, not the ring: 512 x 32 KB
 * (16 MB) covers many milliseconds at 10 Gbit/s and halves the TSO packet
 * rate compared to 16 KB buffers. */
#define kTxPoolPackets  512
#define kMacHdrLen      14
#define kIPv6HdrLen     40
#define kTxDescLenMask  0xFFFF
#define MSSShift_8125   18      /* MSS position in opts2, as in the kext */
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
    IOTimerDispatchSource *statsTimer;
    OSAction *statsAction;

    /* Datapath counters, logged every 5 s while the interface is enabled. */
    uint64_t txSubmitted, txCompleted, txDroppedFifo, txBytes, txTso, txPartial, txPartialFail;
    uint64_t rxDelivered, rxErrors, rxDroppedFifo, rxRefillShort;
    uint64_t isrCount, isrRx, isrTx, isrTimer, isrLink;
    uint32_t lastIsrStatus;
    uint32_t txDebugLogged;
    uint32_t synLogged;

    /* BPF tap state (tcpdump on this interface): bpfTap() tells us which
     * directions have listeners; the NDK expects the driver to tap itself. */
    uint32_t bpfMode;

    /* Hardware tally block (chip-side counters), dumped every stats tick. */
    volatile RtlStatData *statData;
    bool tallyPending;

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
    IOUserNetworkPacketBufferPool *txPool;
    IOUserNetworkPacketBufferPool *rxPool;
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

    /* Offloads the stack asked for (setHardwareAssists), TSO in particular. */
    bool tsoEnabled;

    /* One-shot warnings: a full hand-off FIFO means the framework thread
     * fell behind and we had to drop a packet/completion. Logged once per
     * direction to avoid spamming the datapath. */
    bool txDropWarned;
    bool rxDropWarned;
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

    /*
     * IONewZero() zero-fills the object without running its C++ default
     * member initializers. Every RTL8127Hw member is fine at zero except
     * the MTU: rtl812xInit() derives the chip's maximum receive size from
     * it (mtu + VLAN header + FCS), and with mtu == 0 RxMaxSize ends up at
     * 22 bytes -- the chip then rejects every frame. Seen on hardware as
     * "rx pkts 0" with the runt/oversize tally climbing.
     */
    ivars->mtu = 1500;
    ivars->hw->mtu = ivars->mtu;
    ivars->tsoEnabled = true;
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

#pragma mark - TSO helpers

static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }

/*
 * The RTL8125/8127 giant-send engine wants the TCP checksum field preloaded
 * with the pseudo-header checksum (addresses + protocol, no length), exactly
 * like the kext's prepareTSO4/6(). NDK packets carry the VLAN tag out of
 * band, so the L3 header always starts right after the 14-byte MAC header.
 * Returns the TCP header offset from the start of the frame.
 */
static uint32_t prepareTSO4(uint8_t *frame)
{
    struct ip *iph = (struct ip *)(frame + kMacHdrLen);
    uint32_t il = (uint32_t)(iph->ip_hl & 0x0f) << 2;
    struct tcphdr *th = (struct tcphdr *)((uint8_t *)iph + il);
    const uint16_t *addr = (const uint16_t *)&iph->ip_src;
    uint32_t csum = IPPROTO_TCP;

    for (int i = 0; i < 4; i++) {
        csum += bswap16(addr[i]);
        csum += (csum >> 16);
        csum &= 0xffff;
    }
    th->th_sum = bswap16((uint16_t)csum);
    return kMacHdrLen + il;
}

static uint32_t prepareTSO6(uint8_t *frame)
{
    struct ip6_hdr *ip6 = (struct ip6_hdr *)(frame + kMacHdrLen);
    struct tcphdr *th = (struct tcphdr *)((uint8_t *)ip6 + kIPv6HdrLen);
    uint16_t addr[16];      /* src + dst, 32 bytes at offset 8 of the header */
    uint32_t csum = IPPROTO_TCP;

    ip6->ip6_ctlun.ip6_un1.ip6_un1_plen = 0;
    memcpy(addr, (uint8_t *)ip6 + 8, sizeof(addr));
    for (int i = 0; i < 16; i++) {
        csum += bswap16(addr[i]);
        csum += (csum >> 16);
        csum &= 0xffff;
    }
    th->th_sum = bswap16((uint16_t)csum);
    return kMacHdrLen + kIPv6HdrLen;
}

/*
 * Partial checksum (kIOUserNetworkPacketTxCsumPartial): the stack asks for a
 * one's-complement sum of the bytes from `start` to the end of the frame,
 * stored at `stuff`, with the pseudo-header sum already seeded in place.
 * That is exactly what Apple's user-space TCP (libusrtcp, used by every
 * Network.framework client, from the code-signing timestamp service to
 * Safari) requests. For TCP and UDP over IPv4/IPv6 the chip's checksum
 * engine produces the same result, so map it to the hardware bits and
 * return true; anything else gets the sum computed here.
 */
static bool txPartialChecksum(IOUserNetworkPacket *pkt, uint32_t len,
                              uint16_t start, uint16_t stuff, uint32_t *opts2)
{
    uint8_t *frame = (uint8_t *)pkt->getDataVirtualAddress();
    if (!frame || len < kMacHdrLen + 20)
        return false;

    uint16_t etherType = (uint16_t)(frame[12] << 8 | frame[13]);
    uint8_t proto = 0;
    uint32_t l4off = 0;

    if (etherType == 0x0800) {
        uint32_t ihl = (uint32_t)(frame[kMacHdrLen] & 0x0f) << 2;
        proto = frame[kMacHdrLen + 9];
        l4off = kMacHdrLen + ihl;
        if (proto == IPPROTO_TCP && stuff == l4off + 16) {
            *opts2 = (TxIPCS_C | TxTCPCS_C);
            return true;
        }
        if (proto == IPPROTO_UDP && stuff == l4off + 6) {
            *opts2 = (TxIPCS_C | TxUDPCS_C);
            return true;
        }
    } else if (etherType == 0x86DD) {
        proto = frame[kMacHdrLen + 6];
        l4off = kMacHdrLen + kIPv6HdrLen;
        if (proto == IPPROTO_TCP && stuff == l4off + 16) {
            *opts2 = (TxTCPCS_C | TxIPV6F_C | ((l4off & TCPHO_MAX) << TCPHO_SHIFT));
            return true;
        }
        if (proto == IPPROTO_UDP && stuff == l4off + 6) {
            *opts2 = (TxUDPCS_C | TxIPV6F_C | ((l4off & TCPHO_MAX) << TCPHO_SHIFT));
            return true;
        }
    }

    /* Software fallback: finish the sum the stack started. */
    if (start >= len || stuff + 2 > len)
        return false;
    uint32_t sum = 0;
    const uint8_t *p = frame + start;
    uint32_t n = len - start;
    while (n > 1) { sum += (uint32_t)(p[0] << 8 | p[1]); p += 2; n -= 2; }
    if (n) sum += (uint32_t)(p[0] << 8);
    /* fold in the seed already at `stuff` (it was included in the sum above,
     * which is what the stack expects: it seeds the field, we sum over it) */
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t csum = (uint16_t)~sum;
    if (csum == 0 && proto == IPPROTO_UDP) csum = 0xffff;
    frame[stuff] = (uint8_t)(csum >> 8);
    frame[stuff + 1] = (uint8_t)csum;
    *opts2 = 0;
    return true;
}

/* Beta diagnostics: describe a TCP SYN/SYN-ACK frame (returns false if not one). */
static bool describeTcpSyn(const uint8_t *f, uint32_t len, char *out, size_t outLen)
{
    if (!f || len < 54) return false;
    uint16_t et = (uint16_t)(f[12] << 8 | f[13]);
    uint32_t l4; const char *v;
    if (et == 0x0800 && f[23] == 6) { l4 = 14 + ((f[14] & 0x0f) << 2); v = "v4"; }
    else if (et == 0x86DD && f[20] == 6) { l4 = 54; v = "v6"; }
    else return false;
    if (len < l4 + 14) return false;
    uint8_t flags = f[l4 + 13];
    if (!(flags & 0x02)) return false;
    unsigned sport = (unsigned)(f[l4] << 8 | f[l4 + 1]);
    unsigned dport = (unsigned)(f[l4 + 2] << 8 | f[l4 + 3]);
    if (sport != 80 && dport != 80 && sport != 443 && dport != 443)
        return false;
    snprintf(out, outLen, "%s %u>%u flags 0x%02x len %u", v, sport, dport, flags, len);
    return true;
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
        uint32_t cmd = 0, opts1, opts2 = 0;
        uint32_t index;

        if (__atomic_load_n(&hw->txNumFreeDesc, __ATOMIC_ACQUIRE) <= 2)
            break;

        if (len == 0 || len > kTxBufferSize) {
            /* Cannot happen with our pool geometry; don't feed the chip. */
            pkt->setCompletionStatus(kIOReturnBadArgument);
            if (!fifoPush(&iv->txDoneFifo, pkt))
                iv->txPool->deallocatePacket(pkt);
            accepted++;
            continue;
        }

        /*
         * TSO: same descriptor recipe as the kext outputStart(). A TSO
         * packet that fits in one MTU-sized frame is sent as a plain
         * checksum-offloaded frame. The chip's MSS field is 11 bits, so on
         * jumbo MTUs the segment size is clamped -- smaller frames on the
         * wire, still correct.
         */
        uint16_t mss = 0;
        IOUserNetworkPacketTSOFlags tso = 0;
        pkt->getTSOInfo(&mss, &tso);

        if (iv->tsoEnabled && (tso & (kIOUserNetworkPacketTSOIPV4 | kIOUserNetworkPacketTSOIPV6)) &&
            (len - kMacHdrLen) > iv->mtu) {
            uint8_t *frame = (uint8_t *)pkt->getDataVirtualAddress();
            uint32_t tcpOff;
            uint32_t segsz = mss;

            if (segsz == 0 || segsz > MSS_MAX)
                segsz = MSS_MAX;

            if (tso & kIOUserNetworkPacketTSOIPV4) {
                tcpOff = prepareTSO4(frame);
                cmd = (GiantSendv4 | (tcpOff << GTTCPHO_SHIFT));
            } else {
                tcpOff = prepareTSO6(frame);
                cmd = (GiantSendv6 | (tcpOff << GTTCPHO_SHIFT));
            }
            opts2 = ((segsz & MSSMask) << MSSShift_8125);
        } else {
            /* Checksum offload, same opts2 bits as the kext outputStart(). */
            IOUserNetworkPacketTxChecksumFlags csum = 0;
            uint16_t start = 0, stuff = 0;
            pkt->getTxChecksumInfo(&csum, &start, &stuff);

            if (csum & kIOUserNetworkPacketTxCsumPartial) {
                iv->txPartial++;
                if (!txPartialChecksum(pkt, len, start, stuff, &opts2))
                    iv->txPartialFail++;
            } else if ((csum & kIOUserNetworkPacketTxCsumTCPIPV4) || (tso & kIOUserNetworkPacketTSOIPV4))
                opts2 = (TxIPCS_C | TxTCPCS_C);
            else if (csum & kIOUserNetworkPacketTxCsumUDPIPV4)
                opts2 = (TxIPCS_C | TxUDPCS_C);
            else if ((csum & kIOUserNetworkPacketTxCsumTCPIPV6) || (tso & kIOUserNetworkPacketTSOIPV6))
                opts2 = (TxTCPCS_C | TxIPV6F_C | ((kMacHdrLen + kIPv6HdrLen) << TCPHO_SHIFT));
            else if (csum & kIOUserNetworkPacketTxCsumUDPIPV6)
                opts2 = (TxUDPCS_C | TxIPV6F_C | ((kMacHdrLen + kIPv6HdrLen) << TCPHO_SHIFT));
            else if (csum & kIOUserNetworkPacketTxCsumIPHdr)
                opts2 = TxIPCS_C;
        }

        index = hw->txNextDescIndex;
        volatile RtlDextTxDesc *desc = &iv->txRing[index];

        opts1 = ((len & kTxDescLenMask) | cmd | FirstFrag | LastFrag);
        if (index == kTxLastDesc)
            opts1 |= RingEnd;

        iv->txPkt[index] = pkt;
        desc->addr = OSSwapHostToLittleInt64(iova);
        desc->opts2 = OSSwapHostToLittleInt32(opts2);
        desc->opts1 = OSSwapHostToLittleInt32(opts1 | DescOwn);

        iv->txSubmitted++;
        iv->txBytes += len;
        if (cmd) iv->txTso++;
        if (iv->synLogged < 100) {
            char d[96];
            if (describeTcpSyn((const uint8_t *)pkt->getDataVirtualAddress(), len, d, sizeof(d))) {
                iv->synLogged++;
                uint32_t tf = pkt->getTxCsumFlags();
                Log("tx SYN %{public}s csumflags 0x%x opts2 0x%08x dataoff %u", d, tf, opts2, pkt->getDataOffset());
            }
        }
        if (iv->bpfMode & BPF_MODE_OUTPUT)
            driver->bpfTapOutputPacket(DLT_EN10MB, pkt, nullptr, 0);
        if (iv->txDebugLogged < 4) {
            iv->txDebugLogged++;
            Log("tx#%llu idx %u len %u opts1 0x%08x opts2 0x%08x iova 0x%llx tail %u",
                iv->txSubmitted, index, len, opts1 | DescOwn, opts2, iova, hw->txTailPtr0 + 1);
        }

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
        *freeSpaceBytes = (uint32_t)freeDesc * kTxBufferSize;

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
    bool complete = true;

    for (uint32_t i = 0; i < kNumRxDesc; i++) {
        uint64_t word1;

        if (iv->rxPkt[i])
            continue;

        IOUserNetworkPacket *pkt = fifoPop(&iv->rxFreeFifo);
        if (!pkt && iv->rxPool) {
            if (iv->rxPool->allocatePacket(&pkt) != kIOReturnSuccess)
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
            iv->txCompleted++;
            if (!fifoPush(&iv->txDoneFifo, pkt)) {
                iv->txDroppedFifo++;
                if (!iv->txDropWarned) {
                    iv->txDropWarned = true;
                    Log("tx completion FIFO full - dropping completions (framework thread behind)");
                }
                iv->txPool->deallocatePacket(pkt);
            }
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

        length = (int32_t)(status & 0x3fff) - 4 /* FCS */;

        if ((status & RxRES) || length <= 0 ||
            !(status & FirstFrag) || !(status & LastFrag)) {
            /* Error or fragmented jumbo: recycle the buffer in place. */
            iv->rxErrors++;
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

            iv->rxDelivered++;
            if (iv->synLogged < 100) {
                char d[96];
                if (describeTcpSyn((const uint8_t *)pkt->getDataVirtualAddress(), (uint32_t)length, d, sizeof(d))) {
                    iv->synLogged++;
                    Log("rx SYN %{public}s opts2 0x%08x", d, opts2);
                }
            }
            if (iv->bpfMode & BPF_MODE_INPUT)
                driver->bpfTapInputPacket(DLT_EN10MB, pkt, nullptr, 0);
            if (!fifoPush(&iv->rxDoneFifo, pkt)) {
                iv->rxDroppedFifo++;
                if (!iv->rxDropWarned) {
                    iv->rxDropWarned = true;
                    Log("rx FIFO full - dropping packets (framework thread behind)");
                }
                iv->rxPool->deallocatePacket(pkt);
            }
            delivered = true;
        }
        hw->rxNextDescIndex = (index + 1) & kRxDescMask;
    }

    if (!rxRingRefill(iv))
        iv->rxRefillShort++;

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

            /* Packets refused while the link was down are still queued in
             * the framework: ask for them now, and for fresh rx buffers. */
            if (iv->txSubQueue)
                iv->txSubQueue->requestDequeue();
            if (iv->rxSubQueue)
                iv->rxSubQueue->requestDequeue();
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

    /* Mask, then ack (same order as the kext). */
    RTL_W32(tp, IMR0_8125, 0x0000);
    RTL_W32(tp, ISR0_8125, (status & ~RxFIFOOver));

    iv->isrCount++;
    iv->lastIsrStatus = status;
    if (status & (RxOK | RxDescUnavail)) iv->isrRx++;
    if (status & TxOK) iv->isrTx++;
    if (status & PCSTimeout) iv->isrTimer++;
    if (status & LinkChg) iv->isrLink++;

    if (iv->interfaceEnabled) {
        if (status & (RxOK | RxDescUnavail))
            rxRingService(this);

        if (status & (TxOK | RxOK | PCSTimeout))
            txRingReclaim(this, false);

        /*
         * Interrupt mitigation, as validated in the kext at line rate:
         * after a burst, stop taking TxOK interrupts and let a chip timer
         * (PCSTimeout) sweep the completions instead; RxOK stays armed.
         */
        if (status & (TxOK | RxOK)) {
            RTL_W32(tp, TIMER_INT0_8125, 0x5000);
            RTL_W32(tp, TCTR0_8125, 0x5000);
            hw->intrMask = hw->intrMaskTimer;
        } else if (status & PCSTimeout) {
            RTL_W32(tp, TIMER_INT0_8125, 0x0000);
            hw->intrMask = hw->intrMaskRxTx;
        }
    }

    if (status & LinkChg) {
        updateLinkStatus(this);
        RTL_W32(tp, TIMER_INT0_8125, 0x0000);
        hw->intrMask = hw->intrMaskRxTx;
    }

    RTL_W32(tp, IMR0_8125, hw->intrMask);
}

void IMPL(RTL8127Driver, StatsTimerOccurred)
{
    RTL8127Driver_IVars *iv = ivars;
    RTL8127Hw *hw = iv->hw;
    struct rtl8125_private *tp = &hw->linuxData;

    if (iv->interfaceEnabled) {
        /* Chip tally counters: the dump issued at the previous tick has
         * landed when the CounterDump bit reads back clear. */
        if (iv->tallyPending && !(RTL_R32(tp, CounterAddrLow) & CounterDump) && iv->statData) {
            volatile RtlStatData *st = iv->statData;
            Log("tally: tx pkts %llu err %llu underrun %u | rx pkts %llu err %u missed %u macmissed-runt %u uni %llu bcast %llu mcast %u | octets tx %llu rx %llu",
                (uint64_t)OSSwapLittleToHostInt64(st->txPackets), (uint64_t)OSSwapLittleToHostInt64(st->txErrors),
                OSSwapLittleToHostInt16(st->txUnderun),
                (uint64_t)OSSwapLittleToHostInt64(st->rxPackets), OSSwapLittleToHostInt32(st->rxErrors),
                OSSwapLittleToHostInt16(st->rxMissed), OSSwapLittleToHostInt32(st->rxRunt),
                (uint64_t)OSSwapLittleToHostInt64(st->rxUnicast), (uint64_t)OSSwapLittleToHostInt64(st->rxBroadcast),
                OSSwapLittleToHostInt32(st->rxMulticast),
                (uint64_t)OSSwapLittleToHostInt64(st->txOctets), (uint64_t)OSSwapLittleToHostInt64(st->rxOctets));
            iv->tallyPending = false;
        }
        if (!iv->tallyPending) {
            uint32_t cmd = (uint32_t)(hw->statPhyAddr & 0xffffffffULL);
            RTL_W32(tp, CounterAddrHigh, (uint32_t)(hw->statPhyAddr >> 32));
            RTL_W32(tp, CounterAddrLow, cmd);
            RTL_W32(tp, CounterAddrLow, cmd | CounterDump);
            iv->tallyPending = true;
        }

        {
            uint32_t ri = hw->rxNextDescIndex;
            Log("regs: ChipCmd 0x%02x RxConfig 0x%08x TxConfig 0x%08x RxMaxSize %u RxDesc 0x%08x%08x (ring 0x%llx) TxDesc 0x%08x%08x (ring 0x%llx) | rx next %u opts1 0x%08x addr 0x%llx rxPkt %{public}s",
                RTL_R8(tp, ChipCmd), RTL_R32(tp, RxConfig), RTL_R32(tp, TxConfig), RTL_R16(tp, RxMaxSize),
                RTL_R32(tp, RxDescAddrHigh), RTL_R32(tp, RxDescAddrLow), hw->rxPhyAddr,
                RTL_R32(tp, TxDescStartAddrHigh), RTL_R32(tp, TxDescStartAddrLow), hw->txPhyAddr,
                ri, OSSwapLittleToHostInt32(iv->rxRing[ri].opts1), (uint64_t)OSSwapLittleToHostInt64(iv->rxRing[ri].addr),
                iv->rxPkt[ri] ? "set" : "NULL");
        }

        Log("stats: link %u tx sub %llu done %llu tso %llu partial %llu/%llu bytes %llu free %d tail %u close %u hwclo %u | rx deliv %llu err %llu short %llu | isr %llu rx %llu tx %llu tmr %llu link %llu last 0x%08x imr 0x%08x | fifo drops tx %llu rx %llu",
            iv->linkUp, iv->txSubmitted, iv->txCompleted, iv->txTso, iv->txPartial, iv->txPartialFail, iv->txBytes,
            __atomic_load_n(&hw->txNumFreeDesc, __ATOMIC_ACQUIRE), hw->txTailPtr0, hw->txClosePtr0,
            hw->rtl812xGetHwCloPtr(tp),
            iv->rxDelivered, iv->rxErrors, iv->rxRefillShort,
            iv->isrCount, iv->isrRx, iv->isrTx, iv->isrTimer, iv->isrLink, iv->lastIsrStatus,
            RTL_R32(tp, IMR0_8125), iv->txDroppedFifo, iv->rxDroppedFifo);
    }
    if (iv->statsTimer)
        iv->statsTimer->WakeAtTime(kIOTimerClockMonotonicRaw, time + 5ULL * 1000000000ULL, 0);
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
    Log("chip mcfg %u, maxSpeed %u, MAC-OCP 0xD006=0x%04x -> %{public}s", tp->mcfg,
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
    ivars->statData = (volatile RtlStatData *)virt;
    memset(virt, 0, kStatSize);

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

    /* Statistics timer, 5 s period, on the work queue. */
    if (IOTimerDispatchSource::Create(ivars->workQueue, &ivars->statsTimer) == kIOReturnSuccess &&
        CreateActionStatsTimerOccurred(sizeof(void *), &ivars->statsAction) == kIOReturnSuccess) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        ivars->statsTimer->SetHandler(ivars->statsAction);
        ivars->statsTimer->SetEnable(true);
        ivars->statsTimer->WakeAtTime(kIOTimerClockMonotonicRaw, now + 5ULL * 1000000000ULL, 0);
    } else {
        Log("stats timer not available");
    }

    /*
     * NDK pools + queues. Separate TX and RX pools: TX buffers are sized
     * for TSO packets, RX buffers for jumbo frames. Both are mapped into
     * this process (TSO patches the TCP header) and into the device.
     */
    {
        IOUserNetworkPacketBufferPoolOptions opts = {};
        opts.maxBuffersPerPacket = 1;
        opts.memorySegmentSize = 0;
        opts.poolFlags = (PoolFlagMapToDext | PoolFlagMapToDevice);
        opts.dmaSpecification.maxAddressBits = 64;

        opts.packetCount = kTxPoolPackets;
        opts.bufferCount = kTxPoolPackets;
        opts.bufferSize = kTxBufferSize;
        ret = IOUserNetworkPacketBufferPool::CreateWithOptions(ivars->pciDevice,
                                                               "RTL8127TxPool",
                                                               &opts, &ivars->txPool);
        if (ret != kIOReturnSuccess) {
            Log("tx packet pool creation failed: 0x%x", ret);
            goto fail;
        }

        opts.packetCount = kRxPoolPackets;
        opts.bufferCount = kRxPoolPackets;
        opts.bufferSize = kRxBufferSize;
        ret = IOUserNetworkPacketBufferPool::CreateWithOptions(ivars->pciDevice,
                                                               "RTL8127RxPool",
                                                               &opts, &ivars->rxPool);
        if (ret != kIOReturnSuccess) {
            Log("rx packet pool creation failed: 0x%x", ret);
            goto fail;
        }
    }

    ivars->txSubQueue = IOUserNetworkTxSubmissionQueue::withPool(ivars->txPool,
        kTxPoolPackets, 0, this, txQueryFreeSpace, txDequeueAction);
    ivars->txCompQueue = IOUserNetworkTxCompletionQueue::withPool(ivars->txPool,
        kTxPoolPackets, 0, this, txEnqueueAction);
    ivars->rxSubQueue = IOUserNetworkRxSubmissionQueue::withPool(ivars->rxPool,
        kNumRxDesc, kNumRxDesc, 1, this, rxDequeueAction);
    ivars->rxCompQueue = IOUserNetworkRxCompletionQueue::withPool(ivars->rxPool,
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

        ret = registerEthernetInterface(mac, queues, 4, ivars->txPool, ivars->rxPool);
        if (ret != kIOReturnSuccess) {
            Log("registerEthernetInterface failed: 0x%x", ret);
            goto fail;
        }
        ret = bpfAttach(DLT_EN10MB, kMacHdrLen);
        if (ret != kIOReturnSuccess)
            Log("bpfAttach failed: 0x%x (tcpdump will not see this interface)", ret);
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

    Log("started: datapath ready (TSO, jumbo up to %u), waiting for interface enable", kMaxMtu);

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
        if (iv->statsTimer) {
            iv->statsTimer->SetEnable(false);
            iv->statsTimer->Cancel(^{});
            OSSafeReleaseNULL(iv->statsTimer);
        }
        OSSafeReleaseNULL(iv->statsAction);
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
        if (iv->rxPool) {
            for (uint32_t i = 0; i < kNumRxDesc; i++) {
                if (iv->rxPkt[i]) {
                    iv->rxPool->deallocatePacket(iv->rxPkt[i]);
                    iv->rxPkt[i] = nullptr;
                }
            }
        }
        if (iv->txPool) {
            for (uint32_t i = 0; i < kNumTxDesc; i++) {
                if (iv->txPkt[i]) {
                    iv->txPool->deallocatePacket(iv->txPkt[i]);
                    iv->txPkt[i] = nullptr;
                }
            }
        }
        OSSafeReleaseNULL(iv->txSubQueue);
        OSSafeReleaseNULL(iv->txCompQueue);
        OSSafeReleaseNULL(iv->rxSubQueue);
        OSSafeReleaseNULL(iv->rxCompQueue);
        OSSafeReleaseNULL(iv->txPool);
        OSSafeReleaseNULL(iv->rxPool);

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

        /*
         * The NDK queues start out disabled: until setEnable(true) the
         * framework never runs our dequeue actions nor delivers our
         * completions. Enable them once the hardware is configured.
         */
        if (iv->txSubQueue)  iv->txSubQueue->setEnable(true);
        if (iv->txCompQueue) iv->txCompQueue->setEnable(true);
        if (iv->rxSubQueue)  iv->rxSubQueue->setEnable(true);
        if (iv->rxCompQueue) iv->rxCompQueue->setEnable(true);

        /* Hardware is configured: arm the RX ring and start the engines. */
        rxRingRefill(iv);
        if (iv->rxSubQueue)
            iv->rxSubQueue->requestDequeue();

        iv->interfaceEnabled = true;

        RTL_W8(tp, ChipCmd, CmdTxEnb | CmdRxEnb);
        RTL_W32(tp, IMR0_8125, hw->intrMask);

        updateLinkStatus(this);
        if (iv->txSubQueue)
            iv->txSubQueue->requestDequeue();
    } else {
        iv->interfaceEnabled = false;
        iv->linkUp = false;

        hw->rtl812xDisable();
        txRingReclaim(this, true);

        if (iv->txSubQueue)  { iv->txSubQueue->setEnable(false); iv->txSubQueue->purgePackets(); }
        if (iv->txCompQueue) iv->txCompQueue->setEnable(false);
        if (iv->rxSubQueue)  iv->rxSubQueue->setEnable(false);
        if (iv->rxCompQueue) iv->rxCompQueue->setEnable(false);

        /* Drop ring-held rx packets back into the pool. */
        for (uint32_t i = 0; i < kNumRxDesc; i++) {
            if (iv->rxPkt[i]) {
                iv->rxPool->deallocatePacket(iv->rxPkt[i]);
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

/*
 * Multicast list from the stack (the modern NDK entry point; the RPC
 * variant below is the deprecated one). Without this override the default
 * implementation rejects the request and the interface never joins any
 * link-layer multicast group: no mDNS, no IPv6 solicited-node groups, so
 * nobody on the LAN can resolve our IPv6 addresses. Program the 64-bit
 * hash filter the way the kext's setMulticastList() does.
 */
static uint32_t etherCrc(const uint8_t *data, int length)
{
    uint32_t crc = 0xffffffff;
    while (--length >= 0) {
        uint8_t octet = *data++;
        for (int bit = 0; bit < 8; bit++, octet >>= 1)
            crc = (crc << 1) ^ ((((int32_t)crc < 0) ^ (octet & 1)) ? 0x04c11db7U : 0);
    }
    return crc;
}

IOReturn RTL8127Driver::setMulticastAddresses(const ether_addr_t *addresses, uint32_t count)
{
    RTL8127Hw *hw = ivars->hw;
    uint64_t filter = 0;

    if (count > 32 || !addresses) {
        filter = 0xffffffffffffffffULL;
    } else {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t bit = etherCrc(addresses[i].octet, 6) >> 26;
            filter |= (1ULL << (bit & 0x3f));
        }
        filter = OSSwapInt64(filter);
    }
    hw->multicastFilter = filter;
    if (count)
        set_bit(__M_CAST, &hw->stateFlags);
    else
        clear_bit(__M_CAST, &hw->stateFlags);
    hw->applyRxMode();
    Log("multicast list: %u addresses, filter 0x%016llx", count, filter);
    return kIOReturnSuccess;
}

int RTL8127Driver::bpfTap(uint32_t dataLinkType, uint32_t mode)
{
    if (dataLinkType != DLT_EN10MB)
        return kIOReturnUnsupported;
    ivars->bpfMode = mode;
    Log("bpf tap mode %u", mode);
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
    RTL8127Hw *hw = ivars->hw;
    struct rtl8125_private *tp = &hw->linuxData;

    if (mtu < ETH_ZLEN || mtu > kMaxMtu)
        return kIOReturnUnsupported;

    ivars->mtu = mtu;
    hw->mtu = mtu;

    /* Same bookkeeping as the kext setMaxPacketSize(). */
    tp->rms = mtu + VLAN_ETH_HLEN + ETH_FCS_LEN;
    tp->eee.tx_lpi_timer = mtu + ETH_HLEN + 0x20;
    if (ivars->interfaceEnabled)
        RTL_W16(tp, RxMaxSize, tp->rms);

    Log("MTU %u (rms %u)", mtu, (unsigned int)tp->rms);
    return kIOReturnSuccess;
}

uint32_t RTL8127Driver::getMaxTransferUnit()
{
    return ivars->mtu;
}

uint32_t RTL8127Driver::getHardwareAssists()
{
    uint32_t assists = (kIOUserNetworkHWAssistTxChecksumIPHdr |
                        kIOUserNetworkHWAssistTxChecksumTCP |
                        kIOUserNetworkHWAssistTxChecksumUDP |
                        kIOUserNetworkHWAssistRxChecksum);

    if (ivars->tsoEnabled)
        assists |= (kIOUserNetworkHWAssistTSO4 | kIOUserNetworkHWAssistTSO6);
    return assists;
}

IOReturn RTL8127Driver::setHardwareAssists(uint32_t hardwareAssists, uint32_t hardwareAssistsMask)
{
    if (hardwareAssistsMask & (kIOUserNetworkHWAssistTSO4 | kIOUserNetworkHWAssistTSO6)) {
        ivars->tsoEnabled = (hardwareAssists & (kIOUserNetworkHWAssistTSO4 | kIOUserNetworkHWAssistTSO6)) != 0;
        Log("TSO %{public}s by the stack", ivars->tsoEnabled ? "enabled" : "disabled");
    }
    return kIOReturnSuccess;
}

IOReturn RTL8127Driver::getTSOOptions(IOUserNetworkTSOOptions *options)
{
    if (!options)
        return kIOReturnBadArgument;

    /* Largest TSO packet we accept: it has to fit in one TX buffer. */
    options->tso_mtu4 = kTsoMaxPacket;
    options->tso_mtu6 = kTsoMaxPacket;
    return kIOReturnSuccess;
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
    return setMulticastAddresses((const ether_addr_t *)addresses, count);
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
