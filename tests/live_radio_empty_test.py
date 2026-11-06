"""Applying a configuration with a radio left without multiplex, then one the engine refuses.

1. Two null radios, one multiplex each; the first multiplex moved to the second radio and applied: the first
   radio is left out of engine.ini, the engine (radios numbered 0..n-1) runs, both channels on air, and the
   status shows the second radio with its live state (its engine number translated back).
   It used to keep its number ([radio 1] alone): the engine refused it and restarted in a loop.
   Its serial (serial = "NUL2") is in the status and /api/devices lists the device with the radio holding it.
2. A multiplex added whose output the engine cannot open: the apply fails with the engine's error, the
   previous configuration is on the air again and the change is still pending.

Ports 26100-26999, 18190.

usage: python3 tests/live_radio_empty_test.py
"""
import json
import os
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

URL = "http://127.0.0.1:18190"
check, wait_for, FAILS = lc.check, lc.wait_for, lc.FAILS


def api(method, path):
    req = urllib.request.Request(URL + path, method=method, data=b"{}" if method == "POST" else None,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return json.load(e)


def get(f, default=None):
    try:
        return f(api("GET", "/api/status"))
    except (OSError, ValueError, KeyError, IndexError, TypeError):
        return default


def svc_on(st, name):
    return next(((s.get("live") or {}).get("source") for c in st["carriers"] for s in c["services"] if s["name"] == name), None)


def config(tmp, clips, first_radio="first", extra=""):
    return f"""# test
work_dir = "{tmp}/work"
[network]
name = "Test"
[web]
listen = "127.0.0.1:18190"
[ingest]
base_port = 26100
monitor_base_port = 26900
[monitoring]
tsduck = false
[engine]
binary = "{ROOT}/engine/build/dvbtx-engine"
[[radio]]
name = "first"
driver = "null"
[[radio]]
name = "second"
driver = "null"
serial = "NUL2"
[[carrier]]
name = "Mux A"
radio = "{first_radio}"
standard = "dvbt"
frequency = 482_000_000
  [[carrier.service]]
  name = "Alpha"
  source = "{clips[0]}"
[[carrier]]
name = "Mux B"
radio = "second"
standard = "dvbt"
frequency = 498_000_000
  [[carrier.service]]
  name = "Bravo"
  source = "{clips[1]}"
{extra}"""


def main():
    tmp = tempfile.mkdtemp(prefix="dvbtx-empty-")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "abc"]
    for i, c in enumerate(clips):
        lc.make_clip(c, i * 70, 300 + 150 * i)
    cfgp = os.path.join(tmp, "dvbtx.toml")
    with open(cfgp, "w") as f:
        f.write(config(tmp, clips))
    sup = subprocess.Popen([sys.executable, "-m", "dvbtx", "run", "-c", cfgp], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = []
    threading.Thread(target=lambda: [log.append(l.rstrip()) for l in sup.stdout], daemon=True).start()
    try:
        check(wait_for(lambda: get(lambda s: svc_on(s, "Alpha")) == "primary"
                       and get(lambda s: svc_on(s, "Bravo")) == "primary", 60), "both channels on air")

        # 1. the first radio left without multiplex
        with open(cfgp, "w") as f:
            f.write(config(tmp, clips, first_radio="second"))
        r = api("POST", "/api/config/apply")
        check(r.get("ok") and r.get("restarted"), f"applied with a restart: {r}")
        ini = open(os.path.join(tmp, "work", "engine.ini")).read()
        check("[radio 0]" in ini and "[radio 1]" not in ini, "engine.ini: the remaining radio numbered 0")
        check(wait_for(lambda: get(lambda s: svc_on(s, "Alpha")) == "primary"
                       and get(lambda s: svc_on(s, "Bravo")) == "primary", 60), "both channels on air again")
        st = api("GET", "/api/status")
        radios = st.get("radios", [])
        check([x["name"] for x in radios] == ["second"], f"status: the second radio only ({[x['name'] for x in radios]})")
        live = (radios[0].get("live") or {}) if radios else {}
        check(live.get("index") == 1 and live.get("state", "on") == "on", f"its live state, numbered 1: {live}")
        time.sleep(8)
        check(not any("engine exited" in l for l in log), "the engine does not exit")
        check(get(lambda s: s["engine"]["restarts"]) == 0, "no engine restart")
        check(not get(lambda s: s["alerts"], []), f"no alert: {get(lambda s: s['alerts'])}")

        # its serial: bound in the file, reported by the engine, listed with the radio that holds it
        check(radios and radios[0].get("serial") == "NUL2" and live.get("serial") == "NUL2",
              f"status: serial NUL2 bound and reported ({radios[0].get('serial') if radios else None}, {live.get('serial')})")
        d = api("GET", "/api/devices?driver=null")
        check([(x["serial"], x["radio"], x["open"]) for x in d.get("devices", [])] == [("NUL2", "second", True)]
              and d.get("free") == [], f"devices: NUL2 held by radio second: {d}")
        d = api("GET", "/api/devices?driver=null&radio=1")
        check(d.get("free") == ["NUL2"], f"devices, editing radio second: its own device free for it: {d}")

        # 2. a configuration the engine refuses
        bad = f"""[[carrier]]
name = "Mux C"
standard = "ts"
ts_rate = 2_000_000
output = "{tmp}/no-such-dir/out.ts"
  [[carrier.service]]
  name = "Charlie"
  source = "{clips[2]}"
"""
        with open(cfgp, "w") as f:
            f.write(config(tmp, clips, first_radio="second", extra=bad))
        r = api("POST", "/api/config/apply")
        check(not r.get("ok") and "cannot open" in (r.get("error") or ""), f"apply refused with the engine's error: {r}")
        check(wait_for(lambda: get(lambda s: svc_on(s, "Alpha")) == "primary"
                       and get(lambda s: svc_on(s, "Bravo")) == "primary", 60), "the previous configuration on air")
        check([c["name"] for c in api("GET", "/api/status")["carriers"]] == ["Mux A", "Mux B"], "running: Mux A and B")
        pend = api("GET", "/api/config").get("pending")
        check(bool(pend), f"the change still pending: {pend}")
        time.sleep(8)
        check(get(lambda s: s["engine"]["running"]) is True, "the engine runs")
    finally:
        sup.terminate()
        try:
            sup.wait(20)
        except subprocess.TimeoutExpired:
            sup.kill()
    if FAILS:
        print(f"FAILED: {len(FAILS)} check(s)")
        for l in log[-40:]:
            print("   ", l)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
