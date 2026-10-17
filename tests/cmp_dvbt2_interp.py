#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos).
"""Compare a dvbt2_group output at k times the elementary rate with the native dvbt2_samples output.

usage: cmp_dvbt2_interp.py group.cf32 native.cf32 k offset_bins fft guard frame_samples
1) at the elementary sampling instants: group[k n] = native[n] * exp(j 2 pi off n / fft) (exact);
2) ideal interpolation: native zero-padded in frequency to k times the rate, shifted by the offset
   (continuous oscillator), compared with the group output, over the whole capture minus 1 % at
   each end (circular FFT edges): full band, and after limiting the group output to the native
   band (what the radio's ideal resampler would keep); per segment kind (P1, symbols).
   The native samples alias the out-of-band sidelobes of the continuous-time OFDM signal (the
   rectangular symbols and P1 segments of EN 302 755 clauses 9.5 and 9.8), so this comparison is
   bounded by them (about -45 dB for symbols, -30 dB for P1), not by the synthesis;
3) receiver view: DFT of every symbol's FFT window (and of every P1 'A' segment) at the radio
   rate, on the member's bins, against the DFT of the native window (oscillator phase removed).
"""
import sys

import numpy as np


def snr(ref, x):
    e = np.sum(np.abs(x - ref) ** 2)
    return 10 * np.log10(np.sum(np.abs(ref) ** 2) / e) if e > 0 else float("inf")


def main():
    g = np.fromfile(sys.argv[1], np.complex64).astype(np.complex128)
    nat = np.fromfile(sys.argv[2], np.complex64).astype(np.complex128)
    k, off, N, G, F = (int(v) for v in sys.argv[3:8])
    L = min(len(nat), len(g) // k)
    nat, g = nat[:L], g[:L * k]
    Nt = N * k
    n = np.arange(L)
    dec = g[::k] * np.exp(-2j * np.pi * ((off * k * n) % Nt) / Nt)
    print(f"elementary instants: signal/error {snr(nat, dec):.1f} dB")

    X = np.fft.fft(nat)
    Y = np.zeros(L * k, complex)
    h = L // 2
    Y[:h] = X[:h]
    Y[-(L - h):] = X[h:]
    up = np.fft.ifft(Y) * k
    m = np.arange(L * k)
    up *= np.exp(2j * np.pi * ((off * m) % Nt) / Nt)
    # band-limited copy of the group output: keep the native band around the offset
    Gf = np.fft.fft(g)
    c = int(round(off * L / N))   # offset in bins of this long FFT
    keep = np.zeros(L * k, bool)
    idx = (c + np.arange(-h, L - h)) % (L * k)
    keep[idx] = True
    gb = np.fft.ifft(np.where(keep, Gf, 0))
    edge = L * k // 100
    sl = slice(edge, L * k - edge)
    pos = m % (F * k)
    p1 = pos < 2048 * k
    mid = np.zeros(L * k, bool)
    mid[sl] = True
    print(f"ideal interpolation, full band:   all {snr(up[mid], g[mid]):.1f} dB, "
          f"P1 {snr(up[mid & p1], g[mid & p1]):.1f} dB, symbols {snr(up[mid & ~p1], g[mid & ~p1]):.1f} dB")
    print(f"ideal interpolation, native band: all {snr(up[mid], gb[mid]):.1f} dB, "
          f"P1 {snr(up[mid & p1], gb[mid & p1]):.1f} dB, symbols {snr(up[mid & ~p1], gb[mid & ~p1]):.1f} dB")
    oob = np.sum(np.abs(np.where(keep, 0, Gf)) ** 2) / np.sum(np.abs(Gf) ** 2)
    print(f"group output power outside the native band (OFDM sidelobes): {10 * np.log10(oob):.1f} dB")

    ref, got, ref1, got1 = [], [], [], []
    b = np.arange(-N // 2, N // 2)
    b1 = np.arange(-512, 512)
    for f in range(L // F):
        tf = f * F
        A = np.fft.fft(nat[tf + 542:tf + 1566])
        Ak = np.fft.fft(g[k * (tf + 542):k * (tf + 1566)])
        off1 = off * 1024 // N
        ph = np.exp(2j * np.pi * ((off1 * k * (tf + 542)) % (1024 * k)) / (1024 * k))
        ref1.append(A[b1 % 1024] * ph * k)
        got1.append(Ak[(b1 + off1) % (1024 * k)])
        s0 = tf + 2048
        while s0 + N + G <= tf + F:
            w = s0 + G
            X = np.fft.fft(nat[w:w + N])
            Xk = np.fft.fft(g[k * w:k * w + Nt])
            ph = np.exp(2j * np.pi * ((off * k * w) % Nt) / Nt)
            ref.append(X[b % N] * ph * k)
            got.append(Xk[(b + off) % Nt])
            s0 += N + G
    ref, got = np.concatenate(ref), np.concatenate(got)
    ref1, got1 = np.concatenate(ref1), np.concatenate(got1)
    print(f"receiver view (per-window DFT): symbols {snr(ref, got):.1f} dB, P1 A segment {snr(ref1, got1):.1f} dB")


if __name__ == "__main__":
    main()
