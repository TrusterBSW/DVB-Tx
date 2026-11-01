"""Multiplexes stopped and started again from the web page (POST /api/carrier/N/stop|start): off the air,
the others not interrupted, their sources paused, a radio switched off when its last multiplex stops and
on again with it; the stop kept in the configuration file (enabled = false) across a restart.

Two "null" radios: one with two DVB-T2 multiplexes (synthesised together), one with a DVB-T one.
Ports 26100-26999, 18190.

usage: python3 tests/live_carrier_stop_test.py
"""
import json
import os
import sys
import tempfile
import time
import tomllib
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import live_channels_test as lc          # noqa: E402
import live_radio_missing_test as rm     # noqa: E402  (run, stop, get)

URL = "http://127.0.0.1:18190"
OUT = 26999
lc.OUT = OUT
rm.URL = URL
check, wait_for, FAILS = lc.check, lc.wait_for, lc.FAILS
get = rm.get


def post(path):
    req = urllib.request.Request(URL + path, method="POST", data=b"")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return json.load(e)


def carrier(st, name):
    return next(c for c in st["carriers"] if c["name"] == name)


def config(tmp, clips):
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
[defaults]
backup = "{clips[3]}"

[[radio]]
name = "one"
driver = "null"
[[carrier]]
name = "Mux A1"
radio = "one"
standard = "dvbt2"
frequency = 482_000_000
monitor = "udp://127.0.0.1:{OUT}"
  [[carrier.service]]
  name = "Alpha"
  source = "{clips[0]}"
[[carrier]]
name = "Mux A2"
radio = "one"
standard = "dvbt2"
frequency = 490_000_000
  [[carrier.service]]
  name = "Bravo"
  source = "{clips[1]}"

[[radio]]
name = "two"
driver = "null"
[[carrier]]
name = "Mux B"
radio = "two"
standard = "dvbt"
frequency = 506_000_000
  [[carrier.service]]
  name = "Charlie"
  source = "{clips[2]}"
"""


def video_gap(out):
    with out.lock:
        busiest = max(out.count, key=out.count.get) if out.count else None
        return out.gap.get(busiest, 9)


def main():
    tmp = tempfile.mkdtemp(prefix="dvbtx-cstop-")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "abcd"]
    for i, c in enumerate(clips):
        lc.make_clip(c, i * 60, 300 + 120 * i)
    cfgp = os.path.join(tmp, "dvbtx.toml")
    with open(cfgp, "w") as f:
        f.write(config(tmp, clips))
    out = lc.Output()
    out.start()
    sup, log = rm.run(cfgp)
    try:
        check(wait_for(lambda: all(get(lambda s: rm.svc_on(s, n)) == "primary" for n in ("Alpha", "Bravo", "Charlie")), 40),
              "all on air")
        time.sleep(3)
        out.reset()

        # one of two multiplexes of a radio
        r = post("/api/carrier/1/stop")
        check(r.get("ok") and r.get("enabled") is False, f"stop Mux A2: {r}")
        check(wait_for(lambda: get(lambda s: rm.input_state(s, "Bravo")) == "paused", 10), "its source paused")
        check(get(lambda s: carrier(s, "Mux A2")["enabled"]) is False, "stopped in the status")
        check(get(lambda s: carrier(s, "Mux A2")["live"]["on"]) == 0, "engine: off the air")
        check(get(lambda s: rm.radio_state(s, 0)) == "on", "its radio still on (Mux A1)")
        time.sleep(4)
        check(video_gap(out) < 1.0, f"Mux A1 not interrupted (video gap {video_gap(out):.2f} s)")
        alerts = get(lambda s: [a["message"] for a in s["alerts"]], [])
        check(not any("Bravo" in a or "Mux A2" in a for a in alerts), f"no alert for it: {alerts}")
        with open(cfgp, "rb") as f:
            doc = tomllib.load(f)
        check(doc["carrier"][1].get("enabled") is False, "kept in the file")
        check(not get(lambda s: True) is None and not post("/api/carrier/9/stop").get("ok"), "unknown multiplex refused")

        # the only multiplex of a radio: the radio off
        r = post("/api/carrier/2/stop")
        check(r.get("ok"), f"stop Mux B: {r}")
        check(wait_for(lambda: get(lambda s: rm.radio_state(s, 1)) == "idle", 10), "its radio off (idle)")
        check(wait_for(lambda: get(lambda s: rm.input_state(s, "Charlie")) == "paused", 10), "its source paused")
        alerts = get(lambda s: [a["message"] for a in s["alerts"]], [])
        check(not any("radio 1" in a for a in alerts), f"no radio alert: {alerts}")
        r = post("/api/carrier/2/start")
        check(r.get("ok") and r.get("enabled") is True, f"start Mux B: {r}")
        check(wait_for(lambda: get(lambda s: rm.radio_state(s, 1)) == "on", 10), "its radio on again")
        check(wait_for(lambda: get(lambda s: rm.svc_on(s, "Charlie")) == "primary", 30), "its channel on air again")
        check(get(lambda s: s["engine"]["restarts"]) == 0, "no engine restart")
    finally:
        rm.stop(sup)

    # the stop kept across a restart
    sup, log = rm.run(cfgp)
    try:
        check(wait_for(lambda: get(lambda s: rm.svc_on(s, "Alpha")) == "primary", 40), "restarted: Mux A1 on air")
        check(get(lambda s: carrier(s, "Mux A2")["enabled"]) is False, "Mux A2 still stopped")
        check(wait_for(lambda: get(lambda s: carrier(s, "Mux A2")["live"]["on"]) == 0, 10), "engine: still off the air")
        check(wait_for(lambda: get(lambda s: rm.input_state(s, "Bravo")) == "paused", 10), "its source paused at the start")
        r = post("/api/carrier/1/start")
        check(r.get("ok"), f"start Mux A2: {r}")
        check(wait_for(lambda: get(lambda s: rm.svc_on(s, "Bravo")) == "primary", 30), "Mux A2 on air again")
        with open(cfgp, "rb") as f:
            doc = tomllib.load(f)
        check("enabled" not in doc["carrier"][1], "the file back to normal")
        cfgv = json.load(urllib.request.urlopen(URL + "/api/config", timeout=5))
        check(not cfgv.get("pending"), f"no pending change: {cfgv.get('pending')}")
    finally:
        rm.stop(sup)
        out.stop = True
    if FAILS:
        print(f"FAILED: {len(FAILS)} check(s)")
        for l in log[-30:]:
            print("   ", l)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
