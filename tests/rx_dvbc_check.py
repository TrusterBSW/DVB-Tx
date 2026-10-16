#!/usr/bin/env python3
"""Demodulate a DVB-C I/Q capture with the J.83A receiver of mini-cmts and check the TS.
usage: rx_dvbc_check.py DOCSIS_DIR capture.cs16 sample_rate offset_hz qam out.ts"""
import sys
import numpy as np
sys.path.insert(0, sys.argv[1])
from cmts import rx_j83a as rx
path, fs, off, qam, out = sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
x = rx.load_hackrf(path, skip=int(fs), count=int(2 * fs * 1.2), fmt="sc16")
y = rx.channelize(x, fs, off, sps=4)
s, _ = rx.timing_recover(y, 4)
s, f = rx.freq_correct(s)
sym, eq, mer = rx.equalize(s, qam)
m = 6 if qam == 64 else 8
d = rx.diff_decode(sym, m)
for ph in range(4 if m == 6 else 1):
    b = rx.symbols_to_bytes(d, m, ph)
    off0, score = rx.find_sync(b)
    if off0 is not None:
        break
print(f"MER {mer:.1f} dB, sync score {score:.2f}")
ts = rx.fast_outer_decode(b[off0:])
bad = int(np.sum(ts[:, 1] & 0x80 != 0))
print(f"{ts.shape[0]} packets, {bad} with RS parity errors")
ts.tofile(out)
