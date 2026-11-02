#!/usr/bin/env python3
"""MER of a single-carrier QAM signal in a null-radio I/Q capture (cs16 or cf32), with an ideal receiver:
exact frequency shift, exact root-raised-cosine matched filter in the frequency domain, best timing
phase, data-aided gain from hard decisions.  It measures the transmitter alone (shaping, resampling,
quantisation, neighbour leakage).
usage: rx_sc_mer.py capture fs offset_hz symbol_rate qam [rolloff] [format]"""
import sys
from fractions import Fraction
import numpy as np

path, fs, off, rs, qam = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5])
alpha = float(sys.argv[6]) if len(sys.argv) > 6 else 0.15
fmt = sys.argv[7] if len(sys.argv) > 7 else ("cf32" if path.endswith("cf32") else "cs16")

fr = Fraction(rs / fs).limit_denominator(100000)
nsym = 200000
L = nsym // fr.numerator * fr.numerator
N = L * fr.denominator // fr.numerator           # samples spanning exactly L symbols
import os
size = os.path.getsize(path) // (4 if fmt == "cs16" else 8)
skip = int(fs) if size >= int(fs) + N else max(0, size - N) // 2   # skip the first second when possible
if size < N:
    nsym = (size - 1) * fr.numerator // fr.denominator
    L = nsym // fr.numerator * fr.numerator
    N = L * fr.denominator // fr.numerator
if fmt == "cs16":
    raw = np.fromfile(path, np.int16, count=2 * N, offset=4 * skip).astype(np.float32)
    x = raw[0::2] + 1j * raw[1::2]
else:
    x = np.fromfile(path, np.complex64, count=N, offset=8 * skip).astype(complex)
x = x * np.exp(-2j * np.pi * off / fs * np.arange(N))
X = np.fft.fft(x)
f = np.fft.fftfreq(N, 1 / fs)
a = np.abs(f) / rs
lo, hi = (1 - alpha) / 2, (1 + alpha) / 2
H = np.where(a <= lo, 1.0, np.where(a >= hi, 0.0, np.cos(np.pi / (2 * alpha) * (a - lo))))
sel = H > 0
k = np.round(f[sel] / (rs / L)).astype(int) % L   # fold onto the symbol-rate grid (1 sample/symbol)
m = int(round(np.sqrt(qam)))
lv = np.arange(m) * 2 - (m - 1)


def symbols(tau):
    Y = np.zeros(L, complex)
    np.add.at(Y, k, X[sel] * H[sel] * np.exp(2j * np.pi * f[sel] / rs * tau))
    return np.fft.ifft(Y)


def decide(y):
    return lv[np.clip(np.round((y.real + m - 1) / 2), 0, m - 1).astype(int)] + \
        1j * lv[np.clip(np.round((y.imag + m - 1) / 2), 0, m - 1).astype(int)]


def mer(y):
    y = y / np.sqrt(np.mean(np.abs(y) ** 2)) * np.sqrt(np.mean(lv ** 2) * 2)
    # carrier phase: coarse search (the 4th-power estimate is biased when the symbols are not uniform:
    # null packets), then least squares on the decisions
    z = y[::7]
    ph = max(np.arange(0, 90, 0.5), key=lambda p: -np.sum(np.abs(z * np.exp(-1j * np.radians(p)) -
                                                              decide(z * np.exp(-1j * np.radians(p)))) ** 2))
    y = y * np.exp(-1j * np.radians(ph))
    for _ in range(12):
        d = decide(y)
        y = y * (np.vdot(y, d) / np.vdot(y, y))
    e = y - d
    return 10 * np.log10(np.sum(np.abs(d) ** 2) / np.sum(np.abs(e) ** 2)), y


# square constellations only (16/64/256); timing: coarse grid then refine
taus = np.linspace(0, 1, 32, endpoint=False)
best = max(taus, key=lambda t: mer(symbols(t)[1000:-1000])[0])
# the optimum is sharp (256-QAM at 60 dB needs the timing within 1e-4 symbol): golden section
lo_, hi_ = best - 1 / 32, best + 1 / 32
gr = (np.sqrt(5) - 1) / 2
c, d = hi_ - gr * (hi_ - lo_), lo_ + gr * (hi_ - lo_)
fc, fd = mer(symbols(c)[1000:-1000])[0], mer(symbols(d)[1000:-1000])[0]
while hi_ - lo_ > 1e-6:
    if fc > fd:
        hi_, d, fd = d, c, fc
        c = hi_ - gr * (hi_ - lo_)
        fc = mer(symbols(c)[1000:-1000])[0]
    else:
        lo_, c, fc = c, d, fd
        d = lo_ + gr * (hi_ - lo_)
        fd = mer(symbols(d)[1000:-1000])[0]
best = (lo_ + hi_) / 2
v, y = mer(symbols(best)[1000:-1000])
print(f"MER {v:.1f} dB ({qam}-QAM, {rs / 1e6:g} Msym/s at {off / 1e6:+.6f} MHz, timing {best:.6f})")
