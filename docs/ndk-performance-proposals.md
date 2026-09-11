# Performances NDK après le correctif des flux natifs

Proposition du 12 septembre 2026. À MTU 1500, les 9,41 Gbit/s en TX
monoflux atteignent déjà pratiquement la capacité utile du lien. La priorité
est de conserver ce débit avec plusieurs flux, puis de réduire le coût CPU.

Pour des trames IPv4 pleines avec timestamps TCP, sans VLAN, le calcul donne
`10 × 1448 / 1538 = 9,415 Gbit/s` : 1448 octets utiles pour 1500 octets IP,
auxquels s'ajoutent Ethernet, FCS, préambule et intervalle intertrame.
C'est une estimation dans ces conditions, pas une mesure du format de tous
les paquets du test.

## Mesures disponibles

Les résultats initiaux viennent des mesures transmises par Stéphane et du
journal du serveur iperf3, sur la version 0.2.17 avec un pool TX de 512 paquets
de 32 Kio.

| Sens depuis le Mac | Flux | Débit observé | Retransmissions TCP |
| --- | ---: | ---: | --- |
| TX | 1 | 9,41 / 9,41 Gbit/s | 0 / 0, côté émetteur Mac |
| RX | 1 | 9,26 / 9,32 Gbit/s | 26 / 132, côté émetteur serveur |
| TX | 4 | 4,61 Gbit/s | 0, côté émetteur Mac |
| RX | 4 | 9,39 Gbit/s | 854, côté émetteur serveur |

Le journal serveur confirme un TX à quatre flux durablement autour de
4,5–4,6 Gbit/s, réparti entre les quatre connexions. Le RX multiflux remplit
le lien mais comporte des retransmissions ; il faut les conserver dans la
comparaison même lorsque le débit reste bon.

Le profilage suivant, rapporté par Claude dans le commit
[`f7a8dfb`](https://github.com/stefb69/RTL812xLucy/commit/f7a8dfb74d07446b4f6c193fd1937dd9fba0c74a),
obtient 9,24 Gbit/s à quatre flux puis 4,47 Gbit/s à huit flux, avec
511 paquets en vol. Le ralentissement n'est donc pas systématique dès quatre
flux. Ce commit passe le pool TX à 1024 paquets en 0.2.18, soit 32 Mio de
tampons, hors métadonnées.

L'essai suivant, consigné dans
[`bec3a6b`](https://github.com/stefb69/RTL812xLucy/commit/bec3a6b8b5275cb3d7e8950ae91b85b57774c495),
rapporte 1017 descripteurs occupés, de nombreux refus et des milliers de
retransmissions à huit flux. Avec ces mêmes flux limités à 1,1 Gbit/s chacun,
le débit atteint 8,8 Gbit/s avec deux retransmissions. Cela désigne les
rafales et l'accumulation de paquets comme une autre piste. La version
0.2.19 conserve le pool de 1024 mais limite à 256 les paquets en vol.
La présente branche est basée sur ce commit et conserve cette limite.

Un pool de 512 paquets peut manquer de paquets disponibles alors que le ring
de 1024 descripteurs a encore de la place. Un compteur de refus pour ring
plein à zéro n'écarte pas cette situation. La taille moyenne des paquets TSO
et le délai de retour au framework comptent autant que la taille du pool.
Les essais suivants montrent qu'agrandir le pool ne suffit pas à éliminer
le ralentissement multiflux. Le gain de la limite à 256 reste à mesurer.

## Modifications proposées

Le pilote restitue maintenant les crédits de descripteurs par lot de
complétions. Un lot de N descripteurs terminés demandait N incréments
atomiques du compteur libre ; il en demande un. Les slots sont nettoyés
avant la publication des crédits, l'opération conserve son ordre mémoire
et précède les notifications NDK. La réservation des descripteurs lors de
l'émission garde son fonctionnement actuel.

La limite de soumission et la capacité annoncée par `txQueryFreeSpace`
restent celles de 0.2.19 : au plus 256 paquets en vol. Le regroupement des
crédits retarde leur publication jusqu'à la fin du lot récupéré ; il ne
permet jamais de dépasser cette limite.

Les compteurs supplémentaires suivent la taille des lots TX, les lots de
descripteurs terminés et les paquets fournis aux callbacks de complétion.
Ils servent à distinguer un ring occupé d'un retour tardif des paquets au
framework. Les logs périodiques utilisent une copie des compteurs de
soumission prise sous `txLock`, puis libèrent ce verrou avant le formatage
et l'écriture du log.

Cette réduction du nombre d'opérations atomiques est vérifiable dans le
code. Son effet sur le débit et le CPU reste à mesurer sur la carte ; le
patch ajoute aussi un petit coût de comptage par callback.

Les quatre queues NDK correspondent aux classes BE/BK/VI/VO. Elles partagent
le ring matériel TX, et les relevés disponibles placent les flux iperf dans
BE. Quatre connexions TCP ne créent donc pas quatre rings matériels.

## Comparaison reproductible

Le collecteur [benchmark_ndk.py](../tools/benchmark_ndk.py) exécute TX puis
RX pour 1, 2, 4 et 8 flux, répète la matrice et conserve les résultats
iperf3 JSON avec les relevés `netstat -I … -qq` et `ifconfig` avant/après
chaque essai. Il produit `summary.csv` et `summary.json`. Les métriques
absentes restent nulles ; une erreur de connexion ne devient pas un débit
nul présenté comme une mesure réussie.

Depuis la racine du dépôt, pour préparer les commandes sans trafic :

```sh
python3 tools/benchmark_ndk.py --host 192.168.75.171 --interface en10 --dry-run
```

Pour une campagne, avec le serveur iperf3 déjà démarré :

```sh
python3 tools/benchmark_ndk.py --host 192.168.75.171 --interface en10 \
  --time 15 --omit 3 --reps 3 --output /tmp/ndk-baseline-0219
```

Une fois cette branche construite, signée et chargée, relancer la même
commande avec un autre répertoire de sortie, par exemple
`/tmp/ndk-batched-0219`. Le collecteur ne remplace pas le pilote et ne modifie
aucun paramètre réseau. Vérifier la version réellement active avec
`systemextensionsctl list` et noter le commit exact de chaque build.

Comparer 0.2.19 et cette branche avec le même pool de 1024 paquets, la même
limite de 256 en vol, les mêmes MTU, machines et versions d'iperf3. Modifier
le pool ou la limite en même temps empêcherait d'attribuer un gain au
regroupement des crédits. Répéter ensuite le baseline pour détecter une
dérive entre campagnes.
Ne pas lancer deux campagnes simultanément sur le même lien.

L'iperf3 local est en version 3.21. Depuis 3.16, iperf3 utilise un thread par
flux ; une explication fondée sur l'ancien client monothread ne s'applique
pas ici. `-R` inverse les rôles, et `-O` exclut le démarrage des statistiques.
[FAQ ESnet](https://software.es.net/iperf/faq.html),
[options iperf3](https://software.es.net/iperf/invoking.html).

Les champs CPU du JSON iperf3 décrivent le processus iperf3 local et distant,
pas le CPU du dext. Conserver en parallèle les mesures du processus dext
faites pendant chaque run et les logs périodiques `txq`, `tx completion` et
`stats` du pilote.

## Lire les compteurs

Utiliser les différences entre deux relevés d'un même chargement de pilote,
pendant l'essai, plutôt que les totaux depuis le démarrage. Les relevés toutes
les cinq secondes décrivent des fenêtres assez larges : ils ne capturent pas
tous les pics ni nécessairement les mêmes bornes que la mesure iperf3.

- `ΔtxBytes / ΔtxSubmitted` donne la taille moyenne des paquets confiés au
  matériel, avant segmentation TSO. `ΔtxTso / ΔtxSubmitted` donne leur part TSO.
- `ΔtxSubmitted / ΔtxNonemptyCalls` donne la taille moyenne des lots de
  soumission qui ont rempli au moins un descripteur (`txq: nonempty`).
  Dans `tx completion`, `Δreclaim desc / Δreclaim nonempty` donne celle des
  lots récupérés. Pour un lot de N descripteurs, le changement économise N−1
  incréments atomiques. Ne pas diviser par tous les appels `reclaim calls`,
  qui comprennent les passages sans complétion. Ces nouveaux compteurs
  `reclaim` couvrent les complétions normales sur la work queue, et excluent
  la récupération forcée des descripteurs lors d'un arrêt du lien/interface.
- Les paquets terminés par le matériel et ceux fournis au callback NDK
  correspondent à deux étapes différentes. Le retour du callback ne prouve
  pas encore que le framework a recyclé le paquet dans son pool.
- La profondeur des FIFO de complétion est échantillonnée après les lots
  récupérés et par le timer. Son maximum observé peut manquer un pic bref
  lorsque le consommateur vide la FIFO en parallèle. Une profondeur de −1
  indique qu'aucun échantillon stable n'a été obtenu en trois tentatives.
  Les compteurs de callbacks, du ring et des FIFO ne forment pas une
  photographie atomique de l'ensemble du chemin ; éviter de soustraire
  aveuglément leurs valeurs, surtout au travers d'une désactivation.
- Contrôler les refus, paquets invalides, erreurs matérielles et pertes AQM.
  Depuis 0.2.19, `refused space` signale la limite des 256 paquets en vol,
  même si le ring physique a encore de la place. Un paquet refusé peut être
  proposé à nouveau : le compteur ne mesure pas les pertes de paquets.
  Les paquets invalides peuvent aussi être rendus au framework sans avoir
  été soumis au matériel.

Le but de la campagne est de retrouver un TX multiflux proche de 9,4 Gbit/s,
de maintenir TX/RX monoflux à leur niveau actuel et de comparer CPU et
retransmissions à débit égal. Un écart de 1–2 % demande plusieurs répétitions
avant de conclure. Vérifier aussi un accès HTTP/HTTPS natif après chargement,
pour conserver la validation du correctif des offsets.

## Suite selon les résultats

Si la limite à 256 stabilise le multiflux, le travail suivant peut viser le
CPU. Le remplissage RX parcourt encore le ring entier ; pendant un
TX soutenu, les ACK sollicitent également ce chemin. Un profil CPU doit
montrer son poids avant de remplacer ce parcours par un suivi des slots
manquants, qui doit gérer les erreurs et les retours de buffers.

Les jumbo frames constituent un essai séparé si tout le chemin accepte MTU
9000. Dans les mêmes hypothèses IPv4/TCP que plus haut, la limite utile passe
à `10 × 8948 / 9038 = 9,900 Gbit/s`, avec moins de paquets sur le fil. Aucun
changement de MTU n'est nécessaire pour évaluer cette branche.

## Validation de la proposition

Vérifié sur la base 0.2.19 :

- Build Release de l'application et du dext réussi pour arm64 et x86_64,
  sans signature. Les avertissements restants concernent les API NDK
  dépréciées et le code matériel existant.
- Test des buffers TX réussi sous AddressSanitizer et UndefinedBehaviorSanitizer.
- Quatre tests Python du collecteur réussis, dont la conservation des
  artefacts après des erreurs ou un JSON contenant des nombres non finis.
- Dry-run vérifié : 16 essais, 8 TX et 8 RX, tous liés à l'interface demandée.
- Relecture indépendante de l'ordre de publication des crédits, de la
  concurrence des nouveaux compteurs et de l'intégration sur 0.2.19.

Le pilote n'a pas été installé pour cette proposition. Aucun nouveau
benchmark réseau n'a été lancé pendant les mesures du front. Le gain de
cette branche reste à mesurer après chargement sur la carte.
