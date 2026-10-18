"""An HTTP source whose ffmpeg keeps seeing timestamp discontinuities is started again beside itself
and swapped in without a cut (seen on air: an IPTV stream, after a reconnection, gave one every
0.3 s for an hour, each one making the picture jump).

A local HTTP server streams a looping clip; after STORM_AT seconds the audio PTS of the stream
are moved back 0.17 s every other 0.33 s (ffmpeg then reports "timestamp discontinuity" again and
again). The stream of a new connection is clean. Checks: the source started again, the channel on
air without a cut, the new ffmpeg clean, the other channel undisturbed. Ports 24500-24599, 18190.

usage: python3 tests/live_discont_test.py
"""
import http.server
import json
import os
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import live_channels_test as lc          # noqa: E402  (clips, output receiver)

URL = "http://127.0.0.1:18190"
OUT = 24599
HTTP = 24550
lc.OUT = OUT
STORM_AT = 40
check, wait_for, FAILS = lc.check, lc.wait_for, lc.FAILS
CLIP = None
CONNS = []
T0 = time.monotonic()


def shift_pts(pkt, delta):
    """packet with the PTS (and DTS) of its PES header moved by delta (90 kHz)"""
    if not (pkt[1] & 0x40):
        return pkt
    off = 4 + (1 + pkt[4] if pkt[3] & 0x20 else 0)
    if pkt[off:off + 3] != b"\x00\x00\x01" or not pkt[off + 7] & 0x80:
        return pkt
    b = bytearray(pkt)
    for i in ((off + 9,) if not pkt[off + 7] & 0x40 else (off + 9, off + 14)):
        v = ((b[i] >> 1) & 7) << 30 | b[i + 1] << 22 | (b[i + 2] >> 1) << 15 | b[i + 3] << 7 | b[i + 4] >> 1
        v = (v + delta) % (1 << 33)
        b[i] = (b[i] & 0xF1) | ((v >> 29) & 0x0E)
        b[i + 1] = (v >> 22) & 0xFF
        b[i + 2] = ((v >> 14) & 0xFE) | 1
        b[i + 3] = (v >> 7) & 0xFF
        b[i + 4] = ((v << 1) & 0xFE) | 1
    return bytes(b)


class Live(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "video/mp2t")
        self.end_headers()
        old = time.monotonic() - T0 < STORM_AT          # connections opened after the storm began are clean
        CONNS.append(time.monotonic())
        ff = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-re", "-stream_loop", "-1",
                               "-i", CLIP, "-c", "copy", "-f", "mpegts", "pipe:1"], stdout=subprocess.PIPE)
        t0, buf = time.monotonic(), b""
        try:
            while True:
                buf += ff.stdout.read(188 * 64)
                out = []
                while len(buf) >= 188:
                    p, buf = buf[:188], buf[188:]
                    pid = ((p[1] & 0x1F) << 8) | p[2]
                    t = time.monotonic() - t0
                    if old and time.monotonic() - T0 > STORM_AT and pid == 0x101 and int(t / 0.33) % 2:
                        p = shift_pts(p, -15300)                  # audio 0.17 s back, every other 0.33 s
                    out.append(p)
                self.wfile.write(b"".join(out))
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            ff.kill()


def svc(sid):
    try:
        with urllib.request.urlopen(URL + "/api/status", timeout=5) as r:
            st = json.load(r)
    except OSError:
        return {}, []
    return next((s for c in st["carriers"] for s in c["services"] if s["sid"] == sid), {}), st["inputs"]


def main():
    global CLIP
    tmp = tempfile.mkdtemp(prefix="dvbtx-disc-")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "abc"]
    for i, c in enumerate(clips):
        lc.make_clip(c, i * 70, 300 + 150 * i)
    CLIP = clips[0]
    global T0
    T0 = time.monotonic()
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", HTTP), Live)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    cfgp = os.path.join(tmp, "dvbtx.toml")
    with open(cfgp, "w") as f:
        f.write(f"""work_dir = "{tmp}/work"
[network]
name = "Test"
[web]
listen = "127.0.0.1:18190"
[ingest]
base_port = 24500
monitor_base_port = 24580
[monitoring]
tsduck = false
[engine]
binary = "{ROOT}/engine/build/dvbtx-engine"
[defaults]
backup = "{clips[2]}"
[[carrier]]
name = "Mux TS"
standard = "ts"
ts_rate = 12_000_000
output = "udp://127.0.0.1:{OUT}"
  [[carrier.service]]
  name = "Http"
  source = "http://127.0.0.1:{HTTP}/live.ts"
  [[carrier.service]]
  name = "Other"
  source = "{clips[1]}"
""")
    out = lc.Output()
    out.start()
    sup = subprocess.Popen([sys.executable, "-m", "dvbtx", "run", "-c", cfgp], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = []
    threading.Thread(target=lambda: [log.append(l.rstrip()) for l in sup.stdout], daemon=True).start()
    try:
        check(wait_for(lambda: (svc(0x101)[0].get("live") or {}).get("source") == "primary", 30), "channel on air")
        first = svc(0x101)[0]["input"]["index"]
        time.sleep(max(0, STORM_AT - (time.monotonic() - T0) - 2))
        out.reset()
        check(wait_for(lambda: any("timestamp discontinuities" in l for l in log), 60), "storm of discontinuities seen")
        check(wait_for(lambda: (svc(0x101)[0].get("input") or {}).get("index", first) != first, 15),
              "its source started again (a new input)")
        time.sleep(8)
        s, _ = svc(0x101)
        check((s.get("live") or {}).get("source") == "primary", "channel still on its source")
        with out.lock:
            g, go = out.gap.get(0x101, 9), out.gap.get(0x121, 9)
        check(g < 0.3, f"swapped without a cut (video gap {g * 1000:.0f} ms)")
        check(go < 0.3, f"the other channel undisturbed (video gap {go * 1000:.0f} ms)")
        time.sleep(5)
        new = next((i for i in svc(0x101)[1] if i["index"] == (s.get("input") or {}).get("index")), {})
        check(not any("timestamp discontinuity" in l for l in new.get("log", [])), "the new ffmpeg is clean")
        n0 = sum(1 for t in CONNS if t - T0 < STORM_AT)
        check(len(CONNS) - n0 >= 1, f"a new connection ({len(CONNS) - n0} after the storm began)")
    finally:
        sup.terminate()
        try:
            sup.wait(15)
        except subprocess.TimeoutExpired:
            sup.kill()
        out.stop = True
        srv.shutdown()
    errs = [l for l in log if " E " in l or "Traceback" in l]
    if errs:
        print("dvbtx errors:\n  " + "\n  ".join(errs[-15:]))
    if FAILS:
        print("\n".join(log[-40:]))
    shutil.rmtree(tmp, ignore_errors=True)
    print("all checks passed" if not FAILS else f"FAILED: {len(FAILS)}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
