"""A radio missing at the start, unplugged while running, plugged again: its multiplex off the air and
its sources paused, the other radio's multiplex going on; the engine started again once it is back.
Then a configuration whose only radio is missing: dvbtx runs, its web page answers, nothing on air.
Then a radio missing whose multiplexes are all stopped (enabled = false): not opened (idle), no alert,
not looked for.

The radios are "null" ones; absent=FILE makes one missing while FILE exists. Ports 25100-25999, 18189.

usage: python3 tests/live_radio_missing_test.py
"""
import json
import os
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

URL = "http://127.0.0.1:18189"
OUT = 25999
lc.OUT = OUT
check, wait_for, FAILS = lc.check, lc.wait_for, lc.FAILS


def status():
    with urllib.request.urlopen(URL + "/api/status", timeout=5) as r:
        return json.load(r)


def get(f, default=None):
    try:
        return f(status())
    except (OSError, ValueError, KeyError, IndexError, TypeError):
        return default


def radio_state(st, i):
    return (st["radios"][i].get("live") or {}).get("state")


def svc_on(st, name):
    return next(((s.get("live") or {}).get("source") for c in st["carriers"] for s in c["services"] if s["name"] == name), None)


def input_state(st, name):
    return next((i["state"] for i in st["inputs"] if i["name"] == name and i.get("role") == "primary"), None)


def config(tmp, absent, clips, both=True, ghost_on=True):
    good = f"""[[radio]]
name = "good"
driver = "null"
[[carrier]]
name = "Mux A"
radio = "good"
standard = "dvbt2"
frequency = 482_000_000
monitor = "udp://127.0.0.1:{OUT}"
  [[carrier.service]]
  name = "Alpha"
  source = "{clips[0]}"
""" if both else ""
    return f"""# test
work_dir = "{tmp}/work"
[network]
name = "Test"
[web]
listen = "127.0.0.1:18189"
[ingest]
base_port = 25100
monitor_base_port = 25900
[monitoring]
tsduck = false
[engine]
binary = "{ROOT}/engine/build/dvbtx-engine"
[defaults]
backup = "{clips[2]}"
{good}
[[radio]]
name = "ghost"
driver = "null"
args = "absent={absent}"
[[carrier]]
name = "Mux B"
radio = "ghost"
standard = "dvbt"
frequency = 498_000_000
{"" if ghost_on else "enabled = false"}
  [[carrier.service]]
  name = "Bravo"
  source = "{clips[1]}"
"""


def run(cfgp):
    sup = subprocess.Popen([sys.executable, "-m", "dvbtx", "run", "-c", cfgp], cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = []
    threading.Thread(target=lambda: [log.append(l.rstrip()) for l in sup.stdout], daemon=True).start()
    return sup, log


def stop(sup):
    sup.terminate()
    try:
        sup.wait(20)
    except subprocess.TimeoutExpired:
        sup.kill()


def main():
    tmp = tempfile.mkdtemp(prefix="dvbtx-radio-")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "abc"]
    for i, c in enumerate(clips):
        lc.make_clip(c, i * 70, 300 + 150 * i)
    absent = os.path.join(tmp, "ghost-unplugged")
    open(absent, "w").close()
    cfgp = os.path.join(tmp, "dvbtx.toml")
    with open(cfgp, "w") as f:
        f.write(config(tmp, absent, clips))
    out = lc.Output()
    out.start()
    sup, log = run(cfgp)
    try:
        # missing at the start
        check(wait_for(lambda: get(lambda s: svc_on(s, "Alpha")) == "primary", 40), "the other radio's channel on air")
        check(wait_for(lambda: get(lambda s: radio_state(s, 1)) == "off", 10), "missing radio: off")
        check(wait_for(lambda: get(lambda s: input_state(s, "Bravo")) == "paused", 10), "its channel's source paused")
        def alerts():
            return get(lambda s: [a["message"] for a in s["alerts"]], [])
        check(wait_for(lambda: any("radio 1" in a and "not found" in a and "Mux B" in a for a in alerts()), 15),
              f"alert: {alerts()}")
        check(not any("Bravo" in a for a in alerts()), "no alert of its channels besides")
        time.sleep(20)
        check(not any("engine exited" in l for l in log), "the engine does not exit in a loop")
        check(get(lambda s: s["engine"]["restarts"]) == 0, "no engine restart")

        # plugged: looked for, the engine started again
        os.unlink(absent)
        check(wait_for(lambda: get(lambda s: radio_state(s, 1)) == "on", 40), "radio found again: on")
        check(wait_for(lambda: get(lambda s: svc_on(s, "Bravo")) == "primary", 30), "its channel on air")
        check(get(lambda s: input_state(s, "Bravo")) == "running", "its source running again")
        check(wait_for(lambda: get(lambda s: svc_on(s, "Alpha")) == "primary", 30), "the other one on air again")

        # unplugged while running: lost, the other multiplex not interrupted
        time.sleep(5)
        out.reset()
        open(absent, "w").close()
        check(wait_for(lambda: get(lambda s: radio_state(s, 1)) == "lost", 15), "unplugged radio: lost")
        check(wait_for(lambda: get(lambda s: input_state(s, "Bravo")) == "paused", 10), "its source paused")
        time.sleep(5)
        with out.lock:                   # the busiest PID (video): the SI tables come every 2 s anyway
            busiest = max(out.count, key=out.count.get) if out.count else None
            gap = out.gap.get(busiest, 9)
        check(gap < 1.0, f"the other multiplex not interrupted (longest gap {gap:.2f} s)")
        check(get(lambda s: svc_on(s, "Alpha")) == "primary", "the other channel still on air")
        os.unlink(absent)
        check(wait_for(lambda: get(lambda s: radio_state(s, 1)) == "on", 40), "plugged again: on")
        check(wait_for(lambda: get(lambda s: svc_on(s, "Bravo")) == "primary", 30), "its channel on air again")
    finally:
        stop(sup)

    # no radio at all
    open(absent, "w").close()
    with open(cfgp, "w") as f:
        f.write(config(tmp, absent, clips, both=False))
    sup, log = run(cfgp)
    try:
        check(wait_for(lambda: get(lambda s: radio_state(s, 0)) == "off", 40), "only radio missing: off")
        time.sleep(10)
        check(get(lambda s: s["engine"]["running"]) is True, "the engine runs without transmitting")
        check(not any("engine exited" in l for l in log), "and does not exit")
        check(get(lambda s: s["version"]) is not None, "the web page answers")
    finally:
        stop(sup)

    # a missing radio whose multiplexes are all stopped: idle, not an alert
    with open(cfgp, "w") as f:
        f.write(config(tmp, absent, clips, ghost_on=False))
    sup, log = run(cfgp)
    try:
        check(wait_for(lambda: get(lambda s: svc_on(s, "Alpha")) == "primary", 40), "the other radio's channel on air")
        check(wait_for(lambda: get(lambda s: radio_state(s, 1)) == "idle", 10),
              f"missing radio, its multiplexes stopped: idle ({get(lambda s: radio_state(s, 1))})")
        time.sleep(20)
        al = get(lambda s: [a["message"] for a in s["alerts"]], [])
        check(not al, f"no alert: {al}")
        check(not any("probe" in l.lower() or "looked for" in l for l in log), "not looked for")
        check(get(lambda s: input_state(s, "Bravo")) == "paused", "its channel's source paused")
        check(not any("input Bravo:" in l for l in log), "its source never started (paused from the start)")
        check(get(lambda s: s["engine"]["restarts"]) == 0, "no engine restart")
    finally:
        stop(sup)
        out.stop = True
    if FAILS:
        print(f"FAILED: {len(FAILS)} check(s)")
        for l in log[-30:]:
            print("   ", l)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
