"""Whole transmitter test: channels changed while `dvbtx run` is running, with `dvbtx channel`.

A "ts" multiplex (no radio) goes to UDP; looping clips made here are the sources (stream copy).
Checks on the multiplex received: PAT and SDT (names) after each change, the other channels not
disturbed, the configuration file rewritten; then a hand edit of the file applied by SIGHUP, and
`dvbtx channel` editing the file when dvbtx is not running. Ports 24100-24999 and 18188.

usage: python3 tests/live_supervisor_test.py [t2]
  t2: a DVB-T2 multiplex (32K 256-QAM 5/6) on the null radio instead of a "ts" one, received from
      its monitor copy (the multiplex pulled by the T2 modulator in real time)
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import tomllib
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import live_channels_test as lc          # noqa: E402  (clips, output receiver)

URL = "http://127.0.0.1:18188"
OUT = 24999
lc.OUT = OUT


class Output(lc.Output):
    """+ SDT: service names"""

    def __init__(self):
        super().__init__()
        self.sdt = {}
        self._sdt_buf = b""

    def packet(self, p, now):
        super().packet(p, now)
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid != 0x11:
            return
        pl = p[4:] if not p[3] & 0x20 else p[5 + p[4]:]
        if p[1] & 0x40:
            self._sdt_buf = pl[1 + pl[0]:]
        else:
            self._sdt_buf += pl
        b = self._sdt_buf
        if len(b) < 3 or len(b) < 3 + (((b[1] & 0x0F) << 8) | b[2]):
            return
        n = 3 + (((b[1] & 0x0F) << 8) | b[2])
        sec, self._sdt_buf = b[:n], b""
        if sec[0] != 0x42:
            return
        names, i = {}, 11
        while i + 5 <= n - 4:
            sid = (sec[i] << 8) | sec[i + 1]
            dl = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4]
            j = i + 5
            while j < i + 5 + dl:
                tag, ln = sec[j], sec[j + 1]
                if tag == 0x48:
                    pl_ = sec[j + 3]
                    nl = sec[j + 4 + pl_]
                    raw = sec[j + 5 + pl_:j + 5 + pl_ + nl]
                    if raw and raw[0] < 0x20:
                        raw = raw[1:]
                    names[sid] = raw.decode("iso8859-9", "replace")
                j += 2 + ln
            i += 5 + dl
        self.sdt = names


def cli(*args, check=True):
    r = subprocess.run([sys.executable, "-m", "dvbtx", "channel", *args], cwd=ROOT, capture_output=True, text=True)
    if check and r.returncode:
        print(r.stdout, r.stderr)
    return r


def status():
    with urllib.request.urlopen(URL + "/api/status", timeout=5) as r:
        return json.load(r)


def services():
    try:
        st = status()
    except OSError:
        return {}
    return {s["sid"]: s for c in st["carriers"] for s in c["services"]}


def on(sid):
    return ((services().get(sid) or {}).get("live") or {}).get("source")


check, wait_for, FAILS = lc.check, lc.wait_for, lc.FAILS


def main():
    tmp = tempfile.mkdtemp(prefix="dvbtx-sup-")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "abcde"]
    for i, c in enumerate(clips):
        lc.make_clip(c, i * 70, 300 + 150 * i)
    cfgp = os.path.join(tmp, "dvbtx.toml")
    if "t2" in sys.argv[1:]:
        CARRIER = f"""[[radio]]
name = "null"
driver = "null"
[[carrier]]
name = "Mux T2"
radio = "null"
standard = "dvbt2"
frequency = 482_000_000
monitor = "udp://127.0.0.1:{OUT}"
"""
    else:
        CARRIER = f"""[[carrier]]
name = "Mux TS"
standard = "ts"
ts_rate = 12_000_000
output = "udp://127.0.0.1:{OUT}"
"""
    with open(cfgp, "w") as f:
        f.write(f"""# test
work_dir = "{tmp}/work"
[network]
name = "Test"
[web]
listen = "127.0.0.1:18188"
[ingest]
base_port = 24100
monitor_base_port = 24900
[monitoring]
tsduck = false
[engine]
binary = "{ROOT}/engine/build/dvbtx-engine"
[defaults]
backup = "{clips[4]}"
{CARRIER}
  # the first one
  [[carrier.service]]
  name = "Alpha"
  source = "{clips[0]}"

  [[carrier.service]]
  name = "Bravo"
  source = "{clips[1]}"
""")
    out = Output()
    out.start()
    sup = subprocess.Popen([sys.executable, "-m", "dvbtx", "run", "-c", cfgp], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = []
    import threading
    threading.Thread(target=lambda: [log.append(l.rstrip()) for l in sup.stdout], daemon=True).start()
    try:
        check(wait_for(lambda: on(0x101) == "primary" and on(0x102) == "primary", 30), "both channels on air")
        check(wait_for(lambda: out.sdt == {0x101: "Alpha", 0x102: "Bravo"}, 5), f"SDT {out.sdt}")
        r = cli("list", "-c", cfgp)
        check(r.returncode == 0 and "Alpha" in r.stdout and "primary" in r.stdout, "channel list (live)")

        # add
        out.reset()
        r = cli("-c", cfgp, "add", "Charlie", clips[2], "--number", "7")
        check(r.returncode == 0 and "saved" in r.stdout, f"add: {r.stdout.strip()} {r.stderr.strip()}")
        check(wait_for(lambda: 0x103 in out.pat and out.sdt.get(0x103) == "Charlie", 8), "new channel in PAT and SDT")
        check(wait_for(lambda: on(0x103) == "primary", 15), "new channel on its own source")
        time.sleep(1)
        with out.lock:
            g = out.gap.get(0x101 + 0, 9), out.gap.get(257, 9)
        check(g[1] < 0.3, f"Alpha undisturbed (video gap {g[1] * 1000:.0f} ms)")
        svc = tomllib.load(open(cfgp, "rb"))["carrier"][0]["service"]
        check([s["name"] for s in svc] == ["Alpha", "Bravo", "Charlie"] and svc[2]["lcn"] == 7, "file: Charlie added")
        check("# the first one" in open(cfgp).read(), "file: comments kept")

        # change of source, no cut
        out.reset()
        r = cli("-c", cfgp, "source", "Bravo", clips[3])
        check(r.returncode == 0, f"source: {r.stdout.strip()} {r.stderr.strip()}")
        bravo_video = 0x120 + 1
        check(wait_for(lambda: (services().get(0x102) or {}).get("source") == clips[3], 5), "status shows the new source")
        time.sleep(6)
        with out.lock:
            g = out.gap.get(bravo_video, 9)
        check(g < 0.2, f"Bravo: no cut while its source changed (video gap {g * 1000:.0f} ms)")
        check(on(0x102) == "primary", "Bravo still on primary")
        ins = [i["source"] for i in status()["inputs"]]
        check(clips[1] not in ins, "old source of Bravo stopped")

        # rename, renumber
        r = cli("-c", cfgp, "rename", "7", "Charlie HD")
        check(r.returncode == 0 and wait_for(lambda: out.sdt.get(0x103) == "Charlie HD", 5), "renamed (SDT)")
        r = cli("-c", cfgp, "renumber", "Charlie HD", "1")
        check(r.returncode != 0 and "swap" in r.stderr, "renumber to a taken number refused")
        r = cli("-c", cfgp, "renumber", "Charlie HD", "1", "--swap")
        check(r.returncode == 0, "renumber with swap")
        sv = services()
        check(sv[0x103]["lcn"] == 1 and sv[0x101]["lcn"] == 7, "numbers swapped")

        # remove
        out.reset()
        r = cli("-c", cfgp, "remove", "0x0102")
        check(r.returncode == 0 and wait_for(lambda: 0x102 not in out.pat and 0x102 not in out.sdt, 5),
              "removed: out of PAT and SDT")
        time.sleep(1)
        with out.lock:
            g = out.gap.get(257, 9)
        check(g < 0.3, f"Alpha undisturbed by the removal (video gap {g * 1000:.0f} ms)")
        svc = tomllib.load(open(cfgp, "rb"))["carrier"][0]["service"]
        check([(s["name"], s.get("sid")) for s in svc] == [("Alpha", 0x101), ("Charlie HD", 0x103)],
              "file: Bravo removed, the others keep their service_id")

        # the engine dies after these changes: restarted from the plan (input ids with gaps)
        eng = subprocess.run(["pgrep", "-P", str(sup.pid), "-x", "dvbtx-engine"], capture_output=True, text=True).stdout.split()
        check(len(eng) == 1, f"engine process found ({eng})")
        if eng:
            os.kill(int(eng[0]), signal.SIGKILL)
            time.sleep(2)
            check(wait_for(lambda: on(0x101) == "primary" and on(0x103) == "primary", 30),
                  "after an engine crash, the changed channels are back")

        # API safety
        req = urllib.request.Request(URL + "/api/channels", data=b'{"name":"X","source":"exec:true"}', method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            urllib.request.urlopen(req, timeout=10)
            check(False, "exec: source refused through the API")
        except urllib.error.HTTPError as e:
            check(e.code == 400, "exec: source refused through the API")
        req = urllib.request.Request(URL + "/api/channels/1", method="DELETE", headers={"Origin": "http://evil.example"})
        try:
            urllib.request.urlopen(req, timeout=10)
            check(False, "request from another site refused")
        except urllib.error.HTTPError as e:
            check(e.code == 403 and 0x103 in services(), "request from another site refused")

        # hand edit + SIGHUP
        text = open(cfgp).read().replace('name = "Alpha"', 'name = "Alpha 2"')
        open(cfgp, "w").write(text)
        sup.send_signal(signal.SIGHUP)
        check(wait_for(lambda: out.sdt.get(0x101) == "Alpha 2", 8), "hand edit applied by SIGHUP")
        check(on(0x101) == "primary", "Alpha still on air after the reload")
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            sup.wait(15)
        except subprocess.TimeoutExpired:
            sup.kill()
        out.stop = True

    # dvbtx not running: the file alone
    r = cli("-c", cfgp, "add", "Delta", clips[0])
    check(r.returncode == 0 and "not running" in r.stdout, f"offline add: {r.stdout.strip()} {r.stderr.strip()}")
    svc = tomllib.load(open(cfgp, "rb"))["carrier"][0]["service"]
    check(svc[-1]["name"] == "Delta", "offline: file updated")
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
