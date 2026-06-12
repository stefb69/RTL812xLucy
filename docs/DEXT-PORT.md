# Port DriverKit (dext) du driver RTL8127

Objectif : version DriverKit du driver RTL8127/RTL8127ATF qui se charge en
**sécurité complète** sur Apple Silicon (pas de SIP off), distribuable via une
app Developer ID une fois les entitlements DriverKit accordés
(`com.apple.developer.driverkit` + `.transport.pci` + `.family.networking`).

Le kext RTL812xLucy reste le véhicule de développement/validation : c'est la
référence fonctionnelle dont on porte la logique, milestone par milestone.

## Correspondance kext → dext

| Kext (RTL812xLucy) | Dext (RTL8127Dext) |
|---|---|
| `IOEthernetController` / SPI polled-mode | `IOUserNetworkEthernet` (NetworkingDriverKit, API NDK 2.1) |
| `IOPCIDevice` kernel + mmio mappé (`RTL_W32` sur pointeur) | `IOPCIDevice` PCIDriverKit : `MemoryWrite32(barIndex, off, val)` — pas de pointeur brut |
| `IODMACommand` + mapper (DART) | `IOBufferMemoryDescriptor` + adresses IOVA via PCI ; DART géré par le système |
| `IOFilterInterruptEventSource` (MSI) | `IOInterruptDispatchSource` + `IODispatchQueue` |
| mbuf / pull-model SPI | `IOUserNetworkPacketBufferPool::CreateWithOptions` + `IOUserNetworkTx/RxSubmissionQueue::Create` + queues de complétion |
| `setLinkStatus()` | `reportLinkStatus(LinkStatus, MediaWord)` |
| `rtl8127_fiber.cpp` (SerDes) | port direct — seuls les accès `RTL_W16/R16` changent de forme |
| Tables MCU/PHY (`rtl812xx.cpp`) | port direct (écritures registre uniquement) |

Stratégie registre : reproduire les helpers du kext (`mac_ocp_read/write`,
`mdio_direct_*`, SDS fibre…) au-dessus de `MemoryRead/Write32` — la logique
métier (séquences, valeurs) se copie alors quasi verbatim depuis le kext déjà
porté et validé.

## Milestones

- **D1 — squelette (fait)** : match PCI `0x812710ec 0x0e1010ec`, open device,
  probe du BAR MMIO par signature TxConfig, détection chip + fibre (OCP
  0xD006), lecture MAC. Logs `os_log` uniquement, pas d'interface réseau
  enregistrée. Build OK (arm64 + x86_64, SDK DriverKit 25.5).
- **D2 — bring-up matériel (fait, code complet)** : la couche hardware du kext
  (`rtl812xx.cpp`, `rtl8127_fiber.cpp`, `rtl812x_eeprom.c`, `rtl812x_phy.c`)
  est compilée **verbatim** dans le dext via `compat/` (faux `<IOKit/IOLib.h>`
  mappé sur DriverKit, BAR mappé en pointeur via
  `_CopyDeviceMemoryWithIndex` + `CreateMapping` pour que `RTL_W32/R32`
  marchent inchangés). L'orchestration (`rtl812xInit`, `rtl812xEnable`,
  `rtl812xHwConfig`…) est portée dans `hw/RTL8127Hw.{h,cpp}` (méthodes du kext
  renommées, adapter PCI config `DextPciConfig`).
- **D3 — link fibre (fait, code complet)** : SerDes via la couche verbatim,
  `updateLinkStatus()` sur interruption LinkChg, `reportLinkStatus` avec
  media fibre (`10GBaseSR`/`1000BaseSX`) ou cuivre,
  `getSupportedMediaArray`/`handleChosenMedia` → vitesse forcée 10G/1G.
- **D4 — datapath (fait, code complet)** : rings TX (32 octets,
  USE_NEW_TX_DESC) / RX (16 octets) dans des `IOBufferMemoryDescriptor`
  mappés device via `IODMACommand::PrepareForDMA` (leçon kext : jamais
  d'adresse physique brute), MSI via
  `ConfigureInterrupts(kIOInterruptTypePCIMessaged)` +
  `IOInterruptDispatchSource`, pool `CreateWithOptions(pciDevice, …)` (le
  framework DMA-mappe les buffers paquets), 4 queues NDK (`withPool` +
  actions), FIFOs SPSC lock-free entre interruption et actions framework,
  `RegisterEthernetInterface(mac, pool, queues, 4)`. `wmb()` avant doorbell.
- **D5 — offloads (fait, code complet)** : checksum TX (opts2 identiques au
  kext depuis `getTxChecksumInfo`), checksum RX (`setRxChecksumInfo` depuis
  les bits opts2), `getHardwareAssists`. MTU 1500 (jumbo: plus tard), pas de
  WoL.
- **App hôte (fait)** : `RTL8127App` (SwiftUI, cible du même xcodeproj),
  embarque le dext dans `Contents/Library/SystemExtensions` et soumet
  l'`OSSystemExtensionRequest` d'activation/désactivation.

**État : tout compile et link (dext universel arm64+x86_64 + app).
Non testé sur matériel — nécessite les entitlements DriverKit (demande en
cours chez Apple) ou le mode développeur (voir ci-dessous). Le kext reste la
référence fonctionnelle validée.**

## Développement sans entitlement (en attendant Apple)

```bash
systemextensionsctl developer on        # exécution depuis le build Xcode
# SIP réduit requis (déjà fait pour le kext) ; amfi peut exiger en plus :
#   sudo nvram boot-args="amfi_get_out_of_my_way=1"  (dev uniquement)
```
Le dext et le kext matchent le même matériel : ne pas charger les deux en même
temps (le kext a IOProbeScore 5000, le dext 6000 — désinstaller le kext de
/Library/Extensions pendant les tests dext).

## Points ouverts

- Débit : le modèle pool/queues NDK à 10 Gb/s est peu documenté publiquement —
  à mesurer en D4 (taille de pool, batching, `IOUserNetworkPacketPoller`).
- MSI : vérifier l'activation de la capability MSI côté PCIDriverKit
  (ConfigurationWrite sur la capability vs prise en charge automatique).
- `SetMTU`/jumbo et stats étendues : après D5.
