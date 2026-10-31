# dvbtx — émetteur DVB-T fait maison, multi-porteuses, sur radio logicielle

*[English version: README.md](README.md)*

dvbtx fabrique des multiplex DVB complets à partir de flux en direct, de périphériques de
capture, de fichiers ou de pipes, et les module sur une radio logicielle (HackRF One, USRP
B200/B210, et tout émetteur SoapySDR). Un seul programme remplace les scripts bash, ffmpeg,
TSDuck et GNU Radio de la version précédente :

- **autant de porteuses, de radios et de chaînes que l'on veut**, décrites dans un fichier TOML ou
  directement en ligne de commande ;
- toutes les porteuses d'une radio sont synthétisées **dans une seule FFT** : 2 multiplex sur un
  HackRF, 6 sur un B210 (190 Mbit/s utiles) ;
- multiplexeur en C **cadencé par la radio** : débit exact, PCR recalés, tables PSI/SI générées
  (NIT avec numéros de chaînes, SDT, EIT, TOT), bascule automatique sur un flux de secours ;
- page web de supervision : débits par chaîne, **marge de décodage**, alertes de dépassement,
  analyse TSDuck exportable en JSON ;
- paquet Debian et service systemd.

> ⚠️ **Coax fermé uniquement.** Émettre dans les bandes de télévision est interdit. dvbtx est fait
> pour un réseau coaxial privé ou un câble direct vers le téléviseur, avec des atténuateurs.

## Sommaire

1. [Architecture](#architecture)
2. [Installation](#installation)
3. [Démarrage rapide](#démarrage-rapide)
4. [Configuration](#configuration)
5. [Sources](#sources)
6. [Numéros de chaînes (LCN) et ONID](#numéros-de-chaînes-lcn-et-onid)
7. [Guide des programmes (EPG)](#guide-des-programmes-epg)
8. [Débit utile : `dvbtx calc`](#débit-utile--dvbtx-calc)
9. [Supervision, alertes, export TSDuck](#supervision-alertes-export-tsduck) (aperçu des chaînes, configuration depuis la page, profils)
10. [Transcodage sur GPU](#transcodage-sur-gpu)
11. [Flux de secours](#flux-de-secours)
12. [Service systemd](#service-systemd)
13. [Plusieurs porteuses, plusieurs radios](#plusieurs-porteuses-plusieurs-radios)
14. [Mesures](#mesures)
15. [Validation](#validation)
16. [État et suite](#état-et-suite)

## Architecture

```
  sources                    ingest (1 processus ffmpeg par source)        moteur C (dvbtx-engine)
  ─────────                  ───────────────────────────────────────      ─────────────────────────────────────────
  HLS / HTTP / M3U ─┐                                                     ┌ réception UDP, démux PAT/PMT
  Twitch, YouTube ──┤        ffmpeg -c copy  (ou encodage pour les        │ horodatage idéal de chaque paquet
  RTMP / RTSP / SRT ┼──────► captures)  ──── MPTS/SPTS en UDP local ────► │ (interpolé entre PCR, tampon fixe)
  tvheadend (HTSP) ─┤        relancé automatiquement                      │
  webcam / HDMI ────┤                                                     ├ multiplexeur par porteuse, tiré par
  écran / mire ─────┤        UDP multicast/unicast direct ──────────────► │ le modulateur : SI dues > paquet le
  fichiers, dossier ┤                                                     │ plus urgent > PCR > paquet nul
  pipe, stdin, exec ┘                                                     │ PCR recalés, PID remappés, secours
                                                                          ├ codage DVB-T (RS, entrelaceurs,
  superviseur Python (dvbtx)                                              │ code convolutif, pilotes, TPS)
  ───────────────────────────                                             ├ une IFFT par radio pour toutes
  configuration, plan de fréquences, NIT/SDT/EIT/TOT, EPG,                │ ses porteuses, à sa fréquence
  page web, alertes, analyse TSDuck ◄── copie UDP du multiplex ◄───────── └ HackRF / UHD / SoapySDR (plugins)
```

Pourquoi ce découpage :

- **Chaque source est un processus séparé** relié en UDP : une source qui plante, gèle ou rame ne
  touche pas le reste ; la chaîne passe sur son secours et revient d'elle-même.
- **Le multiplexeur est cadencé par la radio** : chaque fois que le modulateur a besoin d'un paquet,
  il le demande. Plus de `-muxrate` ffmpeg, de `tsp -P regulate` ni de named pipe entre TSDuck et
  GNU Radio qui dérivent l'un par rapport à l'autre : le débit est exact par construction, les
  creux sont comblés par des paquets nuls, et les PCR sont recalculés à l'instant réel d'émission
  (gigue mesurée < 0,5 µs).
- **Le calcul lourd est en C** (multiplexage, codage canal, FFT avec FFTW, conversion) ; le
  superviseur Python ne fait que de la configuration, des tables SI et de la supervision.

## Installation

### Paquet Debian (Debian 12+, Ubuntu 24.04+)

```bash
sudo apt install ./dvbtx_0.1.0_amd64.deb      # tire ffmpeg, python3, libfftw3 ; libhackrf0/libuhd recommandés
```

Le paquet installe `/usr/bin/dvbtx`, le moteur et ses plugins radio dans `/usr/lib/dvbtx/`, la
configuration `/etc/dvbtx/dvbtx.toml`, les exemples `/usr/share/dvbtx/configs/`, le service
`dvbtx.service`, et crée `/var/lib/dvbtx/epg/` et le clip de secours `/var/lib/dvbtx/backup.ts`.

TSDuck (facultatif, pour l'analyse) : paquets sur <https://github.com/tsduck/tsduck/releases>.

### Depuis les sources

```bash
sudo apt install build-essential pkg-config libfftw3-dev ffmpeg python3 \
                 libhackrf-dev libuhd-dev libsoapysdr-dev          # chaque radio est facultative
git clone https://github.com/TrusterBSW/DVB-Tx && cd DVB-Tx
make -C engine                    # build/dvbtx-engine + build/dvbtx-sdr-{hackrf,uhd,soapy,null}.so
python3 -m dvbtx check -c configs/hackrf-1mux.toml
./packaging/build-deb.sh          # pour fabriquer le .deb
```

Python 3.11 minimum (tomllib), aucune dépendance Python hors bibliothèque standard.

## Démarrage rapide

Sans fichier, tout en ligne de commande (une radio, une porteuse) :

```bash
sudo dvbtx --driver hackrf --channel 22 \
     --service "Mire=lavfi:testsrc2=size=1280x720:rate=25" \
     --service "Film=/srv/videos/film.mkv,lcn=2" \
     --service "Live=https://exemple.com/live/index.m3u8,lcn=3"
```

Avec un fichier :

```bash
dvbtx check -c /etc/dvbtx/dvbtx.toml      # vérifie et affiche le plan de fréquences, sans émettre
sudo dvbtx run -c /etc/dvbtx/dvbtx.toml   # émet ; page web sur http://127.0.0.1:8080/
sudo dvbtx run -c ma.toml -s radio.0.gain=40 -s carrier.1.frequency=498MHz   # surcharges
```

Puis lancer une recherche de chaînes sur le téléviseur.

## Configuration

Le fichier de référence, commenté clé par clé : [`packaging/dvbtx.toml`](packaging/dvbtx.toml)
(installé en `/etc/dvbtx/dvbtx.toml`). Trois niveaux, sans nombre fixé à l'avance :

```toml
[[radio]]                      # une radio (répéter pour en avoir plusieurs)
name = "b210"
driver = "uhd"                 # hackrf | uhd | soapy | null
gain = 50

[[carrier]]                    # un multiplex (autant que la radio peut en porter)
name = "Mux 1"
radio = "b210"
standard = "dvbt"
frequency = 482_000_000        # ou channel = 22
bandwidth = 8_000_000          # 5, 6, 7, 8 MHz
mode = "8k"                    # 2k, 8k
constellation = "64qam"        # qpsk, 16qam, 64qam
code_rate = "7/8"              # 1/2 2/3 3/4 5/6 7/8
guard = "1/32"                 # 1/4 1/8 1/16 1/32
tsid = 1

  [[carrier.service]]          # une chaîne (autant que le débit le permet)
  name = "Ma chaîne"
  lcn = 1
  source = "https://exemple.com/live/index.m3u8"
  epg_id = "MaChaine.fr"
```

Ce qui est calculé tout seul, et peut être forcé :

| Valeur | Par défaut |
|---|---|
| fréquence centrale de la radio | au milieu des porteuses ; sous une porteuse seule (fuite de l'OL hors du canal) |
| fréquence d'échantillonnage | multiple entier du débit natif DVB-T couvrant toutes les porteuses (18,29 MS/s pour 2 porteuses, 54,86 pour 6) |
| `sid` (service_id) | `tsid << 8 \| n°` |
| `lcn` | le prochain numéro libre |
| PID | 0x100 + 0x20 × n° de chaîne (PMT, puis les flux) |
| `type` de service | d'après les flux (H.264 → 0x19, HEVC → 0x1F, MPEG-2 → 0x01, sans vidéo → radio) |

Toute clé se surcharge en ligne de commande avec `-s chemin=valeur` (`radio.0.gain=40`,
`carrier.0.service.2.source=...`, `network.onid=0x20FA`). Les exemples de `configs/` :
`hackrf-1mux.toml`, `hackrf-2mux.toml`, `b210-6mux.toml`, `sources.toml` (toutes les sortes de
sources), `ts-output.toml` (sans radio : le TS part en UDP vers un modulateur externe).

## Sources

| Syntaxe | Source |
|---|---|
| `/chemin/film.mkv`, `file:///...` | fichier, en boucle (`loop = true`) |
| `dir:/chemin/dossier` | tous les fichiers vidéo d'un dossier, l'un après l'autre, en boucle (formats mélangés acceptés) |
| `http(s)://...`, `...m3u8` | flux HTTP ou HLS (reconnexion automatique) |
| `m3u:/liste.m3u#Nom` | entrée d'une playlist M3U (par nom, tvg-name, tvg-id ou `#index`) ; URL ou fichier |
| `twitch://chaine` | Twitch via streamlink (ou yt-dlp) |
| `ytdlp:https://...` | tout ce que yt-dlp sait lire (YouTube : vidéo et audio séparés, H.264/AAC choisis par défaut) |
| `htsp://user:mdp@hote:9982/Chaine` | chaîne tvheadend (par nom ou numéro), via son port de streaming HTTP |
| `rtmp://hote/app/cle` ; `rtmp://0.0.0.0:1935/live/cle?listen` | RTMP en lecture ; ou en écoute pour recevoir OBS |
| `rtsp://`, `srt://`, `udp://`, `rtp://` | flux réseau |
| `udp+direct://239.0.0.1:2000` | SPTS lu directement par le moteur, sans ffmpeg |
| `v4l2:/dev/video0?size=1280x720&fps=30&format=mjpeg&audio=pulse:default` | webcam, carte de capture HDMI |
| `screen::0.0?size=1920x1080&fps=25` | écran X11 |
| `lavfi:testsrc2=size=1280x720:rate=25` | mire de test |
| `pipe:/chemin/fifo` | pipe nommé (n'importe quel conteneur lisible par ffmpeg) |
| `stdin:` | entrée standard de dvbtx (`programme \| dvbtx run ...`) |
| `exec:commande args` | sortie d'une commande |

Les sources de capture sont encodées (H.264 + AAC par défaut, débit `auto` = la part du multiplex
laissée par les autres chaînes) ; les autres sont copiées telles quelles. `encode = true` ou
`encode = { video_bitrate = "4M", video_codec = "h264_vaapi", hwaccel = "vaapi" }` force
l'encodage (voir `[defaults.encode]`). Avant de copier un fichier, un flux HTTP/HLS ou une vidéo
yt-dlp, ffprobe vérifie les codecs : une vidéo VP9/AV1 est encodée en H.264, un audio Opus/Vorbis
en AAC (seule la partie incompatible est réencodée ; `codec_check = false` pour ne jamais le faire). `ffmpeg_input`/`ffmpeg_output` ajoutent des options ffmpeg.
Essayées sur le banc : fichiers (y compris VP9/Opus réencodés), dossier (fichiers de formats
différents), HLS, M3U, YouTube via yt-dlp, RTMP en écoute, UDP direct, pipe nommé, stdin, `exec:`
et mire. Pas encore essayées (faute de matériel ou de compte) : Twitch, tvheadend, RTSP, SRT, v4l2
et écran.

## Changer les chaînes pendant l'émission : `dvbtx channel`

On ajoute, supprime, renomme, renumérote une chaîne ou on change sa source pendant que dvbtx
émet : seule cette chaîne est touchée (les autres ne perdent aucun paquet), et le changement est
réécrit dans le fichier de configuration (commentaires conservés ; `sid`, `lcn` et `pid_base` y sont
figés pour que rien ne se décale, les téléviseurs reconnaissant une chaîne à son service_id).

Changer de source ne coupe pas la chaîne : la nouvelle source démarre à côté de l'ancienne, et la
chaîne bascule sur une image clé de la nouvelle au moment où elle doit partir ; l'ancienne joue
jusque-là (15 s au plus). Mesuré : 25 à 33 ms entre deux paquets vidéo pendant la bascule, comme
sans changement.

```bash
dvbtx channel list                                        # numéro, service_id, nom, ce qui passe, source
dvbtx channel add "France 24" https://exemple.org/live.m3u8 --number 3
dvbtx channel add "Sky News" "ytdlp:https://www.youtube.com/@SkyNews/live" --option ytdlp_format=b
dvbtx channel source 3 "ytdlp:https://www.youtube.com/@France24_fr/live"   # sans coupure
dvbtx channel rename 3 "France 24 FR"
dvbtx channel renumber "France 24 FR" 1 --swap            # échange avec la chaîne qui avait le 1
dvbtx channel set 3 epg_id=France24.fr                    # n'importe quel réglage (CLÉ= l'enlève)
dvbtx channel remove 0x0103
```

Une chaîne se désigne par son numéro, son service_id (`0x0103`) ou son nom. Les mêmes opérations :

- **page web** : bouton **+ Ajouter une chaîne** par multiplex, **Modifier** (nom, numéro avec
  échange, sources, cache, secours) et **✕** par chaîne ;
- **API** : `GET/POST /api/channels`, `PATCH/DELETE /api/channels/<numéro|0xSID|nom>` (JSON) ;
- **fichier** : on l'édite à la main, puis `systemctl reload dvbtx` applique les différences chaîne
  par chaîne (les radios et multiplex demandent un redémarrage).

Quand dvbtx ne tourne pas, `dvbtx channel` modifie le fichier seul (pris au prochain démarrage) ;
`--offline` le fait même s'il tourne. Sécurité : les changements par le web sont refusés s'ils
viennent d'un autre site, exigent `web.token` s'il est défini (à mettre dès que `web.listen` n'est
pas `127.0.0.1`), et les sources `exec:`, `pipe:`, `stdin:` comme les arguments bruts
`ffmpeg_input` / `ffmpeg_output` ne passent que par le fichier
(`web.allow_exec_sources` pour l'autoriser). Référence complète : [`docs/CHANNELS.md`](docs/CHANNELS.md).

## Numéros de chaînes (LCN) et ONID

La NIT contient pour chaque multiplex le `private_data_specifier` EACEM (0x28) suivi du
`logical_channel_descriptor` (0x83), comme la TNT française. **Beaucoup de téléviseurs ignorent les
LCN si l'`original_network_id` ne correspond pas à leur pays** : par défaut `onid` et `network_id`
valent `0x20FA` (France). Pour un autre pays, mettre son ONID dans `[network]`. La NIT décrit tous
les multiplex du réseau avec leur vraie fréquence (`frequency_in_nit = false` met 0xFFFFFFFF comme
la TNT).

## Guide des programmes (EPG)

Tout fichier déposé dans `epg.dir` (`/var/lib/dvbtx/epg/` par défaut) est chargé, et relu à chaque
modification — un autre service peut l'écrire :

- **XMLTV** (`*.xml`, `*.xml.gz`) : les chaînes sont associées par `epg_id`, sinon par nom. Les
  heures avec décalage (`+0100`) sont converties en UTC ; sans décalage, `network.timezone`.
- **EIT au format XML de TSDuck** (comme ceux de l'ancien `xmltv2eit.py`) : associés par `service_id`.
- `epg.urls` : des URL XMLTV relues toutes les `url_refresh_s` (Tunarr, ErsatzTV, ...).

dvbtx produit l'EIT présent/suivant (mis à jour à chaque changement d'émission) et l'EIT
programmée sur `epg.days` jours : titre, description courte et longue, genre, âge conseillé
(`<rating>` CSA), en ISO 6937 par défaut comme TSDuck. `dvbtx epg -c ...` montre ce qui est trouvé
pour chaque chaîne ; `sudo systemctl reload dvbtx` force la relecture.

## Débit utile : `dvbtx calc`

```bash
dvbtx calc dvbt --table                     # tout le tableau DVB-T 8 MHz (ci-dessous)
dvbtx calc dvbt --constellation 64qam --code-rate 3/4 --guard 1/8 --services 6
dvbtx calc dvbs2 --symbol-rate 27.5M --modulation 8psk --code-rate 3/4
dvbtx calc dvbc --symbol-rate 6.9M --constellation 256qam
```

Débit utile DVB-T 8 MHz, en Mbit/s (paquets de 188 octets, tables PSI/SI comprises) :

| Modulation | Rendement | GI 1/4 | 1/8 | 1/16 | 1/32 |
|---|---|---|---|---|---|
| QPSK | 1/2 | 4,98 | 5,53 | 5,85 | 6,03 |
| | 2/3 | 6,64 | 7,37 | 7,81 | 8,04 |
| | 3/4 | 7,46 | 8,29 | 8,78 | 9,05 |
| | 5/6 | 8,29 | 9,22 | 9,76 | 10,05 |
| | 7/8 | 8,71 | 9,68 | 10,25 | 10,56 |
| 16-QAM | 1/2 | 9,95 | 11,06 | 11,71 | 12,06 |
| | 2/3 | 13,27 | 14,75 | 15,61 | 16,09 |
| | 3/4 | 14,93 | 16,59 | 17,56 | 18,10 |
| | 5/6 | 16,59 | 18,43 | 19,52 | 20,11 |
| | 7/8 | 17,42 | 19,35 | 20,49 | 21,11 |
| 64-QAM | 1/2 | 14,93 | 16,59 | 17,56 | 18,10 |
| | 2/3 | 19,91 | 22,12 | 23,42 | 24,13 |
| | 3/4 | 22,39 | 24,88 | 26,35 | 27,14 |
| | 5/6 | 24,88 | 27,65 | 29,27 | 30,16 |
| | 7/8 | 26,13 | 29,03 | 30,74 | **31,67** |

Plus robuste = moins de débit. `dvbtx check` affiche la capacité de chaque multiplex, et la page web
alerte quand la somme des sources dépasse ce que le modulateur peut envoyer.

## Supervision, alertes, export TSDuck

**Page web** (`[web] listen`, `127.0.0.1:8080` par défaut ; sans authentification, à n'ouvrir que
sur un réseau de confiance) :

- radios : fréquence, échantillonnage, remplissage du tampon, CPU, sous-débits ;
- multiplex : capacité, occupation par chaîne / PSI-SI / libre, courbe sur 10 minutes, analyse TSDuck ;
- chaînes : source active (principale / secours), débit d'entrée et de sortie, **marge de décodage**
  vidéo et audio, retard d'envoi, erreurs de continuité en entrée, émission en cours et suivante ;
- sources : état du processus ffmpeg, relances, dernières lignes du journal, bouton « Relancer » ;
  **traitement** de chaque source (copie, réencodage CPU ou GPU et sur quelle carte), images par
  seconde, vitesse par rapport au temps réel (en rouge sous 1×), images perdues ou dupliquées, CPU
  des processus et occupation du GPU (moteurs vidéo, lus dans `/proc/PID/fdinfo`) ;
- **aperçu des chaînes**, seulement à la demande : vignettes (bouton « Afficher les aperçus », une
  image par chaîne toutes les 4 s, prise sur les seules images clés : quelques % d'un cœur par
  multiplex) et ▶ pour regarder une chaîne en direct dans le navigateur (vidéo H.264 copiée, son en
  AAC ; « réencoder » en 360p si l'image ne s'affiche pas). C'est ce qui part à l'antenne, vidéo de
  secours comprise : le moteur ouvre une copie du multiplex le temps de l'aperçu
  (`monitor add`), rien ne tourne quand personne ne regarde (`web.preview_max_live` directs à la fois) ;
- carte GPU : les cartes trouvées, ce que chacune sait encoder (appris), sa charge ;
- alertes actives et historique.

**Configuration depuis la page** : « Réglages » sur une radio ou un multiplex (fréquence, norme,
constellation, rendement, intervalle de garde, FFT, pilotes, débit symbole... avec le débit utile
calculé en direct), « + Ajouter une radio / un multiplex », et les chaînes (« Modifier », « + Ajouter
une chaîne »). Chaque modification est vérifiée (le fichier entier doit rester valide, avec les
règles du démarrage) et **enregistrée tout de suite** dans le fichier, commentaires gardés ; rien ne
change à l'antenne avant **« Appliquer »** dans le bandeau qui liste les modifications en attente :
on peut donc tout préparer puis appliquer d'un coup. Si seules des chaînes changent, elles sont
appliquées une par une sans couper les autres ; si une radio ou un multiplex change, l'émission
redémarre (1 à 2 s sans signal, les sources qui ne changent pas continuent). « Annuler les
modifications » remet le fichier comme ce qui tourne.

**Profils** : copies du fichier à côté de lui, `dvbtx.toml.<nom>` (`dvbtx.toml.nuit`,
`dvbtx.toml.test`...). « Enregistrer la configuration actuelle comme profil » copie le fichier ;
« Charger » copie le profil sur le fichier (l'ancien gardé en `.bak`), à appliquer ensuite. Seul
`dvbtx.toml` est jamais appliqué.

**Priorités** : quand un multiplex est saturé (sources copiées dont le débit dépasse la capacité),
`priority` (1 à 10, 5 par défaut) décide qui garde la marge : les chaînes prioritaires restent à
l'heure, les autres prennent le retard puis les pertes. Essayé : 6 Mbit/s demandés pour 5 Mbit/s,
la chaîne en priorité 9 garde 800 ms de marge, celle en priorité 2 descend à 200 ms.

**La marge de décodage** : pour chaque image, le temps entre son émission et l'instant où le
récepteur doit la décoder (DTS − PCR à l'émission, minimum sur chaque seconde). Avec la source
habituelle elle est de ~700 ms (`mux_delay`) ; quand le multiplex est trop plein, elle baisse ; sous
zéro, le téléviseur gèle. C'est l'indicateur à surveiller pour savoir si le modulateur suit.

**Alertes** (`[alerts]`) : demande des sources supérieure à la capacité du canal, multiplex plein,
marge sous `margin_min_ms`, paquets perdus car trop en retard, chaîne sur son secours ou sans
source, source qui redémarre en boucle, sous-débit de la radio, erreurs vues par TSDuck, moteur
arrêté. Chacune est journalisée, et peut être envoyée en POST JSON (`webhook`) ou à une commande
(`command`, variables `DVBTX_ALERT_*`).

**Exports** :

```bash
dvbtx run -c ... --analysis-export udp://10.0.0.5:5000      # rapport TSDuck (tsp -P analyze --json) de chaque multiplex
dvbtx run -c ... --stats-export tcp://10.0.0.5:5001         # statistiques du moteur + alertes, chaque seconde
curl http://127.0.0.1:8080/metrics                          # Prometheus
curl http://127.0.0.1:8080/api/status                       # tout, en JSON
```

Chaque multiplex est aussi copié en UDP sur `127.0.0.1:20900+n` (et sur les adresses de
`carrier.monitor`, par exemple un multicast à regarder avec VLC).

## Transcodage sur GPU

Les sources à réencoder (codec non DVB, capture, mire, `encode = true`) passent par un GPU quand il
y en a un qui convient, sinon par libx264 sur le CPU. Rien à régler : `[ingest] gpu = "auto"` (par
défaut), `"off"` pour toujours le CPU, ou la liste des cartes permises (`"/dev/dri/renderD129"`,
`"cuda:0"`) ; par chaîne, `encode = { hwaccel = "cpu" }` (ou `"vaapi"`, `"qsv"`, `"nvenc"` pour
n'utiliser que ce type), et un codec explicite (`video_codec = "h264_vaapi"`) est pris tel quel.

- **Détection** à chaque démarrage, instantanée : cartes Intel, AMD et NVIDIA, intégrées ou non, leur
  pilote ; une carte sans pilote, confiée à une VM (vfio) ou sous nouveau est signalée et écartée.
- **Apprentissage** : chaque façon d'utiliser chaque carte (VAAPI avec chaque pilote VA installé,
  avec ou sans l'encodeur basse consommation ; QSV ; NVENC ; toute la chaîne sur le GPU, décodage
  sur le GPU avec les images copiées en mémoire, ou seulement l'encodage sur le GPU) encode deux
  fois de suite une mire 1080p (H.264 High comme une diffusion) en 720p, en H.264, HEVC et MPEG-2.
  Une façon est retenue si ffmpeg passe le bouclage, que la sortie a le bon codec et la bonne
  taille, et que **le débit reste sous son plafond** (moyenne et seconde la plus chargée : variable
  comme les sources copiées, jamais au-dessus de la part de la chaîne ; un encodeur qui ne fait que
  de la qualité constante n'a pas de contrôle de débit et est refusé). Parmi celles qui vont au
  moins à la moitié de la plus rapide, la moins gourmande en CPU est choisie. Moins d'une minute.
- L'apprentissage ne se refait que si les cartes, leurs pilotes ou ffmpeg changent (empreinte gardée
  avec les résultats dans `/var/lib/dvbtx/gpu.json`), en tâche de fond : le démarrage n'attend pas,
  les transcodages passent par le CPU en attendant. `dvbtx gpu` montre les cartes et les résultats,
  `dvbtx gpu learn` (ou le bouton de la page) refait l'apprentissage.
- **Répartition** : chaque nouveau transcodage va à la carte qui a le plus de vitesse disponible
  (vitesse apprise ÷ transcodages en cours). Un transcodage qui échoue deux fois sur une carte passe
  à une autre façon, puis au CPU.

Mesuré (1080p → 720p H.264, CPU par image) : libx264 veryfast 18,3 ms ; Intel UHD 630 (i5-9500)
7,1 ms à 219 images/s ; NVIDIA RTX A5500 (vGPU) 6,6 ms à 243 images/s. Une chaîne 25 images/s
réencodée, désentrelacée, avec son AAC : 11 % d'un cœur sur l'UHD 630. Toute la chaîne sur le GPU
irait plus vite encore, mais ffmpeg 7.1 casse quand la source boucle ou change (filtres recréés
sur des images restées dans le GPU) : l'apprentissage l'écarte, et la reprendra si une version de
ffmpeg le corrige.

Intel jusqu'à la 9ᵉ génération (UHD 630...) : le pilote VA libre de Debian n'y fait ni la mise à
l'échelle ni le contrôle de débit ; le pilote non libre suffit (`intel-media-va-driver-non-free`,
section non-free, sans redémarrage). Le firmware HuC (`options i915 enable_guc=2`, redémarrage)
n'est utile qu'en plus, pour l'encodeur basse consommation.

## Plusieurs sources par chaîne, cache

`source` peut être une liste, dans l'ordre de préférence, sans limite de nombre :

```toml
  [[carrier.service]]
  name = "France 24"
  source = ["http://fournisseur/u/p/289",     # FHD, la préférée
            "http://fournisseur/u/p/319",     # HD
            "http://fournisseur/u/p/976"]     # Caraïbes
```

Si la source qui joue ne livre plus rien pendant `failover_s` (10 s), ou n'a pas démarré en
`source_start_s` (30 s), la suivante est lancée (après la dernière : la première) ; la chaîne joue
son secours pendant ce temps, puis repart sur la nouvelle source. Une source instable est aussi
quittée : la chaîne est retombée sur son secours `unstable_switches` fois (3) en
`unstable_window_s` (10 min), ou y est restée `failover_s` + `return_ms` (20 s). Une source
quittée est réessayée de plus en plus tard (5 min, puis 10, 20... jusqu'à 1 h), délai oublié
quand elle a tenu 30 min. Tant qu'elle n'est pas sur la
première, les sources préférées sont réessayées dans l'ordre toutes les `preferred_retry_s` (300 s,
0 : jamais), à côté de celle qui joue : la chaîne y revient sans coupure dès que l'une livre de
nouveau. Si aucune ne marche, c'est le secours général (`backup`). Une source en panne est arrêtée
avant que la suivante démarre : utile avec un abonnement IPTV limité à une connexion. L'essai
d'une source préférée pendant qu'une autre joue ouvre une 2ᵉ connexion pendant quelques secondes
(démarrage, 2 s de flux, puis la bascule, après quoi l'autre est fermée) : la plupart des comptes
le tolèrent, comme au zapping ; sinon le fournisseur refuse l'essai (rien ne change) ou coupe
l'autre (quelques secondes de secours), et `preferred_retry_s = 0` supprime ces essais.
En ligne de commande : `dvbtx channel source "France 24" URL1 URL2 URL3` ; sur la page web, une
source par ligne. La page et `dvbtx channel list` montrent la source utilisée (`source 2/3`).

**Cache** : `buffer_ms` est le retard accepté entre l'arrivée d'une source et son émission. Plus
grand, un flux irrégulier passe sans pertes ni bascule sur le secours, au prix d'un retard plus
grand sur le direct. Par défaut `auto` : 6000 ms pour les sources livrées par segments (YouTube,
Twitch, `.m3u8` : au bord du direct elles arrivent par paquets, un par segment de 2 à 5 s), 1000 ms
sinon. Réglable par chaîne (100 à 30000 ms) : `buffer_ms = 8000` dans le fichier,
`dvbtx channel source 3 URL --cache 8000`, `dvbtx channel set 3 buffer_ms=8000`, ou le champ
**Cache** de la page web (vide : auto). Le changer ne coupe pas la chaîne. Une source qui ne livre
plus rien n'est considérée en panne qu'une fois son cache joué.

## Flux de secours

Chaque chaîne a un secours (`backup`, par défaut `/var/lib/dvbtx/backup.ts`, créé par
`dvbtx make-backup --text "..."` ou `--image fichier.png`). Si la source principale se tait plus de
`switch_timeout_ms` (3 s), la chaîne passe sur le secours, sur une image clé ; elle revient quand la
principale est stable depuis `return_ms` (10 s). Les PID, la PMT et la continuité sont conservés.

## Service systemd

```bash
sudo systemctl enable --now dvbtx         # /etc/dvbtx/dvbtx.toml
journalctl -u dvbtx -f
sudo systemctl reload dvbtx               # relit l'EPG et les chaînes du fichier (appliquées à chaud)
sudo systemctl enable --now dvbtx@salon   # autre instance : /etc/dvbtx/salon.toml (ses propres radios et ports)
```

Le service donne la priorité temps réel aux threads radio (`LimitRTPRIO`) et définit `HOME` pour UHD
(qui lit `~/.config/uhd.conf`, par exemple l'image FPGA d'un clone de B210).

## DVB-T2

```toml
[[carrier]]
name = "Mux T2"
radio = "hackrf"
standard = "dvbt2"
frequency = 482_000_000
bandwidth = 8_000_000
fft = "32k"                     # 1k 2k 4k 8k 16k 32k
extended = true                 # porteuses étendues (8k, 16k, 32k)
guard = "1/128"                 # 1/128 1/32 1/16 19/256 1/8 19/128 1/4
pilot = "PP7"                   # PP1 à PP8 (selon FFT et garde)
constellation = "256qam"        # qpsk 16qam 64qam 256qam
rotation = true
code_rate = "2/3"               # 1/2 3/5 2/3 3/4 4/5 5/6   -> 40,02 Mbit/s avec ces valeurs
# frame = "normal" (ou "short"), fec_blocks, data_symbols, ti_blocks = 0 (automatique),
# l1_constellation = "64qam", t2_system_id = 0x8001
```

Le modulateur T2 (porté de gr-dtv, identique à lui) produit le signal à son débit élémentaire ;
un banc de filtres en fréquence (voir plus bas) le ramène exactement à la fréquence de la radio
(écart à un rééchantillonnage idéal : −110 dB à 16,33 MS/s, −136 dB à 18,29 MS/s), où il se mélange
avec les autres porteuses. Coût mesuré sur le Threadripper, 32K 256-QAM 5/6 à 16,33 MS/s :
modulateur 38 % d'un cœur + rééchantillonnage 38 % (deux threads), contre 95 % sur un seul thread
avec l'ancien filtre polyphase.
`dvbtx calc` et la page web calculent son débit utile ; la NIT contient le
`T2_delivery_system_descriptor`.

## DVB-C, DVB-S et DVB-S2

```toml
[[carrier]]
name = "Câble 1"
radio = "hackrf"
standard = "dvbc"
frequency = 306_000_000
symbol_rate = 6_900_000         # 6,9 Msym/s en 256-QAM = 50,87 Mbit/s
constellation = "256qam"        # 16qam, 32qam, 64qam, 128qam, 256qam (seul le 64-QAM est obligatoire
                                # dans les récepteurs ; 256 est universel ; 128 : HackRF, voir les mesures)
# rolloff = 0.15

[[carrier]]
name = "Sat 1"
radio = "b210"
standard = "dvbs"
frequency = 1_200_000_000       # fréquence intermédiaire (bande L) vue par le démodulateur satellite
symbol_rate = 27_500_000
code_rate = "3/4"
# rolloff = 0.35, lnb_lo = 9_750_000_000 (pour la fréquence annoncée dans la NIT), polarization = "h"

[[carrier]]
name = "Sat 2"
radio = "b210"
standard = "dvbs2"
frequency = 1_400_000_000
symbol_rate = 10_000_000
modulation = "8psk"             # qpsk, 8psk, 16apsk, 32apsk
code_rate = "3/4"               # selon la modulation (QPSK 1/4-9/10, 8PSK 3/5-9/10, ...)
# rolloff = 0.35 (0.25, 0.20), pilots = true, frame = "normal" (ou "short"), gold_code = 0
```

Ces porteuses (DVB-C, DVB-S, DVB-S2, et la T2) sont calculées directement à la fréquence de la
radio par des bancs de filtres en fréquence (overlap-save) : à chaque bloc, les symboles de chaque
porteuse passent par une FFT, sont pondérés par la réponse exacte du cosinus surélevé (plate pour
la T2), placés à la fréquence de la porteuse, et une seule IFFT par banc donne les échantillons de
la radio. Le planificateur choisit une fréquence d'échantillonnage dont les débits symbole sont des
fractions simples (`dvbtx check` l'affiche). Mesuré avec un récepteur idéal
(`tests/rx_sc_mer.py`, radio null, échantillons 16 bits, 256-QAM, multiplex réel) :

| | banc en fréquence | ancien filtre polyphase (16 symboles) |
|---|---|---|
| 1 porteuse DVB-C 6,9 Msym/s | MER 88,8 dB | 50,1 dB |
| 2 porteuses DVB-C adjacentes (6,9 et 6,875 Msym/s, 8 MHz d'écart) | 85,6 et 85,7 dB | 21,0 dB (les lobes du filtre tombent sur la voisine) |
| CPU, 1 porteuse DVB-C 256-QAM à 16,6 MS/s | 31 % (banc) + 31 % (codage) | 88 % |

À l'antenne (tuner Sony) : HackRF T2 32K 256-QAM 5/6 et DVB-C 128-QAM sans erreur, B210 DVB-C
256-QAM et T2 sans erreur (sur 8 minutes de T2 B210, 1 paquet erroné sur 8 millions avec le banc,
5 avec l'ancien filtre, aucune sous-alimentation de la radio) ; le 256-QAM sur HackRF reste limité par le bruit analogique de la radio
(même résultat avec les deux filtres : la mise en forme n'y est pour rien). `synth = "fir"` sur une
porteuse garde l'ancien filtre (utilisé aussi quand le débit n'est pas une fraction simple).

Quand la fréquence de la radio est un multiple entier k de celle de la T2 (64/7 MHz en canal de
8 MHz ; le planificateur choisit une telle fréquence dès que la radio porte de la T2, et place
l'oscillateur sur une grille de 62,5 kHz par rapport aux porteuses T2), la T2 n'est plus
rééchantillonnée du tout : chaque symbole OFDM est une IFFT de k × FFT points à la fréquence de la
radio, partagée par toutes les porteuses T2 de même FFT, intervalle de garde et longueur de trame
(P1 compris). Mesuré sur les pilotes continus (`tests/rx_t2_pilots.py`, radio null) :

| | synthèse T2 directe | banc de filtres |
|---|---|---|
| MER des pilotes, 1 porteuse T2 | 148 dB (référence native : 146 dB) | – |
| MER des pilotes, 2 porteuses T2 adjacentes | 145,5 dB chacune | – |
| CPU du moteur, T2 32K 256-QAM 5/6 | 0,42 cœur à 18,29 MS/s | 1,16 cœur à 16,33 MS/s |
| CPU du moteur, 2 T2 + 1 DVB-C 256-QAM | 1,06 cœur à 27,43 MS/s | 2,68 cœurs à 24,18 MS/s |

Comme pour le DVB-T, les lobes OFDM ne sont pas filtrés (−49 dB en bord de bande) : un DVB-C voisin
sur la même radio mesure 59 dB de MER au lieu de 85, bien au-delà de ce que donnent un convertisseur
et son étage analogique. À l'antenne (HackRF vers un tuner Sony CXD2837ER, 32K 256-QAM 5/6) : C/N
30,1 à 30,6 dB, 0 à 1 TEI par 15 s, 50,077 Mbit/s, comme le banc de filtres à 18,67 MS/s (c'est le
bruit analogique du HackRF qui limite) ; sur un i5-9500 le moteur prend 0,31 cœur contre 0,46 avec
le banc.

## Plusieurs porteuses, plusieurs radios

Toutes les porteuses d'une radio partagent une seule IFFT à la fréquence de la radio : chacune est
placée sur ses propres porteuses OFDM, sans rééchantillonneur, sans mélangeur et donc sans image à
filtrer. Il faut qu'elles aient le même mode, le même intervalle de garde et la même largeur (le reste
— modulation, rendement, contenu — est libre), et des fréquences sur la grille de 1116 Hz (toutes les
fréquences de canaux UHF/VHF y sont).

- HackRF One (20 MS/s max) : 2 multiplex adjacents de 8 MHz.
- USRP B210 (56 MHz de bande analogique) : 6 multiplex adjacents de 8 MHz (7 de 7 MHz).
- Au-delà : d'autres radios, chacune avec ses porteuses (`args = "serial=..."`).

## Mesures

Banc : Threadripper 2950X, Debian 12, sortie radio câblée vers une clé RTL2832U (récepteur DVB-T).

| Configuration | Résultat |
|---|---|
| HackRF, 1 multiplex 64-QAM 7/8 | verrouillage, C/N 30,9 dB, BER après Viterbi 0 (GNU Radio : 25 dB, 1e-3 sur le même banc) |
| HackRF, 2 multiplex (482 + 490 MHz) | les deux verrouillés, C/N ~29 dB, BER 0 ; CPU : radio 25 %, 26 % par multiplex (d'un cœur) |
| B210, 6 multiplex (474-514 MHz) | les six verrouillés, C/N 26 à 34 dB, BER 0 ; CPU : radio 38 %, ~25 % par multiplex ; 0 sous-débit sur 70 s |
| TS reçu par radio, analysé par TSDuck | 0 erreur de continuité, PCR conformes, noms, LCN, ONID, NIT, EIT, TOT corrects, vidéo décodée |
| Téléviseur | les 5 chaînes trouvées avec leurs numéros et leurs noms, EPG avec accents (ISO 8859-9), son AAC et E-AC-3 5.1 |
| DVB-T2 32K étendu 256-QAM **5/6** GI 1/128 PP7 (50,08 Mbit/s), tuner Sony CXD2837ER | HackRF : C/N 31,7 dB, 0 erreur ; B210 : C/N 40 dB, 0 erreur |
| DVB-C 128-QAM 6,9 Msym/s (44,51 Mbit/s), même tuner | B210 : C/N 40 dB, BER 0 ; HackRF : C/N 31 dB, BER 2e-5, 0 erreur après Reed-Solomon |
| DVB-C 256-QAM 6,9 Msym/s (50,87 Mbit/s), même tuner | B210 : C/N 41,5 dB, BER 0, 0 erreur ; HackRF : C/N 33 dB, BER ~6e-4, une vingtaine de paquets perdus par 15 s (le HackRF plafonne vers 32-34 dB, assez pour la T2 grâce à son LDPC, pas pour le 256-QAM du câble qui n'a qu'un Reed-Solomon) ; HackRF en 64-QAM : 0 erreur |

## Validation

- Le codage DVB-T en C est **identique, échantillon par échantillon, à celui de GNU Radio gr-dtv**
  (`tests/gr_dvbt_ref.py`, `tests/cmp_dvbt.py`) sur 2k/8k, QPSK/16/64-QAM, tous les rendements et
  intervalles de garde, sur plus d'une super-trame.
- Multiplex : continuité, gigue PCR < 0,5 µs et débit exact vérifiés par TSDuck (`pcrverify`,
  `continuity`, `analyze`).
- Bascule secours / retour, et toutes les sortes de sources réseau / fichier / pipe, essayées.

## État et suite

| | État |
|---|---|
| DVB-T (2k/8k, QPSK à 64-QAM, non hiérarchique) | ✅ validé par radio |
| HackRF, USRP (UHD), SoapySDR | ✅ HackRF et B210 validés ; Soapy compilé, non essayé |
| DVB-C (J.83 A, 16/32/64/128/256-QAM) | ✅ symboles identiques à l'encodeur mini-cmts (carrées) et constellations en croix relevées sur EN 300 429 ; par radio : 256-QAM et 128-QAM sans erreur sur B210, 128-QAM (44,5 Mbit/s) et 64-QAM sans erreur sur HackRF ; le 256-QAM sur HackRF reste limite (voir les mesures) |
| DVB-S (QPSK 1/2 à 7/8) | ✅ démodulé par leandvb (TS, noms, LCN, vidéo corrects) ; essai radio à faire |
| DVB-T2 (PLP unique, 1K à 32K, normal/étendu, PP1-PP8, QPSK à 256-QAM tournée, trames normales/courtes) | ✅ échantillons identiques à gr-dtv sur 23 configurations (voir `docs/DVBT2_PORT.md`) ; par radio en 32K étendu 256-QAM 5/6 GI 1/128 (50,08 Mbit/s) : 0 erreur sur HackRF et sur B210, noms et LCN corrects |
| DVB-S2 (QPSK, 8PSK, 16APSK, 32APSK, trames normales/courtes, pilotes) | ✅ symboles identiques à gr-dtv sur 9 MODCOD ; essai radio à faire |
| Modulation hiérarchique, MPE | non prévu |

## Licence

GPL-3.0-or-later (voir [LICENSE](LICENSE)).
