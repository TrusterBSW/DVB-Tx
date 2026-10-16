# DVB-T2 modulator: port of GNU Radio gr-dtv to C

`engine/src/dvbt2.[ch]` (with `dvb_fec.[ch]` and the generated `dvbt2_tables.h`) turn MPEG-TS
packets, pulled through a callback, into DVB-T2 complex baseband at the elementary sample rate.
It is a port of the gr-dtv (GNU Radio 3.10, maint-3.10 = v3.10.5.0 for these blocks) DVB-T2
transmitter, derived from the work of Ron Economos (Copyright Free Software Foundation, Inc.),
GPL-3.0-or-later like this project, and it reproduces gr-dtv's output sample for sample.

## Scope

| Feature | Supported |
|---|---|
| Input | one PLP, TS, input mode A (normal mode: CRC-8 in place of the sync byte, no ISSY, no null packet deletion, no in-band signalling) |
| FEC | normal (64800) and short (16200) frames, rates 1/2 3/5 2/3 3/4 4/5 5/6 (BCH + LDPC) |
| Constellations | QPSK, 16-, 64-, 256-QAM, rotation on/off (with the cyclic Q delay) |
| Interleaving | bit (parity + column twist + demux), cell, time (any number of TI blocks), frequency |
| OFDM | 1K 2K 4K 8K 16K 32K, normal and extended carriers, all 7 guard intervals, PP1-PP8, frame closing symbols, P1 |
| Signalling | L1-pre (BPSK), L1-post in BPSK / QPSK / 16-QAM / 64-QAM, T2 version 1.1.1 |
| Bandwidths | 1.7 (T = 71/131 us), 5, 6, 7, 8, 10 MHz |
| Not supported | MISO, PAPR reduction (TR/ACE), multiple PLPs, high efficiency mode, FEFs, T2-Lite |

`dvb_fec.[ch]` (BBFRAME, BB scrambler, BCH, LDPC) also covers the DVB-S2 rates
1/4 1/3 2/5 8/9 9/10 and the S2 variants of the normal 2/3 and short 3/5 LDPC tables
(`DVB_FEC_S2`); `dvbs2.c` uses it.

`dvbt2_cfg_check()` rejects what EN 302 755 forbids: guard interval / FFT combinations (table
67), pilot pattern / FFT / guard interval (table 58, SISO), extended carriers below 8K, frames
longer than 250 ms or shorter than N_P2 + 3 (32K) / N_P2 + 7 symbols, odd L_F in 32K, more FEC
blocks than fit (or > 1023), TI blocks exceeding the TI memory M_TI = 2^19 + 2^15 cells.

Defaults (value 0): `data_symbols` = longest frame <= 250 ms (table 46, even L_F in 32K);
`fec_blocks` = the maximum that fits; `ti_blocks` = the fewest TI blocks that fit M_TI;
`t2_frames` = 2 and `frequency` = 729833333 (gr-dtv's values). gr-dtv hard-codes
cell_id 0, network_id 0x3085, t2_system_id 0x8001: use these to compare with it.

`dvbtx/t2.py` mirrors the parameter derivation in pure Python (`rate()`, `occupied_bandwidth()`,
`check()`, `resolve()`); it agrees with the C code on validity and rate for 150 528 parameter
combinations (8 291 valid).

## Integration notes

- `dvbt2_samples()` returns at most one OFDM symbol per call (P1 = 2048 samples, then
  N + GI per symbol, e.g. 33 024 for 32K 1/128); call it until `max` is reached.
- The first call encodes a whole T2 frame (pulls `fec_blocks * (Kbch - 80) / 1504` packets,
  e.g. 5 185 for the UK-like profile). Afterwards the FEC encoding of frame k+1 is spread over
  the L_F + 1 symbol steps of frame k (about fec_blocks / (L_F + 1) BBFRAMEs per symbol), so
  packets are pulled at a steady pace, by bursts of a few dozen at each symbol.
- Latency: a packet pulled while frame k is on air is transmitted in frame k+1, i.e. one to two
  T2 frames (up to ~0.5 s for 250 ms frames) between pull and air, plus the receiver's
  de-interleaving.
- The TS rate is fixed by the frame structure (`dvbt2_ts_rate()`, exact); the caller must
  deliver a packet at every pull (null packets when idle).
- Memory: two mapped frames (2 x 8 bytes x ~1.6 M cells = 26 MB for 32K) plus small tables.
- Output: mean power ~1 (gr-dtv normalisation 5 / sqrt(27 C_PS) after the IFFT, P1 at 1),
  centre carrier at DC.
- `dvbt2_new()` plans the IFFT with FFTW_MEASURE (0.7 s for 32K) under its own mutex.
  `ofdm.c` has another one: do not create a `dvbt2` while another thread plans with FFTW.
- `dvbt2.h` and `dvb_fec.h` declare `ts_pull_fn` like `dvbt.h` (guarded by
  `DVBTX_TS_PULL_FN`; an identical typedef may be repeated in C11).
- Build: add `src/dvbt2.c` (and `src/dvb_fec.c`, already there) to `SRC` in `engine/Makefile`.

## Performance

AMD Threadripper 2950X (one core, -O3 -march=x86-64-v2), steady state after init:

| Profile | TS rate | CPU / real time |
|---|---|---|
| 32K ext PP7 256QAM 3/5 GI 1/128, L_data 59, 202 FEC | 35.95 Mbit/s | 0.159 |
| 32K ext PP7 256QAM 3/5 GI 1/128, defaults (L_data 67, 229 FEC) | 35.96 Mbit/s | 0.155 |
| 8K PP7 QPSK 1/2 GI 1/32 | 7.08 Mbit/s | 0.116 |
| 1K PP4 16QAM 3/5 GI 1/16 | 15.91 Mbit/s | 0.137 |

The LDPC encoder works on 360-bit words (parity row x0 of a Q x 360 matrix receives the 360
information bits rotated by x1, the accumulator becomes two prefix XORs); the column-twist
interleaver works 8 rows at a time with 8x8 bit transposes; cells are written straight to their
place in the frame (cell + time interleaver + frame mapping in one pass).

## Validation

`tests/dvbt2_ref.c` (C), `tests/gr_dvbt2_ref.py` (gr-dtv blocks dvb_bbheader_bb,
dvb_bbscrambler_bb, dvb_bch_bb, dvb_ldpc_bb, dvbt2_interleaver_bb, dvbt2_modulator_bc,
dvbt2_cellinterleaver_cc, dvbt2_framemapper_cc, dvbt2_freqinterleaver_cc,
dvbt2_pilotgenerator_cc, dvbt2_paprtr_cc (off), digital.ofdm_cyclic_prefixer,
dvbt2_p1insertion_cc, as in the gr-dtv vv0xx examples) and `tests/cmp_dvbt2.py`:

    gcc -O3 -march=x86-64-v2 -std=gnu11 -fcx-limited-range -pthread -o dvbt2_ref \
        tests/dvbt2_ref.c engine/src/dvbt2.c engine/src/dvb_fec.c -lfftw3f -lm
    ./dvbt2_ref in.ts ours.cf32 2 fft=32768 ext=1 gi=1/128 pp=7 bits=8 cr=3/5 ldata=59 fec=202 ti=3
    python3 tests/gr_dvbt2_ref.py in.ts gr.cf32 2 fft=32768 ext=1 gi=1/128 pp=7 bits=8 cr=3/5 ldata=59 fec=202 ti=3
    python3 tests/cmp_dvbt2.py ours.cf32 gr.cf32

GNU Radio 3.10.5 (Debian 12), 2 complete T2 frames each, same 33 MB TS; "identical" = every
sample within 1e-3 x RMS after one global complex scale; the scale came out as exactly 1 and the
largest deviation is float rounding (IFFT plans). TS rate: `dvbt2_ts_rate()` = `t2.rate()`;
the identical output over two frames also proves gr-dtv consumes exactly that many TS bytes
per frame (the UK profile with 2/3 gives 40.0007 Mbit/s in mode A, i.e. the published
40.21 Mbit/s once the sync bytes are removed in high efficiency mode).

| Configuration (8 MHz unless stated, rotation on, L1 64QAM unless stated) | L_data / FEC / TI | TS rate (bit/s) | max err / RMS | Result |
|---|---|---|---|---|
| 32K ext PP7 256QAM 3/5 GI 1/128 | 59 / 202 / 3 | 35 948 521.28 | 8.4e-7 | identical |
| 32K ext PP7 256QAM 3/5 GI 1/128 (defaults) | 67 / 229 / 4 | 35 963 358.28 | 8.1e-7 | identical |
| 32K ext PP7 256QAM 2/3 GI 1/128 | 59 / 202 / 3 | 40 000 737.52 | 8.0e-7 | identical |
| 32K PP4 64QAM 2/3 GI 1/16 (frame closing symbol) | defaults | 26 945 100.35 | 9.2e-7 | identical |
| 32K ext PP8 256QAM 2/3 GI 19/128 short | defaults | 34 449 899.67 | 8.3e-7 | identical |
| 32K ext PP2 16QAM 1/2 GI 1/8, 6 MHz, L1 QPSK | defaults | 9 212 993.28 | 8.0e-7 | identical |
| 16K ext PP2 16QAM 3/4 GI 1/8 (odd L_F) | defaults | 18 498 420.58 | 8.2e-7 | identical |
| 16K ext PP8 256QAM 4/5 GI 19/128 short, no rotation, L1 16QAM | defaults | 40 340 424.17 | 9.5e-7 | identical |
| 16K PP6 64QAM 4/5 GI 1/32, 5 MHz | defaults | 21 147 205.64 | 8.2e-7 | identical |
| 8K PP7 QPSK 1/2 GI 1/32, L1 QPSK | defaults | 7 076 538.62 | 4.8e-7 | identical |
| 8K ext PP5 64QAM 3/4 GI 19/256 short | defaults | 29 929 458.16 | 4.8e-7 | identical |
| 8K PP8 16QAM 3/5 GI 1/16 short, L1 BPSK | defaults | 16 352 752.68 | 4.8e-7 | identical |
| 8K ext PP7 256QAM 3/5 GI 1/128, 7 MHz | defaults | 31 040 832.00 | 4.8e-7 | identical |
| 8K ext PP3 256QAM 5/6 GI 1/8, 10 MHz | defaults | 53 898 670.50 | 4.7e-7 | identical |
| 8K ext PP1 16QAM 3/5 GI 1/4 | defaults | 13 282 002.05 | 4.8e-7 | identical |
| 8K ext PP3 16QAM 3/4 GI 1/8, 20 FEC (dummy cells) | 100 / 20 / 2 | 9 380 434.78 | 4.7e-7 | identical |
| 4K PP7 64QAM 2/3 GI 1/32, L1 16QAM | defaults | 28 387 210.46 | 4.8e-7 | identical |
| 4K PP5 64QAM 3/5 GI 1/16 | defaults | 24 589 309.75 | 4.7e-7 | identical |
| 2K PP2 16QAM 3/5 GI 1/8, L1 QPSK | defaults | 14 364 704.19 | 7.4e-7 | identical |
| 2K PP4 QPSK 2/3 GI 1/32 short, L1 16QAM | defaults | 8 992 642.09 | 7.5e-7 | identical |
| 2K PP1 QPSK 1/2 GI 1/4, 1.7 MHz, L1 BPSK | defaults | 1 030 284.04 | 7.3e-7 | identical |
| 1K PP4 16QAM 3/5 GI 1/16, L1 QPSK | defaults | 15 913 752.67 | 3.7e-7 | identical |
| 1K PP1 64QAM 5/6 GI 1/4 (N_P2 = 16) | defaults | 26 881 720.43 | 3.7e-7 | identical |

Also checked: every FEC code of `dvb_fec` against a literal (bit per byte) port of gr-dtv's BCH
and LDPC; all 952 valid FFT / carrier mode / GI / PP / frame size / constellation combinations
build and run (each carrier map has exactly C_P2, C_DATA and N_FC data cells).

## Differences between gr-dtv and EN 302 755

gr-dtv's behaviour is kept, except where gr-dtv is inconsistent with itself:

- 16K extended carrier mode, PP1: gr-dtv lists the extra continual pilot 3636 instead of 13636
  (table G.2). Its map then has C_DATA + 1 data carriers, so its own output is misaligned in
  that mode; we use 13636 (not comparable with gr-dtv for that single combination).
- gr-dtv's frequency interleaver restarts its even/odd symbol count at each `work()` call
  instead of each T2 frame; this only matters when one call holds several frames with an odd
  L_F. We alternate per T2 frame (l mod 2, clause 6.4), which matched gr-dtv in all tests.
- gr-dtv's FFTSIZE_16K_T2GI would signal S2 = 011 (1K / T2-Lite 16K); we always signal 16K as
  100 and use the 8K/32K "T2GI" codes 110/111 exactly when the guard interval is 1/128, 19/256
  or 19/128 (table 19a), as gr-dtv does with FFTSIZE_8K_T2GI / FFTSIZE_32K_T2GI.
- As gr-dtv: the P2 PAPR reserved carriers are left empty even without PAPR (consistent with
  table 47, which has no TR variant); continual pilots of extended carrier mode use the
  normal-mode indices k = k_32K mod K_mod (clause 9.2.4.1) plus table G.2.

## Tables

`tools/gen_t2_tables.py <gr-dtv source dir> engine/src/dvbt2_tables.h` extracts the LDPC
address tables, PN sequence, P2 reserved carriers, continual pilot groups (and how
`init_pilots()` combines them), P1 patterns, L1 puncturing/padding orders, bit interleaver twists
and demux tables and frequency interleaver permutations from the gr-dtv `.cc` files; it checks
that the frame mapper's L1 LDPC tables equal the generic ones.
