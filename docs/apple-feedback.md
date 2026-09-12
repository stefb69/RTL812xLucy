# Apple feedback: Feedback Assistant reports and a Developer Forums post

Three findings from shipping the RTL8127 NetworkingDriverKit driver that
Apple's DriverKit team should hear about, written up ready to paste. File
them in Feedback Assistant (feedbackassistant.apple.com, or the app), area
**Developer Technologies & SDKs**, topic **DriverKit** (pick "Incorrect or
unexpected behavior" for the first, "Documentation" for the other two). Once
filed, put the FB numbers back here and in the forum post below, and link this
file from the README's Known limitations.

Environment common to all three: MacBook Pro (M5), macOS 26.6.2 (25G83),
Xcode 26.x, DriverKit SDK 25.5, NetworkingDriverKit `IOUserNetworkEthernet`
subclass, dext signed with the `driverkit`, `driverkit.transport.pci` and
`driverkit.family.networking` entitlements. Device: Realtek RTL8127ATF PCIe
10GbE NIC (vendor 0x10ec, device 0x0e10) in a Thunderbolt PCIe enclosure.
Source: https://github.com/stefb69/RTL812xLucy (directory `RTL8127Dext/`).

---

## FB 1: Kernel panic in IOSkywalkFamily when a NetworkingDriverKit dext that called bpfAttach() is replaced

**Title:** IOSkywalkFamily kernel panic (null dereference) on dext replacement after IOUserNetworkEthernet::bpfAttach()

**Area:** DriverKit / NetworkingDriverKit. **Type:** Incorrect/unexpected behavior. **Severity:** kernel panic triggered by a signed, entitled dext.

**Description**

A NetworkingDriverKit dext (subclass of `IOUserNetworkEthernet`) calls
`bpfAttach()` at the end of its `Start()` so that tcpdump sees every frame
on the interface, including Network.framework (native Skywalk) flows that
the BSD host path does not carry. This works on the first activation.
When the dext is then replaced by a new version (OSSystemExtensionRequest
activation of the same bundle identifier, sysextd terminates the old
instance and starts the new one) while tcpdump is attached to the
interface, the kernel panics inside IOSkywalkFamily with a data abort at
address 0x20 (null pointer plus offset). The panicked task is the dext
process itself, on its default dispatch queue; the user-space frames
symbolicate to `Start_Impl` calling `bpfAttach()`.

A dext should not be able to bring the kernel down. Either `bpfAttach()`
should fail gracefully when the interface is in that state, or the kernel
side should not dereference a stale/null BPF attachment during teardown.

**Steps to reproduce**

1. Build the dext at commit `fa48a8c` of the repository above (the last
   version that calls `bpfAttach()`), install it through the host app.
2. Plug the NIC in, wait for the interface to come up (en10 here).
3. `sudo tcpdump -i en10 -n` in a terminal, leave it running.
4. Install the next build of the same dext (any version bump), which makes
   sysextd replace the running instance.

**Expected:** the old instance stops, the new one starts, tcpdump exits or
keeps capturing.

**Actual:** kernel panic. Report: `panic-full-2026-09-12-000508.0002.panic`,
incident DD33A666-F6C7-4914-B9F8-B0922FFB3C0D, macOS 26.6.2 (25G83), kernel
xnu-12377.161.14~5, panic string:

```
panic(cpu 0 caller 0xfffffe003c373e6c): Kernel data abort. at pc 0xfffffe003bcff220,
lr 0x34e17e003bcff094 ... esr: 0x0000000096000006  far: 0x0000000000000020
Panicked task 0xf5fffe2e999daae8: 322 pages, 5 threads: pid 26223: net.wizzz.RTL8127Dext
```

Frames 13 to 15 of the panicked thread are inside
`com.apple.iokit.IOSkywalkFamily(1.0)[FD45D0FB-9615-3FBD-B687-FCC1BCBC0BD9]`
(lr 0xfffffe003ae30908, 0xfffffe003ae30130, 0xfffffe003ae2f678; image
loaded at 0xfffffe003ae00730), called from the dext RPC path.

**Workaround:** the driver no longer calls `bpfAttach()` (commit `f31c309`).
Cost: tcpdump on the interface only sees the kernel host path's frames, not
native Skywalk flows, which makes debugging Network.framework traffic
through a dext very hard.

**Attachments:** the `.panic` file above, a sysdiagnose taken after reboot,
link to the source at `fa48a8c`.

---

## FB 2: Documentation: TX packets from native Skywalk flows carry a non-zero data offset that the address getters do not include

**Title:** IOUserNetworkPacket::getDataVirtualAddress()/getDataIOVirtualAddress() return the buffer base; native-flow TX packets have a 2-byte data offset that is nowhere documented

**Area:** DriverKit / NetworkingDriverKit. **Type:** Documentation (and API suggestion).

**Description**

In a `IOUserNetworkEthernet` driver, TX packets dequeued from the
`IOUserNetworkTxSubmissionQueue` come from two sources: the BSD host path
(sockets, ARP, DHCP, ping) and native Skywalk flows from Network.framework
(Safari, nscurl, URLSession, codesign/notarytool, most system daemons).
Packets from the host path have `getDataOff() == 0`. Packets from native
flows arrive with `getDataOff() == 2`: the Ethernet frame starts two bytes
into the buffer (presumably so that the IP header is 4-byte aligned).

`getDataVirtualAddress()` and `getDataIOVirtualAddress()` return the
address of the **buffer**, not of the data. A driver that hands
`getDataIOVirtualAddress()` and `getDataLength()` to its DMA engine, which
is the obvious reading of those names, transmits two bytes of garbage
followed by a truncated frame for every native-flow packet. BSD traffic
works perfectly, so the failure is very confusing: DHCP, ping, curl and
ssh work, while Safari, App Store, Network.framework clients and
`codesign --timestamp` hang in SYN_SENT. `skywalkctl flow` shows the flows,
the flowswitch counters show nothing dropped, and the frames on the wire
are only visible from the peer side.

Neither the NetworkingDriverKit headers nor the sample code mention the
data offset in the TX path. The fix in our driver is one line
(frame = base + `getDataOff()`, same for the IOVA) and it took several days
and an external review to find.

**Suggestions**

1. Document in `IOUserNetworkPacket.h`, next to `getDataVirtualAddress`,
   `getDataIOVirtualAddress` and `getDataOff`, that the first two return the
   buffer base and that TX data starts at `getDataOff()` bytes into it, and
   that native flows do use a non-zero offset.
2. Better: add `getDataIOVirtualAddress()`/`getDataVirtualAddress()`
   variants that already include the offset, or make the existing ones do
   so (RX packets prepared by the driver set the offset themselves, so the
   semantics are already asymmetric).
3. Mention it in the NetworkingDriverKit sample or in a TechNote: it is the
   one thing that separates "works with ping and curl" from "works with
   Safari".

**Steps to reproduce:** any `IOUserNetworkEthernet` driver that ignores
`getDataOff()` on TX; open Safari on the interface while `curl --interface`
works. Reference fix: commit `89e93d0` and
`RTL8127Dext/RTL8127Dext/RTL8127TxBuffer.h` in the repository above.

---

## FB 3: Documentation: GetMaxTransferUnit()/getMaxTransferUnit() is the maximum MTU, not the current one

**Title:** IOUserNetworkEthernet::getMaxTransferUnit() semantics undocumented: it is the maximum supported MTU and locks the interface MTU

**Area:** DriverKit / NetworkingDriverKit. **Type:** Documentation.

**Description**

`IOUserNetworkEthernet` has `setMaxTransferUnit(uint32_t)` /
`getMaxTransferUnit()` (NDK 2.1) and the deprecated `SetMTU()` /
`GetMaxTransferUnit()`. The natural reading, reinforced by the pairing with
a setter, is that the getter returns the **current** MTU. It does not:
IOSkywalkFamily calls it once at `registerEthernetInterface()` time, uses
the result as the interface's **maximum** MTU, publishes it as
`IOMaxPacketSize` (value + 18) on the `IOSkywalkLegacyEthernet` node and
`IOMaxTransferUnit` on the interface, and any `SIOCSIFMTU` above it fails
with EINVAL in the kernel without the dext ever being called.

A driver that returns its current MTU (1500) therefore ships with jumbo
frames silently impossible: `sudo ifconfig en10 mtu 9000` answers
`ifconfig: ioctl (set mtu): Invalid argument`, and there is no log, no
callback and no hint pointing at the driver.

**Suggestions**

1. Document on both getters that the value is the maximum MTU the
   hardware supports and that it is read once at interface registration.
2. Consider a distinct name or a dedicated property for the maximum, since
   the pair set/get with the same name strongly suggests current value.

**Reference fix:** commit `fceaf8b` in the repository above: the getters
return the hardware maximum (9000), `setMaxTransferUnit()` tracks the
current value.

---

## Apple Developer Forums post

**Forum:** Developer Forums, tags **DriverKit**, **NetworkingDriverKit**,
**System Extensions**. Post after the FB numbers exist; replace `FBxxxxxxxx`.

**Title:** Lessons learned shipping an open-source NetworkingDriverKit NIC driver (Realtek RTL8127, 10GbE)

**Body:**

I've just shipped a signed NetworkingDriverKit driver for the Realtek
RTL8127 10GbE PCIe NICs on Apple Silicon, source at
https://github.com/stefb69/RTL812xLucy (directory `RTL8127Dext`). It runs at
line rate (9.4 Gbit/s each way at MTU 1500, 9.9 with jumbo frames) with
TSO, checksum offload and four TX queues by service class. Since there are
very few public NetworkingDriverKit drivers to learn from, here is what
cost me the most time, in case it saves someone else a week. Three of these
are filed as feedback.

1. **TX packets from native Skywalk flows have a 2-byte data offset**
   (FBxxxxxxxx). `getDataVirtualAddress()` / `getDataIOVirtualAddress()`
   return the buffer base; the frame starts at `getDataOff()`. BSD-path
   packets (ping, curl, ssh, DHCP) have offset 0, Network.framework flows
   (Safari, URLSession, App Store, codesign --timestamp) have offset 2. If
   you DMA from the base, everything "works" except every modern client,
   which sits in SYN_SENT. The headers don't mention it.

2. **`getMaxTransferUnit()` is the maximum MTU, not the current one**
   (FBxxxxxxxx). It is read once at `registerEthernetInterface()` and
   becomes the hard ceiling for `ifconfig mtu`; return your current 1500
   and jumbo frames fail with EINVAL before your dext is called.

3. **Don't call `bpfAttach()` on macOS 26.6** (FBxxxxxxxx). It worked once,
   then panicked the kernel inside IOSkywalkFamily when the dext was
   replaced while tcpdump was attached. Without it, tcpdump on your
   interface only sees host-path frames, not native flows, so debugging
   point 1 is done from the peer side.

4. Smaller ones: the personality needs `IOClass = IOUserNetworkEthernet`
   and `CFBundleIdentifierKernel = com.apple.iokit.IOSkywalkFamily`, not
   `IOUserService`, or `super::Start` fails with 0xe00002bc. All queues are
   created disabled: `setEnable(true)` in `setInterfaceEnable()`, plus
   `requestDequeue()` on the TX queues when the link comes up.
   `setMulticastAddresses()` must be implemented or no multicast group is
   ever joined (mDNS and IPv6 solicited-node are silently dead). Release
   dispatch sources from the `Cancel()` completion block, not right after
   `Cancel()`, or the dext crashes at every upgrade. The dext bundle must be
   named `<bundle id>.dext` or the host app reports "Extension not found in
   App bundle". Dext `os_log` lines show up as `kernel:` messages with the
   `.dext` bundle as sender; use `%{public}s`.

5. Performance question for Apple engineers: with eight or more parallel
   TCP senders at MTU 1500 the stack emits ~3 KB TSO packets at ~160k
   packets/s and the dext saturates one core around 4.5 Gbit/s (fine at
   MTU 9000, fine with one to four streams). Is `IOUserNetworkPacketPoller`
   the intended answer for per-packet cost in a NIC dext, and is there any
   guidance on batch sizes for `IOUserNetworkTxSubmissionQueue` dequeues?

Happy to share more details or test builds if anyone from the
NetworkingDriverKit team is interested; the whole history is in the repo.
