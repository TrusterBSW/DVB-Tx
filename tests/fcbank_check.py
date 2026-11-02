#!/usr/bin/env python3
"""Accuracy of fcbank / scgen: ideal synthesis of the logged symbols (whole-record FFT, exact RRC,
zero padding to the radio rate) compared with each output.  usage: fcbank_check.py DIR [fs rs]"""
import sys
import numpy as np

d = sys.argv[1]
fs = float(sys.argv[2]) if len(sys.argv) > 2 else 20e6
rs = float(sys.argv[3]) if len(sys.argv) > 3 else 6.875e6
alpha, offs, gain = 0.15, (-4e6, 4e6), 0.5


def ideal(sym, n, off):
    L = len(sym)
    N = int(round(L * fs / rs))
    X = np.fft.fft(sym)
    f = np.fft.fftfreq(N, 1 / fs)          # output bins, f = k fs / N = k rs / L
    k = np.round(f / (rs / L)).astype(int)
    x = np.abs(f) / rs
    lo, hi = (1 - alpha) / 2, (1 + alpha) / 2
    H = np.where(x <= lo, 1.0, np.where(x >= hi, 0.0, np.cos(np.pi / (2 * alpha) * (x - lo))))
    Y = X[k % L] * H
    Y = np.roll(Y, 0)
    y = np.fft.ifft(Y) * N
    y *= gain / np.sqrt(np.mean(np.abs(y) ** 2) / np.mean(np.abs(sym) ** 2))
    return y[:n] * np.exp(2j * np.pi * off / fs * np.arange(n))


nominal = offs
for tag in ("fc", "sc"):
    offs = tuple(np.loadtxt(f"{d}/offsets.txt")) if tag == "fc" else nominal
    out = np.fromfile(f"{d}/out_{tag}.cf32", np.complex64).astype(complex)
    n = len(out)
    L = int(n * rs / fs) // 2 * 2
    refs = [ideal(np.fromfile(f"{d}/sym{i}_{tag}.cf32", np.complex64).astype(complex)[:L], n, off)
            for i, off in enumerate(offs)]
    a, b = n // 8, n - n // 8
    F = np.fft.fftfreq(n)
    c = np.fft.ifft(np.fft.fft(out) * np.conj(np.fft.fft(refs[0] * np.exp(-2j * np.pi * offs[0] / fs * np.arange(n)) *
                                                          np.exp(2j * np.pi * offs[0] / fs * np.arange(n)))))
    # delay from the first carrier alone (shift it to DC on both sides)
    sh = np.exp(-2j * np.pi * offs[0] / fs * np.arange(n))
    c = np.fft.ifft(np.fft.fft(out * sh) * np.conj(np.fft.fft(refs[0] * sh)))
    dly = int(np.argmax(np.abs(c)))
    if dly > n // 2:
        dly -= n

    def delayed(x, fd):
        # delay the baseband pulse (not the carrier): shift to DC, delay, shift back
        out_ = []
        for r, off in zip(x, offs):
            s_ = np.exp(-2j * np.pi * off / fs * np.arange(n))
            rr = np.fft.ifft(np.fft.fft(r * s_) * np.exp(-2j * np.pi * F * (dly + fd))) / s_
            out_.append(rr[a:b])
        return out_

    def err(fd):
        R = np.stack(delayed(refs, fd), 1)
        g, *_ = np.linalg.lstsq(R, out[a:b], rcond=None)
        e = out[a:b] - R @ g
        return np.sum(np.abs(e) ** 2) / np.sum(np.abs(R @ g) ** 2), g

    fd = 0.0
    for step in (0.25, 0.05, 0.01, 0.002, 0.0004):
        cand = fd + np.linspace(-4 * step, 4 * step, 9)
        v = [err(x)[0] for x in cand]
        fd = cand[int(np.argmin(v))]
    e, g = err(fd)
    print(f"{tag}: delay {dly}{fd:+.4f} samples, gains {np.abs(g).round(4)}, "
          f"offsets {[round(o) for o in offs]} Hz, error {10 * np.log10(e):.1f} dB (MER-equivalent of the synthesis)")
