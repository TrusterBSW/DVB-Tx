# dvbtx — home-made multi-carrier DVB-T transmitter on a software radio

*[Version française : README.fr.md](README.fr.md)*

dvbtx builds complete DVB multiplexes from live streams, capture devices, files or pipes, and
modulates them on a software radio (HackRF One, USRP B200/B210, any SoapySDR transmitter). One
program replaces the bash scripts, ffmpeg, TSDuck and GNU Radio chain of the previous version:

- **any number of carriers, radios and services**, described in one TOML file or on the command line;
- all the carriers of a radio are synthesised **in a single FFT**: 2 multiplexes on a HackRF,
  6 on a B210 (190 Mbit/s of payload);
- a C multiplexer **clocked by the radio**: exact rate, restamped PCRs, generated PSI/SI (NIT with
  logical channel numbers, SDT, EIT, TOT), automatic switch to a backup clip;
- a monitoring web page: bit rate per service, **decoding margin**, over-capacity alerts, TSDuck
  analysis exportable as JSON;
- Debian package and systemd service.

> ⚠️ **Closed coax only.** Transmitting on broadcast TV frequencies is illegal. dvbtx is meant for a
> private coaxial network or a direct cable to the TV set, with attenuators.

## Architecture

```
  sources                    ingest (one ffmpeg process per source)        C engine (dvbtx-engine)
  ───────                    ──────────────────────────────────────        ───────────────────────────────────────
  HLS / HTTP / M3U ─┐                                                     ┌ UDP reception, PAT/PMT demux
  Twitch, YouTube ──┤        ffmpeg -c copy (or encoding for              │ ideal departure time of every packet
  RTMP / RTSP / SRT ┼──────► captures) ───── SPTS over local UDP ───────► │ (interpolated between PCRs, fixed buffer)
  tvheadend (HTSP) ─┤        restarted automatically                      │
  webcam / HDMI ────┤                                                     ├ one multiplexer per carrier, pulled by
  screen / pattern ─┤        direct UDP multicast/unicast ──────────────► │ the modulator: due SI > most urgent
  files, folder ────┤                                                     │ packet > PCR > null packet;
  pipe, stdin, exec ┘                                                     │ PCR restamp, PID remap, backup switch
                                                                          ├ DVB-T coding (RS, interleavers,
  Python supervisor (dvbtx)                                               │ convolutional code, pilots, TPS)
  ─────────────────────────                                               ├ one IFFT per radio for all its
  configuration, frequency plan, NIT/SDT/EIT/TOT, EPG,                    │ carriers, at the radio rate
  web page, alerts, TSDuck analysis ◄── UDP copy of each multiplex ◄───── └ HackRF / UHD / SoapySDR (plugins)
```

- **Each source is a separate process** linked over UDP: a source that crashes, freezes or lags never
  disturbs the others; its service switches to its backup and comes back on its own.
- **The multiplexer is clocked by the radio**: the modulator asks for each packet when it needs it.
  No more ffmpeg `-muxrate`, `tsp -P regulate` or named pipe between TSDuck and GNU Radio drifting
  apart: the rate is exact by construction, gaps are filled with null packets, PCRs are recomputed
  at the real emission time (measured jitter < 0.5 µs).
- **The heavy work is in C** (multiplexing, channel coding, FFTW, conversion); the Python supervisor
  only does configuration, SI tables and monitoring.

## Installation

```bash
sudo apt install ./dvbtx_0.1.0_amd64.deb       # Debian 12+ / Ubuntu 24.04+
```

From source:

```bash
sudo apt install build-essential pkg-config libfftw3-dev ffmpeg python3 \
                 libhackrf-dev libuhd-dev libsoapysdr-dev       # every radio is optional
make -C engine
python3 -m dvbtx check -c configs/hackrf-1mux.toml
./packaging/build-deb.sh                                         # builds the .deb
```

Python ≥ 3.11 (tomllib), standard library only. TSDuck is optional (analysis).

## Quick start

```bash
# command line only: one radio, one carrier
sudo dvbtx --driver hackrf --channel 22 \
     --service "Pattern=lavfi:testsrc2=size=1280x720:rate=25" \
     --service "Movie=/srv/videos/movie.mkv,lcn=2"

# with a file
dvbtx check -c /etc/dvbtx/dvbtx.toml          # validate and show the frequency plan, no transmission
sudo dvbtx run -c /etc/dvbtx/dvbtx.toml       # web page on http://127.0.0.1:8080/
sudo dvbtx run -c my.toml -s radio.0.gain=40 -s carrier.1.frequency=498MHz
```

## Configuration

Reference file, commented key by key: [`packaging/dvbtx.toml`](packaging/dvbtx.toml)
(installed as `/etc/dvbtx/dvbtx.toml`). `[[radio]]` → `[[carrier]]` (multiplex: standard,
frequency or channel, bandwidth, mode, constellation, code rate, guard interval, TSID) →
`[[carrier.service]]` (name, LCN, source, EPG id, backup, encoding). Computed unless forced: radio
LO and sample rate, service_id, LCN, PIDs, service type (from the streams). Examples in
[`configs/`](configs/).

## Sources

`/path/file.mkv` (looped) · `dir:/folder` (playlist, one file after the other) · `http(s)://`, HLS ·
`m3u:list.m3u#Name` · `twitch://channel` · `ytdlp:URL` · `htsp://user:pass@host:9982/Channel`
(tvheadend) · `rtmp://...` and `rtmp://0.0.0.0:1935/live/key?listen` (receive OBS) · `rtsp://`,
`srt://`, `udp://`, `rtp://` · `udp+direct://239.0.0.1:2000` (read by the engine, no ffmpeg) ·
`v4l2:/dev/video0?size=1280x720&fps=30&audio=pulse:default` (webcam, HDMI capture) ·
`screen::0.0?size=1920x1080` · `lavfi:testsrc2` · `pipe:/path/fifo` · `stdin:` · `exec:command`.
Capture sources are encoded (`video_bitrate = "auto"` takes the share of the multiplex left by the
others), the others copied; `encode = { ... }` forces encoding (libx264/x265, VAAPI, NVENC, MPEG-2).

## Logical channel numbers and ONID

The NIT carries the EACEM private_data_specifier and logical_channel_descriptor (0x83), like the
French DTT. **Many TVs ignore LCNs when the original_network_id does not match their country**:
`onid` and `network_id` default to `0x20FA` (France); set your country's in `[network]`.

## EPG

Every XMLTV or TSDuck EIT XML file dropped in `epg.dir` (`/var/lib/dvbtx/epg/`) is loaded and
re-read when it changes; XMLTV URLs can be polled too (`epg.urls`). EIT present/following and
schedule (`epg.days`), ISO 6937 text, genre and parental rating. `dvbtx epg` shows what each
service gets.

## Useful bit rate: `dvbtx calc`

```bash
dvbtx calc dvbt --table          # full DVB-T table (31.67 Mbit/s for 64-QAM 7/8 1/32 8 MHz)
dvbtx calc dvbt --constellation 64qam --code-rate 3/4 --guard 1/8 --services 6
dvbtx calc dvbs2 --symbol-rate 27.5M --modulation 8psk --code-rate 3/4
```

## Monitoring, alerts, TSDuck export

- web page: radios (buffer, CPU, underruns), multiplexes (occupancy per service, 10-minute graph,
  TSDuck), services (active source, input/output rates, **decoding margin**, lateness, input CC
  errors, now/next), sources (ffmpeg state, restarts, log, restart button), alerts;
- the **decoding margin** is, for every frame, the time between its emission and its decoding time
  in the receiver (DTS − PCR at emission, minimum per second): ~700 ms normally, lower when the
  multiplex is too full, below zero the TV freezes;
- alerts (log, `webhook` JSON POST, `command`): sources asking more than the channel can carry,
  full multiplex, low margin, dropped packets, service on backup, looping source, radio underrun,
  TSDuck errors, engine down;
- `--analysis-export udp://host:port` (or `tcp://`): TSDuck `analyze` JSON report of every
  multiplex; `--stats-export`: engine statistics + alerts every second; `/metrics` for Prometheus;
  `/api/status` JSON; each multiplex is copied to `udp://127.0.0.1:20900+n` (and `carrier.monitor`).

## Backup clip

Each service has a backup (`/var/lib/dvbtx/backup.ts`, made by `dvbtx make-backup --text ...` or
`--image file.png`): after `switch_timeout_ms` (3 s) of silence the service switches to it on a key
frame, and returns once the primary has been stable for `return_ms` (10 s).

## systemd

```bash
sudo systemctl enable --now dvbtx        # /etc/dvbtx/dvbtx.toml
sudo systemctl reload dvbtx              # re-read the EPG, rebuild the SI tables
sudo systemctl enable --now dvbtx@room   # another instance: /etc/dvbtx/room.toml
```

## Measurements

Threadripper 2950X, Debian 12, radio output cabled to an RTL2832U DVB-T receiver:

| Setup | Result |
|---|---|
| HackRF, 1 multiplex 64-QAM 7/8 | lock, C/N 30.9 dB, post-Viterbi BER 0 |
| HackRF, 2 multiplexes (482 + 490 MHz) | both locked, C/N ~29 dB, BER 0; CPU: radio 25 %, 26 % per multiplex (of one core) |
| B210, 6 multiplexes (474-514 MHz) | all six locked, C/N 26-34 dB, BER 0; CPU: radio 38 %, ~25 % per multiplex; no underrun |
| RF-received TS through TSDuck | no continuity error, PCRs OK, names, LCN, ONID, NIT, EIT, TOT correct, video decoded |

The C DVB-T coder is **sample-exact against GNU Radio gr-dtv** (`tests/`) on 2k/8k, QPSK to
64-QAM, every code rate and guard interval.

## Status

DVB-T ✅ (validated over the air) · HackRF ✅ · USRP/UHD ✅ · SoapySDR (built, untested) ·
DVB-C (16/32/64/128/256-QAM) ✅ symbol-exact against the mini-cmts J.83A encoder, received over the air (Sony
tuner: 256-QAM error-free on a B210, 128-QAM on a HackRF) · DVB-S (QPSK 1/2-7/8) ✅ demodulated by leandvb ·
DVB-S2 (QPSK/8PSK/16APSK/32APSK) ✅ symbol-exact against gr-dtv, 8PSK 9/10 at 20 Msym/s decoded error-free by
gr-dvbs2rx ·
DVB-T2 (single PLP, 1K-32K, normal/extended, PP1-PP8, QPSK-256QAM rotated, normal/short frames) ✅ sample-exact
against gr-dtv on 23 configurations (`docs/DVBT2_PORT.md`), resampled to the radio rate (exact, see below) and
mixable with the other carriers: `standard = "dvbt2"` with `fft`, `extended`, `guard`, `pilot`, `constellation`,
`rotation`, `code_rate`, `frame`.

DVB-C, DVB-S and DVB-S2 carriers (`standard = "dvbc"` with `symbol_rate`, `constellation`; `"dvbs"` with
`symbol_rate`, `code_rate`; `"dvbs2"` with `symbol_rate`, `modulation`, `code_rate`, `rolloff`, `pilots`, `frame`)
and DVB-T2 are synthesised directly at the radio rate by frequency-domain filter banks (overlap-save): each block of
symbols goes through an FFT, is weighted by the exact root-raised-cosine response (flat for the DVB-T2 resampling),
placed at the carrier frequency, and one IFFT per bank gives the radio samples. The planner picks a radio rate of
which the symbol rates are simple fractions (`dvbtx check` shows it). Measured with an ideal receiver
(`tests/rx_sc_mer.py`, null radio, 16-bit samples, 256-QAM): 85.6 dB of MER with an adjacent carrier, against
21 dB with the former 16-symbol polyphase filter; DVB-T2 resampling within -110 dB of ideal. `synth = "fir"` on a
carrier keeps the polyphase filter (also used when the rate is not a simple fraction).

When the radio rate is a whole multiple k of the DVB-T2 rate (64/7 MHz in 8 MHz channels; the planner prefers
such a rate when the radio carries T2, and puts the LO on a 62.5 kHz grid from the T2 carriers), the T2 carriers
are not resampled at all: each OFDM symbol is one IFFT of k x FFT points at the radio rate, shared by all the T2
carriers with the same FFT size, guard interval and frame length (P1 included). Measured on the continual pilots
(`tests/rx_t2_pilots.py`, null radio): 148 dB for one carrier (146 dB for the native-rate reference), 145.5 dB
for two adjacent T2 carriers. Engine CPU (whole process), T2 32K 256-QAM 5/6: 0.42 core at 18.29 MS/s against
1.16 with the filter bank at 16.33 MS/s; two T2 and one DVB-C 256-QAM carrier: 1.06 cores at 27.43 MS/s against
2.68 at 24.18 MS/s. Like DVB-T, the OFDM side lobes are not filtered (-49 dB at the band edge): an adjacent DVB-C
carrier on the same radio measures 59 dB of MER instead of 85 (far above what a DAC and its analog stage give).

## License

GPL-3.0-or-later.
