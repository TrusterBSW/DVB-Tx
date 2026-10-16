# Validation scripts

| Script | What it checks | Needs |
|---|---|---|
| `dvbt_ref.c` + `gr_dvbt_ref.py` + `cmp_dvbt.py` | the C DVB-T coder against GNU Radio gr-dtv, carrier by carrier | GNU Radio 3.10 with gr-dtv |
| `dvbc_ref.c` + `cmp_dvbc_docsis.py` | DVB-C (J.83 A) symbols against the mini-cmts encoder (validated with a cable modem) | the mini-cmts sources |
| `rx_dvbc_check.py` | demodulates a DVB-C I/Q capture (null radio, `format=cs16`) with the mini-cmts receiver | the mini-cmts sources, numpy, scipy |
| `bench_dvbt.c` | speed of the DVB-T coder (share of one core per carrier) | — |
| `gr_dvbt_tx.py` | known-good GNU Radio DVB-T transmitter on a HackRF, to compare reception | gr-osmosdr |

DVB-T comparison (6 configurations, 300 symbols each):

```bash
gcc -O2 -o /tmp/dvbt_ref tests/dvbt_ref.c engine/src/dvbt.c engine/src/outer.c engine/src/ofdm.c engine/src/util.c -lfftw3f -lm -lpthread
/tmp/dvbt_ref in.ts mine.cf32 8192 6 7 8 32 300          # fft bits k n gi symbols
python3 tests/gr_dvbt_ref.py in.ts gr.cf32 8192 6 7 8 32 300
python3 tests/cmp_dvbt.py mine.cf32 gr.cf32 8192 32 0    # -> "300 symbols compared, 0 with differences"
```

DVB-S: the null radio writes the I/Q, `leandvb --s16 -f RATE --sr SYMRATE --cr 3/4 --tune OFFSET < iq > rx.ts`
(leansdr), then `tsp -I file rx.ts -P analyze`.

Over the air: a DVB-T USB stick (RTL2832U) cabled to the radio, `dvbv5-zap -c channels.conf -P -o rx.ts -t 20 NAME`,
then `tsp -I file rx.ts -P continuity -P pcrverify -P analyze` and `tstables`.
