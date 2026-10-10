# Apple's AppleEthernetRL (macOS 27): what it is, where it comes from

Analysis of 9-10 Oct 2026 on macOS 27.0.1 (26A434), from the boot kernel
collection (`kernelcache` on the Preboot volume, LZFSE payload decompressed
with `compression_tool`, fileset entry `com.apple.driver.AppleEthernetRL`,
~154 KB of code and data, 329 distinct strings). No code was disassembled;
this is based on strings, class names, log formats and the Info.plist.

## Identity

- `com.apple.driver.AppleEthernetRL` 1.0.0d1, plugin of IONetworkingFamily,
  copyright 2024, `OSBundleRequired Network-Root` (in the boot collection,
  so it matches before any third-party driver extension is registered).
- Matches `IOPCIMatch 0x812410ec&0xfffcffff` (RTL8125/8126/8127, PCI
  10ec:8124-8127), `IOPCITunnelCompatible` (Thunderbolt/USB4 enclosures),
  IOProbeScore 1000. Does **not** match 10ec:0e10.
- Depends on IOSkywalkFamily, AppleSkywalkAVB, IOTimeSyncFamily: a Skywalk
  (legacy-Ethernet bridge) driver with AVB/PTP support.
- Source files visible in assertion strings: `AppleEthernetRL.cpp`,
  `AppleEthernetRLIPC.cpp`, `AppleEthernetRL_AVB.cpp`,
  `AppleEthernetRL_Firmware.cpp`, plus `en_main.cpp` and `eq.cpp`.
- Classes: `AppleEthernetRL`, `AppleEthernetRLUserClient`,
  `AppleEthernetRLClock` (PTP clock), `AppleEthernetRLFirmwareUpdater`,
  `AppleEthernetRLIPC`, `AppleEthernetRLKDPPoller` (kernel debugger
  polling).
- Device description strings: "Realtek PCIe 2.5GbE / 5GbE / 10GbE Family
  Controller" (the wording of Realtek's Windows drivers).

## Lineage: Realtek's BSD driver, not RTL812xLucy

Fingerprints of Realtek's own **FreeBSD `re` driver** family, which Realtek
also hands to OEMs:

- Log prefix `rl::%s(%d)` ("rl"/"re" is the BSD Realtek driver naming).
- `re_check_mac_version` (a function of Realtek's FreeBSD `if_re.c`).
- Register and flag names `RE_IMR0_8125`, `RE_ISR_SYSTEM_ERR`,
  `RE_PERR_NUM_CAUSES` (`RE_` prefix as in Realtek's `if_rereg.h`).
- `re_check_link_status`, `re_get_linkchg_intr`, `re_link_state_change`,
  and FreeBSD idioms in the log formats: `sc.if_capenable` (the softc),
  `ifm_status` / `ifm_active` (ifmedia), `if_re`-style link handling.
- Descriptor paths named `enqueueRx8DWPackets` / `dequeueTx4DWLegacyPackets`
  (32-byte and 16-byte descriptor formats, as in the BSD driver).

Nothing from the Linux `r8127`/`r8125` naming that RTL812xLucy and this
fork use (`rtl8125_*`, `mdio_direct_*`, `CFG_METHOD_*`, `HwSupp*`,
`rtl8125_private`, `GiantSend*`), nothing from Mieze's kext (`RTL8125::`,
`LucyRTL8125`, `rtl812x*`, `txHangCheck`, medium tables), nothing from this
project (`RTL8127Driver`, `RTL8127Hw`, `rtl8127_fiber`). Searched over the
whole 127 MB kernel collection, not only the driver.

Apple-specific additions that exist in no Realtek or community driver:

- A firmware IPC layer: BL1/BL2/"NS" firmware versions, "FW stage",
  fuse type and hardware id, writes to "secure"/"non-secure" slots,
  "forcing P-mode". The Realtek part in the new Macs runs Apple-managed
  firmware; a retail card ("IPC not supported, skipping firmware
  initialization") is driven without it.
- mDNS offload (`GetMdnsCapabilities`), PTP/AVB over Skywalk pseudo-rings,
  parity-error interrupt handling, ASPM debug switches, KDP poller.

Conclusion: AppleEthernetRL is Apple's own driver built on Realtek-supplied
reference code (BSD lineage) for the Realtek parts Apple now solders in
Macs; third-party RTL8125/8126/8127 cards are covered as a side effect of
the PCI match. It owes nothing to RTL812xLucy or to this fork.

## What it does not do (measured on the RTL8127ATF SFP+ card)

- No fiber/SFP+ awareness at all: no `fiber`, `SFP`, `SerDes` or `sds`
  string anywhere in the driver. The SFP+ card works because the RTL8127ATF
  firmware brings the 10GBASE-R SerDes up on its own; the driver treats the
  link as copper and reports 10Gbase-T. Wake-on-LAN (magic packet, wake
  ports, NIC proxy) is implemented.
- No jumbo frames: `IOMaxPacketSize = 1518`, MTU capped at 1500.
- Line rate otherwise: 9.2 Gbit/s TX, 9.1 RX, 9.1 TX with 8 streams, 9.4
  RX with 8 streams; media shown as 10Gbase-T.
- Cannot be displaced once attached: `IOCatalogueTerminate` and
  `kmutil unload -c AppleEthernetRL` are no-ops on a boot-collection kext,
  with or without the interface down. At boot it wins even when the dext
  personality is registered first (16:38 boot: dext listed at +7 s, card
  matched by Apple's kext at +10 s). Only a hot-plug after boot hands the
  card to a higher-score dext.
