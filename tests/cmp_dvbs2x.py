#!/usr/bin/env python3
"""Every DVB-S2X MODCOD of engine/src/dvbs2x_tables.h: the dvbtx coder (tests/dvbs2_ref.c) against
GNU Radio gr-dtv, symbol by symbol, 3 PLFRAMEs each, pilots off and on.

usage: python3 tests/cmp_dvbs2x.py [filter]      (needs GNU Radio with gr-dtv, gcc, FFTW not needed)
"""
import math
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from gnuradio import blocks, dtv, gr

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "engine", "src")


def gr_symbols(ts, out, const, num, den, short, pilots, nsym):
    fs = dtv.FECFRAME_SHORT if short else dtv.FECFRAME_NORMAL
    cr = getattr(dtv, f"C{num}_{den}")
    mod = getattr(dtv, f"MOD_{const}")
    tb = gr.top_block()
    chain = [blocks.file_source(1, ts, True),
             dtv.dvb_bbheader_bb(dtv.STANDARD_DVBS2, fs, cr, dtv.RO_0_35, dtv.INPUTMODE_NORMAL, dtv.INBAND_OFF,
                                 168, 4000000),
             dtv.dvb_bbscrambler_bb(dtv.STANDARD_DVBS2, fs, cr),
             dtv.dvb_bch_bb(dtv.STANDARD_DVBS2, fs, cr),
             dtv.dvb_ldpc_bb(dtv.STANDARD_DVBS2, fs, cr, mod),        # 128APSK: 6 padding bits
             dtv.dvbs2_interleaver_bb(fs, cr, mod),
             dtv.dvbs2_modulator_bc(fs, cr, mod, dtv.INTERPOLATION_OFF),
             dtv.dvbs2_physical_cc(fs, cr, mod, dtv.PILOTS_ON if pilots else dtv.PILOTS_OFF, 0),
             blocks.keep_m_in_n(gr.sizeof_gr_complex, 1, 2, 0),
             blocks.head(gr.sizeof_gr_complex, nsym), blocks.file_sink(gr.sizeof_gr_complex, out)]
    for a, b in zip(chain, chain[1:]):
        tb.connect(a, b)
    tb.run()


def main():
    filt = sys.argv[1] if len(sys.argv) > 1 else ""
    rows = re.findall(r'\{ 0x[0-9a-f]{2}, (\d), (\d+), (\d+), ([01]), "(\w+)"',
                      open(os.path.join(SRC, "dvbs2x_tables.h")).read())
    tmp = tempfile.mkdtemp(prefix="s2x-")
    ref = os.path.join(tmp, "dvbs2_ref")
    subprocess.run(["gcc", "-O2", "-std=gnu11", "-pthread", "-o", ref, os.path.join(ROOT, "tests", "dvbs2_ref.c"),
                    os.path.join(SRC, "dvbs2.c"), os.path.join(SRC, "dvb_fec.c"), os.path.join(SRC, "util.c"),
                    "-lm"], check=True)
    ts = os.path.join(tmp, "in.ts")
    rng = np.random.default_rng(1)
    pk = rng.integers(0, 256, (4000, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    pk.tofile(ts)
    fails = n = 0
    for bits, num, den, short, const in rows:
        bits, num, den, short = int(bits), int(num), int(den), int(short)
        name = f"{const} {num}/{den}{' short' if short else ''}"
        if filt not in name:
            continue
        nldpc = 16200 if short else 64800
        slots = math.ceil(math.ceil(nldpc / bits) / 90)
        for pilots in (0, 1):
            flen = 90 + 90 * slots + (((slots - 1) // 16) * 36 if pilots else 0)
            nsym = 3 * flen
            a, b = os.path.join(tmp, "a.cf32"), os.path.join(tmp, "b.cf32")
            r = subprocess.run([ref, ts, a, str(bits), str(num), str(den), str(short), str(pilots), str(nsym)],
                               capture_output=True, text=True)
            if r.returncode:
                print(f"FAIL {name} pilots {pilots}: dvbtx refuses it: {r.stderr.strip()}")
                fails += 1
                continue
            gr_symbols(ts, b, const, num, den, short, pilots, nsym)
            x, y = np.fromfile(a, np.complex64), np.fromfile(b, np.complex64)
            n += 1
            err = float(np.max(np.abs(x - y))) if len(x) == len(y) == nsym else float("inf")
            ok = err < 1e-4
            fails += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {name:28} pilots {pilots}: {nsym} symbols, max error {err:.2e}")
    print(f"{n - fails} of {n} identical to gr-dtv" if n else "nothing compared")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
