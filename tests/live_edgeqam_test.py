"""dvbtx feeding an edgeQAM board (FPGA DVB-T modulator on the network), against tests/fake_edgeqam.py:
4 multiplexes consumed at the exact DVB-T 2k 64-QAM 7/8 1/32 rate on a clock 5 ppm fast.

1. Two multiplexes on channels 21 and 23: sent as CBR TS to the board's multiplexes 1 and 3; the board set to
   474 MHz with multiplexes 2 and 4 disabled (/set); its buffers filled to 100 ms (2105 packets) then tracked: no
   null inserted, none dropped; the rate trim near +5 ppm, the PCRs steady against the board's clock
   (the packet index on air) and within the 30 ppm of MPEG; the status and no alert.
   Its frequency changed by someone else: set again.
2. The board restarted (buffers, counters and settings lost): set again, its buffers filled again.

Ports 27001-27004, 27100-27999, 28181, 18191. The board's output of multiplex 1 is recorded (~45 s,
~170 MB) under work/ (on disk), removed at the end.

usage: python3 tests/live_edgeqam_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import live_channels_test as lc          # noqa: E402  (clips)

URL = "http://127.0.0.1:18191"
BOARD = "http://127.0.0.1:28181"
RATE = 31_668_449.2
PPM = 5.0
TARGET = round(0.100 * RATE / 1504)        # buffer_ms = 100 (the default for a board): 2105 packets
check, wait_for, FAILS = lc.check, lc.wait_for, lc.FAILS


def api(method, path, base=URL):
    req = urllib.request.Request(base + path, method=method, data=b"{}" if method == "POST" else None,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return json.load(e)


def get(f, default=None, base=URL, path="/api/status"):
    try:
        return f(api("GET", path, base))
    except (OSError, ValueError, KeyError, IndexError, TypeError):
        return default


def mods(st):
    r = next(x for x in st["radios"] if x["driver"] == "edgeqam")
    return (r.get("live") or {}).get("modulator") or {}


def board_mux(k):
    return get(lambda s: s["inputs"][k - 1], base=BOARD, path="/status")


def pcr_check(path):
    """(packets with a PCR, slope in ppm, max residual in us) of the PCRs against the packet index on air,
    from 8 s after the stream arrived (its buffer filled at +2 % meanwhile) to the end of the recording"""
    data = open(path, "rb").read()
    pts = []
    for i in range(0, len(data) - 187, 188):
        p = data[i:i + 188]
        if p[0] != 0x47 or not (p[3] & 0x20) or p[4] < 7 or not (p[5] & 0x10):
            continue
        b = p[6:12]
        base = (b[0] << 25) | (b[1] << 17) | (b[2] << 9) | (b[3] << 1) | (b[4] >> 7)
        ext = ((b[4] & 1) << 8) | b[5]
        pid = ((p[1] & 0x1F) << 8) | p[2]
        pts.append((pid, i // 188, (base * 300 + ext) / 27e6))
    if not pts:
        return 0, None, None
    pid = max({p[0] for p in pts}, key=lambda q: sum(1 for p in pts if p[0] == q))
    t_pkt = 1504 / (RATE * (1 + PPM * 1e-6))       # one packet on air, in board seconds
    first = min(n for q, n, _ in pts if q == pid) * t_pkt     # the stream arrives (the board ran on nulls before)
    xs = [(n * t_pkt, v) for q, n, v in pts if q == pid and n * t_pkt > first + 8]    # its buffer filled
    if len(xs) < 50:
        return len(xs), None, None
    # unwrap is not needed over a minute; a least-squares line
    mx = sum(x for x, _ in xs) / len(xs)
    my = sum(y for _, y in xs) / len(xs)
    sxx = sum((x - mx) ** 2 for x, _ in xs)
    slope = sum((x - mx) * (y - my) for x, y in xs) / sxx
    res = max(abs(y - my - slope * (x - mx)) for x, y in xs)
    return len(xs), (slope - 1) * 1e6, res * 1e6


def config(tmp, clips):
    return f"""# test
work_dir = "{tmp}/work"
[network]
name = "Test"
[web]
listen = "127.0.0.1:18191"
[ingest]
base_port = 27100
monitor_base_port = 27900
[monitoring]
tsduck = false
[engine]
binary = "{ROOT}/engine/build/dvbtx-engine"
[[radio]]
name = "fishball"
driver = "edgeqam"
address = "127.0.0.1"
ports = [27001, 27002, 27003, 27004]
api = 28181
[[carrier]]
name = "Mux A"
radio = "fishball"
channel = 21
  [[carrier.service]]
  name = "Alpha"
  source = "{clips[0]}"
[[carrier]]
name = "Mux C"
radio = "fishball"
channel = 23
  [[carrier.service]]
  name = "Charlie"
  source = "{clips[1]}"
"""


def main():
    tmp = tempfile.mkdtemp(prefix="dvbtx-edgeqam-")
    os.makedirs(os.path.join(ROOT, "work"), exist_ok=True)
    rec_dir = tempfile.mkdtemp(prefix="edgeqam-rec-", dir=os.path.join(ROOT, "work"))
    rec = os.path.join(rec_dir, "mux1.ts")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "ac"]
    for i, c in enumerate(clips):
        lc.make_clip(c, i * 70, 300 + 150 * i)
    cfgp = os.path.join(tmp, "dvbtx.toml")
    with open(cfgp, "w") as f:
        f.write(config(tmp, clips))
    board = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_edgeqam.py"), "--ppm", str(PPM),
                              "--port", "27001", "--api", "28181", "--record", rec, "--record-mux", "1",
                              "--record-s", "45"])
    time.sleep(1)
    sup = subprocess.Popen([sys.executable, "-m", "dvbtx", "run", "-c", cfgp], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = []
    threading.Thread(target=lambda: [log.append(l.rstrip()) for l in sup.stdout], daemon=True).start()
    try:
        check(wait_for(lambda: get(lambda s: all(m and m["state"] == "track" for m in mods(s).values())
                                   and len(mods(s)) == 2), 40), "multiplexes 1 and 3: buffers filled, tracked")
        ini = open(os.path.join(tmp, "work", "engine.ini")).read()
        check("output = udp://127.0.0.1:27001" in ini and "output = udp://127.0.0.1:27003" in ini
              and "fill_tune = /set?freq=474000000&disable=10" in ini, "engine.ini: ports 1 and 3, set 474 MHz, 2 and 4 off")
        b = get(lambda s: s, base=BOARD, path="/status") or {}
        check(b.get("freq_mux1_hz") == 474_000_000 and b.get("disable_mask") == 10,
              f"the board set: {b.get('freq_mux1_hz')} Hz, disable {b.get('disable_mask')}")
        st = api("GET", "/api/status")
        r = next((x for x in st["radios"] if x["driver"] == "edgeqam"), {})
        check(f"fill_target = {TARGET}" in ini, f"engine.ini: fill_target = {TARGET} (100 ms)")
        check(r.get("name") == "fishball" and (r.get("live") or {}).get("state") == "on"
              and set(mods(st)) == {"1", "3"}, f"status: board fishball, multiplexes 1 and 3 ({r.get('live')})")
        check(set(lc_on(st)) == {"primary"}, f"both channels on their source: {lc_on(st)}")
        m0 = {k: board_mux(k) for k in (1, 3)}
        time.sleep(25)
        m1 = {k: board_mux(k) for k in (1, 3)}
        for k in (1, 3):
            ins = m1[k]["null_inserted"] - m0[k]["null_inserted"]
            drop = m1[k]["dropped"] - m0[k]["dropped"]
            check(ins == 0 and drop == 0 and abs(m1[k]["queued"] - TARGET) < 1000,
                  f"multiplex {k}, 25 s tracked: {ins} nulls inserted, {drop} dropped, {m1[k]['queued']} queued")
        st = api("GET", "/api/status")
        tr = {k: m["trim_ppm"] for k, m in mods(st).items()}
        check(all(-30 < v < 30 for v in tr.values()), f"rate trims within 30 ppm: {tr}")
        check(not st["alerts"], f"no alert: {[a['message'] for a in st['alerts']]}")
        n, slope, res = pcr_check(rec) if wait_for(lambda: os.path.getsize(rec) >= int(45 * RATE / 8 * 0.98), 20) \
            else (0, None, None)
        check(slope is not None and abs(slope) < 30 and res < 500,
              f"PCRs of multiplex 1 against the board's clock: {n} PCRs, rate {slope if slope is None else round(slope, 2)} ppm, "
              f"largest deviation {res if res is None else round(res, 1)} us")

        # someone else changes its frequency: dvbtx sets it again
        api("GET", "/set?freq=498000000", BOARD)
        check(wait_for(lambda: get(lambda s: s["freq_mux1_hz"], base=BOARD, path="/status") == 474_000_000, 5),
              "its frequency changed by someone else: set again")

        # 2. the board restarted: settings lost, buffers empty
        sets0 = get(lambda s: s["sets"], base=BOARD, path="/status")
        api("POST", "/restart", BOARD)
        check(wait_for(lambda: get(lambda s: s["sets"], base=BOARD, path="/status", default=0) > sets0
                       and get(lambda s: s["freq_mux1_hz"], base=BOARD, path="/status") == 474_000_000, 10),
              "after its restart, the board set again")
        check(wait_for(lambda: get(lambda s: all(m and m["state"] == "track" and abs(m["queued"] - TARGET) < 1000
                                                 for m in mods(s).values()))
                       and all(abs(board_mux(k)["queued"] - TARGET) < 1000 for k in (1, 3)), 30),
              f"filled and tracked again: {[board_mux(k)['queued'] for k in (1, 3)]} queued")
        time.sleep(3)
        check(get(lambda s: s["engine"]["restarts"]) == 0, "no engine restart")
    finally:
        sup.terminate()
        try:
            sup.wait(20)
        except subprocess.TimeoutExpired:
            sup.kill()
        board.terminate()
        shutil.rmtree(rec_dir, ignore_errors=True)
    if FAILS:
        print(f"FAILED: {len(FAILS)} check(s)")
        for l in log[-40:]:
            print("   ", l)
        sys.exit(1)
    print("all checks passed")


def lc_on(st):
    return [(s.get("live") or {}).get("source") for c in st["carriers"] for s in c["services"]]


if __name__ == "__main__":
    main()
