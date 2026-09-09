# Release notes draft: next tag

Tag proposal: `v1.1.2-rtl8127.beta3` or `v1.2.0-rtl8127` once the dext has
moved 10G traffic on a card. beta2 (10 Sept 2026) shipped the notarized pkg/zip. The CI workflow builds the kext,
dext, host app and installer .pkg and attaches them to the release; paste
this text as the release body (it replaces the generic body in build.yml for
this release).

---

RTL8127 / RTL8127ATF 10GbE support for Apple Silicon Macs.

**What works**

- RTL8127ATF (SFP+): validated on an M5 Mac, macOS <VERSION>, Thunderbolt
  PCIe enclosure, SFP+ DAC to a 10G switch. iperf3 single stream
  ~9.35 Gbit/s TX / ~9.19 Gbit/s RX.
- RTL8125 / RTL8126 unchanged from upstream RTL812xLucy 1.1.2.

**Changes since beta2**

- <kext fixes for macOS 26.6, if any>
- Dext: TCP segmentation offload (IPv4/IPv6), jumbo frames up to 9000,
  interrupt mitigation ported from the kext. <hardware status>
- Kext: RX length field read on 14 bits; frames over 8191 bytes (jumbo on
  Apple Silicon) were truncated.
- <if the dext reached line rate on hardware: this is the last release that
  ships the kext; it stays in the tree as the upstream PR vehicle.>
- README rewritten with step-by-step Apple Silicon install instructions,
  tested-setups table and known limitations.
- Issue templates: hardware reports are now the way to tell us what works.

**Assets**

- `RTL812xLucy-*.kext.zip`: the kext, universal x86_64 + arm64e. Unsigned:
  Reduced Security + user kext management + `csrutil disable` on Apple
  Silicon. Install steps in the README.
- `RTL8127App-*.app.zip` / `RTL8127-*.pkg`: host app embedding the DriverKit
  extension. <signed / unsigned, loads only with developer mode>
- `RTL8127Dext-*.dext.zip`: the standalone dext + dSYM.

**Known limitations**

- Link medium displayed as 10GBase-T over SFP+/DAC (cosmetic).
- RTL8127A (RJ45) path ported but untested. Reports welcome.
- Dext: no Wake on LAN.
