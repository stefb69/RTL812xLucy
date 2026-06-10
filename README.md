# RTL8127ATF driver for macOS (Apple Silicon)

This is a fork of [Mieze/RTL812xLucy](https://github.com/Mieze/RTL812xLucy) by
Laura Müller, extended with experimental support for the Realtek **RTL8127**
10GbE controller, in particular the **RTL8127ATF** fiber (SFP+) variant, and
with an **arm64e** build for Apple Silicon Macs (PCIe slot or Thunderbolt
enclosure, `IOPCITunnelCompatible`). The hardware-specific code is ported from
Realtek's official `r8127` Linux driver. GPLv2, like the original.

Status: builds universal (x86_64 + arm64e), all symbols resolve against the
macOS 26.5 arm64e kernel; not yet validated on real RTL8127 hardware.

Build: `xcodebuild -project RTL812xLucy.xcodeproj -target RTL812xLucy
-configuration Release build CODE_SIGNING_ALLOWED=NO`

Loading an unsigned development kext on Apple Silicon requires reduced
security: boot into recoveryOS, Startup Security Utility → Reduced Security +
allow user management of kernel extensions, plus `csrutil disable` for an
unsigned build. Then copy the kext to `/Library/Extensions`, approve it in
System Settings → Privacy & Security and reboot.

The `KernelSPI/` overlay headers re-insert the IONetworkingFamily polled-mode
SPI that Apple strips from the public SDK (see `KernelSPI/gen_spi_headers.py`).

---

# RTL812xLucy

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

