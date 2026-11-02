<h1 align="center">dvbtx</h1>

<p align="center">
  <b>Your own TV channels on your own coax.</b><br>
  Live streams, cameras and files made into real DVB multiplexes, modulated on a software radio.
</p>

<p align="center">
  <a href="README.fr.md">Version française</a> ·
  <a href="#install">Install</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#documentation">Documentation</a>
</p>

<p align="center">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue">
  <img alt="platform" src="https://img.shields.io/badge/platform-Debian%2012%20%7C%2013%20%7C%20Ubuntu%2024.04-informational">
  <img alt="standards" src="https://img.shields.io/badge/DVB-T%20%7C%20T2%20%7C%20C%20%7C%20S%20%7C%20S2-success">
  <img alt="radios" src="https://img.shields.io/badge/radios-HackRF%20%7C%20USRP%20%7C%20SoapySDR-orange">
</p>

---

dvbtx turns a PC and a software radio into a small TV head-end. Give it a list of channels (IPTV
links, YouTube or Twitch lives, an HDMI capture, a webcam, a folder of films), and any TV plugged
on the coax finds them with their names, numbers and programme guide, as on regular digital TV.

- **Every DVB flavour**: DVB-T, DVB-T2, DVB-C, DVB-S and DVB-S2, mixed on the same radio.
- **Many multiplexes per radio**: 2 on a HackRF One, 6 on a USRP B210, all synthesised together;
  any number of radios.
- **Sources of every kind**: HLS, RTMP (OBS), SRT, RTSP, UDP, tvheadend, YouTube, Twitch, M3U
  playlists, webcams and HDMI captures, screen, files, folders, pipes.
- **Broadcast-grade multiplexing**: a C engine clocked by the radio, exact bit rate, PCRs
  restamped, complete PSI/SI (NIT with channel numbers, SDT, EIT from XMLTV, TOT).
- **It keeps going**: a backup clip when a source fails, several sources per channel in order of
  preference, a radio unplugged takes only its own multiplexes off the air.
- **A web page** to watch and drive it all: bit rates, decoding margin, previews, alerts, channels
  and configuration edited live, Stop/Start per multiplex.
- **Ready to run**: Debian packages, systemd service, power profiles for battery use.

> [!WARNING]
> **Closed coax only.** Transmitting on broadcast frequencies is illegal. dvbtx is meant for a
> private coaxial network or a cable to the TV set, with attenuators.

## Contents

- [Install](#install)
- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [Configuration](#configuration)
- [Sources](#sources)
- [Changing channels on air](#changing-channels-on-air)
- [Web page](#web-page)
- [Service and power profiles](#service-and-power-profiles)
- [Standards and radios](#standards-and-radios)
- [Measurements](#measurements)
- [Documentation](#documentation)
- [Building from source](#building-from-source)
- [License](#license)

## Install

Download the package for your system from the
[releases](https://github.com/TrusterBSW/DVB-Tx/releases) and install it with apt (it pulls the
dependencies):

| System | Package |
|---|---|
| Debian 13 (trixie), Proxmox VE 9 | `dvbtx_0.1.0+deb13_amd64.deb` |
| Debian 12 (bookworm), Proxmox VE 8 | `dvbtx_0.1.0+deb12_amd64.deb` |
| Ubuntu 24.04 (noble) | `dvbtx_0.1.0+ubuntu24.04_amd64.deb` |

```bash
sudo apt install ./dvbtx_0.1.0+deb13_amd64.deb
sudo apt install streamlink yt-dlp tsduck     # optional: HLS without stalls, YouTube, analysis
```

One package per release because the radio plugins link that release's UHD and SoapySDR
libraries. Each plugin is only *recommended*: a missing library disables that radio, nothing else.

## Quick start

```bash
dvbtx devices                                    # 1. the radios plugged in, with their args
sudo cp /usr/share/dvbtx/configs/hackrf-1mux.toml /etc/dvbtx/dvbtx.toml
sudoedit /etc/dvbtx/dvbtx.toml                   # 2. your channels
dvbtx check                                      # 3. validate, see the frequency plan
sudo systemctl enable --now dvbtx                # 4. on air, now and at every boot
xdg-open http://127.0.0.1:8080/                  # 5. the web page
```

Then scan the channels on the TV. To try without a file:

```bash
sudo dvbtx --driver hackrf --channel 22 \
     --service "Pattern=lavfi:testsrc2=size=1280x720:rate=25" \
     --service "Movie=/srv/videos/movie.mkv,lcn=2"
```

`dvbtx -h` lists the commands, `dvbtx help COMMAND` details one, `man dvbtx` has everything.

## How it works

```
 sources                  ingest (one process each)               C engine (dvbtx-engine)
 ───────                  ─────────────────────────               ─────────────────────────────────────
 HLS / HTTP / M3U ──┐                                            ┌ UDP reception, PAT/PMT demux
 YouTube, Twitch ───┤     ffmpeg / streamlink / yt-dlp           │ de-jitter buffer, departure times
 RTMP / RTSP / SRT ─┼───► copy, or encode (CPU or GPU) ──UDP───► ├ one multiplexer per carrier, pulled
 tvheadend (HTSP) ──┤     restarted on failure                   │ by the modulator: SI > due packets
 webcam / HDMI ─────┤                                            │ > PCR > null; backup switch
 files, folders ────┤     udp+direct:// read by the engine ────► ├ DVB-T/T2/C/S/S2 channel coding
 pipes, stdin ──────┘                                            ├ one synthesis per radio for all
                                                                 │ its carriers, at the radio rate
 Python supervisor (dvbtx): configuration, frequency plan,       └ HackRF / UHD / SoapySDR plugins
 SI tables and EPG, alerts, watchdog, web page, TSDuck ◄── copy of each multiplex
```

- **Each source is its own process**: one that crashes, freezes or lags never disturbs the others;
  its channel plays the backup clip and comes back on its own.
- **The radio sets the pace**: the modulator asks for each packet when it needs it, so the bit rate
  is exact by construction, gaps are filled with null packets, PCRs are stamped at the real
  emission time (jitter < 0.5 µs).
- **The heavy work is in C** (multiplexing, coding, FFTW); the Python supervisor does the
  configuration, the SI tables, the monitoring and the web page.

Details: `man 7 dvbtx`.

## Configuration

One TOML file, `/etc/dvbtx/dvbtx.toml`: radios, then the multiplexes (carriers) of each radio,
then the channels (services) of each multiplex.

```toml
[network]
name = "Home TV"

[[radio]]
name = "hackrf"
driver = "hackrf"              # hackrf, uhd, soapy, null
gain = 30

[[carrier]]
name = "Mux 1"
radio = "hackrf"
channel = 22                   # or frequency = 482_000_000
standard = "dvbt"              # dvbt, dvbt2, dvbc, dvbs, dvbs2
  [[carrier.service]]
  name = "News"
  source = ["https://example.org/live.m3u8", "ytdlp:https://www.youtube.com/@channel/live"]
  [[carrier.service]]
  name = "Films"
  source = "dir:/srv/films"
```

Everything not given is computed: radio frequency and sample rate, service_id, channel numbers,
PIDs, service type. `-s radio.0.gain=40` overrides any key for one run. Every key is described in
`man 5 dvbtx.toml` and, commented, in [`packaging/dvbtx.toml`](packaging/dvbtx.toml); more examples
in [`configs/`](configs/).

## Sources

| Source | Example |
|---|---|
| file (looped), folder | `/srv/movie.mkv`, `dir:/srv/films` |
| HTTP, HLS | `https://host/live.m3u8`, `…#quality=720p`, `…#variant=2` |
| M3U playlist entry | `m3u:/srv/list.m3u#Channel name` |
| YouTube, Twitch, any streamlink site | `ytdlp:https://youtube.com/…`, `twitch://name`, `streamlink:URL` |
| tvheadend | `htsp://user:pass@host:9982/Channel` |
| RTMP (OBS), RTSP, SRT, UDP, RTP | `rtmp://0.0.0.0:1935/live/key?listen`, `srt://…`, `udp://239.0.0.1:2000` |
| UDP straight to the engine | `udp+direct://239.0.0.1:2000` |
| webcam, HDMI capture, screen | `v4l2:/dev/video0?size=1280x720&fps=30`, `screen::0.0` |
| test pattern | `lavfi:testsrc2` |
| pipe, stdin, command | `pipe:/path/fifo`, `stdin:`, `exec:command` |

Streams already in MPEG-TS-friendly codecs are copied, captures are encoded (on a GPU when one
works: Intel, AMD, NVIDIA are detected and tested once). A list of sources is played in order of
preference: the next one when the one playing fails, back to a preferred one when it works again,
the backup clip when none does. HLS goes through streamlink when installed, which rides the
discontinuities (ads, next episode) that stall ffmpeg. Everything: `man 7 dvbtx-sources`.

## Changing channels on air

```bash
dvbtx channel list
dvbtx channel add "France 24" https://example.org/live.m3u8 --number 3
dvbtx channel source 3 "ytdlp:https://www.youtube.com/@France24_fr/live"
dvbtx channel rename 3 "France 24 FR"
dvbtx channel renumber "France 24 FR" 1 --swap
dvbtx channel remove 0x0103
```

Only that channel is touched (the others lose no packet), and the file is updated (comments kept,
numbers pinned so that nothing shifts). A change of source has no cut: the new one takes over on a
key frame. `man dvbtx-channel`.

## Web page

`http://127.0.0.1:8080/` by default (`web.listen`):

- **dashboard**: radios (state, buffer, underruns), multiplexes (fill per channel, 10-minute
  graph, **Stop / Start**), alerts;
- **channels**: what each one plays, its bit rate, its **decoding margin** (how early frames reach
  the TV: ~700 ms normally, below zero the picture freezes), what is on now and next, thumbnails, a
  live view in the browser, the multiplex as on air in VLC (`.ts` link);
- **sources**: the process of each source (copy or transcode, CPU or which GPU, speed, restarts,
  log, restart button);
- **configuration**: radios, multiplexes and channels edited with the useful bit rate computed as
  you type; changes are saved at once and go on air with **Apply** (channels one by one, without
  touching the others); profiles to switch between setups;
- **logs**; a JSON API (`/api/status`, `/api/channels`...), Prometheus `/metrics`, TSDuck analysis
  exported as JSON.

`man 7 dvbtx-web`.

## Service and power profiles

```bash
sudo systemctl enable --now dvbtx         # /etc/dvbtx/dvbtx.toml, at every boot
sudo systemctl reload dvbtx               # EPG and channels of the file, applied live
sudo systemctl enable --now dvbtx@room    # another transmitter: /etc/dvbtx/room.toml
journalctl -u dvbtx -f
```

**On battery**, `dvbtx-power` keeps the whole machine in its most frugal state while nothing is on
air, and switches to a transmit profile while dvbtx runs (opt-in):

```bash
sudo systemctl enable --now dvbtx-power
sudo dvbtx power status
```

Measured on an Intel i3-8100T with four multiplexes on two radios: idle 1.37 W → 0.62 W, on air
22.1 W → 13.9 W (processor package), no underrun. `man 8 dvbtx-power`.

From a source checkout (no package): `sudo packaging/install-service.sh -c FILE [--power]`.

## Standards and radios

| | Status |
|---|---|
| DVB-T (2k/8k, QPSK to 64-QAM) | ✅ on air; sample-exact against GNU Radio gr-dtv |
| DVB-T2 (single PLP, 1K–32K, extended, PP1–PP8, up to 256-QAM rotated) | ✅ on air (32K 256-QAM 5/6, 50 Mbit/s, error-free); sample-exact against gr-dtv on 23 configurations |
| DVB-C (16 to 256-QAM) | ✅ on air (256-QAM error-free on a B210, 128-QAM on a HackRF) |
| DVB-S (QPSK 1/2–7/8) | ✅ demodulated by leandvb |
| DVB-S2 (QPSK, 8PSK, 16APSK, 32APSK) | ✅ symbol-exact against gr-dtv, 8PSK 9/10 decoded error-free by gr-dvbs2rx |
| HackRF One | ✅ up to 20 MS/s: 2 adjacent 8 MHz multiplexes |
| USRP B200/B210 (UHD) | ✅ 56 MHz: 6 adjacent 8 MHz multiplexes |
| SoapySDR transmitters | built, not tried |

## Measurements

Radio output cabled to a DVB-T/T2/C receiver:

| Setup | Result |
|---|---|
| HackRF, 1 DVB-T multiplex 64-QAM 7/8 | C/N 30.9 dB, BER 0 |
| B210, 6 DVB-T multiplexes (474–514 MHz) | all locked, C/N 26–34 dB, BER 0; radio 38 % of a core, ~25 % per multiplex |
| DVB-T2 32K 256-QAM 5/6 (50.08 Mbit/s) | HackRF C/N 31.7 dB, B210 40 dB, no error |
| DVB-C 256-QAM 6.9 Msym/s (50.87 Mbit/s) | B210 C/N 41.5 dB, no error |
| Received stream through TSDuck | no continuity error, PCRs in spec, NIT, SDT, EIT, TOT correct |
| TV set | channels found with names, numbers, EPG, AAC and E-AC-3 5.1 sound |

The synthesis, filter banks and DVB-T2 port are described in `man 7 dvbtx` and
[`docs/DVBT2_PORT.md`](docs/DVBT2_PORT.md); the validation scripts in [`tests/`](tests/README.md).

## Documentation

| Page | What it covers |
|---|---|
| `dvbtx -h`, `dvbtx help COMMAND` | every command and option |
| [`dvbtx(1)`](man/dvbtx.1) | the command line |
| [`dvbtx-channel(1)`](man/dvbtx-channel.1) | changing channels while on air |
| [`dvbtx.toml(5)`](man/dvbtx.toml.5) | the configuration file, key by key |
| [`dvbtx(7)`](man/dvbtx.7) | how it works: components, data flow, timing, failures |
| [`dvbtx-sources(7)`](man/dvbtx-sources.7) | every kind of source, failover, cache, backup clip |
| [`dvbtx-web(7)`](man/dvbtx-web.7) | the web page and its HTTP API |
| [`dvbtx-engine(8)`](man/dvbtx-engine.8) | the C engine, its INI file, commands and statistics |
| [`dvbtx-power(8)`](man/dvbtx-power.8) | power profiles for battery use |

Installed with the package (`man dvbtx.toml`); from the sources, `man -l man/dvbtx.toml.5`.
Every page also exists in French ([`man/fr/`](man/fr/)), shown with a French locale or `man -L fr`.

## Building from source

```bash
sudo apt install build-essential pkg-config libfftw3-dev ffmpeg python3 \
                 libhackrf-dev libuhd-dev libsoapysdr-dev       # every radio is optional
make -C engine
python3 -m dvbtx check -c configs/hackrf-1mux.toml
sudo packaging/install-service.sh -c /path/to/dvbtx.toml       # as a service, from here
./packaging/build-deb.sh                                         # .deb for this system
./packaging/build-all-debs.sh                                    # .deb for every release (docker)
```

Python ≥ 3.11, standard library only. TSDuck is optional (analysis).

## License

GPL-3.0-or-later. © 2024–2026 BILLY Rémi.
