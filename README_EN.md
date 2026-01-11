# **DVB-Tx — Creation of a complete homemade DVB-T transmitter with a HackRF**


DVB-Tx is a project allowing to generate a **multiplex DVB-T complet**, ready to be injected in a local coaxial network.
It automatically combines :

1. **FFmpeg** — Multiplexing of video/audio sources 
2. **TSDuck** — Generation and insertion of SI/PSI tables 
3. **GNU Radio** — DVB-T modulation to Raw I/Q
4. **HackRF One** — Transmission of Raw I/Q en RF

The goal : To allow everyone to broadcast their own TV Channel locally, easily and quickly with an HackRF. 

---

## **📦 Table of Contents**

1. [System architecture](#system-architecture)  
2. [Operating diagram](#operating-diagram)  
3. [Dependencies](#dependencies)  
4. [Installation](#installation)  
5. [Startup](#startup)  
6. [DVB-T Rate and Settings](#dvb-t-rate-and-settings)    
7. [Repository content](#repository-content)  
8. [Multiplex customization](#multiplex-customization)  
9. [Known limitations](#known-limitations)  
10. [Legal warning](#legal-warning)  
11. [Evolution prospects](#evolution-prospects)  
---

## **System architecture**

The DVB-T multiplex is built in three stages :

- **FFmpeg** : reads the sources (files ou live feed) and generates a MPEG-TS flux
- **TSDuck** : injects the DVB tables (PAT, PMT, SDT, NIT, EIT…), checks and analyzes the flux
- **GNU Radio** : modules the MPEG-TS flux in a RF DVB-T signal

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

## **Dependencies**

### Mandatory:
- **FFmpeg**  
- **TSDuck**  
- **GNU Radio 3.10**  
- **HackRF Tools** (if using RF)

### Recommanded :
- **screen** (used to put FFmpeg and GNU Radio in background)
- **Git-LFS** (used to download the video exemple available in /Video)

---

## **Installation**

### 1. Installation FFmpeg, GNU Radio et screen (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install git ffmpeg gnuradio screen
```

### 2. Installation Git-LFS

```bash
git lfs install
```

### 3. Installation of TSDuck

Download the `.deb` package matching your distribution from the [GitHub releases](https://github.com/tsduck/tsduck/releases).

```bash
wget https://github.com/tsduck/tsduck/releases/download/v3.40-4165/tsduck_3.40-4165.debian13_amd64.deb
sudo dpkg -i tsduck_3.40-4165.debian13_amd64.deb
sudo apt -f install
```
Once TSDuck is installed, you can check and update to the latest version with:

```bash
tsversion --check    # Check if a new version is available
tsversion --upgrade  # Download and install the latest version
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

### **Complete table of DVB-T speeds**

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
- ~~Broadcast web dashboard to share services status~~ [URL2Stream](https://github.com/TrusterBSW/URL2Stream)
- Integration with **DizqueTV** (playlists and advanced planning)  
- Backup video/image in cas a live feed drops 
- TSDuck exports toward **InfluxDB + Grafana**

---
