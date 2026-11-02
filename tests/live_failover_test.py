"""A channel with several sources in order of preference, on the whole transmitter (`dvbtx run`).

Its two sources are UDP streams sent from here (started and stopped to make them fail), its backup
a clip; a second channel of the same multiplex must not be disturbed. Checks: the first source
used; it fails -> the second (after failover_s), the backup meanwhile; the first comes back -> the
channel returns to it without a cut (tried beside the second every preferred_retry_s); both fail ->
backup, then the first one that comes back is used; `dvbtx channel` and the API give the list.
Ports 24600-24999 and 18189.

usage: python3 tests/live_failover_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tomllib
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import live_channels_test as lc          # noqa: E402  (clips, output receiver)

URL = "http://127.0.0.1:18189"
OUT = 24998
lc.OUT = OUT
S1, S2, S3, S4 = 24601, 24602, 24603, 24604
check, wait_for, FAILS = lc.check, lc.wait_for, lc.FAILS


def svc(sid):
    try:
        with urllib.request.urlopen(URL + "/api/status", timeout=5) as r:
            st = json.load(r)
    except OSError:
        return {}
    return next((s for c in st["carriers"] for s in c["services"] if s["sid"] == sid), {})


def on(sid):
    return (svc(sid).get("live") or {}).get("source")


def main():
    tmp = tempfile.mkdtemp(prefix="dvbtx-fo-")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "abcd"]
    for i, c in enumerate(clips):
        lc.make_clip(c, i * 70, 300 + 150 * i)
    cfgp = os.path.join(tmp, "dvbtx.toml")
    with open(cfgp, "w") as f:
        f.write(f"""work_dir = "{tmp}/work"
[network]
name = "Test"
[web]
listen = "127.0.0.1:18189"
[ingest]
base_port = 24700
monitor_base_port = 24900
[monitoring]
tsduck = false
[engine]
binary = "{ROOT}/engine/build/dvbtx-engine"
[defaults]
backup = "{clips[3]}"
[[carrier]]
name = "Mux TS"
standard = "ts"
ts_rate = 12_000_000
output = "udp://127.0.0.1:{OUT}"
  [[carrier.service]]
  name = "Multi"
  source = ["udp://127.0.0.1:{S1}", "udp://127.0.0.1:{S2}"]
  failover_s = 3
  source_start_s = 15
  preferred_retry_s = 6
  return_ms = 2000

  [[carrier.service]]
  name = "Flappy"
  source = ["udp://127.0.0.1:{S3}", "udp://127.0.0.1:{S4}"]
  switch_timeout_ms = 1500
  return_ms = 2000
  failover_s = 4
  source_start_s = 15
  preferred_retry_s = 600
  unstable_window_s = 120

  [[carrier.service]]
  name = "Other"
  source = "{clips[2]}"
""")
    send = {S1: lc.source(clips[0], S1), S2: lc.source(clips[1], S2), S4: lc.source(clips[1], S4)}
    flap = {"on": True, "stop": False}

    def flapper():                 # S3: 5 s on, 2.5 s off (less than failover_s: never "down" by itself)
        while not flap["stop"]:
            p = lc.source(clips[0], S3)
            time.sleep(5 if flap["on"] else 3600)
            p.kill()
            time.sleep(2.5)
    threading.Thread(target=flapper, daemon=True).start()
    out = lc.Output()
    out.start()
    sup = subprocess.Popen([sys.executable, "-m", "dvbtx", "run", "-c", cfgp], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = []
    threading.Thread(target=lambda: [log.append(l.rstrip()) for l in sup.stdout], daemon=True).start()
    other_video = 0x140 + 1
    try:
        check(wait_for(lambda: on(0x101) == "primary" and on(0x102) == "primary", 30), "both channels on air")
        s = svc(0x101)
        check(s.get("sources") == [f"udp://127.0.0.1:{S1}", f"udp://127.0.0.1:{S2}"] and s.get("active") == 0,
              f"the first source is used ({s.get('active')})")
        r = subprocess.run([sys.executable, "-m", "dvbtx", "channel", "list", "-c", cfgp], cwd=ROOT,
                           capture_output=True, text=True)
        check(f"*1 udp://127.0.0.1:{S1}" in r.stdout and f" 2 udp://127.0.0.1:{S2}" in r.stdout,
              "channel list shows both sources, the one in use marked")

        # an unstable source (back and forth to the backup) is left for the next one
        check(wait_for(lambda: svc(0x102).get("active") == 1, 90), "unstable source left for the next one")
        check(any("unstable" in l for l in log), "logged as unstable")
        check(wait_for(lambda: on(0x102) == "primary", 15), "the unstable channel back on air from its second source")
        time.sleep(15)
        check(svc(0x102).get("active") == 1, "the unstable source is not taken back at once")

        # the first source fails: the second one
        out.reset()
        send[S1].kill()
        check(wait_for(lambda: svc(0x101).get("active") == 1, 15), "first source down: the second one is used")
        check(wait_for(lambda: on(0x101) == "primary", 15), "back on air from the second source")
        with out.lock:
            g = out.gap.get(other_video, 9)
        check(g < 0.3, f"the other channel undisturbed (video gap {g * 1000:.0f} ms)")
        check(wait_for(lambda: any("source 2 of 2" in a["message"] for a in
                                   json.load(urllib.request.urlopen(URL + "/api/status", timeout=5))["alerts"]), 10),
              "alert: not on the preferred source")

        # the first comes back: tried beside the second, then used without a cut
        send[S1] = lc.source(clips[0], S1)
        time.sleep(1)
        out.reset()
        check(wait_for(lambda: svc(0x101).get("active") == 0, 40), "first source back: used again")
        time.sleep(3)
        with out.lock:
            g = out.gap.get(0x101, 9)
        check(g < 0.3 and on(0x101) == "primary", f"return to the first source without a cut (video gap {g * 1000:.0f} ms)")

        # both fail: backup; the second comes back first: used
        send[S1].kill()
        send[S2].kill()
        check(wait_for(lambda: on(0x101) == "backup", 10), "both sources down: backup")
        time.sleep(10)
        check(on(0x101) == "backup", "still on backup while no source works")
        send[S2] = lc.source(clips[1], S2)
        check(wait_for(lambda: svc(0x101).get("active") == 1 and on(0x101) == "primary", 40),
              "the source that comes back is found and used")

        # the list changed through the API: the first one of the new list
        req = urllib.request.Request(URL + "/api/channels/Multi", method="PATCH",
                                     data=json.dumps({"source": [clips[0], f"udp://127.0.0.1:{S2}"]}).encode(),
                                     headers={"Content-Type": "application/json"})
        res = json.load(urllib.request.urlopen(req, timeout=60))
        check(res.get("ok") and res.get("saved"), f"list changed through the API: {res.get('changes')}")
        check(wait_for(lambda: svc(0x101).get("source") == clips[0] and on(0x101) == "primary", 20),
              "the first source of the new list plays")
        raw = tomllib.load(open(cfgp, "rb"))["carrier"][0]["service"][0]
        check(raw["source"] == [clips[0], f"udp://127.0.0.1:{S2}"], "file: the list written")
    finally:
        flap["stop"] = True
        sup.terminate()
        try:
            sup.wait(15)
        except subprocess.TimeoutExpired:
            sup.kill()
        for p in send.values():
            p.kill()
        out.stop = True
    errs = [l for l in log if " E " in l or "Traceback" in l]
    if errs:
        print("dvbtx errors:\n  " + "\n  ".join(errs[-15:]))
    if FAILS:
        print("\n".join(log[-50:]))
    shutil.rmtree(tmp, ignore_errors=True)
    print("all checks passed" if not FAILS else f"FAILED: {len(FAILS)}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
