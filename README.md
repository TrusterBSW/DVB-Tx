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
`m3u:list.m3u#Name` · `twitch://channel` · `ytdlp:URL` · `streamlink:URL` · `htsp://user:pass@host:9982/Channel`
(tvheadend) · `rtmp://...` and `rtmp://0.0.0.0:1935/live/key?listen` (receive OBS) · `rtsp://`,
`srt://`, `udp://`, `rtp://` · `udp+direct://239.0.0.1:2000` (read by the engine, no ffmpeg) ·
`v4l2:/dev/video0?size=1280x720&fps=30&audio=pulse:default` (webcam, HDMI capture) ·
`screen::0.0?size=1920x1080` · `lavfi:testsrc2` · `pipe:/path/fifo` · `stdin:` · `exec:command`.
Capture sources are encoded (`video_bitrate = "auto"` takes the share of the multiplex left by the
others), the others copied; `encode = { ... }` forces encoding (libx264/x265, VAAPI, NVENC, MPEG-2).

HLS URLs are read through [streamlink](https://streamlink.github.io/) when it is installed (`hls = "auto"`,
per channel or in `[defaults]`): it downloads the segments and hands them on as they are, and ffmpeg's
MPEG-TS reader rebases the timestamps at each discontinuity. ffmpeg's own HLS reader stalls there (an ad,
the next episode on Pluto, Samsung TV Plus...) and falls behind short live playlists, the channel then
falling back to its backup: seen 4 s lost every 10 s on a Pluto channel, none in 8 min through
streamlink. A source asking something only ffmpeg does (`#variant=N`, a `map` on programs,
`ffmpeg_input`) stays on ffmpeg; `hls = "ffmpeg"` or `"streamlink"` forces one. `URL#quality=720p`
(or `3300k`, `best`, `1000k,worst`) picks streamlink's quality for one source, `best` by default;
`streamlink:URL` reads anything streamlink has a plugin for (Dailymotion, YouTube...).

## Channels while on air: `dvbtx channel`

Add, remove, rename, renumber a channel or give it another source while dvbtx runs: only that
channel is touched (the others lose no packet), and the change is written back to the
configuration file (comments kept, `sid` / `lcn` / `pid_base` pinned so that nothing shifts).
A change of source does not cut the channel: the old source plays until the new one has a key frame
due, then the switch is made on it (measured: 25-33 ms between two video packets, as without change).

```bash
dvbtx channel list
dvbtx channel add "France 24" https://example.org/live.m3u8 --number 3
dvbtx channel source 3 "ytdlp:https://www.youtube.com/@France24_fr/live"
dvbtx channel rename 3 "France 24 FR"
dvbtx channel renumber "France 24 FR" 1 --swap
dvbtx channel remove 0x0103
```

The same from the web page (**+ Ajouter une chaîne**, **Modifier**, **✕**), from the API
(`GET/POST /api/channels`, `PATCH/DELETE /api/channels/<number|0xSID|name>`), or by editing the
file then `systemctl reload dvbtx`. When dvbtx is not running, `dvbtx channel` edits the file.
Changes from the web are refused from other sites and need `web.token` if one is set; `exec:`
sources only from the file. Everything: [`docs/CHANNELS.md`](docs/CHANNELS.md).

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

- web page in tabs (dashboard, channels, sources, configuration, logs; `#chaines`, `#config`... in the
  address): network settings with the ONID picked by country (terrestrial: 0x2000 + ISO 3166 numeric
  code), per-channel priority (who loses first when the multiplex is full) and, for re-encoded
  channels, a share of the bit rate (`bitrate_weight`: SD 1, HD 2, Full HD 3, 4K 6) up to
  `max_bitrate`;
- radios (buffer, CPU, underruns), multiplexes (occupancy per service, 10-minute graph,
  TSDuck), services (active source, input/output rates, **decoding margin**, lateness, input CC
  errors, now/next), sources (ffmpeg state, restarts, log, restart button), alerts;
- the **decoding margin** is, for every frame, the time between its emission and its decoding time
  in the receiver (DTS − PCR at emission, minimum per second): ~700 ms normally, lower when the
  multiplex is too full, below zero the TV freezes;
- alerts (log, `webhook` JSON POST, `command`): sources asking more than the channel can carry,
  full multiplex, low margin, dropped packets, service on backup, looping source, radio underrun,
  TSDuck errors, engine down, radio missing;
- a **radio missing** at the start or unplugged while on air takes only its own multiplexes off
  the air (their sources paused), the other radios go on; it is looked for every 15 s and the
  engine started again once it is back (a short cut of the others). With no radio at all, dvbtx
  runs without transmitting and its web page stays;
- `--analysis-export udp://host:port` (or `tcp://`): TSDuck `analyze` JSON report of every
  multiplex; `--stats-export`: engine statistics + alerts every second; `/metrics` for Prometheus;
  `/api/status` JSON; each multiplex is copied to `udp://127.0.0.1:20900+n` (and `carrier.monitor`);
- what each source does (copy, CPU or GPU transcode and on which card), its frames per second, speed
  against real time, dropped/duplicated frames, CPU and GPU engine load;
- channel previews: thumbnails shown by default (`web.preview_thumbnails`; the eye of the preview
  column hides them), one key frame per channel every 60 s (`web.preview_thumb_every_s`) while a page
shows them, each shot reading the multiplex only a few seconds; a live view
  in the browser (the picture as on air, H.264 copied; only the container, TS -> MP4, and the sound,
  MP2/AC-3 -> AAC, change; `#live=<sid>` opens one; a click on a channel's picture opens it); and a **TS** link per
  multiplex: the multiplex exactly as on air, every channel (`/api/mux/<n>.ts`, relayed without
  ffmpeg; for mpv, VLC...). The engine
  opens a copy of the multiplex for the preview (`monitor add`), nothing runs when nobody looks;
- configuration from the page: radios, multiplexes (frequency, standard, modulation, with the useful
  rate computed live), channels. Every change is validated and saved in the file at once (comments
  kept), nothing changes on air until **Apply** in the banner listing the pending changes: channels
  only are applied one by one, a radio or multiplex change restarts the transmission (1-2 s, the
  unchanged sources keep running). Profiles are copies of the file beside it (`dvbtx.toml.<name>`):
  save one, load one over the file, then apply; only `dvbtx.toml` is ever applied.

## GPU transcoding

Sources that must be re-encoded go through a GPU when one works, else libx264 on the CPU
(`[ingest] gpu = "auto" | "off" | list of devices`; per channel `encode = { hwaccel = "cpu" }`).
The cards (Intel, AMD, NVIDIA, integrated or not) are detected at every start; every way to use them
(VAAPI per VA driver, low power or not, QSV, NVENC; the whole chain on the GPU, decoding on it with frames copied to memory, or only the encoding)
is tested once on a 1080p→720p capped-rate transcode in H.264, HEVC and MPEG-2: kept if the codec and
size are right and the **bit rate stays under its cap** (no rate control: refused), ranked by speed. The learning runs again (in the background, CPU
meanwhile) only when the cards, their drivers or ffmpeg change (`/var/lib/dvbtx/gpu.json`);
`dvbtx gpu` shows it, `dvbtx gpu learn` or the web button redoes it. New transcodes go to the card
with the most speed left; one that fails twice moves to another way, then to the CPU.
The clip (H.264 High, like a broadcast) is played twice: a way that breaks where the source loops is
refused (ffmpeg 7.1 with the frames kept on the GPU); among the ways at least half as fast as the
fastest, the one using the least CPU is chosen. Measured, 1080p→720p H.264, CPU per frame: libx264
veryfast 18.3 ms, Intel UHD 630 7.1 ms (219 frames/s), NVIDIA RTX A5500 6.6 ms (243 frames/s). Intel
up to gen 9: Debian's free VA driver has no scaling nor rate control there, the non-free one is enough.

## Several sources per channel, cache

`source` may be a list, in order of preference, as long as needed:
`source = ["http://provider/u/p/289", "http://provider/u/p/319", "http://provider/u/p/976"]`.
When the source playing has delivered nothing for `failover_s` (10 s), or has not started within
`source_start_s` (30 s), the next one is started (after the last, the first again), the channel
playing its backup meanwhile. While not on the first one, the preferred ones are tried again in
order every `preferred_retry_s` (300 s, 0: never), beside the one playing: the channel goes back
to one without a cut as soon as it delivers. An unstable source is left too: the channel fell back
to its backup `unstable_switches` times (3) within `unstable_window_s` (10 min), or stayed on it
`failover_s` + `return_ms` (20 s); a source left is tried again later each time (5 min, 10, 20...
up to 1 h), forgiven once it has held 30 min. None works: the general `backup`. A failed source is
stopped before the next one starts (IPTV accounts limited to one connection).
`dvbtx channel source 3 URL1 URL2 URL3`, or one source per line on the web page.
An HLS master playlist offers several qualities and ffmpeg takes the first one listed: `URL#variant=N`
plays only its variant N (0 = the first, as `ffprobe -show_programs` numbers them), for this source
alone, so that a list can fall back to a lower quality of the same stream:
`source = ["https://host/live.m3u8#variant=3", "https://host/live.m3u8#variant=0"]`. The fragment
is not sent to the server.

**Cache**: `buffer_ms`, the delay accepted between the arrival of a source and its transmission:
larger, an irregular stream goes through without losses or switch to the backup, the channel being
later than live. `auto` (default): 6000 ms for sources delivered in segments (YouTube, Twitch,
`.m3u8`: at the live edge they arrive in bursts, one per segment), 1000 ms otherwise. Per channel,
100-30000 ms: `buffer_ms = 8000`, `--cache 8000`, or the **Cache** field of the web page; changing
it does not cut the channel. A source is down only once its cache has played out.

## Backup clip

Each service has a backup (`/var/lib/dvbtx/backup.ts`, made by `dvbtx make-backup --text ...` or
`--image file.png`): after `switch_timeout_ms` (3 s) of silence the service switches to it on a key
frame, and returns once the primary has been stable for `return_ms` (10 s).

## systemd

```bash
sudo systemctl enable --now dvbtx        # /etc/dvbtx/dvbtx.toml
sudo systemctl reload dvbtx              # re-read the EPG and the channels of the file (applied live)
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
Over the air (32K 256-QAM 5/6, 50.077 Mbit/s, to an Astrometa key: RTL2832P USB bridge, Sony CXD2837ER
demodulator): C/N 30.1-30.6 dB from a HackRF, 40 dB from a B210, no uncorrectable FEC block, the same with the
filter bank; on an i5-9500 the engine takes 0.31 core against 0.46 with the bank. The few bad packets this key
delivers (5 to 15 a minute: inserted garbage packets, half of them flagged TEI, and short bursts of lost packets)
come from the key, not from the signal: the sent and received streams aligned packet by packet show no bit error,
their number does not change with the radio, the number of carriers, the LO position, the synthesis, the
modulation, the code rate or the level, and the key shows the same on a commercial cable network (DVB-C, DVB-T,
DVB-T2, DOCSIS).

## License

GPL-3.0-or-later.
