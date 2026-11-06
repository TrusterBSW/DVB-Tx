# Pistes d'optimisation du moteur (à regarder plus tard)

État au 2026-11-03, après les commits `541c546` (banc de filtres sur plusieurs cœurs, symboles
par blocs, sc16 vers UHD) et `8a70747` (marge de 90 % de la demi-bande sur uhd).

## Où on en est

B210 → B210 (récepteur dvbrx avec égaliseur sur les pilotes), gain 40, 256APSK 135/180 :

| Débit symbole | Roll-off | Cadence radio | Débit utile | SNR reçu | Erreurs TS |
|---|---|---|---|---|---|
| 40 Msym/s | 0,10 | 56 MS/s | 231 Mbit/s | 20,5 dB (sans égaliseur) | 0 |
| 45 Msym/s | 0,10 | 56 MS/s | 260 Mbit/s | 24,0 dB | 0 |
| 50 Msym/s | 0,10 | 61,36 MS/s | 289 Mbit/s | 24,1 dB | 0 |
| 52 Msym/s | 0,05 | 61,425 MS/s | **300 Mbit/s** | 23,8 dB | 0 |

52 Msym/s en roll-off 0,05 occupe 54,6 MHz : c'est le maximum du B210, dont le filtre
analogique plafonne à 56 MHz, avec la marge de 10 % du planificateur. L'ancien moteur
décrochait à 37,5 Msym/s.

## Où part le processeur (i3-8100T, 4 cœurs à 3,1 GHz, 52 Msym/s)

Environ 2,4 cœurs en tout :

| Étage | Charge | Thread(s) |
|---|---|---|
| Banc de filtres (FFT) | 114 % | thread du banc + 3 workers |
| Codage DVB-S2X (BBFRAME, BCH, LDPC, mapping) + multiplexeur | 71 % | un seul thread par porteuse |
| Envoi UHD | ~45 % | thread d'envoi du module (code interne d'UHD) |
| Thread radio | ~10 % | un seul |

Un bloc du banc à N = 17 010 (L = 14 400), mesuré au compteur de cycles (`rdtsc`) :

| Étape | kcycles par bloc |
|---|---|
| remplissage (`fill`, séquentiel) | 296 |
| FFT directe (L) | 140 |
| accumulation des raies | 124 |
| FFT inverse (N) | 333 |
| remise à zéro de Y | 10 |

Le remplissage tourne dans le seul thread du banc. C'est donc lui qui limite le parallélisme.

## Pistes, de la plus rentable à la moins sûre

### 1. Remplissage et accumulation du banc (patch prêt : `fcbank-fill-runs.patch`)

- Les nouveaux symboles s'écrivent directement dans le bloc. Seul le recouvrement (L − h
  symboles) est gardé, au lieu d'un `memmove` de la fenêtre suivi d'une copie de L symboles.
- La fréquence résiduelle (décalage hors grille, inférieur à une raie) n'est plus appliquée
  par une multiplication complexe en double à chaque symbole. Elle passe par la phase du
  premier symbole (en double) et une table `e^{j2π·dfph·s}` en float, dans une boucle
  vectorisable.
- Les raies sont accumulées par segments contigus (`src` et `dst` consécutifs), dans une boucle
  vectorisable, au lieu d'un accès indexé raie par raie.
- Vérifié : précision inchangée (`fcbank_check` : −76,6 dB sur grille, −83,9 dB hors grille,
  comme l'ancien code), et sortie identique pour 0 à 5 workers (`fcbank_threads_check`). La
  sortie n'est plus identique au bit près à celle d'avant le patch : la rotation en float
  diffère vers 1e-7.
- Gain mesuré sur l'i3 : environ 22 % sur le banc.
- À appliquer avec `git apply docs/optimisation/fcbank-fill-runs.patch`.

### 2. Blocs FFT d'environ 8k au lieu de 16k

- Aujourd'hui, `fcbank_new` vise N ≥ 16 384. À N = 17 010, deux tampons de 136 et 115 Ko
  dépassent le cache L2 de 256 Ko de l'i3. La FFT inverse coûte alors 333 kcycles, contre
  98 kcycles à N = 9 072 (pour un bloc deux fois plus petit).
- Le recouvrement reste d'environ 256 symboles, soit 96 % d'échantillons utiles à N ≈ 9k.
- Gain mesuré : environ 18 %. **Avec la piste 1, le banc passe de 106 % à 64 % d'un cœur (−40 %)
  à 52 Msym/s, sur un seul thread.**
- À vérifier avant : les petites porteuses (DVB-C à 6,9 Msym/s, groupes DVB-T2, banc à 16 MS/s).
  Elles ont moins de raies par bloc, et la grille des fréquences exactes (`df`) devient plus
  grossière.

### 3. Cadences radio « FFT-friendly » dans le planificateur

- À 50 Msym/s, le planificateur a choisi 61,3636 MS/s. Le rapport Rs/Fs fait alors entrer un
  facteur 11 dans N et L, FFTW passe en `FFTW_ESTIMATE` (plan moins bon), et le banc monte
  à 122 %, contre 114 % à 52 Msym/s à 61,425 MS/s.
- Piste : parmi les cadences possibles, préférer celles dont le dénominateur de Rs/Fs est
  7-lisse (2, 3, 5, 7), ou mieux 5-lisse.

### 4. Thread radio : envoyer le bloc sans le recopier

- Avec une seule porteuse (pas de DVB-T, un seul banc), `radio_thread` remet le bloc à zéro, y
  additionne l'anneau du banc, puis `ud_write` le convertit. On peut passer directement la case
  de l'anneau à `drv->write`.
- Gain estimé : 5 % d'un cœur environ (deux passes mémoire de 8 octets par échantillon en moins).

### 5. Codage DVB-S2X — fait le 2026-11-06 (non commité)

C'est un seul thread, à 71 % à 52 Msym/s. Au-delà, il saturerait avant le banc.

Par trame 256APSK 135/180 (kcycles, mesuré au `rdtsc` sur le PC) :

| Étape | Avant | Après | Comment |
|---|---|---|---|
| BBFRAME + CRC-8 | 32 | 32 | inchangé |
| embrouillage + BCH | 48 | 26 | BCH 8 octets à la fois par multiplication sans retenue (PCLMUL, réduction de Barrett), choisi à l'exécution ; l'ancienne version reste pour les processeurs sans PCLMUL |
| LDPC | 101 | 72 | parité en ordre naturel par transpositions de blocs de 8 × 8 bits au lieu de bit par bit |
| entrelacement (demux) | 45 | 36 | une seule passe sur les symboles au lieu d'une par bit |
| mapping + trame PL | 32 | 32 | inchangé |
| total | 31 cycles/symbole | 23,5 cycles/symbole | −23 % |

Vérifications :
- les 202 configurations DVB-S2/S2X (trames normales et courtes, pilotes ou non) sont identiques à l'octet près à l'ancien code ;
- 53 codes LDPC et 76 codes BCH (S2, S2X, T2) ont été comparés sur des entrées aléatoires, sans aucune différence ;
- en DVB-T2, l'écart avec l'ancien code est le bruit d'arrondi de FFTW (8·10⁻⁷ du RMS, le même que l'ancien code contre lui-même).

Sur l'i3 à 52 Msym/s, le thread de la porteuse (multiplexeur compris) passe de 63 % à 57 %.

Il reste :
- le cœur du LDPC (≈ 45 kcycles) : AVX2 en multiversion (`target_clones`) ;
- la sortie en ordre naturel (≈ 25 kcycles) ;
- l'entrelacement (36 kcycles) ;
- le multiplexeur, qui n'est pas mesuré à part.

Paralléliser trame par trame ne réduit pas la charge totale : utile seulement au-delà de ce que permet la radio.

### 6. Envoi UHD — fait le 2026-11-06 (non commité)

À 61,4 MS/s, UHD prenait 58 % d'un cœur : 34 % dans notre thread `uhd-send` et 24 % dans son thread de transport USB.
Les trames USB de 16 360 octets, au lieu des 8 176 par défaut, divisent le nombre de paquets par deux. UHD descend alors à
40 % (26 + 14), sans aucun sous-débit sur 2 minutes à 52 Msym/s.
À 32 760 octets, UHD accepte la taille, mais le B210 décroche dès l'émission.

Le module UHD l'ajoute de lui-même (`send_frame_size=16360`) quand la carte visée est un B2xx en USB 3. Il le vérifie
dans sysfs, d'après le fabricant et la vitesse, pour la carte de `serial=...` ou pour toutes celles qui sont branchées.
Si `args` donne `send_frame_size`, sa valeur prime.

Les threads du moteur ont maintenant des noms, visibles dans `top -H` : `carrier0`, `bank0`, `bank-w0`, `radio0`,
`uhd-send`, `uhd-async`, `t2-w0`, `inputs`. Le thread de transport créé par UHD hérite du nom `radio0`.

Bilan sur l'i3 (52 Msym/s, 256APSK 135/180, roll-off 0,05) : le moteur passe de 248 % à 217 % d'un cœur au total.

Les pistes 1 à 4 ramèneraient l'i3 d'environ 2,4 à environ 1,7 cœur à 52 Msym/s. C'est de la
consommation et de la chaleur en moins sur batterie, pas seulement de la charge par cœur.

## À envisager : le GPU pour alléger le processeur

L'ingest utilise déjà le GPU pour les transcodages (`dvbtx gpu`, QSV, VAAPI, NVENC). La
synthèse radio pourrait aussi y passer. L'i3 a un iGPU Intel UHD Graphics 630 (GT2, environ
400 GFLOPS en float). Il partage la mémoire avec le processeur, donc il n'y a pas de copie PCIe.

Ce qui se prête au GPU :

- **Le banc de filtres.** C'est le candidat évident : FFT directe, pondération RRC, placement
  des raies et FFT inverse, sur des lots de blocs indépendants. C'est le schéma idéal pour un
  GPU (plusieurs dizaines de blocs par lancement). À 61,44 MS/s, il faut environ 8 GFLOP/s, que
  l'UHD 630 absorbe largement.
  - Bibliothèques : VkFFT (Vulkan, OpenCL ou CUDA, toutes tailles y compris les facteurs 3, 5,
    7, 11, 13), clFFT (OpenCL, ancienne), cuFFT (NVIDIA seulement).
  - Pour rester portable (Intel, AMD, NVIDIA, et Raspberry Pi pour certains), VkFFT sur Vulkan
    ou OpenCL serait le choix.
- **Le codage LDPC**, par lots de trames : des XOR de mots de 360 bits, très parallèles. Moins
  rentable que le banc, car le codage est déjà léger (environ 25 % d'un cœur pour BCH + LDPC
  à 45 Msym/s).
- **La conversion sc16 et la somme des porteuses** pourraient suivre dans le même noyau, pour
  ne rendre au processeur que les échantillons prêts à envoyer.

Les contraintes :

- **Latence** : il faut des lots de blocs pour amortir les lancements (environ 0,3 ms par bloc
  à 61 MS/s). Une latence de quelques dizaines de ms reste acceptable : l'avance du moteur
  est déjà de 300 ms sur le B210.
- **Le transcodage de l'ingest** partage le même GPU : il faut vérifier qu'ils cohabitent sans
  faire de trous à l'antenne.
- **La consommation** : sur batterie, un iGPU réveillé en permanence peut coûter plus que les
  cœurs qu'il libère aux faibles débits. Il faudrait un seuil, comme pour `synth_threads`
  (GPU seulement au-delà de 30-40 MS/s), et mesurer au RAPL (paquet et GPU) avant de choisir.
- **Le code** : un backend optionnel (`synth = "gpu"`, ou automatique si un GPU est trouvé),
  avec un repli sur le banc processeur, et une vérification de précision contre `fcbank` (même
  `fcbank_check`).
- **Le paquet** : la dépendance Vulkan ou OpenCL doit rester optionnelle, comme UHD et
  SoapySDR (module chargé seulement si la bibliothèque est présente).

Ordre proposé : d'abord les pistes 1 à 4 (simples, sûres, environ −30 % de processeur), puis
mesurer. Le GPU ne vaudra la peine que si l'on veut plusieurs porteuses larges à la fois (par
exemple deux radios à 61 MS/s), ou libérer l'i3 pour d'autres usages.

# Matériel à prendre en charge

## bladeRF (Nuand)

Aujourd'hui, un bladeRF ne passe que par SoapySDR (`driver = "soapy"`, `args = "driver=bladerf"`),
si le module SoapyBladeRF est installé. Le module générique `sdr_soapy.c` envoie du CF32 et ne
connaît pas les limites de la carte. Le planificateur ne sait rien d'elle non plus : pas de
cadence pratique, pas de marge au bord de bande. Rien n'a été essayé sur une vraie carte.

**Les cartes :**
- **bladeRF 2.0 micro (xA4 / xA9)** : AD9361 comme le B210, 47 MHz à 6 GHz, jusqu'à 61,44 MS/s
  (56 MHz de bande analogique), 2 × 2, USB 3. Mêmes performances attendues que le B210, donc
  jusqu'à environ 300 Mbit/s dans un seul multiplex DVB-S2X.
- **bladeRF x40 / x115 (première génération)** : LMS6002D, 300 MHz à 3,8 GHz, 40 MS/s au plus,
  28 MHz de bande analogique au plus, USB 3.

**Décision : le module doit utiliser le pilote officiel de Nuand (libbladeRF), pas
SoapySDR.** SoapySDR reste disponible pour les autres cartes, mais on ne s'appuie pas sur
SoapyBladeRF pour le bladeRF. La raison : le contrôle du format (sc16 et sc8), le mode 8 bits à
122,88 MS/s, les métadonnées de sous-débit, et un processeur moins chargé sans couche
intermédiaire.

**Proposition : un module natif `dvbtx-sdr-bladerf.so` sur libbladeRF**, sur le modèle de
`sdr_uhd.c` :
- API synchrone (`bladerf_sync_config` / `bladerf_sync_tx`), format `BLADERF_FORMAT_SC16_Q11`
  (12 bits, pleine échelle ±2048). Notre conversion SSE2 vers sc16 sert avec un autre facteur
  d'échelle.
- Anneau et thread d'envoi comme dans le module UHD. Sous-débits via les métadonnées
  (`BLADERF_META_STATUS_UNDERRUN`) ou les compteurs de la bibliothèque.
- Arguments : `serial=` (voir la fonction « numéro de série » plus bas), cadence, bande
  analogique (`bladerf_set_bandwidth`), gain (`bladerf_set_gain`), horloge de référence
  externe (`clock = "external"`, 10 MHz), canal TX1 ou TX2.
- Planificateur : `DRIVER_RATES["bladerf"]` (0,52 à 61,44 MS/s pour la 2.0, 40 MS/s pour la
  x40/x115) et `PRACTICAL_RATES` (56 MS/s pour la 2.0). La marge de 90 % de la demi-bande,
  comme pour uhd, puisque la puce est la même AD9361.
- `dvbtx devices` : `bladeRF-cli -p` pour les numéros de série. Détection de la génération
  (`bladerf_get_board_name` : « bladerf1 » ou « bladerf2 ») pour choisir les limites.
- Paquet : module construit seulement si `pkg-config libbladeRF` le trouve (comme UHD et
  SoapySDR), dépendance `libbladerf2` en « Recommends » dans le .deb, pour Debian 12 et 13 et
  Ubuntu 24.04.
- Documentation FR et EN : dvbtx-engine.8 (arguments du module), dvbtx.toml.5 (pilote
  `bladerf`), README (tableau du matériel).

**Mode 8 bits à 122,88 MS/s (bladeRF 2.0 micro)** : libbladeRF 2.4+ et un FPGA récent proposent
un mode « oversample », format `BLADERF_FORMAT_SC8_Q7`, jusqu'à 122,88 MS/s sur un seul canal.
- **USB** : 2 octets par échantillon, soit 246 Mo/s. C'est le même débit que 61,44 MS/s en sc16,
  et USB 3 tient.
- **Quantification** : 8 bits donnent environ 48 dB en pleine échelle, soit environ 35 dB avec
  nos 13 dB de recul. C'est assez pour le DVB-S2X 256APSK (environ 24 dB nécessaires) et pour
  le DVB-T/T2. C'est juste pour le DVB-C 256-QAM (environ 30 dB) : il faudra peut-être
  moins de recul, ou un recul choisi par norme.
- **Bande utile, à vérifier dans la documentation Nuand et sur la carte** : savoir si ce mode
  élargit vraiment la bande émise au-delà des 56 MHz du filtre analogique de l'AD9361, ou s'il
  donne seulement plus de marge jusqu'à Nyquist.
  - Dans le second cas, le gain est quand même réel : les bords du canal restent loin des
    filtres numériques. C'est ce qui nous a coûté 18 dB de plancher sur le B210 au-delà de
    40 Msym/s.
  - Dans le premier cas, une seule radio pourrait porter plusieurs multiplex étalés sur plus
    de 56 MHz (plusieurs DVB-T, T2 et C côte à côte).
- **Processeur** : la synthèse coûte deux fois plus qu'à 61,44 MS/s. Sur l'i3, le banc passerait
  d'environ 1,1 à 2,3 cœurs, ce qui est trop sans les pistes 1 à 4 (−40 % sur le banc) ou sans
  le GPU. Ce mode justifierait à lui seul le passage du banc sur GPU.
- **Code** : la conversion de sortie vers sc8 (SSE2 : `_mm_packs_epi32`, puis
  `_mm_packs_epi16`, avec saturation). Le planificateur doit savoir choisir entre sc16 jusqu'à
  61,44 MS/s et sc8 au-delà, par exemple avec `args = "format=sc8"` ou automatiquement selon la
  cadence demandée.
- **Coût en plus** : environ 1 h 30 sur le module natif, plus les essais : qualité à 8 bits,
  sous-débits à 122,88 MS/s, et largeur réelle mesurée par dvbrx.

**Coût : environ 3 à 4 h** de code, de paquet et de documentation, plus les essais avec une
vraie carte : niveau de sortie, sous-débits à 56-61 MS/s, et réception par dvbrx comme pour le
B210 (DVB-T, DVB-C 256-QAM, DVB-S2X large). **Il faut une carte bladeRF** : à prévoir avant de
commencer. Sans carte, on peut seulement compiler et valider avec la radio null.

(Écarté : passer par SoapySDR en apprenant seulement les limites de la carte au
planificateur. C'est plus rapide à faire, mais ça ne donne pas le mode 8 bits et ça coûte plus de
processeur.)

# Défauts et fonction à venir (estimations du 2026-11-03)

Détail des deux défauts : `docs/A_REVOIR.md` (remontés par la session dvbrx).

## Défaut 1 : une radio sans porteuse fait redémarrer le moteur en boucle

Reproduit aussi le 2026-11-03 à 22 h 15 sur l'i3, pendant les essais radio.

**Cause** : `write_engine_ini` (dvbtx/config.py) saute les radios sans porteuse mais garde leur
numéro (`[radio 1]` sans `[radio 0]`). Le moteur refuse l'INI (« must be numbered 0..15 in
order ») et redémarre en boucle, alors que l'API a répondu `ok`.

**Correction :**
- **a) Renuméroter les radios écrites** (0..n−1) dans `engine.ini`. Il faut retraduire ces
  numéros dans les statistiques du moteur, car le superviseur compare `r["index"]` à
  `car.radio.index` : alertes, `radios_off`, avance d'horloge, page web. Ajouter un cas à
  `planner_test.py` et un essai avec deux radios null.
  **Environ 1 h à 1 h 30**, risque faible.
- **b) Ne plus échouer en silence** : si le moteur sort en erreur juste après un « Appliquer »,
  revenir à la configuration précédente et renvoyer l'erreur à l'API et à la page web.
  Alternative : un mode `dvbtx-engine --check` qui valide l'INI avant l'application.
  **Environ 1 h.**

## Défaut 2 : les porteuses arrêtées comptent dans la bande d'une radio

**Cause, en partie voulue** : une porteuse arrêtée reste dans `engine.ini` (en sourdine) pour
que « Démarrer » la remette à l'antenne sans redémarrer le moteur. Elle doit donc garder sa
place dans la bande.

**Options :**
- **Message clair** (recommandé, 15 min) : « 3 porteuses, dont 2 arrêtées, demandent… :
  supprimez-les ou mettez-les sur une autre radio ».
- **Ne plus planifier les porteuses arrêtées** (environ 1 h 30) : « Démarrer » une porteuse non
  planifiée provoquerait alors une nouvelle planification et un redémarrage du moteur, soit
  quelques secondes de coupure pour les autres porteuses de la radio. On perd l'intérêt du
  Démarrer/Arrêter sans coupure.

## Fonction : radios liées à leur numéro de série

**Aujourd'hui** : le numéro de série ne peut aller que dans `args = "serial=…"`, à la main. Sans
lui, le pilote prend le premier appareil trouvé. Avec deux HackRF ou deux B210, ils peuvent donc
s'échanger après un redémarrage ou un rebranchement.

**Proposition :**
- **Clé `serial`** dans `[[radio]]`, traduite par pilote (HackRF `serial=`, UHD `serial=`,
  SoapySDR `serial=`). Le planificateur refuse deux radios avec le même numéro. Le numéro est
  affiché dans l'état de la radio (page web, `/api/status`).
- **API `/api/devices?driver=…`** : les appareils présents, via `hackrf_info`,
  `uhd_find_devices` et `SoapySDRUtil` (déjà utilisés par `dvbtx devices`, installés sur
  l'i3), avec un délai maximal.
- **Boîte d'ajout ou de modification d'une radio** (page web) :
  - un champ « Numéro de série » ;
  - une case « Enregistrer automatiquement le numéro de série » : elle prend l'appareil de ce
    pilote qui n'est pas déjà attribué à une autre radio ;
  - s'il y en a plusieurs, une liste déroulante pour choisir.
- **CLI** : `dvbtx devices --save NOM_DE_RADIO` (même choix).
- **Documentation** FR et EN (dvbtx.toml.5, dvbtx-web.7, dvbtx.1) et un test de configuration
  (doublons, traduction en `args`).

**Coût : environ 3 à 4 h.**

**Point à vérifier d'abord** : la détection d'un appareil déjà ouvert par le moteur.
`uhd_find_devices` ou `hackrf_info` pourraient perturber une émission en cours. Si c'est le
cas, le moteur indique lui-même le numéro de série de chaque radio ouverte (nouvelle entrée
dans ses statistiques), et la détection n'interroge que les appareils libres.
**Environ 30 min de plus.**

## Total et ordre proposé

Environ 6 à 8 h, essais sur l'i3 compris (créneau à coordonner avec la session dvbrx) :
1. défaut 1 (il coupe l'émission sans prévenir) ;
2. message du défaut 2 ;
3. numéros de série.
