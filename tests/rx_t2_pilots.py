#!/usr/bin/env python3
"""DVB-T2 synthesis check on a null-radio capture, without a T2 decoder: symbol timing from the
guard-interval correlation, FFT of each symbol at the radio rate, then the continual pilots
(BPSK, same carriers in every symbol: found as the bins of constant amplitude) give a MER:
error across the BPSK axis after a phase per carrier and per symbol. No channel filter: a single
carrier next to the T2 one leaks into the rectangular FFT window (a real receiver filters it out).

  rx_t2_pilots.py capture.cf32|.cs16 rate offset_hz [fft=32768] [gi=1/128] [bw=8] [seconds=1]
"""
import sys
from fractions import Fraction

import numpy as np


def load(path, n):
    if path.endswith("cs16"):
        a = np.fromfile(path, np.int16, 2 * n).astype(np.float32)
        return a[0::2] + 1j * a[1::2]
    return np.fromfile(path, np.complex64, n)


def main():
    path, rate, off = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    kw = dict(a.split("=") for a in sys.argv[4:])
    fft = int(kw.get("fft", 32768))
    gi = Fraction(kw.get("gi", "1/128"))
    secs = float(kw.get("seconds", 1))
    native = float(kw.get("bw", 8)) * 8e6 / 7          # elementary rate (5..10 MHz channels)
    k = round(rate / native)
    assert abs(k * native - rate) < 1e-3, f"rate is not k x {native:.3f}"
    N, G = k * fft, int(k * fft * gi)
    S = N + G
    skip = int(0.5 * rate)
    x = load(path, skip + int(secs * rate))[skip:].astype(np.complex128)
    x *= np.exp(-2j * np.pi * off / rate * np.arange(len(x)))
    # guard-interval correlation: one peak per symbol; a P1 (2048 k samples) between frames moves the
    # next peak, so each one is searched within half a symbol of where it is expected
    m = np.abs(np.convolve(x[:-N] * np.conj(x[N:]), np.ones(G), "valid"))
    starts = []
    pos = int(np.argmax(m[:S]))
    while pos + S < len(m) and len(starts) < 400:
        lo = max(pos - S // 2, 0)
        pos = lo + int(np.argmax(m[lo:pos + S // 2]))
        if m[pos] > 0.5 * np.median(m):
            starts.append(pos)
        pos += S
    # the peak is noisy (+-20 samples): within a frame the symbols are exactly S apart, so one origin is
    # fitted per run of symbols (between two P1)
    st = np.array(starts)
    runs = np.split(np.arange(len(st)), np.where(np.abs(np.diff(st) - S) > G // 2)[0] + 1)
    fitted, run = [], []
    for j, r in enumerate(runs):
        if len(r) < 8:
            continue
        i = np.arange(len(r))
        s0 = int(round(np.median(st[r] - i * S)))
        ok = [s for s in s0 + i[1:] * S if s >= 0 and s + S <= len(x)]   # without the first: P2 after a P1
        fitted += ok
        run += [j] * len(ok)
    starts, run = fitted, np.array(run)
    # FFT of each symbol, window started in the middle of the guard interval (a timing error of the fit
    # then only turns the carriers, no interference from the next symbol), carriers around 0 Hz
    K = fft // 2
    Y = np.array([np.fft.fftshift(np.fft.fft(x[s + G // 2:s + G // 2 + N]))[N // 2 - K:N // 2 + K] for s in starts])
    # continual pilots: the bins of constant amplitude (BPSK; data cells vary), selected on the amplitude
    # since the phase may turn from symbol to symbol (DVB-T: one IFFT per symbol, no continuous oscillator)
    A = np.abs(Y)
    pilots = np.where(A.std(0) < 0.02 * A.mean(0))[0]
    Z = Y[:, pilots]
    ph = np.angle((Z ** 2).sum(1)) / 2                               # per symbol (mod pi)
    Z = Z * np.exp(-1j * ph)[:, None]
    for j in np.unique(run):                  # per carrier and frame (each frame has its timing error)
        Z[run == j] *= np.exp(-1j * np.angle((Z[run == j] ** 2).mean(0)) / 2)[None, :]
    ph = np.angle((Z ** 2).sum(1)) / 2
    Z = Z * np.exp(-1j * ph)[:, None]
    # BPSK pilots of several amplitudes (P2, continual, continual on a scattered position): the error across
    # the BPSK axis, doubled (circular noise), against the pilot power
    err = 2 * np.mean(Z.imag ** 2) / np.mean(Z.real ** 2)
    print(f"k={k}, {len(starts)} symbols, {len(pilots)} continual-pilot bins, "
          f"pilot MER {-10 * np.log10(err):.1f} dB")


if __name__ == "__main__":
    main()
