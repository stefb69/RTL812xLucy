# Relecture du problème des flux natifs NDK

Relecture du 12 septembre 2026, à partir de
[ndk-native-flows-issue.md](ndk-native-flows-issue.md), du pilote local et des
sources Apple XNU. Les constats matériels et les hypothèses sont distingués
ci-dessous.

**Défaut trouvé et corrigé dans le code : le chemin TX ignorait le décalage
des données du paquet dans son tampon, tant pour le CPU que pour la DMA.**
Le correctif doit encore être validé sur la carte. Il explique les symptômes
si les SYN natifs arrivent effectivement avec le décalage attendu de 2 octets.

## Vérification sur la version 0.2.16

`systemextensionsctl list` confirme `0.2.16/216`, active. Après ce remplacement :

- `ifconfig en10` échoue encore avec `SIOCGIFXMEDIA: Input/output error`.
- Le nœud `IOSkywalkLegacyEthernet` annonce toujours `IOLinkSpeed = 0`,
  `IOActiveMedium = ""`, `IOSelectedMedium = ""`, mais `IOLinkStatus = 3`.
- `nscurl --max-time 12 --verbose --ignore-location
  http://captive.apple.com/hotspot-detect.html` expire (`NSURLErrorDomain -1001`).
- `curl --interface en10` obtient HTTP 200 pour la même URL, avec une connexion
  en 0,016 seconde. Ce test compare une URL, pas nécessairement la même adresse
  distante : les deux clients peuvent choisir différemment entre IPv4 et IPv6.

La modification de l'ordre d'initialisation de la liste des médias ne suffit
donc pas à corriger le reporting sur cette machine. Elle ne valide pas encore
l'hypothèse selon laquelle les médias expliqueraient la panne des flux natifs.
L'addendum de Claude rapporte les mêmes propriétés vides sur le nœud Wi-Fi
Apple et retire cette piste comme différence entre les pilotes.

## Trois mesures qui ne localisent pas la perte

### Le compteur global TX du flowswitch

Dans les sources XNU examinées, `FSW_STATS_TX_PACKETS` est déclaré mais n'est
pas incrémenté. Son affichage à zéro ne prouve donc pas qu'un paquet est bloqué.
Les compteurs de copie et de pertes doivent être lus séparément.
[Déclaration XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/os_stats_private.h#L859).

### Les statistiques des queues

La lecture locale donne :

```text
kern.skywalk.netif.netif_queue_stat_enable: 0
kern.skywalk.ring_stat_enable: 0
```

`nx_netif_update_stats()` retourne immédiatement lorsque le premier paramètre
vaut zéro. Les zéros de `skywalkctl interface -I en10 -Q` ne signifient donc pas
que les queues sont inutilisées.
[Condition dans XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/nexus/netif/nx_netif.c#L4448).
Le premier paramètre peut être activé pour
une mesure courte, puis restauré à sa valeur initiale :

```sh
sudo sysctl -w kern.skywalk.netif.netif_queue_stat_enable=1
skywalkctl interface -I en10 -Q
# Reproduire un seul essai, puis relire les compteurs.
skywalkctl interface -I en10 -Q
sudo sysctl -w kern.skywalk.netif.netif_queue_stat_enable=0
```

Ces commandes de modification n'ont pas été exécutées pendant la relecture.
La restauration à zéro suppose que la valeur initiale est toujours zéro.
`netstat -I en10 -qq` expose aussi l'état de l'AQM : longueur des files, paquets
retirés, pertes, pacing. La lecture faite après l'essai montre une file vide,
des paquets retirés et aucune perte AQM comptabilisée.

### Le silence des logs du pilote

Dans la version 0.2.16, `synLogged` est partagé entre TX et RX et plafonné à
100 pour toute la durée de vie du pilote. `describeTcpSyn()` filtre le port 80,
mais ne teste pas le bit SYN : un téléchargement HTTP peut épuiser ce budget.
Le compteur d'appels TX est placé après le refus pour lien inactif, et le test
des descripteurs disponibles précède les logs de paquets.

Autre limite : le budget `offLogged` de 12 lignes est commun aux décalages de
données et de segments mémoire. Des paquets BSD à décalage de données nul mais
à décalage de segment non nul peuvent le consommer avant le premier flux natif.

Il faut des compteurs permanents pour les entrées de callback, les refus et
les décalages non nuls. Le plafond doit limiter seulement le texte des logs.

## Défaut d'adressage TX

`dp_copy_to_dev_pkt()` réserve l'espace d'en-tête Ethernet du flowswitch en plus
du headroom demandé par l'interface. Pour Ethernet, cet espace vaut 16 octets.
`fsw_ethernet_frame()` ajoute ensuite l'en-tête de 14 octets, ce qui laisse
2 octets avant la trame lorsque le headroom de l'interface vaut zéro.
[Copie native](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/nexus/flowswitch/fsw_dp.c#L2730),
[ajout d'Ethernet](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/nexus/flowswitch/fsw_ethernet.c#L521).

Le chemin BSD `nx_netif_mbuf_to_kpkt()` copie au décalage `if_tx_headroom`, donc
zéro dans cette configuration. Les deux origines ne garantissent pas le même
décalage dans le tampon.
[Copie BSD](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/nexus/netif/nx_netif_host.c#L690).

La version 0.2.16 utilise directement `getDataVirtualAddress()` et
`getDataIOVirtualAddress()` dans le chemin TX. L'inspection du framework
NetworkingDriverKit installé confirme que ces méthodes renvoient les champs
d'adresse sans ajouter `dataOffset`. Les setters de décalage modifient seulement
le champ d'offset, jamais les adresses. `getDataOffset()` et `getDataOff()` lisent
le même champ ; le premier ajoute un contrôle de dépassement des 16 bits.

Ces deux octets oubliés produiraient une mauvaise adresse MAC et un mauvais
EtherType sur le fil. Le filtre de logs du pilote ne reconnaîtrait plus le SYN.
BPF, lui, ajoute explicitement le décalage du buflet lorsqu'il copie les octets
et peut donc afficher une trame correcte.
[Copie des paquets pour BPF](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/packet/packet_kern.c#L914).

Le correctif dans `RTL8127TxBuffer.h` résout ensemble les adresses CPU et DMA :

```text
frame = getDataVirtualAddress()   + getDataOff()
iova  = getDataIOVirtualAddress() + getDataOff()
```

Il vérifie `length <= capacity - offset`, après vérification d'`offset`, et
les débordements des adresses. Le chemin TX emploie la même vue pour le
descripteur, TSO, le checksum partiel et les logs. Les valeurs `start` et
`stuff` du checksum ne sont pas déplacées sans preuve de leur origine ; si
des demandes de checksum partiel apparaissent, leurs logs doivent être examinés.
La géométrie des pools et le headroom demandé par le pilote ne changent pas.

L'implémentation indépendante AQC111 additionne également le décalage au
pointeur CPU. Son code confirme cet usage ; son README ne prouve pas une
validation des flux natifs de Network.framework.
[AQC111, TX](https://github.com/jquirke/AQC111Driver/blob/0a0b0c0f602121777cc9c7d0e2f16130d5968dea/AQC111/AQC111/AQC111NIC.cpp#L1425).

L'inspection locale du cache DriverKit arm64e est reproductible avec les
artefacts conservés dans `/private/tmp/ndk-cache-extract-text.py` et
`/private/tmp/ndk-cache-text-arm64.dis`. Le cache inspecté se trouve dans
`/System/Volumes/Preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/`.
Son SHA-256 est
`c0cb9e3f489efe860ec67122bd51b93c816eac5ff4f48884375f0c908fea536d`.
Les getters CPU et DMA commencent respectivement à `0x18012cd8c` et
`0x18012cda4` dans ce cache.

## L'égalité large_buf / gso_mtu n'est pas requise

L'addendum propose de tester une égalité stricte à 16384. Le code XNU public
utilise au contraire `large_buf_size >= sk_fsw_gso_mtu` pour activer GSO logiciel.
Avec TSO matériel, il prend directement `large_buf_size` comme MTU TSO.
[fsw_tso_setup](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/nexus/flowswitch/fsw.c#L323).
Les valeurs 32512/32768 sont compatibles avec la logique qui choisit le maximum
des MTU TSO, ou le maximum de la taille existante et de `gso_mtu`.
[Ajustement des tailles](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/net/dlil.c#L657).
Cela ne prouve pas l'absence de tout bug de géométrie dans macOS, mais ne justifie
pas de changer ces tailles en même temps que la correction d'adressage.

## Ce que disent les compteurs des flows

L'aide de `skywalkctl flow` nomme les colonnes `InPkts/InSPkts` et
`OutPkts/OutSPkts`. Les structures XNU distinguent paquets et super-paquets.
`7/0` ne désigne donc pas sept transmissions et zéro perte.
[Structures](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/os_stats_private.h#L1312).
Le compteur de sortie est mis à jour avant l'enqueue matérielle ; il ne prouve
pas une émission sur le fil.
[flow_track_stats](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/skywalk/nexus/flowswitch/flow/flow_track.c#L660).

`TxCopyMbuf` existe aussi dans le chemin natif de copie mbuf vers paquet. Le
nom de classe IORegistry `IOSkywalkLegacyEthernet` ne suffit pas à choisir
entre ce chemin et le mode XNU `NA_NETIF_COMPAT`. La présence d'un SYN dans BPF
ne prouve pas non plus son passage par les sockets BSD.

## Médias et configuration des pools

Journaliser `getSupportedMediaArray()` permet de vérifier à quel moment il est
appelé, avec quelle capacité et quelle liste retournée. La liste reste vide
pendant `super::Start()`, même dans la version 0.2.16. Le contrat public ne
précise pas assez l'ordre des appels pour attribuer le problème à cette étape.
Il faut également conserver le retour de `reportLinkStatus()` : le log actuel
« link up » ne garantit pas que la notification a été acceptée.

Les headers SDK acceptent explicitement des pools TX et RX distincts pour
l'overload de `registerEthernetInterface()` utilisé. Ils indiquent aussi que
`getTxDataOffset()` vaut zéro par défaut. Aucun élément examiné ne justifie de
changer arbitrairement la taille des buffers, les flags DMA, ou d'ajouter
`PoolFlagVirtualDevice`, réservé aux interfaces virtuelles.

Les constantes de média ne comprennent pas automatiquement le bit Full Duplex.
Ce défaut de reporting et les cas 10/100 Mbit/s manquants de `handleChosenMedia()`
méritent une correction séparée ; leur rôle dans la panne native n'est pas établi.

## Validation du correctif

Le test `tests/rtl8127_tx_buffer_test.cpp` vérifie les octets visibles par le CPU
et la DMA aux offsets 0, 2, 8 et 18, les écritures d'en-tête et les dépassements
de tampon ou d'adresse. Il passe avec AddressSanitizer et UndefinedBehaviorSanitizer.

```sh
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/rtl8127_tx_buffer_test.cpp -o /private/tmp/rtl8127_tx_buffer_test
/private/tmp/rtl8127_tx_buffer_test
```

La compilation Release du dext et de l'app réussit pour **arm64 et x86_64**.
Les avertissements restants du fichier pilote portent sur les anciennes
déclarations RPC NDK dépréciées. Deux relectures indépendantes n'ont identifié
aucun blocage dans la correction d'offset.
L'app de vérification, non signée et non activée, est dans
`/private/tmp/rtl8127-ndk-review-build/Release/RTL8127App.app` ; le log de build
est dans `/private/tmp/rtl8127-ndk-review-build.log`.
Le numéro de version du projet n'a pas été modifié : utiliser un nouveau
numéro pour le prochain build signé afin d'identifier sans ambiguïté le code chargé.

Après construction, signature et activation d'une version contenant le correctif :

1. Vérifier le numéro chargé, puis lancer un seul essai `nscurl` port 80 et un
   essai BSD vers la même destination. Relever aussi le flow pour confirmer
   que Network.framework utilise `en10`.
2. Observer `tx offered with offset` et le compteur permanent
   `offered offset-nonzero` dans `txq`. Ce compteur porte sur les paquets proposés,
   y compris ceux refusés ; un même paquet proposé à nouveau peut être recompté.
3. Chercher les logs `tx SYN` et `rx SYN`, désormais séparés et fondés sur un
   vrai bit SYN. Un SYN natif avec `dataoff 2`, puis un SYN-ACK et une réponse
   HTTP confirmeraient le mécanisme.
4. Si `offset-nonzero` reste nul pendant l'essai, le défaut d'adressage est
   corrigé mais n'explique pas ce blocage. Utiliser alors les compteurs de
   refus lien/capacité, les statistiques de queue activées et une capture
   externe pour poursuivre la localisation.

Les sources publiques citées sont celles de `xnu-12377.121.6`, antérieures au
noyau installé `12377.161.14`. L'inspection NDK, elle, porte sur le framework
installé. La compilation et les tests de tampon ne remplacent pas le test réseau
sur la carte.
