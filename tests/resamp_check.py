#!/usr/bin/env python3
"""Shift back and resample a scgen output to the native rate, align with the input, report the SNR."""
import sys
from fractions import Fraction
import numpy as np
from scipy.signal import resample_poly
x = np.fromfile(sys.argv[1], np.complex64)
y = np.fromfile(sys.argv[2], np.complex64)
fi, fo, off = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
y = y * np.exp(-2j * np.pi * off / fo * np.arange(y.size))
# compare at the output rate, where the delay of scgen is an integer number of samples
fr = Fraction(fo / fi).limit_denominator(1000)
if fr.denominator == 1:          # ideal interpolation: zero-padded spectrum of a long block
    N = 1 << 19
    X = np.fft.fft(x[:N])
    U = fr.numerator
    Z = np.zeros(N * U, np.complex128)
    Z[:N // 2] = X[:N // 2]
    Z[-N // 2:] = X[-N // 2:]
    x = (np.fft.ifft(Z) * U)[N * U // 8: N * U * 7 // 8]
    y = y[N * U // 8:]
else:
    x = resample_poly(x[:400000], fr.numerator, fr.denominator, window=("kaiser", 14.0))
z = y
n = min(len(x), len(z)) - 4000
best = None
for d in range(0, 400):
    a, b = x[2000:2000 + n - 500], z[2000 + d:2000 + d + n - 500]
    c = np.vdot(a, b) / np.vdot(a, a)
    e = np.mean(np.abs(b - c * a) ** 2) / np.mean(np.abs(c * a) ** 2)
    if best is None or e < best[0]:
        best = (e, d, c)
e, d, c = best
# in-band error only (the OFDM side lobes outside the occupied band are filtered differently)
occ = float(sys.argv[6]) if len(sys.argv) > 6 else 0.85
a, b = c * x[2000:2000 + n - 500], z[2000 + d:2000 + d + n - 500]
L = 1 << 16
ea = eb = 0.0
for k in range(0, len(a) - L, L):
    A, B = np.fft.fft(a[k:k + L]), np.fft.fft(b[k:k + L])
    f = np.fft.fftfreq(L) * fo
    m = np.abs(f) < occ * fi / 2 * 0.995
    ea += np.sum(np.abs(B[m] - A[m]) ** 2)
    eb += np.sum(np.abs(A[m]) ** 2)
print(f"delay {d} samples, SNR {-10 * np.log10(e):.1f} dB overall, {10 * np.log10(eb / ea):.1f} dB in the occupied band")
