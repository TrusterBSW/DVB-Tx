#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos).
"""Compare two DVB-T2 cf32 captures sample by sample, allowing one global complex scale factor.

usage: cmp_dvbt2.py ours.cf32 gr.cf32 [frame_samples fft guard_samples] [threshold]
Prints the scale, the error/signal ratio, and the first differing sample (located in frame,
P1 or symbol when the frame geometry is given). Exit status 1 if a sample differs by more than
threshold (default 1e-3) times the RMS level.
"""
import sys

import numpy as np


def main():
    a, b = sys.argv[1], sys.argv[2]
    geo = [int(x) for x in sys.argv[3:6]] if len(sys.argv) >= 6 else None
    thr = float(sys.argv[6]) if len(sys.argv) > 6 else (float(sys.argv[3]) if len(sys.argv) == 4 else 1e-3)
    x = np.fromfile(a, np.complex64).astype(np.complex128)
    y = np.fromfile(b, np.complex64).astype(np.complex128)
    n = min(len(x), len(y))
    if n == 0:
        print("empty input")
        return 1
    if len(x) != len(y):
        print(f"lengths differ: {len(x)} vs {len(y)}, comparing {n}")
    x, y = x[:n], y[:n]
    sc = np.vdot(x, y) / np.vdot(x, x)
    rms = np.sqrt(np.mean(np.abs(y) ** 2))
    err = np.abs(y - sc * x) / rms
    snr = 10 * np.log10(np.mean(np.abs(y) ** 2) / max(np.mean(np.abs(y - sc * x) ** 2), 1e-300))
    print(f"{n} samples, scale {sc.real:+.6f}{sc.imag:+.6f}j (|scale| {abs(sc):.6f}), "
          f"max error {err.max():.3e} x RMS, signal/error {snr:.1f} dB")
    bad = np.nonzero(err > thr)[0]
    if len(bad) == 0:
        print(f"IDENTICAL within {thr:g} x RMS")
        return 0
    i = int(bad[0])
    where = ""
    if geo:
        F, N, G = geo
        f, r = divmod(i, F)
        if r < 2048:
            where = f" (frame {f}, P1 sample {r})"
        else:
            s, k = divmod(r - 2048, N + G)
            where = f" (frame {f}, symbol {s}, sample {k})"
    print(f"DIFFERENT: {len(bad)} samples above {thr:g} x RMS, first at {i}{where}: "
          f"ours*scale {sc * x[i]:.5f} gr {y[i]:.5f}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
