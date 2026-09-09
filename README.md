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

Throughput, kext, iperf3 single TCP stream to a Linux peer on the same 10G
switch:

| Direction | Throughput |
|---|---|
| TX (Mac to peer) | ~9.35 Gbit/s |
| RX (peer to Mac) | ~9.19 Gbit/s |

That is 10GbE line rate in both directions, with negligible retransmissions.

Two drivers live in this repository:

- **`RTL812xLucy.kext`**: the kernel extension. This is the validated driver
  with the numbers above. On Apple Silicon it requires Reduced Security while
  unsigned (see below).
- **`RTL8127Dext/`**: a DriverKit system extension plus a small host app. The
  goal is a driver that installs like any other on a Mac with full security
  enabled. The code is complete and builds, but it has not been loaded on
  hardware yet: it needs the DriverKit PCI and networking entitlements from
  Apple, which are requested and pending. Details in
  [docs/DEXT-PORT.md](docs/DEXT-PORT.md).

## Install (kext, Apple Silicon)

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
  on Apple Silicon. A signed DriverKit dext is the planned fix.
- The link medium is reported as 10GBase-T even over SFP+ DAC or fiber.
  macOS has no medium constant for those; it is a display issue only.
- No thermal sensor readout on the RTL8127 (the `rtl812xtool -t` probe is
  disabled for this chip).
- The dext currently does MTU 1500 only, no jumbo frames and no Wake on LAN.

## Help wanted

- Testing on **RTL8127A / RJ45** cards (10GBASE-T). The copper path is
  ported but no one has run it yet.
- Reports from other Thunderbolt enclosures and from Mac Pro PCIe slots.
- Testing on macOS 26.6 and later.
- Once entitlements arrive: dext testers.

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
