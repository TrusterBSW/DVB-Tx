#!/usr/bin/env python3
"""Software DVB-S / DVB-S2 receiver for testing the modulators, from a null-radio capture or live
from a USRP (B210), then a check of the transport stream.

  DVB-S : leandvb (leansdr), QPSK 1/2 .. 7/8
  DVB-S2: dvbs2-rx (gr-dvbs2rx), QPSK and 8PSK (16APSK / 32APSK are not supported by it)

File (the I/Q the null radio writes, format=cs16 or cf32):
  rx_sat.py dvbs2 --file /tmp/s2.cs16 --rate 30e6 --offset 2.5e6 --sr 20e6 --modcod 8psk9/10 -o rx.ts
  rx_sat.py dvbs  --file /tmp/s.cs16  --rate 30e6 --offset 0 --sr 20e6 --cr 7/8 -o rx.ts
Live (B210 receiving the transmitter through a coupler/attenuator):
  rx_sat.py dvbs2 --usrp serial=XXXX --freq 1.2e9 --rate 30e6 --sr 20e6 --modcod 8psk9/10 --gain 30 -t 20 -o rx.ts
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

LEANDVB = shutil.which("leandvb") or "/root/sat/leansdr/src/apps/leandvb"
DVBS2RX = shutil.which("dvbs2-rx") or "/usr/local/bin/dvbs2-rx"


def iq_chunks(path, fmt, skip_s, seconds, rate, chunk=1 << 20):
    """complex64 chunks of a capture"""
    item = 4 if fmt == "cs16" else 8
    start = int(skip_s * rate)
    total = os.path.getsize(path) // item - start
    if seconds:
        total = min(total, int(seconds * rate))
    with open(path, "rb") as f:
        f.seek(start * item)
        done = 0
        while done < total:
            n = min(chunk, total - done)
            if fmt == "cs16":
                a = np.fromfile(f, np.int16, 2 * n).astype(np.float32) / 32768.0
                x = (a[0::2] + 1j * a[1::2]).astype(np.complex64)
            else:
                x = np.fromfile(f, np.complex64, n)
            if not len(x):
                break
            yield done, x
            done += len(x)


def baseband_fc32(args, out):
    """shift the carrier to 0 Hz and write cf32 (dvbs2-rx reads fc32 centred on the carrier)"""
    with open(out, "wb") as o:
        for pos, x in iq_chunks(args.file, args.format, args.skip, args.seconds, args.rate):
            n = np.arange(pos, pos + len(x))
            (x * np.exp(-2j * np.pi * args.offset / args.rate * n)).astype(np.complex64).tofile(o)


def ts_check(path, expect_rate=None, seconds=None):
    data = np.fromfile(path, np.uint8)
    if len(data) < 188:
        print("no transport stream")
        return 1
    # align on the sync bytes
    for start in range(188):
        if np.all(data[start:start + 188 * 50:188] == 0x47):
            break
    else:
        print("no TS sync found")
        return 1
    n = (len(data) - start) // 188
    p = data[start:start + n * 188].reshape(n, 188)
    sync = int(np.sum(p[:, 0] != 0x47))
    tei = int(np.sum(p[:, 1] & 0x80 != 0))
    pid = ((p[:, 1].astype(int) & 0x1F) << 8) | p[:, 2]
    cc = p[:, 3] & 0x0F
    has_payload = (p[:, 3] & 0x10) != 0
    errs = 0
    for q in np.unique(pid):
        if q == 0x1FFF:
            continue
        m = (pid == q) & has_payload & (p[:, 1] & 0x80 == 0)
        c = cc[m].astype(int)
        if len(c) > 1:
            d = (c[1:] - c[:-1]) % 16
            errs += int(np.sum((d != 1) & (d != 0)))
    pids = sorted(int(q) for q in np.unique(pid) if q != 0x1FFF)
    print(f"{n} packets ({n * 188 * 8 / 1e6:.1f} Mbit)" +
          (f", {n * 188 * 8 / seconds / 1e6:.3f} Mbit/s" if seconds else "") +
          f", sync errors {sync}, TEI {tei}, CC errors {errs}, PIDs {', '.join(hex(q) for q in pids[:12])}")
    return 0 if (sync == 0 and tei == 0 and errs == 0) else 2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("standard", choices=("dvbs", "dvbs2"))
    ap.add_argument("--file", help="capture (null radio)")
    ap.add_argument("--format", choices=("cs16", "cf32"), help="capture format (default: from the extension)")
    ap.add_argument("--usrp", help="live: UHD device arguments, e.g. serial=XXXX")
    ap.add_argument("--freq", type=float, default=0, help="live: carrier frequency (Hz)")
    ap.add_argument("--gain", type=float, default=30, help="live: receive gain (dB)")
    ap.add_argument("--rate", type=float, required=True, help="sample rate (Hz)")
    ap.add_argument("--offset", type=float, default=0, help="file: carrier offset in the capture (Hz)")
    ap.add_argument("--sr", type=float, required=True, help="symbol rate (Bd)")
    ap.add_argument("--cr", default="3/4", help="DVB-S code rate")
    ap.add_argument("--modcod", default="qpsk3/4", help="DVB-S2 MODCOD (qpsk1/4 .. 8psk9/10)")
    ap.add_argument("--frame", default="normal", choices=("normal", "short"))
    ap.add_argument("--pilots", default="on", choices=("on", "off", "auto"))
    ap.add_argument("--rolloff", default="0.35", choices=("0.35", "0.25", "0.2"))
    ap.add_argument("--skip", type=float, default=1.0, help="file: seconds skipped at the start")
    ap.add_argument("-t", "--seconds", type=float, default=0, help="seconds to process (file: all)")
    ap.add_argument("-o", "--out", default="rx.ts")
    a = ap.parse_args()
    if not a.file and not a.usrp:
        ap.error("--file or --usrp")
    if a.file and not a.format:
        a.format = "cf32" if a.file.endswith(("cf32", "fc32")) else "cs16"
    seconds = a.seconds or (a.file and (os.path.getsize(a.file) // (4 if a.format == "cs16" else 8) / a.rate - a.skip))

    if a.standard == "dvbs":
        cmd = [LEANDVB, "--f32" if (a.usrp or a.format == "cf32") else "--s16", "-f", f"{a.rate:.0f}",
               "--sr", f"{a.sr:.0f}", "--cr", a.cr, "--roll-off", a.rolloff, "--tune", f"{a.offset:.0f}"]
        if a.usrp:
            # leandvb reads stdin: uhd_rx_cfile writes the samples to a pipe
            rx = subprocess.Popen(["uhd_rx_cfile", "-a", a.usrp, "-f", f"{a.freq:.0f}", "-r", f"{a.rate:.0f}",
                                   "-g", str(a.gain), "-N", str(int(a.seconds * a.rate)), "/dev/stdout"],
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            with open(a.out, "wb") as o:
                subprocess.run(cmd, stdin=rx.stdout, stdout=o, stderr=subprocess.DEVNULL)
            rx.wait()
        else:
            item = 4 if a.format == "cs16" else 8
            with open(a.file, "rb") as f, open(a.out, "wb") as o:
                f.seek(int(a.skip * a.rate) * item)
                subprocess.run(cmd, stdin=f, stdout=o, stderr=subprocess.DEVNULL)
    else:
        cmd = [DVBS2RX, "--samp-rate", f"{a.rate:.0f}", "-s", f"{a.sr:.0f}", "-m", a.modcod,
               "--frame-size", a.frame, "-p", a.pilots, "-r", a.rolloff, "--sink", "file", "--out-file", a.out]
        if a.usrp:
            cmd += ["--source", "usrp", "--usrp-args", a.usrp, "-f", f"{a.freq:.0f}", "--usrp-gain", str(a.gain)]
            p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                p.wait(timeout=a.seconds or 20)
            except subprocess.TimeoutExpired:
                p.terminate()
                p.wait()
        else:
            tmp = tempfile.NamedTemporaryFile(suffix=".fc32", delete=False, dir=os.path.dirname(a.out) or ".")
            tmp.close()
            try:
                baseband_fc32(a, tmp.name)
                subprocess.run(cmd + ["--source", "file", "--in-file", tmp.name],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            finally:
                os.unlink(tmp.name)
    return ts_check(a.out, seconds=seconds)


if __name__ == "__main__":
    sys.exit(main())
