# Announcement drafts

Ready-to-post texts for the launch. Nothing here is published automatically.
Validation pass done on 12 Sept 2026 (dext 0.2.22 on hardware, line rate,
jumbo). Release tag: `v1.1.2-rtl8127.beta2` (dext 0.2.22). Post once that release is public.

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
ported from Realtek's GPL r8127 Linux driver, then moved into a DriverKit
system extension (dext). It's signed with Developer ID and notarized, so it
installs on a stock Apple Silicon Mac with full security on: run the pkg,
approve the driver once in System Settings, plug the card in. Validated on an
M5 Mac with the SFP+ variant (RTL8127ATF) in a Thunderbolt enclosure, DAC to a
10G switch: iperf3 9.4 Gbit/s TX / 9.4 RX at MTU 1500, 9.6 / 9.9 with jumbo
frames, with TSO and checksum offload. Line rate both ways.

Two interesting parts. First, DMA on Apple Silicon for the original kext:
Apple's DART IOMMU rejects the raw physical addresses
IOMemoryDescriptor::getPhysicalSegment() hands you, the system mapper is not
applied by default the way it is on Intel, and every DMA buffer has to go
through an IODMACommand bound to the device's mapper or you get a storm of
"STE invalid" faults and a dead datapath. Second, NetworkingDriverKit, which
has almost no public documentation beyond the headers: the Skywalk stack hands
you TX packets whose payload starts at a 2-byte offset inside the buffer
(Network.framework flows only, BSD sockets don't), the BPF tap API panics the
kernel on macOS 26, and the "max transfer unit" callback is the maximum, not
the current MTU. All of it is written up in docs/DEXT-PORT.md.

Caveats, honestly: the RJ45 variant (RTL8127A) uses the same silicon and the
copper path is ported, but I only own the SFP+ card, so reports from RJ45
owners are very welcome. Eight or more parallel TCP streams *sending* from the
Mac at MTU 1500 top out around 4.5 Gbit/s (per-packet cost in the dext, fine
with jumbo frames); normal use is line rate. There's an upstream PR open
against Mieze's repo for the kext part; the fork is maintained on its own.

If you have a 10G NAS and a Mac and didn't want to pay $200 for an Aquantia
Thunderbolt adapter, this is for you. Happy to answer questions.

---

## 2. Reddit r/homelab (long form; adapt the first line for r/truenas, r/HomeNetworking, r/mac, r/macmini)

**Title:** I wrote a macOS driver for the $40 Realtek RTL8127 10GbE cards (Apple Silicon, line rate over SFP+)

**Body:**

TL;DR: open-source, signed driver, installs like a normal app (no SIP or
security changes), works on Apple Silicon Macs with an RTL8127 card in a
Thunderbolt enclosure, 9.4 Gbit/s each way in iperf3 (9.9 with jumbo
frames), GPLv2.
https://github.com/stefb69/RTL812xLucy

Background: the RTL8127 is the cheap 10GbE PCIe chip everyone's been putting
in NAS boxes and homelab hosts since late 2025. Linux and TrueNAS support it,
Windows too, macOS doesn't, and Realtek has no plans to ship a Mac driver.
Meanwhile the "supported" way to get 10G on a Mac is a $150-250 Thunderbolt
adapter.

I ported Realtek's Linux r8127 driver into Mieze's RTL812xLucy macOS driver
(the RTL8125/8126 one), then moved it into a DriverKit extension so it can be
signed and installed on a stock Mac. Tested on an M5 Mac, macOS 26.6, card in
a Thunderbolt PCIe enclosure, SFP+ DAC to a 10G switch: iperf3 9.4 Gbit/s TX
and RX at MTU 1500, 9.6 / 9.9 with jumbo frames, TSO and checksum offload
on, zero errors.

What you need to know before trying it:

- It's a DriverKit system extension, signed with Developer ID and notarized:
  run the pkg, approve the driver once in System Settings, plug the card in.
  No recoveryOS, no Reduced Security, no `csrutil disable`. (The original
  unsigned kext is still in the repo for Intel Macs and hackintoshes.)
- I own the SFP+ variant (RTL8127ATF). The RJ45 one (RTL8127A) should work,
  the copper path is ported, but nobody has tested it yet. If you have one,
  please open a hardware report on the repo, working or not.
- macOS shows the link as "10GBase-T" even over DAC. Cosmetic.
- Thunderbolt enclosure reports welcome; I've only tried one.

The installer pkg is on the GitHub Releases page (v1.1.2-rtl8127.beta2). Issues are open.

---

## 3. Short version (forum replies, blog comments, STH / TrueNAS / MacRumors threads)

For anyone looking for macOS support for the RTL8127 / RTL8127ATF: there's now
an open-source driver for Apple Silicon Macs, a fork of Mieze's RTL812xLucy
with the r8127 Linux hardware code ported over and an arm64e build. Validated
on an M5 in a Thunderbolt enclosure with the SFP+ variant, iperf3 ~9.3 Gbit/s
both ways. It's a signed, notarized DriverKit extension, so it installs
with full security on (pkg, approve once, done). RJ45 (RTL8127A) testers
wanted. https://github.com/stefb69/RTL812xLucy

---

## 4. Comment under Jeff Geerling's "New 10 GbE USB adapters are cooler, smaller, cheaper"

Someone linked the repo here already, so a short follow-up from the author:

Author of that RTL8127 macOS driver here. Since the post went up it's been
validated on real hardware: M5 Mac, RTL8127ATF (SFP+) in a Thunderbolt
enclosure, iperf3 9.4 Gbit/s each way (9.9 with jumbo frames), so the PCIe
RTL8127 does reach line rate on macOS, unlike the USB RTL8159 stuck in CDC
mode. It's now a signed, notarized DriverKit extension: pkg installer, full
security stays on. Caveat: the RJ45 variant hasn't been tested yet (I only
have the SFP+ card).
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
10G switch: 9.4 Gbit/s each way in iperf3, 9.9 with jumbo frames. So a $40
card plus an enclosure does line-rate 10G on a Mac. It ships as a signed,
notarized DriverKit extension (pkg installer, approve once in System
Settings), so there's no SIP or Reduced Security dance.

Honest caveat: I've only got the SFP+ card, so the RJ45 RTL8127A is untested
(same silicon, copper path ported).

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
your test bench, a report from you would be very valuable. It's a signed
DriverKit extension, so it installs with full security on.

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

---

## 8. Feedback to the card's manufacturer (Lekuo / 深圳市乐扩多媒体有限公司)

The tested unit is the Lekuo **DTB3F11**, a USB4/Thunderbolt to 10G SFP+
adapter built around the RTL8127ATF
(https://www.lekuo.com/product_view.php?id=665). Their product FAQ says
"Mac users should purchase with caution ... macOS does not currently have
official drivers for the Realtek RTL8127 chipset". Contact form:
https://www.lekuo.com/list.php?pid=4&ty=28 (name, email, message), or
support@lekuo.com / sales@lekuo.com. Send both languages in one message.

**English**

Subject: macOS driver now available for your DTB3F11 (RTL8127ATF) adapter

Hello,

I own your DTB3F11 USB4 to 10G SFP+ adapter and use it on an Apple Silicon
MacBook Pro (M5, macOS 26.6). Your product FAQ currently tells Mac users to
"purchase with caution" because macOS has no driver for the RTL8127. That
is no longer the case: I have written and released an open-source macOS
driver for the RTL8127 / RTL8127ATF, and your DTB3F11 is the exact unit it
was developed and validated on.

- It is a DriverKit system extension, signed with an Apple Developer ID and
  notarized, so it installs on a standard Mac with full security enabled
  (run the installer, approve the driver once in System Settings).
- Measured with your adapter over a DAC cable: 9.4 Gbit/s in both
  directions at MTU 1500, 9.9 Gbit/s with 9000-byte jumbo frames, with TCP
  segmentation and checksum offload.
- Project page: https://github.com/stefb69/RTL812xLucy
- Installer: https://github.com/stefb69/RTL812xLucy/releases/tag/v1.1.2-rtl8127.beta2

Two suggestions: update the Mac answer in the FAQ, and add the driver link
to the product page's Driver Download section as "macOS (open-source
third-party driver)". It is GPLv2, free, and you are welcome to link or
mirror it. If you would like to test it on your other RTL8127 products
(the dual-port DTB3F22 or your PCIe cards), I am happy to help, and a
sample of the RJ45 variant would let me validate that path too.

Best regards,
Stéphane Benoit
https://github.com/stefb69/RTL812xLucy

**中文**

主题：贵司 DTB3F11（RTL8127ATF）网卡现已有 macOS 驱动

您好：

我购买了贵司的 DTB3F11 USB4 转 10G SFP+ 网卡，在 Apple Silicon 的 MacBook
Pro（M5，macOS 26.6）上使用。贵司产品页的常见问题中目前写明"Mac 用户请谨慎
购买"，理由是 macOS 没有 RTL8127 芯片的驱动。这一情况已经改变：我为
RTL8127 / RTL8127ATF 编写并发布了一款开源 macOS 驱动，而且开发和验证
所用的正是贵司的 DTB3F11。

- 驱动为 DriverKit 系统扩展，已用 Apple 开发者 ID 签名并经过公证，可以在
  保持完整安全设置的普通 Mac 上安装（运行安装包，在系统设置中允许一次即可）。
- 用贵司网卡通过 DAC 线实测：MTU 1500 下双向 9.4 Gbit/s，9000 字节巨型帧下
  9.9 Gbit/s，支持 TCP 分段卸载和校验和卸载。
- 项目主页：https://github.com/stefb69/RTL812xLucy
- 安装包：https://github.com/stefb69/RTL812xLucy/releases/tag/v1.1.2-rtl8127.beta2

两点建议：更新常见问题中关于 Mac 的回答；在产品页"驱动下载"栏目中加入该
驱动链接，标注为"macOS（第三方开源驱动）"。驱动采用 GPLv2 许可、完全免费，
欢迎链接或镜像。如果贵司希望在其他 RTL8127 产品（双口 DTB3F22 或 PCIe
网卡）上测试，我很乐意配合；若能提供一块 RJ45 版本的样品，我也可以验证
该路径。

此致
Stéphane Benoit（法国）
https://github.com/stefb69/RTL812xLucy

---

## Follow-up hygiene

- Answer every issue and comment within a day for the first two weeks.
- Turn each hardware report into a row of the README "Tested setups" table.
- Watch `gh api repos/stefb69/RTL812xLucy/traffic/popular/referrers` to see
  which channel actually brings people.
