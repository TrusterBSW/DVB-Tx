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
9. [Supervision, alertes, export TSDuck](#supervision-alertes-export-tsduck)
10. [Flux de secours](#flux-de-secours)
11. [Service systemd](#service-systemd)
12. [Plusieurs porteuses, plusieurs radios](#plusieurs-porteuses-plusieurs-radios)
13. [Mesures](#mesures)
14. [Validation](#validation)
15. [État et suite](#état-et-suite)

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
- alertes actives et historique.

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

## Flux de secours

Chaque chaîne a un secours (`backup`, par défaut `/var/lib/dvbtx/backup.ts`, créé par
`dvbtx make-backup --text "..."` ou `--image fichier.png`). Si la source principale se tait plus de
`switch_timeout_ms` (3 s), la chaîne passe sur le secours, sur une image clé ; elle revient quand la
principale est stable depuis `return_ms` (10 s). Les PID, la PMT et la continuité sont conservés.

## Service systemd

```bash
sudo systemctl enable --now dvbtx         # /etc/dvbtx/dvbtx.toml
journalctl -u dvbtx -f
sudo systemctl reload dvbtx               # relit l'EPG et reconstruit les tables SI
sudo systemctl enable --now dvbtx@salon   # autre instance : /etc/dvbtx/salon.toml (ses propres radios et ports)
```

Le service donne la priorité temps réel aux threads radio (`LimitRTPRIO`) et définit `HOME` pour UHD
(qui lit `~/.config/uhd.conf`, par exemple l'image FPGA d'un clone de B210).

## DVB-C et DVB-S

```toml
[[carrier]]
name = "Câble 1"
radio = "hackrf"
standard = "dvbc"
frequency = 306_000_000
symbol_rate = 6_900_000         # 6,9 Msym/s en 256-QAM = 50,87 Mbit/s
constellation = "256qam"        # 16qam, 64qam, 256qam
# rolloff = 0.15

[[carrier]]
name = "Sat 1"
radio = "b210"
standard = "dvbs"
frequency = 1_200_000_000       # fréquence intermédiaire (bande L) vue par le démodulateur satellite
symbol_rate = 27_500_000
code_rate = "3/4"
# rolloff = 0.35, lnb_lo = 9_750_000_000 (pour la fréquence annoncée dans la NIT), polarization = "h"
```

Ces porteuses passent par un filtre polyphase (mise en forme RRC + rééchantillonnage vers la
fréquence de la radio en une passe) puis un oscillateur numérique : elles se placent à n'importe
quelle fréquence et se mélangent librement avec des porteuses DVB-T sur la même radio.

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
| DVB-C (J.83 A, 16/64/256-QAM) | ✅ symboles identiques à l'encodeur mini-cmts (validé sur modem câble) ; démodulé en logiciel (MER 37 dB, 0 erreur RS) ; essai radio à faire avec un tuner DVB-C |
| DVB-S (QPSK 1/2 à 7/8) | ✅ démodulé par leandvb (TS, noms, LCN, vidéo corrects) ; essai radio à faire |
| DVB-T2 | en cours (portage de gr-dtv, validation contre gr-dtv) |
| DVB-S2 | prévu (même FEC que DVB-T2) |
| Modulation hiérarchique, MPE | non prévu |

## Licence

GPL-3.0-or-later (voir [LICENSE](LICENSE)).
