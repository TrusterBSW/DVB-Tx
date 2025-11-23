#  🇬🇧 ENGLISH BELOW 🇺🇸

# **DVB-Tx — Création d’un émetteur DVB-T complet fait maison avec un HackRF**

DVB-Tx est un projet permettant de générer un **multiplex DVB-T complet**, prêt à être injecté dans un réseau coaxial local.  
Il combine automatiquement :

1. **FFmpeg** — Multiplexage des sources vidéo/audio  
2. **TSDuck** — Génération et insertion des tables SI/PSI  
3. **GNU Radio** — Modulation DVB-T vers Raw I/Q
4. **HackRF One** — Transmission des Raw I/Q en RF

L’objectif : permettre à chacun de diffuser ses propres chaînes de télévision en local, facilement et rapidement avec un HackRF.

---

## **📦 Table des matières**

1. [Architecture du système](#architecture-du-système)  
2. [Schéma de fonctionnement](#schéma-de-fonctionnement)  
3. [Dépendances](#dépendances)  
4. [Installation](#installation)  
5. [Démarrage](#démarrage)  
6. [Options du script start.sh](#options-du-script-startsh)  
7. [Débit et paramètres DVB-T](#débit-et-paramètres-dvb-t)  
8. [Tableau complet des débits DVB-T](#tableau-complet-des-débits-dvb-t)  
9. [Contenu du dépôt](#contenu-du-dépôt)  
10. [Personnalisation du multiplexe](#personalisation-du-multiplexe)  
11. [Limitations connues](#limitations-connues)  
12. [Avertissement légal](#avertissement-légal)  
13. [Perspectives d’évolution](#perspectives-dévolution)  
14. [Contributeurs](#contributeurs)  
15. [Licence](#licence)

---

## **Architecture du système**

Le multiplex DVB-T est construit en trois étapes :

- **FFmpeg** : lit les sources (fichiers ou flux live) et génère un flux MPEG-TS  
- **TSDuck** : injecte les tables DVB (PAT, PMT, SDT, NIT, EIT…), vérifie et analyse le flux  
- **GNU Radio** : module le flux MPEG-TS en un signal RF DVB-T

### **Interconnexions :**

- FFmpeg → **pipe direct** → TSDuck  
- TSDuck → **named pipe `mpeg-live.ts`** → GNU Radio

---

## **Schéma de fonctionnement**

```
          ┌───────────────┐       ┌──────────────┐       ┌────────────────────┐
Sources → │    FFmpeg     │  ─→─  │    TSDuck    │  ─→─  │     GNU Radio      │ → Signal RF
(video)   │ (Mux TS)      │ pipe  │ (SI/PSI +    │ named │ (Modulation DVB-T) │
          └───────────────┘       │  analyses)   │ pipe  └────────────────────┘
                                  └──────────────┘
```

---

## **Dépendances**

### Obligatoires :
- **FFmpeg**  
- **TSDuck**  
- **GNU Radio 3.10**  
- **HackRF Tools** (si utilisation RF)

### Recommandé :
- **screen**

---

## **Installation**

### Installation FFmpeg, GNU Radio et screen (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install ffmpeg gnuradio screen
```

### Installation de TSDuck

```bash
curl -fsSL https://tsduck.io/install.sh | sudo bash
```
---

## **Démarrage**

### Récupération du projet

```bash
git clone https://github.com/TrusterBSW/DVB-Tx
cd DVB-Tx/DVB
```

### Diffuser les fichiers Video inclus
```./start.sh file```

### Diffuser des flux en direct
```./start.sh live```

**Options du script start.sh**

| Option | Description |
|--------|-------------|
| `-f` | Attacher le screen FFmpeg (debug des entrées) |
| `-a` | Attacher l’analyse TSDuck (debug du multiplexe) |
| `-g` | Attacher le screen GNU Radio (debug modulation) |

### Exemple d’utilisation :

```bash
./start.sh file -a
```
---

## **Débit et paramètres DVB-T**

Le projet utilise par défaut :

📌 **64-QAM — Code rate 7/8 — Guard interval 1/32 — Mode 8K**  
➡️ **Débit utile maximal : 31,67 Mbit/s**

Ce débit doit inclure :

- les vidéos  
- l’audio  
- les tables SI/PSI (PAT/PMT/SDT/NIT/EIT/...)

⚠️ Les vidéos étant souvent en VBR, leur débit varie dans le temps.  
Un système de limitation/dropping est inclus dans `live-stream.sh` pour éviter les dépassements.

---

## **Tableau complet des débits DVB-T**

Débits utiles en **Mbit/s** selon modulation, code rate et guard interval (valeurs officielles DVB-T — Mode 8K).

| **Modulation** | **Code rate** | **1/4** | **1/8** | **1/16** | **1/32** |
|----------------|---------------|---------|---------|----------|----------|
| **QPSK** | 1/2 | 4,98 | 5,53 | 5,85 | 6,03 |
| | 2/3 | 6,64 | 7,37 | 7,81 | 8,04 |
| | 3/4 | 7,46 | 8,29 | 8,78 | 9,05 |
| | 5/6 | 8,29 | 9,22 | 9,76 | 10,05 |
| | 7/8 | 8,71 | 9,68 | 10,25 | 10,56 |
| **16-QAM** | 1/2 | 9,95 | 11,06 | 11,71 | 12,06 |
| | 2/3 | 13,27 | 14,75 | 15,61 | 16,09 |
| | 3/4 | 14,93 | 16,59 | 17,56 | 18,10 |
| | 5/6 | 16,59 | 18,43 | 19,52 | 20,11 |
| | 7/8 | 17,42 | 19,35 | 20,49 | 21,11 |
| **64-QAM** | 1/2 | 14,93 | 16,59 | 17,56 | 18,10 |
| | 2/3 | 19,91 | 22,12 | 23,42 | 24,13 |
| | 3/4 | 22,39 | 24,88 | 26,35 | 27,14 |
| | 5/6 | 24,88 | 27,65 | 29,27 | 30,16 |
| | 7/8 | 26,13 | 29,03 | 30,74 | **31,67** |

➡️ Vous pouvez choisir le compromis souhaité :  
- **Robustesse ↑ → Débit ↓**  
- **Débit ↑ → Robustesse ↓**

---

## **Contenu du dépôt**

| Dossier | Description |
|--------|-------------|
| `/Video` | Vidéos de démonstration |
| `/EIT` | Exemples d’EIT pour chaque chaîne |
| `/DVB` | Scripts FFmpeg, TSDuck et GNU Radio |
| `mpeg-live-analyzed.txt` | Analyse TSDuck mise à jour en direct |

---

## **Personnalisation du multiplexe**

Vous pouvez librement :

### ✔ Choisir les sources que vous voulez  
- Fichiers  
- Flux live  
- Caméras IP  
- Caméras USB  
- Carte de capture HDMI  
- etc.

### ✔ Ajouter autant de chaînes que vous le souhaitez  
Pour cela, il faudra :

1. **Modifier le script correspondant :**  
   - `file-source.sh` (mode fichier)  
   - `live-stream.sh` (mode live)  
2. **Mettre à jour les paramètres TSDuck** pour prendre en compte les nouveaux flux  
3. **Mettre à jour la NIT** pour que le téléviseur détecte toutes les chaînes  
4. **Respecter le débit maximal** (31,67 Mbit/s par défaut)

---

## **Limitations connues**

### ❗ Les numéros de chaînes (LCN) ne sont pas reconnus par les téléviseurs

Lors des essais effectué durant le developpement du projet, les numéros de chaine ne sont pas reconnu par mon poste TV (une LG CX).
Cependant, ceux-ci sont bien présent d'apres l'analyse TSDuck, et sont bien vue par TvHeadEnd.  
Toute contribution ou retour d’expérience est bienvenu pour améliorer ce point.

---

## **Avertissement légal**

⚠️ **L’émission RF dans les fréquences DVB-T est strictement réglementée.**

Ce projet est **légal** **uniquement** si :

✔ la diffusion reste **dans un réseau coaxial fermé**,  
✔ ou directement raccordée au téléviseur par câble.

❌ Il est interdit de rayonner dans les airs.

---

## **Perspectives d’évolution**

- Gestion de **deux multiplexes simultanés** sur un seul HackRF (~63 Mbit/s)  
- Génération automatique des **EIT** depuis **XMLTV**  
- Intégration avec **DizqueTV** (playlists et planning avancé)  
- Vidéo/image de secours si une source live tombe  
- Export TSDuck vers **InfluxDB + Grafana**

---






#  🇬🇧 ENGLISH HERE 🇺🇸

# **DVB-Tx — Creation of a complete homemade DVB-T transmitter with a HackRF**


DVB-Tx is a project allowing to generate a **multiplex DVB-T complet**, ready to be injected in a local coaxial network.
It automatically combines :

1. **FFmpeg** — Multiplexing of video/audio sources 
2. **TSDuck** — Generation and insertion of SI/PSI tables 
3. **GNU Radio** — DVB-T modulation to Raw I/Q
4. **HackRF One** — Transmission of Raw I/Q en RF

The goal : To allow everyone to broadcast their own TV Channel locally, easily and quickly with an HackRF. 

---

## *📦 Table of Contents**

1. [System architecture](#system-architecture)  
2. [Operating diagram](#operating-diagram)  
3. [Dependencies](#dependencies)  
4. [Installation](#installation)  
5. [Startup](#startup)  
6. [Script options start.sh](#options-du-script-startsh)  
7. [DVB-T Rate and Settings](#rate-and-parameters-dvb-t)  
8. [Complete table of DVB-T speeds](#full-table-of-speeds-dvb-t)  
9. [Repository content](#repository-content)  
10. [Multiplex customization](#personalization-of-multiplex)  
11. [Known limitations](#known-limitations)  
12. [Legal warning](#legal-warning)  
13. [Evolution prospects](#perspectives-devolution)  
14. [Contributors](#contributors)  
15. [Licence](#licence)
---

## **System architecture**

The DVB-T multiplex is built in three stages :

- **FFmpeg** : reads the sources (files ou live feed) and generates a MPEG-TS flux ;
- **TSDuck** : injects the DVB tables (PAT, PMT, SDT, NIT, EIT…), checks and analyzes the flux ;
- **GNU Radio** : modules the MPEG-TS flux in a RF DVB-T signal ;

### **Interconnexions :**

- FFmpeg → **pipe direct** → TSDuck  
- TSDuck → **named pipe `mpeg-live.ts`** → GNU Radio

---

## **Operating diagram**

```
          ┌───────────────┐       ┌──────────────┐       ┌────────────────────┐
Sources → │    FFmpeg     │  ─→─  │    TSDuck    │  ─→─  │     GNU Radio      │ → RF Signal 
(video)   │ (Mux TS)      │ pipe  │ (SI/PSI +    │ named │ (Modulation DVB-T) │
          └───────────────┘       │  analyses)   │ pipe  └────────────────────┘
                                  └──────────────┘
```

---

## **Dependances**

### Mandatory:
- **FFmpeg**  
- **TSDuck**  
- **GNU Radio 3.10**  
- **HackRF Tools** (if using RF)

### Recommanded :
- **screen**

---

## **Installation**

### Installation FFmpeg, GNU Radio et screen (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install ffmpeg gnuradio screen
```

### Installation of TSDuck

```bash
curl -fsSL https://tsduck.io/install.sh | sudo bash
```
---

## **Startup**

### Project recuperation

```bash
git clone https://github.com/TrusterBSW/DVB-Tx
cd DVB-Tx/DVB
```

### Broadcast the inlcuded video files
```./start.sh file```

### Broadcast live streams
```./start.sh live```

**Script options start.sh**

| Option | Description |
|--------|-------------|
| `-f` | Attach the FFmpeg screen (debug des entries) |
| `-a` | Attach the TSDuck analysis (debug of the multiplex) |
| `-g` | Attacher the GNU Radio screen (debug modulation) |

### Exemple of use :

```bash
./start.sh file -a
```
---

## **DVB-T Rate and Settings**

The project uses by default :

📌 **64-QAM — Code rate 7/8 — Guard interval 1/32 — Mode 8K**  
➡️ **Maximum useful flow : 31,67 Mbit/s**

This flow must include :

- The videos
- The audio  
- The SI/PSI tables (PAT/PMT/SDT/NIT/EIT/...)

⚠️ Since videos are often in VBR, so their bit rate can vary in time.  
A limitation/dropping system is included in `live-stream.sh` to avoid overflow.

---

## **Complete table of DVB-T speeds**

Useful rates in **Mbit/s** according to modulation, code rate and guard interval (offical DVB-T values — 8K Mode).

| **Modulation** | **Code rate** | **1/4** | **1/8** | **1/16** | **1/32** |
|----------------|---------------|---------|---------|----------|----------|
| **QPSK** | 1/2 | 4,98 | 5,53 | 5,85 | 6,03 |
| | 2/3 | 6,64 | 7,37 | 7,81 | 8,04 |
| | 3/4 | 7,46 | 8,29 | 8,78 | 9,05 |
| | 5/6 | 8,29 | 9,22 | 9,76 | 10,05 |
| | 7/8 | 8,71 | 9,68 | 10,25 | 10,56 |
| **16-QAM** | 1/2 | 9,95 | 11,06 | 11,71 | 12,06 |
| | 2/3 | 13,27 | 14,75 | 15,61 | 16,09 |
| | 3/4 | 14,93 | 16,59 | 17,56 | 18,10 |
| | 5/6 | 16,59 | 18,43 | 19,52 | 20,11 |
| | 7/8 | 17,42 | 19,35 | 20,49 | 21,11 |
| **64-QAM** | 1/2 | 14,93 | 16,59 | 17,56 | 18,10 |
| | 2/3 | 19,91 | 22,12 | 23,42 | 24,13 |
| | 3/4 | 22,39 | 24,88 | 26,35 | 27,14 |
| | 5/6 | 24,88 | 27,65 | 29,27 | 30,16 |
| | 7/8 | 26,13 | 29,03 | 30,74 | **31,67** |

➡️ You can choose the compromise you prefer :  
- Robustness ↑ → Flow ↓**  
- ** Flow ↑ → Robustness ↓**

---

## **Repository content**

| File| Description |
|--------|-------------|
| `/Video` | demonstration videos |
| `/EIT` | EIT examples for each station |
| `/DVB` | FFmpeg, TSDuck et GNU Radio Scripts |
| `mpeg-live-analyzed.txt` | TSDuck analysis updated live |

---

## **Multiplex customization**

You can :

### ✔ Choose the sources you want  
- Files ;
- Live stream ;
- IP Cameras ;
- USB Cameras ;
- HDMI Capture cards ;
- etc.

### ✔ Add as many stations as you want 
For this, you'll need to :

1. **Edit the corresponding script :**  
   - `file-source.sh` (file mode)  
   - `live-stream.sh` (live mode)  
2. **Update the TSDuck settings** to take into account the new feeds ;
3. **Update the NIT** so the television can detect all the stations ;
4. **Respect the maximum flow rate** (31,67 Mbit/s by default)

---

## **Known limitations**

### ❗ The station numbers (LCN) are not recognized by the TV.

During testing done during project development, station numbers are not recognized by my TV (LG CX model).
However, they are present according t the TSDuck analysis, and are visible for TvHeadEnd.
Any contribution or feedback is welcome to improve this point.



---

## **Legal warning**

⚠️ **RF broadcast in DVB-T frequencies is strictly regulated.**


This project is **only** **legal** if :

✔ Diffusion remains **in a closed coaxial network**,  
✔ Or directly connected to the television through cable.

❌ It is forbidden to broadcast in the air.

---

## **Evolution prospects**

- Management of **two simultaneous multiplexes** on one HackRF (~63 Mbit/s)  
- Automatic generation of the **EIT** from **XMLTV**  
- Integration with **DizqueTV** (playlists and advanced planning)  
- Backup video/image in cas a live feed drops 
- TSDuck exports toward **InfluxDB + Grafana**

---
