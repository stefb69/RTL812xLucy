# RTL8127 / RTL8127ATF 10GbE driver for Apple Silicon Macs

Open-source macOS driver for the Realtek **RTL8127** family of 10 Gigabit
Ethernet controllers, the sub-$50 PCIe 10GbE NICs that shipped in late 2025.
It runs on **Apple Silicon** (arm64e kext, DriverKit dext in progress) in a
PCIe slot or a Thunderbolt enclosure, and reaches **10GbE line rate**.

This is a fork of [Mieze/RTL812xLucy](https://github.com/Mieze/RTL812xLucy)
by Laura Müller. The RTL8127 hardware code is ported from Realtek's official
`r8127` Linux driver. GPLv2, like the original. An upstream pull request is
open ([Mieze/RTL812xLucy#6](https://github.com/Mieze/RTL812xLucy/pull/6));
until it lands, this fork is maintained on its own.

## Why

Realtek RTL8127 cards are the cheapest way to get 10GbE today, and Linux
(6.16+), TrueNAS and Windows support them out of the box. macOS does not:
Realtek ships no macOS driver, so a Mac next to a 10G NAS still needs a
$150-250 Aquantia/Marvell Thunderbolt adapter. This driver closes that gap for
Apple Silicon Macs, including the SFP+ variant (RTL8127ATF) for DAC and fiber.

## Status

| Hardware | Status |
|---|---|
| RTL8127ATF (SFP+, PCI `10ec:0e10`) | **Validated.** DHCP, 10G full duplex over SFP+ DAC, multi-GB transfers both ways, zero errors. |
| RTL8127A / RTL8127 (10GBASE-T RJ45, PCI `10ec:8127`) | Same silicon, copper PHY path ported from `r8127`, not yet tested on a card. Testers wanted. |
| RTL8125 / RTL8126 (2.5G / 5G) | Unchanged from upstream RTL812xLucy. |

| Platform | Status |
|---|---|
| Apple Silicon (M-series), macOS 26.5 | **Validated** on an M5 Mac, card in a Thunderbolt enclosure. |
| Apple Silicon, macOS 26.6 | Re-validation in progress. |
| Intel Mac / hackintosh (x86_64) | Builds universal; RTL8127 path not tested on Intel. |

Throughput, iperf3 to a Linux peer on the same 10G switch, MTU 1500:

| Test | Dext (recommended) | Kext |
|---|---|---|
| TX, 1 stream | 9.40 Gbit/s | 9.35 Gbit/s |
| RX, 1 stream | 9.38 Gbit/s | 9.19 Gbit/s |
| TX, 4 streams | 9.22 Gbit/s | not measured |
| RX, 4 streams | 9.39 Gbit/s | not measured |
| RX, 8 streams | 9.39 Gbit/s | not measured |
| TX, 8 streams | 4.5 Gbit/s | not measured |

With jumbo frames (`sudo ifconfig en10 mtu 9000`, peer and switch at 9000),
dext 0.2.22:

| Test | Dext |
|---|---|
| TX, 1 stream | 9.63 Gbit/s |
| RX, 1 stream | 9.89 Gbit/s |
| TX, 4 streams | 9.65 Gbit/s |
| RX, 4 streams | 9.89 Gbit/s |
| TX, 8 streams | 9.71 Gbit/s |
| RX, 8 streams | 9.77 Gbit/s |

Line rate in both directions with no retransmissions. The one exception is
eight or more parallel *transmit* streams at MTU 1500, where per-packet
overhead in the dext caps the aggregate (see Known limitations); with jumbo
frames that case runs at line rate too.

Two drivers live in this repository:

- **`RTL812xLucy.kext`**: the kernel extension. This is the validated driver
  with the numbers above. On Apple Silicon it requires Reduced Security while
  unsigned (see below).
- **`RTL8127Dext/`**: a DriverKit system extension (dext) plus the host app
  that installs it. Signed with Developer ID and notarized, so it installs
  on a Mac with **full security enabled**: open the app, approve the driver
  once in System Settings, done. The app then shows whether a card is
  detected, whether the driver is attached and the link state, in English,
  French, German, Spanish, Italian, Japanese and Simplified Chinese. The
  install flow is validated; the datapath has not yet been exercised on a
  card with the dext (the kext has). Details in
  [docs/DEXT-PORT.md](docs/DEXT-PORT.md).

## Install (dext, recommended)

Download `RTL8127-*-notarized.pkg` from
[Releases](https://github.com/stefb69/RTL812xLucy/releases) and run it. It
puts **RTL8127App** in Applications and opens it. The app installs the
driver by itself; macOS asks you to allow it once and the app opens the
right System Settings pane for that (General, Login Items & Extensions,
Driver Extensions: turn on RTL8127App). No recoveryOS, no security
changes. Plug in the card and the app shows the link state; configure the
interface in System Settings, Network like any Ethernet port.

To remove the driver, open the app and click "Remove driver". Then delete
the app.

## Install (kext, advanced)

The kext is the driver that has been validated at line rate on hardware.
Download the latest `RTL812xLucy-*.kext.zip` from
[Releases](https://github.com/stefb69/RTL812xLucy/releases), or build it
(see below). The kext is unsigned, so macOS must be allowed to load it:

1. Shut down, then hold the power button until "Loading startup options"
   appears. Open **Options** to boot into recoveryOS.
2. Utilities menu, **Startup Security Utility**. Select your system disk,
   choose **Reduced Security** and tick **Allow user management of kernel
   extensions from identified developers**.
3. Still in recoveryOS, open Terminal and run `csrutil disable`. This is
   only needed while the kext is unsigned.
4. Reboot into macOS. Copy the kext:

   ```bash
   sudo cp -R RTL812xLucy.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/RTL812xLucy.kext
   sudo kmutil install --volume-root / --update-all
   ```

5. Open **System Settings, Privacy & Security**, approve the extension when
   prompted, and reboot again.

After the reboot the card shows up in **System Settings, Network** as a
10GBase-T interface (macOS has no SFP+/DAC medium type, so that label is
cosmetic). Set it to DHCP or a manual address like any Ethernet port.

To remove the driver, delete `/Library/Extensions/RTL812xLucy.kext`, run
`sudo kmutil install --volume-root / --update-all`, and re-enable full
security in recoveryOS if you want.

## Build

Xcode 26 or later.

```bash
xcodebuild -project RTL812xLucy.xcodeproj -target RTL812xLucy \
  -configuration Release build CODE_SIGNING_ALLOWED=NO
```

The result is a universal x86_64 + arm64e kext in `build/Release/`. The
DriverKit dext and its host app build from `RTL8127Dext/RTL8127Dext.xcodeproj`
(target `RTL8127App`). GitHub Actions builds both on every push and attaches
them to tagged releases.

The `KernelSPI/` overlay headers re-insert the IONetworkingFamily polled-mode
SPI that Apple strips from the public SDK; regenerate them with
`KernelSPI/gen_spi_headers.py` after a major SDK update.

## Tested setups

| Mac | macOS | Card | Enclosure | Link |
|---|---|---|---|---|
| M5 | 26.5 | RTL8127ATF (SFP+) | Thunderbolt PCIe enclosure | 10G, SFP+ DAC to a 10G switch |

If it works (or does not) for you, please open a
[hardware report](https://github.com/stefb69/RTL812xLucy/issues/new?template=hardware-report.yml)
with your Mac, macOS version, card, enclosure and link type. That table is
built from reports.

## Known limitations

- The kext is unsigned: Reduced Security and `csrutil disable` are required
  on Apple Silicon. Use the signed dext unless you need the kext.
- The link medium is reported as 10GBase-T even over SFP+ DAC or fiber.
  macOS has no medium constant for those; it is a display issue only.
- No thermal sensor readout on the RTL8127 (the `rtl812xtool -t` probe is
  disabled for this chip).
- The dext has no Wake on LAN.
- Dext, many parallel transmit streams (8+) at MTU 1500: the aggregate drops
  to about half the link rate (not at MTU 9000). With that many streams TCP emits small TSO packets
  (~3 KB) and the dext's per-packet cost saturates one core. Normal use,
  including four parallel streams, runs at line rate. Planned fix: the
  NetworkingDriverKit packet poller (polling under load, as the kext does).
- tcpdump on the dext interface only sees frames handled by the kernel's
  own path, not Network.framework flows: the BPF tap API panics macOS 26.6
  inside IOSkywalkFamily, so the driver does not use it.

## Help wanted

- Testing on **RTL8127A / RJ45** cards (10GBASE-T). The copper path is
  ported but no one has run it yet.
- Reports from other Thunderbolt enclosures and from Mac Pro PCIe slots.
- Testing on macOS 26.6 and later.
- Dext reports: the signed dext installs cleanly, but nobody has pushed
  10G traffic through it yet. If you have a card, please try it and report.

## Diagnostics

`rtl812xtool/main.c` builds a small command-line tool that reads the
hardware statistics counters from the running driver:

```bash
clang -o rtl812xtool rtl812xtool/main.c -I RTL812xLucy -framework IOKit -framework CoreFoundation
sudo ./rtl812xtool -s en10
```

Driver-side counters are in `ioreg -r -n en10 -l` under `IONetworkStatsKey`.
Kext start-up logs are only visible through `sudo dmesg`.

## Credits

- [Laura Müller (Mieze)](https://github.com/Mieze) for RTL812xLucy and the
  whole LucyRTL8125 lineage this builds on.
- Realtek for the GPL `r8127` Linux driver the RTL8127 support is ported from.
- The Apple Silicon DMA lessons (DART, `IODMACommand`, arm64e SPI) are
  documented in the commit history and in
  [docs/DELTAS-8126-8127.md](docs/DELTAS-8126-8127.md).

---

# Upstream README (RTL812xLucy)

A new macOS driver for the Realtek RTL812x family of 2.5GBit and 5Gbit Ethernet Controllers

**Please direct support requests to the project's thread on [Insanelymac.com](https://www.insanelymac.com/forum/topic/362326-rtl812xlucy-for-the-realtek-rtl812x-family/)**


**Key Features of RTL812xLucy**

* Supports all versions of Realtek's RTL8125 2.5Gbit and RTL8126 5Gbit Ethernet Controllers:
  - RTL8125A
  - RTL8125B
  - RTL8125BP
  - RTL8125CP
  - RTL8125D
  - RTL8126A
* Support for AppleVTD (Tahoe included), but also works without it.
* TCP segmentation offload with IPv4 and IPv6.
* Support for TCP/IPv4, UDP/IPv4, TCP/IPv6 and UDP/IPv6 checksum offload.
* Supports jumbo frames up to 9000 bytes.
* Fully optimized for Catalina. Note that older versions of macOS might not support 2.5Gbit and 5Gbit Ethernet.
* Support for Energy Efficient Ethernet (EEE).
* Support for VLAN tagging in hardware.
* The driver is published under GPLv2.

**Current Status**

* The driver has been successfully tested with Tahoe, Sequoia and Monterey but should work fine with Catalina and above.
* The performance problem when using TCP segmentation offload (TSO) has been fixed in version 1.1.0. TSO is now working at line speed.

**A word on AppleVTD**

Although RTL812x supports AppleVTD, there is no guarantee that your mainboard also does. In case you are unsure if you need AppleVTD, leave it disabled and you'll be on the safe side. When you enable AppleVTD and experience one of the following issues, it's most likely that your board doesn't support AppleVTD:

- Kernel Panics.
- The machine suddenly reboots, freezes and/or the fans speed up.
- No network connection at all.
- The link status keeps going up and down.
- Very low connection throughput.

**What can you do to resolve the issue?**
- Check your board's DMAR table and see if there are any reserved memory regions in it.
- If there are reserved memory regions, you might want to patch your DMAR removing these regions. If it resolves the issue, congratulations! Be careful, because the board's manufacturer did add these regions with intention. Removing them may produce unexpected results too, like the problems described above.
- Otherwise you have to keep AppleVTD disabled, because it is incompatible with your board and there is no way to make it compatible.

**Installation**
- Use OpenCore to inject the driver.
<img width="683" height="145" alt="Bildschirmfoto 2026-02-06 um 20 33 23" src="https://github.com/user-attachments/assets/3a75f549-1ba2-4059-8bb4-d89b8b8b06ed" />

**Contributions**

If you find my projects useful, please consider to buy me a cup of coffee: https://buymeacoffee.com/mieze

Thank you for your support! Your contribution helps me to continue development.
