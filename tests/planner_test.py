"""Frequency planner checks: LO offset and sample rate choice (no transmitter needed).

usage: python3 tests/planner_test.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from dvbtx import config as cfgmod        # noqa: E402

HEAD = '''work_dir = "/tmp/dvbtx-test"
[network]
name = "Test"
[[radio]]
name = "r"
driver = "{driver}"
{radio}
'''

CARRIER = '''[[carrier]]
name = "{name}"
radio = "r"
{body}
  [[carrier.service]]
  name = "S{name}"
  source = "/srv/{name}.ts"
'''


def s2(sr, freq=1_200_000_000):
    return (f'standard = "dvbs2"\nfrequency = {freq}\nsymbol_rate = {sr}\nrolloff = 0.2\n'
            'modulation = "8psk"\ncode_rate = "3/4"')


def plan(carriers, driver="hackrf", radio=""):
    text = HEAD.format(driver=driver, radio=radio) + "".join(
        CARRIER.format(name=f"M{i}", body=b) for i, b in enumerate(carriers))
    p = cfgmod.build(cfgmod.load(text=text))
    return p, p.radios[0]


def fails(carriers, **kw):
    try:
        plan(carriers, **kw)
    except cfgmod.ConfigError as e:
        return str(e)
    return None


def notes(r):
    return " | ".join(r.notes)


t0 = time.time()

# 7 Msym/s: fits with the LO outside the channel, as before
p, r = plan([s2(7_000_000)])
c = p.carriers[0]
assert c.offset > cfgmod.occupied_half(c), (c.offset, notes(r))
assert not r.notes, notes(r)

# 15.5 Msym/s (18.6 MHz): LO in the middle, under the practical limit
p, r = plan([s2(15_500_000)])
c = p.carriers[0]
assert r.rate <= 19.7e6, r.rate
assert abs(c.offset) < cfgmod.occupied_half(c), c.offset
assert "LO inside the channel" in notes(r), notes(r)
assert "warning" not in notes(r), notes(r)
print(f"15.5 Msym/s: {r.rate / 1e6:.6f} MS/s, offset {c.offset / 1e6:+.3f} MHz: {notes(r)}")

# lo_offset = 0 forced: same LO, no note about the fallback
p0, r0 = plan([s2(15_500_000)], radio="lo_offset = 0")
assert r0.center == r.center and r0.rate == r.rate, (r0.center, r.center, r0.rate, r.rate)
assert not r0.notes, notes(r0)

# 16.4 Msym/s: only fits at the very limit, with a warning
p, r = plan([s2(16_400_000)])
assert r.rate <= 20e6 * 1.0001, r.rate
assert "LO inside the channel" in notes(r) and "warning" in notes(r), notes(r)
print(f"16.4 Msym/s: {r.rate / 1e6:.6f} MS/s: {notes(r)}")

# too wide even with the LO in the middle: refused
assert fails([s2(20_000_000)]) is not None

# a forced non-zero offset is not changed: refused when too wide
assert fails([s2(15_500_000)], radio="lo_offset = 9_800_000") is not None

# two carriers: centred between them, no fallback note
p, r = plan([s2(3_000_000, 1_200_000_000), s2(3_000_000, 1_210_000_000)])
assert abs(r.center - 1_205_000_000) < 1e3, r.center
assert not any("LO" in n for n in r.notes), notes(r)

# DVB-T 8 MHz: unchanged (LO below the channel)
p, r = plan(['channel = 22'])
c = p.carriers[0]
assert c.offset > cfgmod.occupied_half(c) and not r.notes, (c.offset, notes(r))

# UHD: a wide DVB-S2 carrier, kept under the AD9361 filter limit or warned
p, r = plan(['standard = "dvbs2"\nfrequency = 1_200_000_000\nsymbol_rate = 45_000_000\nrolloff = 0.2\n'
             'modulation = "qpsk"\ncode_rate = "1/2"'], driver="uhd")
print(f"uhd 45 Msym/s: {r.rate / 1e6:.6f} MS/s: {notes(r)}")
assert r.rate <= 56e6 or "warning" in notes(r)

# a radio without multiplex is left out of engine.ini: the others numbered 0..n-1, the carriers follow
# (it used to keep its number: [radio 1] alone, refused by the engine, which restarted in a loop)
text = (HEAD.format(driver="null", radio="") + '[[radio]]\nname = "b"\ndriver = "null"\n'
        + CARRIER.format(name="M0", body=s2(3_000_000)).replace('radio = "r"', 'radio = "b"'))
p = cfgmod.build(cfgmod.load(text=text))
assert [r.index for r in cfgmod.engine_radios(p)] == [1], [r.index for r in p.radios]
os.makedirs("/tmp/dvbtx-test", exist_ok=True)
ini = "/tmp/dvbtx-test/planner_engine.ini"
cfgmod.write_engine_ini(p, ini, "/tmp/dvbtx-test")
lines = open(ini).read().splitlines()
assert [x for x in lines if x.startswith("[radio")] == ["[radio 0]"], lines
assert "radio = 0" in lines, lines

# the statistics of the engine back to the configuration's numbers
from dvbtx.supervisor import Supervisor     # noqa: E402
st = {"radios": [{"index": 0}], "carriers": [{"carrier": 0, "radio": 0}, {"carrier": 1, "radio": -1}]}
Supervisor._radio_numbers(st, [1])
assert st["radios"][0]["index"] == 1 and st["carriers"][0]["radio"] == 1 and st["carriers"][1]["radio"] == -1, st

# a stopped multiplex still counts in its radio's band: the error says so and names it
far = s2(3_000_000, 1_400_000_000)
e = fails([s2(3_000_000), far.replace("symbol_rate", "enabled = false\nsymbol_rate")])
assert e and "1 of them stopped: M1" in e, e

# a single carrier too wide: the rate of its least demanding placement (LO in the middle), not of the LO
# outside the channel tried first (it said 126 MS/s for DVB-S 41 Msym/s on a B210)
e = fails(['standard = "dvbs"\nfrequency = 1_200_000_000\nsymbol_rate = 41_000_000\nrolloff = 0.35\n'
           'code_rate = "3/4"'], driver="uhd")
assert e and "needs 61.500 MS/s" in e and "lower its symbol rate" in e, e

print(f"planner_test: OK ({time.time() - t0:.2f} s)")
