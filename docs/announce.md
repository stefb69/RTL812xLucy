# Announcement drafts

Ready-to-post texts for the launch. Nothing here is published automatically.
Post only after the validation pass (kext on macOS 26.6, dext on hardware) and
after a fresh tagged release exists. Replace `<TAG>` with the release tag and
`<DEXT STATUS>` with one of the two sentences at the bottom.

Order: message to Jeff Geerling and comment on his post first, then Show HN
and r/homelab the same morning (US weekday morning), then the other
communities over the following days.

---

## 1. Show HN

**Title** (80 chars max):

    Show HN: Open-source 10GbE driver for Realtek RTL8127 NICs on Apple Silicon Macs

**URL:** https://github.com/stefb69/RTL812xLucy

**First comment (post it right after submitting):**

Hi HN. Realtek's RTL8127 is the sub-$50 10GbE PCIe NIC that showed up last
year (STH review was on here in December). Linux 6.16+, TrueNAS 26.04 and
Windows support it; macOS has nothing, and Realtek has said nothing about a
Mac driver. So I ported it.

What it is: a fork of Mieze's RTL812xLucy macOS driver (the well-known
RTL8125/8126 kext from the hackintosh world) with the RTL8127 hardware layer
ported from Realtek's GPL r8127 Linux driver, plus an arm64e build so it runs
on Apple Silicon in a Thunderbolt enclosure or a Mac Pro slot. It's validated
on an M5 Mac with the SFP+ variant (RTL8127ATF) over a DAC to a 10G switch:
iperf3 single stream ~9.35 Gbit/s TX / ~9.19 Gbit/s RX, so line rate both ways.

The interesting part was DMA on Apple Silicon. Apple's DART IOMMU rejects the
raw physical addresses IOMemoryDescriptor::getPhysicalSegment() hands you, the
system mapper is not applied by default the way it is on Intel, and every DMA
buffer has to go through an IODMACommand bound to the device's mapper or you
get a storm of "STE invalid" faults and a dead datapath. There's also the fact
that the polled-mode networking SPI the driver relies on is stripped from the
public SDK, so the repo carries a generated header overlay that re-inserts the
vtable slots. Details are in the commit history if you like that sort of thing.

Caveats, honestly: the kext is unsigned, so on Apple Silicon you need Reduced
Security and csrutil disable. <DEXT STATUS> The RJ45 variant (RTL8127A) uses
the same silicon and the copper path is ported, but I only own the SFP+ card,
so reports from RJ45 owners are very welcome. There's an upstream PR open
against Mieze's repo; the fork is maintained on its own until it lands.

If you have a 10G NAS and a Mac and didn't want to pay $200 for an Aquantia
Thunderbolt adapter, this is for you. Happy to answer questions.

---

## 2. Reddit r/homelab (long form; adapt the first line for r/truenas, r/HomeNetworking, r/mac, r/macmini)

**Title:** I wrote a macOS driver for the $40 Realtek RTL8127 10GbE cards (Apple Silicon, line rate over SFP+)

**Body:**

TL;DR: open-source driver, works on Apple Silicon Macs with an RTL8127 card in
a Thunderbolt enclosure, 9.3 Gbit/s each way in iperf3, GPLv2.
https://github.com/stefb69/RTL812xLucy

Background: the RTL8127 is the cheap 10GbE PCIe chip everyone's been putting
in NAS boxes and homelab hosts since late 2025. Linux and TrueNAS support it,
Windows too, macOS doesn't, and Realtek has no plans to ship a Mac driver.
Meanwhile the "supported" way to get 10G on a Mac is a $150-250 Thunderbolt
adapter.

I ported Realtek's Linux r8127 driver into Mieze's RTL812xLucy macOS driver
(the RTL8125/8126 one) and built it for arm64e. Tested on an M5 Mac, macOS
26.5, card in a Thunderbolt PCIe enclosure, SFP+ DAC to a 10G switch. DHCP,
stable link, multi-GB transfers both directions, zero errors.

What you need to know before trying it:

- It's a kext and it's unsigned, so you need Reduced Security + "allow user
  kernel extensions" from recoveryOS, plus `csrutil disable`. Step-by-step in
  the README. <DEXT STATUS>
- I own the SFP+ variant (RTL8127ATF). The RJ45 one (RTL8127A) should work,
  the copper path is ported, but nobody has tested it yet. If you have one,
  please open a hardware report on the repo, working or not.
- macOS shows the link as "10GBase-T" even over DAC. Cosmetic.
- Thunderbolt enclosure reports welcome; I've only tried one.

Release zip is on the GitHub Releases page (<TAG>). Issues are open.

---

## 3. Short version (forum replies, blog comments, STH / TrueNAS / MacRumors threads)

For anyone looking for macOS support for the RTL8127 / RTL8127ATF: there's now
an open-source driver for Apple Silicon Macs, a fork of Mieze's RTL812xLucy
with the r8127 Linux hardware code ported over and an arm64e build. Validated
on an M5 in a Thunderbolt enclosure with the SFP+ variant, iperf3 ~9.3 Gbit/s
both ways. Unsigned kext for now (Reduced Security needed), <DEXT STATUS>
RJ45 (RTL8127A) testers wanted. https://github.com/stefb69/RTL812xLucy

---

## 4. Comment under Jeff Geerling's "New 10 GbE USB adapters are cooler, smaller, cheaper"

Someone linked the repo here already, so a short follow-up from the author:

Author of that RTL8127 macOS driver here. Since the post went up it's been
validated on real hardware: M5 Mac, RTL8127ATF (SFP+) in a Thunderbolt
enclosure, iperf3 ~9.35 Gbit/s TX / ~9.19 Gbit/s RX, so the PCIe RTL8127 does
reach line rate on macOS, unlike the USB RTL8159 stuck in CDC mode. Caveats:
unsigned kext, so Reduced Security on Apple Silicon for now, <DEXT STATUS> and
the RJ45 variant hasn't been tested yet (I only have the SFP+ card).
https://github.com/stefb69/RTL812xLucy

---

## 5. Message to Jeff Geerling (contact form at jeffgeerling.com/contact, or a reply on X/Mastodon)

Subject: macOS driver for the RTL8127 10GbE cards, Apple Silicon, line rate

Hi Jeff,

You've covered the new cheap Realtek 10G parts (RTL8159 USB, RTL8127 PCIe) a
few times, and the recurring caveat was "no macOS driver, don't bother on a
Mac". I wrote one. It's a fork of Mieze's RTL812xLucy with Realtek's r8127
Linux code ported over and an arm64e build, so it runs on Apple Silicon Macs
in a Thunderbolt enclosure or a Mac Pro slot.

Validated on an M5 Mac with the SFP+ variant (RTL8127ATF) over a DAC to a
10G switch: ~9.35 Gbit/s TX / ~9.19 Gbit/s RX in iperf3, single stream. So a
$40 card plus an enclosure does line-rate 10G on a Mac.

Honest caveats: it's an unsigned kext, so Reduced Security is required on
Apple Silicon for now. <DEXT STATUS> And I've only got the SFP+ card, so the
RJ45 RTL8127A is untested (same silicon, copper path ported).

Repo: https://github.com/stefb69/RTL812xLucy

If you still have an RTL8127 card around and a Mac with an enclosure, I'd love
to hear whether it works for you, and I'm happy to answer any questions.

Thanks for the coverage of these parts, it's what got me to try.

Stéphane

---

## 6. Comment on Christian Kohlschütter's "Realtek's 10GbE RJ45 NIC performance revisited"

You wrote "Realtek, if you read this: pretty please provide a real driver for
macOS". Realtek didn't, but there's now a community one for the RTL8127 on
Apple Silicon: https://github.com/stefb69/RTL812xLucy (fork of Mieze's
RTL812xLucy, r8127 hardware code ported, arm64e build). Validated with the
SFP+ RTL8127ATF in a Thunderbolt enclosure at ~9.3 Gbit/s both ways. The RJ45
RTL8127A path is ported but untested since I only own the SFP+ card; given
your test bench, a report from you would be very valuable. Unsigned kext for
now (Reduced Security), <DEXT STATUS>

---

## 7. Note in Mieze's Insanelymac thread

Short, polite, no complaint about the PR:

For RTL8127 / RTL8127ATF (10G) users: I've published a fork of RTL812xLucy
with RTL8127 support ported from Realtek's r8127 driver and an Apple Silicon
(arm64e) build, validated on real hardware at line rate. There's a PR open
here for the chip support. In the meantime the fork, releases and issue
tracker are at https://github.com/stefb69/RTL812xLucy. Thanks Mieze for the
driver this is built on.

---

## `<DEXT STATUS>` sentences

Pick one depending on where the DriverKit dext stands at launch:

- Signed dext available: "There's also a signed DriverKit version (dext) that
  installs like a normal app with full security enabled; that's the one most
  people should use."
- Dext tested but unsigned: "A DriverKit version that will install with full
  security enabled is working and waiting on Apple's entitlement approval to
  be signed."
- Dext not yet tested: "A DriverKit version that will install with full
  security enabled is written and waiting on Apple's entitlement approval
  before it can be tested and signed."

## Follow-up hygiene

- Answer every issue and comment within a day for the first two weeks.
- Turn each hardware report into a row of the README "Tested setups" table.
- Watch `gh api repos/stefb69/RTL812xLucy/traffic/popular/referrers` to see
  which channel actually brings people.
