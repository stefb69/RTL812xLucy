# Release notes: v1.1.2-rtl8127.beta2 (dext 0.2.27, 14 Sept 2026)

Releases are published by hand with the notarized installer built by
`packaging/sign-and-notarize.sh`; CI only builds and checks. The text below
is the release body.

---

RTL8127 / RTL8127ATF 10GbE support for Apple Silicon Macs, as a signed and
notarized DriverKit extension. Download `RTL8127-0.2.27.pkg`, run it, approve
the driver once in System Settings, plug the card in. No recoveryOS, no
Reduced Security, no `csrutil` changes.

**Validated on hardware** (M5 Mac, macOS 26.6, RTL8127ATF SFP+ in a
Thunderbolt enclosure, DAC to a 10G switch, Linux iperf3 peer):

| Test | MTU 1500 | MTU 9000 |
|---|---|---|
| TX, 1 stream | 9.40 Gbit/s | 9.63 Gbit/s |
| RX, 1 stream | 9.38 Gbit/s | 9.89 Gbit/s |
| TX, 4 streams | 9.22 Gbit/s | 9.65 Gbit/s |
| RX, 4 streams | 9.39 Gbit/s | 9.89 Gbit/s |
| TX, 8 streams | 4.5 Gbit/s | 9.71 Gbit/s |
| RX, 8 streams | 9.39 Gbit/s | 9.77 Gbit/s |

DHCP, IPv4/IPv6, multicast (mDNS/Bonjour), Network.framework and BSD socket
traffic, TCP segmentation offload (IPv4/IPv6), checksum offload, jumbo frames
up to 9000, all exercised on the card.

**What is in the package**

- `RTL8127App.app` (Applications): installs and removes the driver, shows
  card detection, driver state and link, in English, French, German, Spanish,
  Italian, Japanese and Simplified Chinese.
- `net.wizzz.RTL8127Dext.dext` inside it: the DriverKit driver, Developer ID
  signed, notarized, stapled.

**Changes since beta1** (kext-only, June 2026)

- DriverKit port of the RTL8127 driver: hardware layer shared verbatim with
  the kext, NetworkingDriverKit datapath with four TX queues (service
  classes), TSO4/6, checksum offload, multicast hash filter, kext-style
  interrupt mitigation, hardware statistics.
- Signed with Apple's DriverKit entitlements (PCI transport, networking
  family), notarized installer package.
- Kext: RX length field read on 14 bits (frames over 8191 bytes were
  truncated). The kext is no longer shipped in releases; build it from
  source if you need it.
- README rewritten as an install guide, issue templates (hardware reports),
  benchmark tool in `tools/`.

**Fixed in 0.2.23** (13 Sept 2026, replaces the 0.2.22 package first
published under this tag): reception stopped for good after the link
dropped and came back (a 1-2 s link flap on the DAC or switch), until the
card was replugged. The chip is soft-reset on every link drop and restarts
its rx ring at descriptor 0; the driver now restarts both rings with it, and
a 5-second safety net re-arms the rx ring if reception ever stalls again.
If you installed 0.2.22, run the new package; the card may need a replug
once to switch to the new driver instance.

**Fixed in 0.2.27** (14 Sept 2026): RTL8127 RJ45 (10GBASE-T) cards linked
at 5G only. The PHY code inherited from the RTL8125/8126 driver never
advertised 10GBASE-T during auto-negotiation; it now does, like Realtek's
r8127 Linux driver. Reported by a board vendor's macOS test; not yet
re-tested on a copper card (the SFP+ path is unchanged). If you have an RJ45
RTL8127, please try it and open a hardware report.

**Fixed in 0.2.26** (13 Sept 2026): Energy Efficient Ethernet was left
enabled on the SFP+ SerDes link (the kext and Realtek's Linux driver keep
it off there). Symptoms: a steady trickle of rx CRC errors when the link
was idle and a 1-2 s link drop every 20-40 minutes. EEE is now off. A link
drop is also confirmed by a second PHY read before the chip is reset.

**Known limitations**

- Eight or more parallel TCP streams sending from the Mac at MTU 1500 top
  out around 4.5 Gbit/s (per-packet cost in the dext). Not an issue at MTU
  9000 or with up to four streams.
- Link medium displayed as 10GBase-T over SFP+/DAC (cosmetic).
- RTL8127A (RJ45) path ported but untested. Reports welcome.
- No Wake on LAN. tcpdump only sees kernel-path traffic (the BPF tap API
  panics macOS 26 inside IOSkywalkFamily, so it is not used).

**Upgrading from an earlier 0.2.x package**: run the pkg; if the card does
not come back, unplug and replug it (the previous driver instance can hold
the device until then).

Checksums (SHA-256):

- `RTL8127-0.2.27.pkg`: e94b2592aedaf4c2b31de470720a2966edf27cb0b3d016b75e25f72d31fa5861
