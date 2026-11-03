# Validation scripts

| Script | What it checks | Needs |
|---|---|---|
| `dvbt_ref.c` + `gr_dvbt_ref.py` + `cmp_dvbt.py` | the C DVB-T coder against GNU Radio gr-dtv, carrier by carrier | GNU Radio 3.10 with gr-dtv |
| `dvbc_ref.c` + `cmp_dvbc_docsis.py` | DVB-C (J.83 A) symbols against the mini-cmts encoder (validated with a cable modem) | the mini-cmts sources |
| `rx_dvbc_check.py` | demodulates a DVB-C I/Q capture (null radio, `format=cs16`) with the mini-cmts receiver | the mini-cmts sources, numpy, scipy |
| `rx_sc_mer.py` | MER of a QAM carrier in a null-radio capture with an ideal receiver (exact shift, exact RRC matched filter, timing to 1e-6 symbol): measures the transmitter alone | numpy |
| `rx_t2_pilots.py` | MER of the DVB-T2 continual pilots in a null-radio capture (symbol timing from the guard interval, FFT at the radio rate): checks the direct T2 synthesis | numpy |
| `dvbt2_group_test.c` + `cmp_dvbt2_interp.py` | the T2 synthesis at k x the T2 rate against `dvbt2_samples()` (identity, interpolation, sums of carriers, speed) | FFTW |
| `fcbank_check.c` + `fcbank_check.py` | the frequency-domain filter bank (DVB-C/S/S2/T2 shaping at the radio rate) against an ideal synthesis, and its speed against the time-domain filter | FFTW |
| `fcbank_threads_check.c` | the filter bank with 1 to 5 worker threads against the single-thread bank, two members stopped and started again: bit-identical output | FFTW |
| `rx_sat.py` | DVB-S (leandvb) and DVB-S2 (gr-dvbs2rx: QPSK, 8PSK) reception of a capture or live from a USRP, then a TS check (sync, TEI, continuity) | leansdr, gr-dvbs2rx |
| `live_channels_test.py` | the engine with services and inputs added, changed, removed while running: PAT content and version, the other services undisturbed, no video gap on a change of source (33 checks) | ffmpeg |
| `live_supervisor_test.py` | the same through `dvbtx run` and `dvbtx channel`: PAT and SDT received, the configuration file rewritten, a hand edit applied by SIGHUP, an engine crash after changes, the API refusing `exec:` and other sites, the file edited when dvbtx is stopped (32 checks) | ffmpeg |
| `live_failover_test.py` | a channel with two sources in order of preference (UDP streams started and stopped): an unstable source (on 5 s, off 2.5 s) left for the next one and not taken back at once, the second used when the first fails, back to the first without a cut, backup when both fail, the list changed through the API and written to the file; the other channel undisturbed (19 checks) | ffmpeg |
| `live_discont_test.py` | an HTTP source whose ffmpeg keeps reporting timestamp discontinuities (audio PTS moved back 0.17 s every other 0.33 s by a local server): started again beside itself and swapped in without a cut, the new ffmpeg clean, the other channel undisturbed (8 checks) | ffmpeg |
| `cmp_dvbs2x.py` | every DVB-S2X MODCOD (55, pilots off and on) of the dvbtx coder against GNU Radio gr-dtv, symbol by symbol (110 of 110 identical) | GNU Radio with gr-dtv |
| `dvbs2_coder_check.c` | DVB-S2 symbols of several MODCODs written to a file, to compare two builds with `cmp` (bit-exactness of an optimisation), and their speed | — |
| `planner_test.py` | the frequency planner: LO inside a single wide carrier, warnings near a radio's practical rate | — |
| `channels_test.py` | the channel operations on the configuration and the TOML editor (comments kept, sub-tables, refusals, a file edited meanwhile left untouched) | — |
| `bench_dvbt.c` | speed of the DVB-T coder (share of one core per carrier) | — |
| `gr_dvbt_tx.py` | known-good GNU Radio DVB-T transmitter on a HackRF, to compare reception | gr-osmosdr |

DVB-T comparison (6 configurations, 300 symbols each):

```bash
gcc -O2 -o /tmp/dvbt_ref tests/dvbt_ref.c engine/src/dvbt.c engine/src/outer.c engine/src/ofdm.c engine/src/util.c -lfftw3f -lm -lpthread
/tmp/dvbt_ref in.ts mine.cf32 8192 6 7 8 32 300          # fft bits k n gi symbols
python3 tests/gr_dvbt_ref.py in.ts gr.cf32 8192 6 7 8 32 300
python3 tests/cmp_dvbt.py mine.cf32 gr.cf32 8192 32 0    # -> "300 symbols compared, 0 with differences"
```

DVB-S / DVB-S2: the null radio writes the I/Q (`args = "file=/tmp/s2.cs16,format=cs16"`), then
`rx_sat.py dvbs2 --file /tmp/s2.cs16 --rate RATE --offset OFFSET --sr 20e6 --modcod 8psk9/10 --rolloff 0.2`
(rate and offset: `dvbtx check`). Results (20 Msym/s): DVB-S2 8PSK 9/10 rolloff 0.2 pilots, 157 025 packets
(52.1 Mbit/s) without any error; DVB-S QPSK 7/8, 103 338 packets, one TEI on the very first (acquisition).

DVB-C shaping (null radio, cs16, 256-QAM, real multiplex), `rx_sc_mer.py`: two adjacent carriers (6.9 and
6.875 Msym/s, 8 MHz apart, 16.23 MS/s) 85.6 and 85.7 dB with the filter bank, 21.0 dB with the former
time-domain filter (16-symbol RRC, its side lobes fall on the neighbour); one carrier 88.8 dB (bank).

Over the air: a DVB-T USB stick (RTL2832U) cabled to the radio, `dvbv5-zap -c channels.conf -P -o rx.ts -t 20 NAME`,
then `tsp -I file rx.ts -P continuity -P pcrverify -P analyze` and `tstables`.
