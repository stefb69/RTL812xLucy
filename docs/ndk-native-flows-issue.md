# Open issue: user-space TCP (Skywalk channel flows) never reaches the wire on a NetworkingDriverKit dext

Status: **RESOLVED 2026-09-12 01:22 CEST** by the external review
([ndk-native-flows-review.md](ndk-native-flows-review.md), commit 89e93d0):
native (flowswitch) TX packets carry the Ethernet frame at a 2-byte data
offset inside the buffer, and `getDataVirtualAddress()` /
`getDataIOVirtualAddress()` return the buffer base without that offset. The
driver parsed and DMA'd from the base, so every native frame left the wire
shifted by two bytes and no peer ever answered. Fix: resolve CPU and DMA
addresses as base + `getDataOff()` (`RTL8127TxBuffer.h`). Verified on hardware
with build 0.2.17: native SYNs logged with `dataoff 2`, SYN-ACKs received,
codesign timestamps and Network.framework HTTP/HTTPS work over en10.

The driver's own diagnostics missed it because the 12-line "odd frame"
budget was consumed by ARP requests (42 bytes) before any native frame, and
the offset log budget by BSD packets with a non-zero memory-segment offset.

Original write-up kept below for the record.

---


## Setup

- macOS 26.6.2 (Darwin 25.6.0, xnu-12377.161.14, Apple Silicon M5, Mac17,2).
- DriverKit extension (dext) for a Realtek RTL8127ATF 10GbE PCIe NIC (SFP+,
  in a Thunderbolt enclosure), built against the DriverKit 25.5 SDK,
  NetworkingDriverKit (NDK) API level 2.4. Signed Developer ID, notarized,
  entitlements: driverkit, driverkit.transport.pci (vendor mask
  `0x000010ec&0x0000FFFF`), driverkit.family.networking.
- Driver class `RTL8127Driver : IOUserNetworkEthernet`. Info.plist personality:
  `IOClass = IOUserNetworkEthernet`, `CFBundleIdentifierKernel =
  com.apple.iokit.IOSkywalkFamily`, `IOProviderClass = IOPCIDevice`,
  `OSBundleLibraries` = IOSkywalkFamily 1.0 (same as Apple's
  com.apple.DriverKit-AppleEthernetE1000.dext personality).
- Source: https://github.com/stefb69/RTL812xLucy branch `rtl8127`, file
  `RTL8127Dext/RTL8127Dext/RTL8127Driver.cpp` (the hardware layer is the
  validated kext's code compiled verbatim).

## What works

- Interface `en10` is created (`IOSkywalkLegacyEthernetInterface` over
  `IOSkywalkLegacyEthernet`, same classes Apple's own Wi-Fi dext shows for en0).
- DHCP binds, ARP/NDP work, IPv4 and IPv6 both reach the Internet.
- Everything that uses **BSD sockets** works at line rate: iperf3 9.35 Gbit/s
  receive, 8.5 Gbit/s transmit, curl, Chromium-based browsers, ping, dig, a
  130 MB download byte-identical to the same download over Wi-Fi. TX and RX
  checksum offload, TSO, multicast filtering and jumbo frames are implemented.

## What fails

Every connection made through **Network.framework / libusrtcp (Skywalk
"channel flows")** times out when `en10` is the default route: `nscurl`,
codesign's timestamp XPC service, apsd, exchangesyncd, helpd,
AddressBookSource... The TCP handshake never completes.

`sudo skywalkctl flow -I en10` (excerpt, during an `nscurl` attempt):

```
Proto Local                       Remote                     InBytes OutBytes InPkts OutPkts SvC State                     Process
tcp6  [our global v6].60483       2620:149:981:603::10.http  0       490      0/0    7/0     BE  SYN_SENT  CLOSED          nscurl.11868
tcp4  192.168.75.172.64562        17.32.213.161.http         0       350      0/0    7/0     BE  SYN_SENT  CLOSED          nscurl.11868
tcp4  192.168.75.172.64569        17.188.185.134.5223        0       150      0/0    3/0     RD  SYN_SENT  CLOSED          apsd.140
tcp6  [our global v6].50396       2600:1901:0:9e23::.https   15232   0        119/117 0/0    BE  (kernel flow)             kernel_task.0(claude.1867)
```

User flows: N SYNs out, 0 bytes in, state SYN_SENT. Kernel (BSD socket) flows
on the same interface: InPkts counted, i.e. flowswitch RX classification works.

## Where the SYNs are, and are not, seen

1. `sudo tcpdump -i en10 'tcp port 80'` (BPF tap = kernel host path, before the
   driver) shows the user-flow SYNs leaving once per second with correct
   headers (`[SEW]` then `[S]`, mss, wscale, TS, sackOK), and **no SYN-ACK ever
   coming back**.
2. The dext instruments its TX dequeue action (the only path to the hardware):
   it logs every TCP frame involving port 80 in either direction, every frame
   with an unexpected length or ethertype, and per-queue dequeue counters.
   During the same attempts: **no port-80 frame is ever handed to the driver**,
   not even the SYN. Port-443 SYNs from BSD sockets are logged fine by the same
   code (before the filter was narrowed to port 80). The only "odd" frames are
   ARP requests (ethertype 0x0806, 42 bytes, service class CTL).
3. `skywalkctl flow-switch -I en10 -v` TX section during attempts: "copied
   pkt -> pkt" increases (the flowswitch copies user packets into the netif),
   "total Tx packets" stays 0, no TX drop counter moves. RX section: "dropped,
   flow lookup failure" grows slowly (+40..80 per attempt) but tcpdump shows no
   inbound port-80 packet at all, so those are probably unrelated.
4. `skywalkctl interface -I en10 -Q` shows the netif queue set with RX[0] and
   TX[0..3] (BE, BK, VI, VO after the change below); all their packet counters
   stay at 0 at all times, even for BSD traffic, which flows through the
   "host" path (`TxCopyMbuf` counter in `skywalkctl interface -I en10`).

So: the SYN enters the ifnet output path (BPF sees it), but never reaches the
driver's TX submission queue, and no counter admits to dropping it.

## Driver structure relevant to TX

- Two `IOUserNetworkPacketBufferPool`s created with `CreateWithOptions`:
  TX pool 512 packets x 32 KB buffers, RX pool 1536 x 9216, both
  `maxBuffersPerPacket = 1`, `poolFlags = PoolFlagMapToDext | PoolFlagMapToDevice`,
  `dmaSpecification.maxAddressBits = 64`, owner = the IOPCIDevice.
- Queues: four `IOUserNetworkTxSubmissionQueue::withPoolAndServiceClass(txPool,
  svc, 512, id, ...)` for BE/BK/VI/VO with ids 0..3, four
  `IOUserNetworkTxCompletionQueue::withPool(txPool, 512, id, ...)`, one
  `IOUserNetworkRxSubmissionQueue::withPool(rxPool, 1024, 1024, 4, ...)`, one
  `IOUserNetworkRxCompletionQueue::withPool(rxPool, 1024, 4, ...)`.
  Registered with `registerEthernetInterface(mac, queues, 10, txPool, rxPool)`
  (NDK 2.4 variant). `setEnable(true)` is called on all queues in
  `setInterfaceEnable(true)`; `requestDequeue()` on TX queues on link up and
  after every completion batch.
- The dequeue action fills one 32-byte TX descriptor per packet (data IOVA
  from `getDataIOVirtualAddress()`, length from `getDataLength()`), rings the
  doorbell; completions go back to the completion queue of the packet's class.
- `getHardwareAssists()`: IPHdr/TCP/UDP TX checksum, RX checksum, TSO4/TSO6.
  `getTSOOptions()`: tso_mtu 32512. `setHardwareAssists(assists, mask)`
  implemented. `getTxDataOffset()` not overridden (0).
- Statistics observed with everything working over BSD sockets: only TX
  queue 0 (BE) ever receives dequeue calls; queues 1..3 never.

## Things already ruled out, with evidence

- Partial checksum requests (`kIOUserNetworkPacketTxCsumPartial`): handled in
  code, but the counter of such packets stays at 0. Not the cause.
- Advertising no TX offload at all (assists = RX checksum only): user flows
  still fail identically. Not the cause. (Side effect worth noting: with no
  offload advertised, `ifconfig en10` reports `PARTIAL_CSUM,ZEROINVERT_CSUM`
  and the netif starts using a `TxCopySum` path; with offload advertised it
  does not.)
- Only one TX queue (BE) vs four service-class queues: fails identically.
- Multicast filter: was broken (no `setMulticastAddresses` override), now
  fixed and verified with `netstat -g`; no effect on this issue.
- Data offset: `getDataOffset()` is 0 on every TX packet seen.
- pfSense/router: BSD flows to the same hosts and ports succeed at the same
  time from the same interface.
- Firewall: macOS application firewall disabled, no change.
- BPF tapping by the driver (`bpfAttach` + `bpfTapOutputPacket`): tried, it
  panics the kernel inside IOSkywalkFamily on driver replacement
  (`panic-full-2026-09-12-000508`), removed again. Not relevant to the flows.

## Newest finding, not yet tested on hardware (build 0.2.16)

`ioreg` on our `IOSkywalkLegacyEthernet` node shows `IOLinkSpeed = 0`,
`IOActiveMedium = ""`, `IOSelectedMedium = ""`, no medium dictionary, and
`ifconfig en10` fails with `SIOCGIFXMEDIA: Input/output error` and prints no
`media:`/`status:` lines. Cause found in Start(): the media list returned by
`getSupportedMediaArray()` was populated *after* `registerEthernetInterface()`,
so the framework got an empty list at registration. Fixed (list built before
registration, `getInitialMedia()` returns auto-select). Whether the missing
medium/link-speed information is what makes the netif/flowswitch drop native
TX packets is the open question; it is the only remaining difference found so
far with Apple's dexts, whose interfaces show `media: autoselect` and
`status: active`.

## Questions for the reviewer

1. Where can a Skywalk channel-flow TX packet be dropped between the BPF tap
   on the ifnet output path and the dext's `IOUserNetworkTxSubmissionQueue`
   dequeue action, without any flowswitch/netif drop counter moving? Is there a
   known requirement (media/link speed, `IOFeatures`, packet metadata such as
   `setLinkHeaderLength`, headroom via `getTxDataOffset`, pool flags such as
   `PoolFlagVirtualDevice`, `kPoolFlagIODirectionIn`) that gates native TX?
2. Is the empty medium dictionary / `IOLinkSpeed = 0` a plausible cause, i.e.
   does the flowswitch or `IOSkywalkLegacyEthernet` consult the active medium
   before transmitting native packets?
3. Is the observation "user flows show `OutPkts 7/0` in `skywalkctl flow` while
   kernel flows show `OutPkts 0/0`" consistent with user TX going through the
   netif dev ring rather than the host path on a legacy Ethernet netif, and if
   so, how is that ring supposed to reach the dext's submission queue?
4. Anything in the pool geometry (32 KB buffers, 512 packets, single buffer
   per packet, explicit map flags) known to break the native TX path while the
   mbuf copy path keeps working?

## Addendum, 2026-09-12 01:45 CEST (after build 0.2.16)

- The media-list fix changed nothing: our `IOSkywalkLegacyEthernet` node still
  shows `IOLinkSpeed 0`, empty `IOActiveMedium`; but so does Apple's Wi-Fi
  node (en0), so those properties are not the discriminator. Hypothesis
  withdrawn.
- `kern.skywalk.netif.netif_queue_stat_enable = 0`: the netif queue counters
  quoted above are disabled system-wide, their zeros mean nothing.
- Nexus provider parameters at creation (`nxprov_params_adjust` in the kernel
  log), for every netif since boot: ours is **identical** to Apple's dexts
  (`AppleBCMWLANSkywalkInterface.en0`, `AppleUserECM.en7`): `flags 0x6, req 0x0,
  rings 2/2/0/0/0, slots 2/2/0/0/0, buf 2048, nexusadv_size 112, capabs 0x4,
  max_frags 1, large_buf 0`.
- The **only difference found anywhere** is on the flowswitch nexus attached to
  the interface: `com.apple.flowswitch.en10` has `large_buf 32512` (build with
  `getTSOOptions()` tso_mtu = 32512) or `32768` (build with TSO off, TX pool
  buffers 32768), whereas every Apple interface (en0, en7, awdl0) has
  `large_buf 16384`, which is exactly `kern.skywalk.flowswitch.gso_mtu = 16384`
  (en4/5/6 show 16000). Before build 0.2.7 our TX buffers were 16 KB with
  tso_mtu 16128, i.e. `large_buf` was presumably 16128, and user flows failed
  then too. So the candidate is "large_buf must equal gso_mtu (16384)", i.e.
  TX pool buffer size and TSO mtu both 16384, not "smaller or larger than".
  Untested (waiting for the review).
- Since 0.2.16 the driver does see port-80 frames of *kernel* flows (ACK/FIN of
  a BSD-socket HTTP connection), confirming the port-80 logger works; still no
  user-flow SYN reaches it.
