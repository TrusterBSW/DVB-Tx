# Validation scripts

| Script | What it checks | Needs |
|---|---|---|
| `dvbt_ref.c` + `gr_dvbt_ref.py` + `cmp_dvbt.py` | the C DVB-T coder against GNU Radio gr-dtv, carrier by carrier | GNU Radio 3.10 with gr-dtv |
| `dvbc_ref.c` + `cmp_dvbc_docsis.py` | DVB-C (J.83 A) symbols against the mini-cmts encoder (validated with a cable modem) | the mini-cmts sources |
| `rx_dvbc_check.py` | demodulates a DVB-C I/Q capture (null radio, `format=cs16`) with the mini-cmts receiver | the mini-cmts sources, numpy, scipy |
| `rx_sc_mer.py` | MER of a QAM carrier in a null-radio capture with an ideal receiver (exact shift, exact RRC matched filter, timing to 1e-6 symbol): measures the transmitter alone | numpy |
| `fcbank_check.c` + `fcbank_check.py` | the frequency-domain filter bank (DVB-C/S/S2/T2 shaping at the radio rate) against an ideal synthesis, and its speed against the time-domain filter | FFTW |
| `rx_sat.py` | DVB-S (leandvb) and DVB-S2 (gr-dvbs2rx: QPSK, 8PSK) reception of a capture or live from a USRP, then a TS check (sync, TEI, continuity) | leansdr, gr-dvbs2rx |
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
