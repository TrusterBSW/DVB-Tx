<h1 align="center">dvbtx</h1>

<p align="center">
  <b>Vos propres chaînes de télé, sur votre propre câble coaxial.</b><br>
  Flux en direct, caméras et fichiers transformés en vrais multiplex DVB, modulés sur une radio logicielle.
</p>

<p align="center">
  <a href="README.md">English version</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#démarrage-rapide">Démarrage rapide</a> ·
  <a href="#documentation">Documentation</a>
</p>

<p align="center">
  <img alt="licence" src="https://img.shields.io/badge/licence-GPL--3.0--or--later-blue">
  <img alt="systèmes" src="https://img.shields.io/badge/syst%C3%A8mes-Debian%2012%20%7C%2013%20%7C%20Ubuntu%2024.04-informational">
  <img alt="normes" src="https://img.shields.io/badge/DVB-T%20%7C%20T2%20%7C%20C%20%7C%20S%20%7C%20S2-success">
  <img alt="radios" src="https://img.shields.io/badge/radios-HackRF%20%7C%20USRP%20%7C%20SoapySDR-orange">
</p>

---

dvbtx transforme un PC et une radio logicielle en petite tête de réseau télé. Donnez-lui une liste
de chaînes (liens IPTV, directs YouTube ou Twitch, capture HDMI, webcam, dossier de films) : tout
téléviseur branché sur le câble les trouve, avec leurs noms, leurs numéros et le guide des
programmes, comme sur la TNT.

- **Toutes les normes DVB** : DVB-T, DVB-T2, DVB-C, DVB-S et DVB-S2/S2X (jusqu'au 256APSK),
  mélangées sur la même radio.
- **Plusieurs multiplex par radio** : 2 sur un HackRF One, 6 sur un USRP B210, synthétisés
  ensemble ; autant de radios que voulu, chacune liée à sa carte par son numéro de série.
- **Toutes sortes de sources** : HLS, RTMP (OBS), SRT, RTSP, UDP, tvheadend, YouTube, Twitch, listes
  M3U, webcams et cartes de capture HDMI, écran, fichiers, dossiers, tubes.
- **Un multiplexage de qualité broadcast** : un moteur en C cadencé par la radio, débit exact, PCR
  recalculés, PSI/SI complets (NIT avec numéros de chaînes, SDT, EIT depuis XMLTV, TOT).
- **Il tient bon** : un flux de secours quand une source tombe, plusieurs sources par chaîne par
  ordre de préférence, une radio débranchée n'arrête que ses propres multiplex.
- **Une page web** pour tout suivre et tout piloter : débits, marge de décodage, aperçus, alertes,
  chaînes et configuration modifiées en direct, Arrêter/Démarrer par multiplex.
- **Prêt à l'emploi** : paquets Debian, service systemd, profils d'énergie pour tourner sur batterie.

> [!WARNING]
> **Câble coaxial fermé uniquement.** Émettre sur les fréquences de télévision est interdit. dvbtx
> est fait pour un réseau coaxial privé ou un câble direct vers le téléviseur, avec atténuateurs.

## Sommaire

- [Installation](#installation)
- [Démarrage rapide](#démarrage-rapide)
- [Fonctionnement](#fonctionnement)
- [Configuration](#configuration)
- [Sources](#sources)
- [Guide des programmes (EPG)](#guide-des-programmes-epg)
- [Changer les chaînes pendant l'émission](#changer-les-chaînes-pendant-lémission)
- [Page web](#page-web)
- [Service et profils d'énergie](#service-et-profils-dénergie)
- [Normes et radios](#normes-et-radios)
- [Mesures](#mesures)
- [Documentation](#documentation)
- [Compiler depuis les sources](#compiler-depuis-les-sources)
- [Licence](#licence)

## Installation

Un seul paquet pour Debian 12 et 13, Proxmox VE 8 et 9, et Ubuntu 24.04 ou plus récent (amd64). En
une ligne :

```bash
curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Tx/main/install.sh | sudo sh
# --extras : aussi streamlink et yt-dlp (HLS sans blocage, sources YouTube et Twitch)
curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Tx/main/install.sh | sudo sh -s -- --extras
```

Ou à la main :

```bash
wget https://github.com/TrusterBSW/DVB-Tx/raw/main/releases/dvbtx_0.2.0_amd64.deb
sudo apt install ./dvbtx_0.2.0_amd64.deb
sudo apt install streamlink yt-dlp      # facultatif : HLS sans blocage, sources YouTube et Twitch
```

TSDuck (facultatif, analyse des multiplex) n'est pas dans les dépôts Debian et Ubuntu : ses paquets
sont sur sa [page de versions](https://github.com/tsduck/tsduck/releases).

La bibliothèque UHD change de version d'une distribution à l'autre (4.3, 4.6, 4.8). Le paquet
contient donc le module USRP compilé pour chacune, et le moteur charge celui dont la bibliothèque est
installée. Chaque bibliothèque radio n'est que *recommandée* : s'il en manque une, seule cette radio
est indisponible.

`packaging/build-all-debs.sh` construit le paquet dans des conteneurs, puis l'installe et le vérifie
dans un conteneur neuf de chaque version. GitHub Actions fait de même à chaque étiquette (tag) et le
publie sur la page [Releases](https://github.com/TrusterBSW/DVB-Tx/releases).

## Démarrage rapide

```bash
dvbtx devices                                    # 1. les radios branchées, avec leurs numéros de série
sudo cp /usr/share/dvbtx/configs/hackrf-1mux.toml /etc/dvbtx/dvbtx.toml
sudoedit /etc/dvbtx/dvbtx.toml                   # 2. vos chaînes
dvbtx check                                      # 3. vérifier, voir le plan de fréquences
sudo systemctl enable --now dvbtx                # 4. à l'antenne, maintenant et à chaque démarrage
xdg-open http://127.0.0.1:8080/                  # 5. la page web
```

Lancez ensuite une recherche de chaînes sur le téléviseur. Pour essayer sans fichier :

```bash
sudo dvbtx --driver hackrf --channel 22 \
     --service "Mire=lavfi:testsrc2=size=1280x720:rate=25" \
     --service "Film=/srv/videos/film.mkv,lcn=2"
```

`dvbtx -h` liste les commandes, `dvbtx help COMMANDE` en détaille une, `man dvbtx` dit tout.

## Fonctionnement

```
 sources                  ingest (un processus chacune)           moteur C (dvbtx-engine)
 ───────                  ─────────────────────────────           ─────────────────────────────────────
 HLS / HTTP / M3U ──┐                                            ┌ réception UDP, démux PAT/PMT
 YouTube, Twitch ───┤     ffmpeg / streamlink / yt-dlp           │ tampon anti-gigue, heures de départ
 RTMP / RTSP / SRT ─┼───► copie, ou encodage (CPU ou GPU) ─UDP─► ├ un multiplexeur par porteuse, tiré
 tvheadend (HTSP) ──┤     relancé en cas de panne                │ par le modulateur : SI > paquets dus
 webcam / HDMI ─────┤                                            │ > PCR > bourrage ; bascule secours
 fichiers, dossiers ┤     udp+direct:// lu par le moteur ──────► ├ codage DVB-T/T2/C/S/S2
 tubes, stdin ──────┘                                            ├ une synthèse par radio pour toutes
                                                                 │ ses porteuses, à sa fréquence
 superviseur Python (dvbtx) : configuration, plan de fréquences, └ modules HackRF / UHD / SoapySDR
 tables SI et EPG, alertes, chien de garde, page web, TSDuck ◄── copie de chaque multiplex
```

- **Chaque source est un processus à part** : une source qui plante, se fige ou prend du retard ne
  gêne jamais les autres ; sa chaîne passe sur le flux de secours et revient toute seule.
- **C'est la radio qui donne le rythme** : le modulateur demande chaque paquet au moment où il en a
  besoin. Le débit est donc exact par construction, les trous sont comblés par du bourrage, les PCR
  sont datés à l'heure réelle d'émission (gigue < 0,5 µs).
- **Le travail lourd est en C** (multiplexage, codage, FFTW) ; le superviseur Python s'occupe de la
  configuration, des tables SI, de la supervision et de la page web.

Détails : `man 7 dvbtx`.

## Configuration

Un fichier TOML, `/etc/dvbtx/dvbtx.toml` : les radios, puis les multiplex (porteuses) de chaque
radio, puis les chaînes (services) de chaque multiplex.

```toml
[network]
name = "Ma TV"

[[radio]]
name = "hackrf"
driver = "hackrf"              # hackrf, uhd, soapy, null
serial = "457863c82b6a6a5f"    # facultatif : cette carte (dvbtx devices --save hackrf)
gain = 30

[[carrier]]
name = "Mux 1"
radio = "hackrf"
channel = 22                   # ou frequency = 482_000_000
standard = "dvbt"              # dvbt, dvbt2, dvbc, dvbs, dvbs2
  [[carrier.service]]
  name = "Infos"
  source = ["https://example.org/live.m3u8", "ytdlp:https://www.youtube.com/@chaine/live"]
  [[carrier.service]]
  name = "Films"
  source = "dir:/srv/films"
```

En DVB-S/S2, `frequency` est la fréquence satellite à entrer sur la TV (`10_950_000_000`) : la FI en
bande L émise sur le câble est calculée pour son LNB (universel par défaut).

Tout ce qui n'est pas donné est calculé : fréquence et cadence de la radio, service_id, numéros de
chaînes, PID, type de service. `-s radio.0.gain=40` remplace n'importe quelle clé le temps d'un
lancement. Chaque clé est décrite dans `man 5 dvbtx.toml` et, commentée, dans
[`packaging/dvbtx.toml`](packaging/dvbtx.toml) ; d'autres exemples dans [`configs/`](configs/).

## Sources

| Source | Exemple |
|---|---|
| fichier (en boucle), dossier | `/srv/film.mkv`, `dir:/srv/films` |
| HTTP, HLS | `https://hote/live.m3u8`, `…#quality=720p`, `…#variant=2` |
| entrée d'une liste M3U | `m3u:/srv/liste.m3u#Nom de la chaîne` |
| YouTube, Twitch, tout site connu de streamlink | `ytdlp:https://youtube.com/…`, `twitch://nom`, `streamlink:URL` |
| tvheadend | `htsp://user:pass@hote:9982/Chaine` |
| RTMP (OBS), RTSP, SRT, UDP, RTP | `rtmp://0.0.0.0:1935/live/cle?listen`, `srt://…`, `udp://239.0.0.1:2000` |
| UDP directement au moteur | `udp+direct://239.0.0.1:2000` |
| webcam, capture HDMI, écran | `v4l2:/dev/video0?size=1280x720&fps=30`, `screen::0.0` |
| mire | `lavfi:testsrc2` |
| tube, entrée standard, commande | `pipe:/chemin/fifo`, `stdin:`, `exec:commande` |

Les flux dont les codecs passent en MPEG-TS sont copiés, les captures sont encodées (sur un GPU quand
il y en a un qui marche : Intel, AMD, NVIDIA sont détectés et essayés une fois). Une liste de sources
est jouée par ordre de préférence : la suivante quand celle qui joue tombe, retour à une préférée
quand elle remarche, le flux de secours quand aucune ne marche. Le HLS passe par streamlink quand il
est installé : il encaisse les discontinuités (pubs, épisode suivant) qui bloquent ffmpeg. Tout :
`man 7 dvbtx-sources`.

## Guide des programmes (EPG)

Le téléviseur affiche le programme en cours et le suivant, et celui des jours à venir (EIT
présent/suivant et programme), à partir de guides XMLTV :

```toml
[epg]
dir = "/var/lib/dvbtx/epg"                  # tout *.xml / *.xml.gz déposé ici
urls = ["http://tunarr:8000/api/xmltv.xml"] # XMLTV téléchargé toutes les heures (url_refresh_s)
days = 7

[[carrier]]
  [[carrier.service]]
  name = "France 24"
  epg_id = "France24.fr"                    # l'identifiant de la chaîne dans le XMLTV ; sans lui, par le nom
```

Le paquet règle `dir` sur `/var/lib/dvbtx/epg` : un guide copié là (ou écrit par un autre service,
un grabber ou Tunarr) est pris en compte dans les 10 minutes, ou tout de suite avec `sudo systemctl
reload dvbtx`, sans redémarrage. Les fichiers
EIT XML de TSDuck marchent aussi (associés par service_id).
`dvbtx epg` montre, pour chaque chaîne, combien d'événements elle a reçus et ce qui passe maintenant
et ensuite. Toutes les clés : `man 5 dvbtx.toml`, section `[epg]`.

## Changer les chaînes pendant l'émission

```bash
dvbtx channel list
dvbtx channel add "France 24" https://example.org/live.m3u8 --number 3
dvbtx channel source 3 "ytdlp:https://www.youtube.com/@France24_fr/live"
dvbtx channel rename 3 "France 24 FR"
dvbtx channel renumber "France 24 FR" 1 --swap
dvbtx channel remove 0x0103
```

Seule cette chaîne est touchée (les autres ne perdent aucun paquet), et le fichier est mis à jour
(commentaires gardés, numéros figés pour que rien ne se décale). Un changement de source se fait
sans coupure : la nouvelle prend le relais sur une image clé. `man dvbtx-channel`.

## Page web

`http://127.0.0.1:8080/` par défaut (`web.listen`) :

- **tableau de bord** : radios (état, tampon, sous-débits), multiplex (remplissage par chaîne,
  graphe sur 10 minutes, **Arrêter / Démarrer**), alertes ;
- **chaînes** : ce que joue chacune, son débit, sa **marge de décodage** (l'avance avec laquelle les
  images arrivent au téléviseur : ~700 ms normalement, sous zéro l'image se fige), le programme en
  cours et le suivant, des vignettes, une vue en direct dans le navigateur, le multiplex tel
  qu'émis dans VLC (lien `.ts`) ;
- **sources** : le processus de chaque source (copie ou transcodage, CPU ou quel GPU, vitesse,
  redémarrages, journal, bouton de relance) ;
- **configuration** : radios, multiplex et chaînes modifiés avec le débit utile calculé pendant la
  saisie, une radio liée en un clic à une carte libre de son pilote ; les changements sont
  enregistrés tout de suite et passent à l'antenne avec **Appliquer** (les chaînes une par une, sans
  toucher aux autres ; une configuration refusée par le moteur laisse la précédente à l'antenne) ;
  des profils pour passer d'un montage à l'autre ;
- **journal** ; une API JSON (`/api/status`, `/api/channels`...), `/metrics` pour Prometheus,
  l'analyse TSDuck exportée en JSON.

`man 7 dvbtx-web`.

## Service et profils d'énergie

```bash
sudo systemctl enable --now dvbtx         # /etc/dvbtx/dvbtx.toml, à chaque démarrage
sudo systemctl reload dvbtx               # EPG et chaînes du fichier, appliqués à chaud
sudo systemctl enable --now dvbtx@salon   # un autre émetteur : /etc/dvbtx/salon.toml
journalctl -u dvbtx -f
```

**Sur batterie**, `dvbtx-power` garde toute la machine dans son état le plus économe tant que rien
n'émet, et passe à un profil d'émission pendant que dvbtx tourne (à activer) :

```bash
sudo systemctl enable --now dvbtx-power
sudo dvbtx power status
```

Mesuré sur un Intel i3-8100T avec quatre multiplex sur deux radios : au repos 1,37 W → 0,62 W, à
l'antenne 22,1 W → 13,9 W (processeur), sans sous-débit. `man 8 dvbtx-power`.

Depuis les sources (sans paquet) : `sudo packaging/install-service.sh -c FICHIER [--power]`.

## Normes et radios

| | État |
|---|---|
| DVB-T (2k/8k, QPSK à 64-QAM) | ✅ à l'antenne ; identique échantillon par échantillon à GNU Radio gr-dtv |
| DVB-T2 (PLP unique, 1K à 32K, étendu, PP1 à PP8, jusqu'à 256-QAM tournée) | ✅ à l'antenne (32K 256-QAM 5/6, 50 Mbit/s, sans erreur) ; identique à gr-dtv sur 23 configurations |
| DVB-C (16 à 256-QAM) | ✅ à l'antenne (256-QAM sans erreur sur B210, 128-QAM sur HackRF) |
| DVB-S (QPSK 1/2 à 7/8) | ✅ démodulé par leandvb |
| DVB-S2 (QPSK, 8PSK, 16APSK, 32APSK) | ✅ symboles identiques à gr-dtv, 8PSK 9/10 décodé sans erreur par gr-dvbs2rx |
| DVB-S2X (8APSK à 256APSK, roll-off jusqu'à 0,05) | ✅ chaque MODCOD identique à gr-dtv au symbole près ; reçu sans erreur jusqu'au 256APSK à 52 Msym/s |
| HackRF One | ✅ jusqu'à 20 MS/s : 2 multiplex adjacents de 8 MHz |
| USRP B200/B210 (UHD) | ✅ 56 MHz : 6 multiplex adjacents de 8 MHz, ou une porteuse DVB-S2X jusqu'à 52 Msym/s |
| émetteurs SoapySDR | compilé, pas essayé |

## Mesures

Sortie radio câblée vers un récepteur DVB-T/T2/C :

| Montage | Résultat |
|---|---|
| HackRF, 1 multiplex DVB-T 64-QAM 7/8 | C/N 30,9 dB, BER 0 |
| B210, 6 multiplex DVB-T (474 à 514 MHz) | tous verrouillés, C/N 26 à 34 dB, BER 0 ; radio 38 % d'un cœur, ~25 % par multiplex |
| DVB-T2 32K 256-QAM 5/6 (50,08 Mbit/s) | HackRF C/N 31,7 dB, B210 40 dB, aucune erreur |
| DVB-C 256-QAM 6,9 Msym/s (50,87 Mbit/s) | B210 C/N 41,5 dB, aucune erreur |
| DVB-S2X 256APSK 135/180, 52 Msym/s, roll-off 0,05 (300 Mbit/s) | B210 vers B210 (récepteur dvbrx) : SNR 23,8 dB, aucune erreur ; 2,2 cœurs d'un i3-8100T |
| DVB-S2 QPSK 3/4 à 46, 50, 52 Msym/s | B210 vers B210 : SNR 16,6 à 17,8 dB, aucune erreur une fois accroché |
| flux reçu passé dans TSDuck | aucune erreur de continuité, PCR conformes, NIT, SDT, EIT, TOT corrects |
| téléviseur | chaînes trouvées avec noms, numéros, EPG, son AAC et E-AC-3 5.1 |

La synthèse, les bancs de filtres et le portage DVB-T2 sont décrits dans `man 7 dvbtx` et
[`docs/DVBT2_PORT.md`](docs/DVBT2_PORT.md) ; les scripts de validation dans [`tests/`](tests/README.md).

## Documentation

Les pages de manuel existent en français et en anglais : `man` prend celle de votre langue
(`man -L fr dvbtx`, `man -L en dvbtx` pour forcer l'une ou l'autre).

| Page | Contenu |
|---|---|
| `dvbtx -h`, `dvbtx help COMMANDE` | toutes les commandes et options |
| [`dvbtx(1)`](man/fr/dvbtx.1) | la ligne de commande |
| [`dvbtx-channel(1)`](man/fr/dvbtx-channel.1) | changer les chaînes pendant l'émission |
| [`dvbtx.toml(5)`](man/fr/dvbtx.toml.5) | le fichier de configuration, clé par clé |
| [`dvbtx(7)`](man/fr/dvbtx.7) | le fonctionnement : composants, circulation des données, horloge, pannes |
| [`dvbtx-sources(7)`](man/fr/dvbtx-sources.7) | toutes les sortes de sources, bascule, cache, flux de secours |
| [`dvbtx-web(7)`](man/fr/dvbtx-web.7) | la page web et son API HTTP |
| [`dvbtx-engine(8)`](man/fr/dvbtx-engine.8) | le moteur C, son fichier INI, ses commandes et statistiques |
| [`dvbtx-power(8)`](man/fr/dvbtx-power.8) | les profils d'énergie pour la batterie |

Installées avec le paquet (`man dvbtx.toml`) ; depuis les sources, `man -l man/fr/dvbtx.toml.5`
(en anglais : `man/dvbtx.toml.5`).

## Compiler depuis les sources

```bash
sudo apt install build-essential pkg-config libfftw3-dev ffmpeg python3 \
                 libhackrf-dev libuhd-dev libsoapysdr-dev       # chaque radio est facultative
make -C engine
python3 -m dvbtx check -c configs/hackrf-1mux.toml
sudo packaging/install-service.sh -c /chemin/vers/dvbtx.toml   # en service, depuis ce dossier
./packaging/build-deb.sh                                         # .deb pour ce système
./packaging/build-all-debs.sh                                    # le .deb de toutes les versions (docker)
```

Python ≥ 3.11, bibliothèque standard seulement. TSDuck est facultatif (analyse).

## Première version

La première version de DVB-Tx (scripts bash autour de ffmpeg, TSDuck et d'un graphe GNU Radio) est
conservée dans [`old/`](old/), avec son historique.

## Licence

GPL-3.0-or-later. © 2024–2026 BILLY Rémi.
