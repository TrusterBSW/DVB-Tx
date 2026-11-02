#!/usr/bin/env python3
"""Compare two cf32 files sample by sample (optionally allowing a global complex scale)."""
import sys
import numpy as np
a, b = np.fromfile(sys.argv[1], np.complex64), np.fromfile(sys.argv[2], np.complex64)
n = min(a.size, b.size)
a, b = a[:n], b[:n]
sc = np.vdot(a, b) / np.vdot(a, a) if len(sys.argv) > 3 else 1.0
err = np.abs(b - sc * a)
bad = np.nonzero(err > 1e-3 * max(1e-9, np.sqrt(np.mean(np.abs(b) ** 2))))[0]
print(f"{n} samples compared, {bad.size} differ" + (f", first at {bad[0]}: {a[bad[0]]} vs {b[bad[0]]}" if bad.size else "") + (f", scale {sc:.4g}" if len(sys.argv) > 3 else ""))
