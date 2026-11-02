#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos).
"""Reference: modulate a TS file with the GNU Radio gr-dtv DVB-T2 chain (cf32 at the elementary rate).

usage: gr_dvbt2_ref.py in.ts out.cf32 nframes [key=value ...]
same keys as tests/dvbt2_ref.c: bw fft ext gi pp bits rot cr short fec ldata ti l1
(defaults: 8 MHz 32K extended 1/128 PP7 256QAM rotated 3/5 normal, L1 64QAM; fec/ldata/ti 0 =
resolved like the C library through dvbtx/t2.py). Block order and parameters as in the gr-dtv
examples (vv001-cr35.grc ...), with input mode A (INPUTMODE_NORMAL), VERSION_111, PAPR off,
cell_id 0, network_id 0x3085, t2_system_id 0x8001 (hard-coded in gr-dtv), 2 T2 frames per
super-frame.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dvbtx"))
import t2  # noqa: E402

from gnuradio import blocks, digital, dtv, gr  # noqa: E402


def main():
    ts, out, nframes = sys.argv[1], sys.argv[2], int(sys.argv[3])
    o = dict(bw="8e6", fft="32768", ext="1", gi="1/128", pp="7", bits="8", rot="1", cr="3/5", short="0",
             fec="0", ldata="0", ti="0", l1="6")
    for a in sys.argv[4:]:
        k, v = a.split("=", 1)
        if k not in o:
            sys.exit("unknown option " + k)
        o[k] = v
    con = {2: "qpsk", 4: "16qam", 6: "64qam", 8: "256qam"}[int(o["bits"])]
    l1n = {1: "bpsk", 2: "qpsk", 4: "16qam", 6: "64qam"}[int(o["l1"])]
    r = t2.resolve(bandwidth=float(o["bw"]), fft=int(o["fft"]), extended=o["ext"] == "1", guard=o["gi"],
                   pilot=int(o["pp"]), constellation=con, code_rate=o["cr"],
                   frame="short" if o["short"] == "1" else "normal", fec_blocks=int(o["fec"]),
                   data_symbols=int(o["ldata"]), ti_blocks=int(o["ti"]), l1_constellation=l1n)
    N, gis = r["fft"], r["guard_samples"]
    nfec, ldata, nti = r["fec_blocks"], r["data_symbols"], r["ti_blocks"]
    t2gi = r["guard"] in ((1, 128), (19, 256), (19, 128))
    fftsize = {1024: dtv.FFTSIZE_1K, 2048: dtv.FFTSIZE_2K, 4096: dtv.FFTSIZE_4K,
               8192: dtv.FFTSIZE_8K_T2GI if t2gi else dtv.FFTSIZE_8K, 16384: dtv.FFTSIZE_16K,
               32768: dtv.FFTSIZE_32K_T2GI if t2gi else dtv.FFTSIZE_32K}[N]
    gi = {(1, 32): dtv.GI_1_32, (1, 16): dtv.GI_1_16, (1, 8): dtv.GI_1_8, (1, 4): dtv.GI_1_4,
          (1, 128): dtv.GI_1_128, (19, 128): dtv.GI_19_128, (19, 256): dtv.GI_19_256}[r["guard"]]
    pp = [dtv.PILOT_PP1, dtv.PILOT_PP2, dtv.PILOT_PP3, dtv.PILOT_PP4, dtv.PILOT_PP5, dtv.PILOT_PP6,
          dtv.PILOT_PP7, dtv.PILOT_PP8][r["pilot"] - 1]
    mod = {2: dtv.MOD_QPSK, 4: dtv.MOD_16QAM, 6: dtv.MOD_64QAM, 8: dtv.MOD_256QAM}[r["bits"]]
    rate = {"1/2": dtv.C1_2, "3/5": dtv.C3_5, "2/3": dtv.C2_3, "3/4": dtv.C3_4, "4/5": dtv.C4_5,
            "5/6": dtv.C5_6}[r["code_rate"]]
    fsize = dtv.FECFRAME_SHORT if r["short"] else dtv.FECFRAME_NORMAL
    rot = dtv.ROTATION_ON if o["rot"] == "1" else dtv.ROTATION_OFF
    carriers = dtv.CARRIERS_EXTENDED if r["extended"] else dtv.CARRIERS_NORMAL
    l1 = {1: dtv.L1_MOD_BPSK, 2: dtv.L1_MOD_QPSK, 4: dtv.L1_MOD_16QAM, 6: dtv.L1_MOD_64QAM}[r["l1_bits"]]
    bwmhz = float(o["bw"]) / 1e6
    bw = {1.7: dtv.BANDWIDTH_1_7_MHZ, 5.0: dtv.BANDWIDTH_5_0_MHZ, 6.0: dtv.BANDWIDTH_6_0_MHZ,
          7.0: dtv.BANDWIDTH_7_0_MHZ, 8.0: dtv.BANDWIDTH_8_0_MHZ,
          10.0: dtv.BANDWIDTH_10_0_MHZ}[1.7 if abs(bwmhz - 1.7) < 0.02 else round(bwmhz, 1)]
    print(f"gr: ldata={ldata} fec={nfec} ti={nti} frame_samples={r['frame_samples']} "
          f"ts_rate={float(r['ts_rate']):.6f}", flush=True)

    tb = gr.top_block()
    chain = [
        blocks.file_source(gr.sizeof_char, ts, True),
        dtv.dvb_bbheader_bb(dtv.STANDARD_DVBT2, fsize, rate, dtv.RO_0_35, dtv.INPUTMODE_NORMAL,
                            dtv.INBAND_OFF, nfec, 4000000),
        dtv.dvb_bbscrambler_bb(dtv.STANDARD_DVBT2, fsize, rate),
        dtv.dvb_bch_bb(dtv.STANDARD_DVBT2, fsize, rate),
        dtv.dvb_ldpc_bb(dtv.STANDARD_DVBT2, fsize, rate, dtv.MOD_OTHER),
        dtv.dvbt2_interleaver_bb(fsize, rate, mod),
        dtv.dvbt2_modulator_bc(fsize, mod, rot),
        dtv.dvbt2_cellinterleaver_cc(fsize, mod, nfec, nti),
        dtv.dvbt2_framemapper_cc(fsize, rate, mod, rot, nfec, nti, carriers, fftsize, gi, l1, pp, 2, ldata,
                                 dtv.PAPR_OFF, dtv.VERSION_111, dtv.PREAMBLE_T2_SISO, dtv.INPUTMODE_NORMAL,
                                 dtv.RESERVED_OFF, dtv.L1_SCRAMBLED_OFF, dtv.INBAND_OFF),
        dtv.dvbt2_freqinterleaver_cc(carriers, fftsize, pp, gi, ldata, dtv.PAPR_OFF, dtv.VERSION_111,
                                     dtv.PREAMBLE_T2_SISO),
        dtv.dvbt2_pilotgenerator_cc(carriers, fftsize, pp, gi, ldata, dtv.PAPR_OFF, dtv.VERSION_111,
                                    dtv.PREAMBLE_T2_SISO, dtv.MISO_TX1, dtv.EQUALIZATION_OFF, bw, N),
        dtv.dvbt2_paprtr_cc(carriers, fftsize, pp, gi, ldata, dtv.PAPR_OFF, dtv.VERSION_111, 3.0, 10, N),
        digital.ofdm_cyclic_prefixer(N, N + gis, 0, ""),
        dtv.dvbt2_p1insertion_cc(carriers, fftsize, gi, ldata, dtv.PREAMBLE_T2_SISO, dtv.SHOWLEVELS_OFF, 3.01),
        blocks.head(gr.sizeof_gr_complex, nframes * r["frame_samples"]),
        blocks.file_sink(gr.sizeof_gr_complex, out),
    ]
    for a, b in zip(chain, chain[1:]):
        tb.connect(a, b)
    tb.run()


if __name__ == "__main__":
    main()
