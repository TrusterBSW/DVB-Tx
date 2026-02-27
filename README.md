🇬🇧 **ENGLISH README** [HERE](https://github.com/TrusterBSW/DVB-Tx/blob/main/README_EN.md) 🇺🇸

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
6. [Débit et paramètres DVB-T](#débit-et-paramètres-dvb-t)  
7. [Contenu du dépôt](#contenu-du-dépôt)  
8. [Personnalisation du multiplexe](#personnalisation-du-multiplexe)  
9. [Limitations connues](#limitations-connues)  
10. [Avertissement légal](#avertissement-légal)  
11. [Perspectives d’évolution](#perspectives-dévolution)  


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
- **screen** (utilisé pour mettre FFmpeg et GNU Radio en arrière plan)
- **Git-LFS** (utilisé pour les vidéo d'exemple disponible dans /Video)

---

## **Installation**

### 1. Installation FFmpeg, GNU Radio et screen (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install git ffmpeg gnuradio gr-osmosdr screen git-lfs
```

### 2. Installation de TSDuck

Téléchargez le paquet `.deb` correspondant à votre distribution depuis les [releases GitHub](https://github.com/tsduck/tsduck/releases).

```bash
wget https://github.com/tsduck/tsduck/releases/download/v3.40-4165/tsduck_3.40-4165.debian13_amd64.deb
sudo dpkg -i tsduck_3.40-4165.debian13_amd64.deb
sudo apt -f install
```
#### Mise à jour automatique

Une fois TSDuck installé, vous pouvez vérifier et mettre à jour vers la dernière version avec :

```bash
tsversion --check    # Vérifie si une nouvelle version est disponible
tsversion --upgrade  # Télécharge et installe la dernière version
sudo apt -f install
```

---

## **Démarrage**

### Récupération du projet

```bash
git clone https://github.com/TrusterBSW/DVB-Tx
cd DVB-Tx/DVB
```

### Ajouts des droits à l'utilisateur

```bash
sudo usermod -a -G plugdev $USER
```

### Diffuser les fichiers Video inclus
```./start.sh file -f ```

### Diffuser des flux en direct
```./start.sh live -f ```

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

### **Tableau complet des débits DVB-T**

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
- ~~Diffuser des dashboard web pour diffuser l'etat des services~~ [URL2Stream](https://github.com/TrusterBSW/URL2Stream)
- Intégration avec **DizqueTV** (playlists et planning avancé)  
- Vidéo/image de secours si une source live tombe  
- Export TSDuck vers **InfluxDB + Grafana**

---
