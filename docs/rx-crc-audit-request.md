# Demande de relecture : erreurs CRC en réception et coupures de lien (13 sept. 2026)

Contexte pour un relecteur externe. Tout ce qui suit est mesuré sur la
machine de test ; les hypothèses sont marquées comme telles. Code :
branche `rtl8127`, dext 0.2.25 (commit 499a03a), `RTL8127Dext/`.

## Matériel

- MacBook Pro M5, macOS 26.6.2 (25G83).
- Adaptateur **Lekuo DTB3F11** : USB4/Thunderbolt vers SFP+ 10G, chip
  **RTL8127ATF** (PCI 10ec:0e10), boîtier alu sans ventilateur, alimenté par
  le port USB4 du Mac. Vu comme un device PCIe tunnelé.
- Switch **MikroTik CRS305-1G-4S+IN**, RouterOS 7.24.2 (mis à jour pendant
  les tests, sans effet). Trois DAC SFP+ et un module 10G cuivre (uplink).
- Peer Linux (bazzite, Mellanox ConnectX, MTU 1500) sur un autre port DAC.

## Chronologie

| Quand | Observation |
|---|---|
| Nuit 11→12 sept | Benchs iperf3 à line rate. Tally chip : **4 erreurs RX sur 27 M trames**, 0 coupure de lien. |
| 12 sept. journée | Compteur d'erreurs à ~2 700 pour 63 M trames en fin d'après-midi. Premières coupures de lien (1-2 s) à 19:29. |
| 12 sept. 19:29 et 13 sept. 00:15 | Après une coupure de lien, RX définitivement mort (tempête RxDescUnavail 48k/s, 0 trame livrée) jusqu'au rebranchement Thunderbolt. **Bug driver, corrigé en 0.2.23** : `rtl812xLinkDownPatch()` → `rtl8125_hw_reset()` remet le pointeur RX du chip à 0, le dext ne réarmait que l'anneau TX (le kext fait `clearRxTxRings()`). Depuis : 10+ coupures, RX repart à chaque fois, aucun déclenchement du filet de sécurité `rx stall`. |
| 13 sept. 06:47 → 13:15 | Erreurs CRC en continu, ~5/min ; coupures de lien de 1-2 s toutes les 20-40 min. |

## Nature des erreurs

Tally chip (dump DMA des compteurs matériels, structure `RtlStatData`),
loggé toutes les 5 s (`tally:` et `tally-err:`) :

- `rxErrors` monte (~5/min). `alignErrors32`, `rxFrame2Long`, `rxRunt`,
  `rxMacError`, `rxUnknownOpcode`, `rxPauseAll`, `rxTcamDropped`, `rdu`,
  `tdu` : **tous à 0**. Donc CRC pur sur des trames bien cadrées et de
  longueur normale.
- Côté driver : `rxErrors` des descripteurs (bit RxRES) = 0, `short` = 0 :
  le MAC ne remonte pas ces trames.
- Timing : erreurs isolées (1 par intervalle de 5 s le plus souvent),
  espacement aléatoire (5 à 45 s), **indépendant du trafic** : 5 à 25
  erreurs par 5 min que le Mac reçoive 25 Mo ou 1,6 Go. Une rafale de 153
  entre 09:42 et 09:47.
- **Un seul sens** : le switch compte `rx-fcs-error = 0` sur le port du
  Mac (sens Mac → switch propre), la Mellanox du peer aussi.

## Tests faits, un à la fois

| Test | Résultat |
|---|---|
| Retourner le DAC bout pour bout | erreurs inchangées côté Mac, toujours 0 côté switch → pas une paire du câble |
| Remplacer le DAC par un autre | inchangé |
| Port switch `auto-negotiation=no speed=10G-baseCR` (le RTL8127ATF ne négocie pas) | inchangé, coupures de lien aussi |
| Changer de port switch (sfp-sfpplus2 → sfp-sfpplus3) | inchangé (288 → 298 en 4 min, une coupure à 13:11) |
| Mise à jour RouterOS + reboot switch | inchangé |
| Retourner le boîtier (ouïes vers le haut) | inchangé |
| Température du die (PHY OCP 0xBD84, même lecture que r8127 `/proc/.../temp`) | **67-70 °C stable** pendant tout le test, boîtier retourné ou non ; sonde vivante (valeur brute qui bouge) |
| MTU du peer | revenu à 1500 (pas de trames jumbo sur le LAN) |

## Ce qui est exclu, ce qui reste

Exclus : les deux câbles, le port du switch, l'autonégociation, le firmware
du switch, la température du die, une trame anormale sur le LAN (la
ventilation des erreurs est à zéro), le trafic.

Reste : le récepteur de l'adaptateur (SerDes RX du RTL8127ATF, cage SFP+
et contacts, alimentation depuis le port USB4), et une éventuelle
contribution du driver (voir questions). La nuit propre du 11 au 12 avec
le **même** driver/chip/câble/port plaide pour une dégradation matérielle
apparue le 12 en journée.

## Questions au relecteur

1. **Une cause côté driver est-elle possible pour des CRC purs, dans un
   seul sens, à taux constant dans le temps ?** Le chip vérifie le CRC
   avant la DMA ; je ne vois pas comment le dext pourrait produire ce
   compteur. Y a-t-il une configuration SerDes/fibre dans `r8127_fiber.c`
   (équalisation RX, réglages 10GBASE-CR vs SR, `rtl8127_set_sds_phy_caps_10g_8127`)
   que le portage `RTL812xLucy/rtl8127_fiber.cpp` appliquerait
   différemment, ou qu'un `rtl8125_hw_reset()` à chaque coupure de lien
   laisserait dans un état dégradé ? Comparer avec r8127 : que fait-il
   exactement au link down/up en mode fibre ?
2. **Les coupures de lien sont-elles cause ou conséquence ?** Le chemin
   actuel : interruption LinkChg → lecture `PHYstatus` → si lien absent,
   `rtl812xLinkDownPatch()` (soft reset) + réarmement des anneaux +
   `rtl812xSetPhyMedium()`. Une lecture transitoire de `PHYstatus` sans
   lien (glitch pendant une rafale de CRC) déclencherait-elle un reset qui
   **crée** la coupure de 1-2 s ? Le kext fait pareil ; r8127 aussi ?
   Faut-il une confirmation (relecture après quelques ms) avant de
   traiter un link down ?
3. **Le compteur `rxErrors` du tally** peut-il inclure autre chose que le
   CRC quand tous les autres champs sont à zéro (par exemple des symboles
   invalides 64b/66b, des erreurs du SerDes hors trame) ?
4. Y a-t-il dans r8127 une lecture de diagnostic SerDes ou SFP (registres
   SDS via `rtl8127_sds_phy_read_8127`, EEPROM du module par I2C, compteurs
   d'erreurs de symboles) qu'on pourrait logger pour distinguer récepteur
   SerDes, cage, et alimentation ?
5. Autre chose à tester à moindre coût avant de conclure au matériel ?

Fichiers utiles : `RTL8127Dext/RTL8127Dext/RTL8127Driver.cpp`
(`updateLinkStatus`, `rxRingRearm`, `StatsTimerOccurred`),
`RTL812xLucy/rtl8127_fiber.cpp`, `RTL812xLucy/rtl812x_hw.h` (`RtlStatData`),
`reference/r8127/` (source Linux, non versionné, disponible localement),
`docs/DEXT-PORT.md` (pièges NDK).
