#!/usr/bin/env python3
"""Compare two DVB-T cf32 captures carrier by carrier (gr-dtv puts the centre carrier one bin below DC)."""
import sys
import numpy as np
a, b, fft, gi = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
grshift = int(sys.argv[5]) if len(sys.argv) > 5 else -1
cp = fft // gi
L = fft + cp
x = np.fromfile(a, np.complex64); y = np.fromfile(b, np.complex64)
ns = min(len(x), len(y)) // L
nc = 6817 if fft == 8192 else 1705
kc = nc // 2
bins = (np.arange(nc) - kc) % fft
worst = 0
for s in range(ns):
    X = np.fft.fft(x[s*L+cp:(s+1)*L]); Y = np.fft.fft(y[s*L+cp:(s+1)*L])
    cx = X[bins]; cy = Y[(bins + grshift) % fft]
    sc = np.vdot(cx, cy) / np.vdot(cx, cx)
    err = np.abs(cy - sc * cx) / np.sqrt(np.mean(np.abs(cy)**2))
    bad = np.nonzero(err > 0.05)[0]
    if len(bad):
        worst += 1
        if worst <= 5:
            print(f"sym {s}: {len(bad)} carriers differ, first {bad[:10]}, scale {sc:.4g}")
print(f"{ns} symbols compared, {worst} with differences")
