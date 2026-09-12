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
  les bits opts2), `getHardwareAssists`. Pas de WoL.
- **D6 — perf (fait, code complet, 10 sept. 2026)** :
  - **TSO IPv4/IPv6** : même recette que `outputStart()` du kext
    (GiantSendv4/v6 + offset TCP dans opts1, MSS dans opts2, pseudo-checksum
    préchargé dans l'en-tête TCP). Le pool TX a des tampons de 16 Ko, un par
    paquet ; `getTSOOptions` borne le paquet TSO à cette taille. MSS 11 bits :
    en jumbo la taille de segment est plafonnée à 2047, trames plus petites
    mais correctes. `setHardwareAssists(assists, mask)` suit l'état demandé
    par la pile.
  - **Jumbo jusqu'à 9000** : pool RX séparé, tampons 9216, `RxMaxSize`
    réécrit à chaud, longueur RX lue sur 14 bits (`0x3fff`, comme r8127 ; le
    kext utilisait `0x1fff`, faux au-delà de 8191 octets sur arm64 où les
    tampons font 16 Ko — corrigé aussi).
  - **Mitigation d'interruptions** portée de l'ISR du kext : après une
    rafale TxOK/RxOK, `TIMER_INT0`/`TCTR0` = 0x5000 et masque « timer »
    (TxOK coupé, RxOK + PCSTimeout), retour au masque normal sur PCSTimeout
    ou LinkChg.
  - Pools mappés dext + device (`PoolFlagMapToDext | PoolFlagMapToDevice`),
    enregistrement `registerEthernetInterface(mac, queues, txPool, rxPool)`.
- **App hôte (fait)** : `RTL8127App` (SwiftUI, cible du même xcodeproj),
  embarque le dext dans `Contents/Library/SystemExtensions`. Au lancement
  elle interroge l'état (`propertiesRequest`), active le dext si besoin,
  ouvre le panneau Extensions de pilote de Réglages Système quand macOS
  demande l'approbation, puis affiche en continu (scan IORegistry toutes
  les 2 s) la carte détectée, le pilote attaché, le lien (vitesse, BSD
  name, IPv4). Localisée en/fr/de/es/it/ja/zh-Hans via
  `RTL8127App/Localizable.xcstrings`. Le bundle du dext doit s'appeler
  `net.wizzz.RTL8127Dext.dext` (PRODUCT_NAME = bundle ID), sinon sysextd
  ne le trouve pas dans l'app.

**État (9 sept. 2026) : build Developer ID signé et notarisé
(`packaging/sign-and-notarize.sh`). Parcours d'installation validé sur un
Mac en Sécurité maximale, SIP activé, sans kext : app → activation →
approbation → `systemextensionsctl list` = `activated enabled`, dext chargé
par kernelmanagerd. Pas encore testé avec une carte branchée (matching
PCI, lien, débit). Le kext reste la référence fonctionnelle validée.**

## Signature Developer ID et notarisation

Le projet est en signature manuelle, team `LRA39582TA`, identité
`Developer ID Application`, avec deux profils de provisioning nommés :

| Cible | Bundle ID | Profil (`PROVISIONING_PROFILE_SPECIFIER`) | Entitlements |
|---|---|---|---|
| RTL8127Dext | `net.wizzz.RTL8127Dext` | `RTL8127Dext Developer ID` | driverkit, driverkit.family.networking, driverkit.transport.pci (`IOPCIPrimaryMatch` = `0x000010ec&0x0000FFFF`) |
| RTL8127App | `net.wizzz.RTL8127App` | `RTL8127App Developer ID` | system-extension.install (+ hardened runtime) |

Création des profils sur developer.apple.com (Certificates, Identifiers &
Profiles) :

1. Identifiers → App IDs → `net.wizzz.RTL8127Dext` (type App, bundle ID
   explicite) : cocher **DriverKit**, **DriverKit Family Networking**,
   **DriverKit PCI (PrimaryMatch)**, et leurs variantes « (development) »
   pour pouvoir aussi créer un profil Development plus tard. Le portail
   génère le match PCI sous forme de masque vendeur
   (`0x000010ec&0x0000FFFF`, tout Realtek) : `RTL8127Dext.entitlements`
   doit porter exactement cette valeur, sinon la signature n'est pas
   acceptée au chargement. Le filtrage par device reste dans l'Info.plist
   (`IOPCIPrimaryMatch` explicite).
2. Identifiers → `net.wizzz.RTL8127App` : cocher **System Extension**.
3. Profiles → + → Distribution → **Developer ID** → App ID
   `net.wizzz.RTL8127Dext` → certificat Developer ID Application → nom
   exactement `RTL8127Dext Developer ID`. Apple propose la liste des
   entitlements DriverKit à inclure : sélectionner les trois.
4. Même chose pour `net.wizzz.RTL8127App`, nom `RTL8127App Developer ID`.
5. Télécharger les deux `.provisionprofile` et les installer. Le
   double-clic ne suffit pas toujours : au besoin les copier dans
   `~/Library/Developer/Xcode/UserData/Provisioning Profiles/` sous le nom
   `<UUID>.provisionprofile` (UUID lu avec
   `security cms -D -i <fichier> | plutil -extract UUID raw -o - -`).

L'app est construite avec `CODE_SIGN_INJECT_BASE_ENTITLEMENTS = NO` en
Release pour ne pas embarquer `get-task-allow`, que la notarisation refuse
sur un build fait avec `xcodebuild build` (sans archive).

Ensuite `packaging/sign-and-notarize.sh <version>` construit, vérifie les
signatures et entitlements, notarise (`notarytool`, profil de credentials
`rtl8127-notary` à créer une fois avec `xcrun notarytool store-credentials`),
agrafe le ticket et produit `dist/RTL8127App-<version>.zip`. Avec
`DEVELOPER_ID_INSTALLER="Developer ID Installer: Wizzz.net (LRA39582TA)"`
il produit aussi `dist/RTL8127-<version>.pkg`, signé et notarisé, avec un
postinstall qui ouvre l'app pour l'utilisateur connecté. Le pkg est le
livrable recommandé : macOS n'active un dext que depuis une app dans
`/Applications`, le pkg l'y met lui-même (l'app le vérifie aussi et propose
de s'y déplacer). Après un tag, uploader le pkg notarisé à la main sur la
release (un seul livrable pour le dext), la CI ne signe pas.

Sans les profils installés, construire avec `CODE_SIGNING_ALLOWED=NO`
(c'est ce que fait la CI).

## Développement sans profil signé

```bash
systemextensionsctl developer on        # exécution depuis le build Xcode
# SIP réduit requis (déjà fait pour le kext) ; amfi peut exiger en plus :
#   sudo nvram boot-args="amfi_get_out_of_my_way=1"  (dev uniquement)
```
Le dext et le kext matchent le même matériel : ne pas charger les deux en même
temps (le kext a IOProbeScore 5000, le dext 6000 — désinstaller le kext de
/Library/Extensions pendant les tests dext).

## Leçons du premier test matériel du dext (11-12 sept. 2026)

Pièges NDK rencontrés, tous corrigés (voir l'historique git et
[ndk-native-flows-issue.md](ndk-native-flows-issue.md)) :

- Personality : `IOClass = IOUserNetworkEthernet` + `CFBundleIdentifierKernel =
  com.apple.iokit.IOSkywalkFamily` (pas `IOUserService`), sinon `super::Start`
  échoue en 0xe00002bc.
- Les 4 files NDK naissent désactivées : `setEnable(true)` à l'activation de
  l'interface ; `requestDequeue()` sur les files TX au lien montant.
- `USE_NEW_TX_DESC` doit être défini pour le hw layer du dext (descripteurs TX
  32 octets), sinon le chip s'arrête après le premier descripteur.
- `IONewZero()` n'exécute pas les initialiseurs C++ : positionner `hw->mtu`
  explicitement, sinon `RxMaxSize` = 22 et le chip rejette toute trame.
- `setMulticastAddresses()` (entrée NDK moderne) doit être implémenté, sinon
  aucun groupe multicast (mDNS, solicited-node IPv6) n'est joint.
- Les paquets natifs Skywalk (Network.framework) ont un décalage de données
  de 2 octets : adresse CPU et IOVA = base + `getDataOff()`.
- Une file TX par classe de service (BE/BK/VI/VO), comme les dexts d'Apple.
- Ne pas appeler `bpfAttach()` : panic noyau dans IOSkywalkFamily au
  remplacement du driver (macOS 26.6.2).
- Libérer les dispatch sources depuis le bloc de complétion de `Cancel()`.
- Logs : les `os_log` du dext apparaissent comme messages `kernel:` avec
  l'émetteur `<bundle>.dext` ; chaînes en `%{public}s` ; les messages `DK:` du
  noyau ne sont pas conservés (`log stream` en direct).

## Performance mesurée (12 sept. 2026, 0.2.21, peer Linux, MTU 1500)

TX 1 flux 9,40 ; RX 1 flux 9,38 ; TX 4 flux 9,22 ; RX 4/8 flux 9,39 ;
**TX 8 flux 4,5** (paquets TSO de ~3 Ko sous ACK-clocking à 8 flux, ~160k
paquets/s, dext à 88 % CPU : coût par paquet du chemin NDK, RPC par lot +
copie). Profondeur de file TX plafonnée à 4 Mo en vol
(`kTxInflightBytesLimit`) : un plafond en paquets bloquait les ACK en
réception. Piste pour les 8+ flux : `IOUserNetworkPacketPoller` (polling sous
charge, équivalent du mode polled du kext), voir aussi
[ndk-performance-proposals.md](ndk-performance-proposals.md).

**Jumbo (MTU 9000, 0.2.22)** : TX 1/4/8 flux 9,63 / 9,65 / 9,71 ; RX 1/4/8
flux 9,89 / 9,89 / 9,77 ; 0 retransmission. Le cas 8 flux TX disparaît en
jumbo (paquets TSO plus gros, moins de paquets/s). Piège NDK :
`GetMaxTransferUnit` / `getMaxTransferUnit()` doivent renvoyer le MTU
**maximum** supporté (le framework le lit une fois à l'enregistrement et le
publie en `IOMaxPacketSize`) ; renvoyer le MTU courant verrouille
l'interface à 1500 (`ifconfig mtu 9000` → EINVAL sans appel au dext).
Note : le `ping` de macOS limite `-s` à 8184 ; tester le 8972 depuis le peer
Linux (`ping -M do -s 8972`).

## Points ouverts

- Débit : le modèle pool/queues NDK à 10 Gb/s est peu documenté publiquement —
  à mesurer en D4 (taille de pool, batching, `IOUserNetworkPacketPoller`).
- MSI : vérifier l'activation de la capability MSI côté PCIDriverKit
  (ConfigurationWrite sur la capability vs prise en charge automatique).
- Stats étendues (`addHardwareCountsWithInterfaceStatistics`) : plus tard.
- VLAN hardware (tag out-of-band NDK → `TxVlanTag`) : plus tard.
- Mémoire des pools : ~20 Mo TX + ~14 Mo RX câblés en permanence ; à revoir
  si ça gêne (tampons TX plus petits + TSO plafonné plus bas).
- Devenir du kext : une fois le dext validé à line-rate sur la carte, ne plus
  le livrer dans les releases (il reste dans l'arbre comme référence et
  support de la PR upstream).
