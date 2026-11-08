"""Radios bound to their serial number: the tools' output read, the serial key checked and given to the driver.

usage: python3 tests/devices_test.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from dvbtx import config as cfgmod        # noqa: E402
from dvbtx import devices                  # noqa: E402

HACKRF = """hackrf_info version: 2024.02.1
libhackrf version: 2024.02.1 (0.9)
Found HackRF
Index: 0
Serial number: 0000000000000000457863c82b6a6a5f
Board ID Number: 2 (HackRF One)
Firmware Version: 2024.02.1 (API:1.08)
Part ID Number: 0xa000cb3c 0x005b4f60
Hardware supported by installed firmware:
    HackRF One

Found HackRF
Index: 1
Serial number: 0x00000000 0x00000000 0x14d463dc 0x2f7b1d5f
Board ID Number: 2 (HackRF One)
hackrf_open() failed: Resource busy (-1000)
"""

UHD = """[INFO] [UHD] linux; GNU C++ version 12.2.0; Boost_108300; UHD_4.3.0.0+ds1-5
--------------------------------------------------
-- UHD Device 0
--------------------------------------------------
Device Address:
    serial: 3449ABC
    name: MyB210
    product: B210
    type: b200


--------------------------------------------------
-- UHD Device 1
--------------------------------------------------
Device Address:
    serial: 30C5E1F
    product: B200mini
    type: b200
"""

SOAPY = """######################################################
##     Soapy SDR -- the SDR abstraction library     ##
######################################################

Found device 0
  driver = lime
  label = LimeSDR Mini [USB 3.0] 1D3AC1E4A2B4C5
  serial = 1D3AC1E4A2B4C5

Found device 1
  driver = rtlsdr
  label = Generic RTL2832U OEM :: 00000001
  serial = 00000001

Found device 2
  driver = bladerf
  label = bladeRF #0 [ABCD1234]
"""

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


h = devices.parse("hackrf", HACKRF)
check([d["serial"] for d in h] == ["0000000000000000457863c82b6a6a5f", "000000000000000014d463dc2f7b1d5f"],
      f"hackrf_info, both serial formats: {h}")
check(h[0]["label"] == "2 (HackRF One)", "hackrf label")
u = devices.parse("uhd", UHD)
check([(d["serial"], d["label"]) for d in u] == [("3449ABC", "B210 MyB210"), ("30C5E1F", "B200mini")], f"uhd_find_devices: {u}")
s = devices.parse("soapy", SOAPY)
check([d["serial"] for d in s] == ["1D3AC1E4A2B4C5"], f"SoapySDRUtil: receive-only and serial-less skipped: {s}")

AVAHI = r"""+;wlp53s0;IPv4;edgeQAM\032plutoplus;_dvbmod._udp;local
=;wlp53s0;IPv6;edgeQAM\032plutoplus;_dvbmod._udp;local;plutoplus.local;fe80::1;1001;"std=dvbt"
=;wlp53s0;IPv4;edgeQAM\032plutoplus;_dvbmod._udp;local;plutoplus.local;10.5.1.96;1001;"std=dvbt" "mode=2k" "qam=64" "cr=7/8" "gi=1/32" "bw=8" "rate=31668449" "nmux=2" "ports=1001,1002,1003,1004" "freq1=474000000" "disable=0" "api=8080"
=;eth0;IPv4;edgeQAM\032plutoplus;_dvbmod._udp;local;plutoplus.local;10.5.1.96;1001;"std=dvbt" "nmux=4" "ports=1001,1002,1003,1004"
=;eth0;IPv4;other\032kind;_dvbmod._udp;local;b.local;10.5.1.97;1001;"std=dvbs2"
"""
e = devices.parse("edgeqam", AVAHI)
check([(d["serial"], d["label"], d["address"], d["ports"], d["api"]) for d in e]
      == [("plutoplus", "edgeQAM plutoplus", "10.5.1.96", [1001, 1002], 8080)],
      f"avahi-browse: the edgeQAM board once, IPv4, its nmux ports, other standards skipped: {e}")

AVAHI2 = r"""=;eth0;IPv4;edgeQAM\032plutoplus;_dvbmod._udp;local;plutoplus.local;10.5.1.96;1001;"std=dvbt" "nmux=4" "ports=1001,1002,1003,1004" "id=0060881fa308"
=;eth0;IPv4;salon;_dvbmod._udp;local;plutoplus-2.local;10.5.1.98;1001;"std=dvbt" "nmux=4" "ports=1001,1002,1003,1004" "id=0060881fb410"
"""
e2 = devices.parse("edgeqam", AVAHI2)
check([(d["serial"], d["host"], d["label"]) for d in e2] == [("0060881fa308", "plutoplus", "edgeQAM plutoplus"),
                                                             ("0060881fb410", "plutoplus-2", "salon")],
      f"avahi-browse, two boards: their TXT id as serial, host and service name kept: {e2}")
check([devices.board_matches(e2[1], k) for k in ("0060881FB410", "plutoplus-2", "salon", "plutoplus", "")]
      == [True, True, True, False, False], "a board answers to its id, host or service name")
check(devices.owner("edgeqam", e2[1]["serial"], {"salon": "tv"}, e2[1]) == "tv", "owner: a board bound by its name")

check(devices.serial_of("num_send_frames=256, serial=3449ABC") == "3449ABC", "serial_of")
check(devices.serial_of("rt=60") is None, "serial_of: none")
check(devices.with_serial("rt=60,serial=OLD", "NEW") == "rt=60,serial=NEW", "with_serial replaces")
check(devices.with_serial("", "NEW") == "serial=NEW", "with_serial adds")
check(devices.with_serial("serial=OLD,rt=60", None) == "rt=60", "with_serial removes")

HEAD = '''work_dir = "/tmp/dvbtx-test"
[network]
name = "Test"
'''
RADIO = '''[[radio]]
name = "{name}"
driver = "{driver}"
{extra}
'''
CAR = '''[[carrier]]
name = "M"
radio = "{radio}"
standard = "dvbt"
frequency = 482_000_000
  [[carrier.service]]
  name = "S"
  source = "/srv/s.ts"
'''


def build(radios, radio="a"):
    text = HEAD + "".join(RADIO.format(name=n, driver=d, extra=e) for n, d, e in radios) + CAR.format(radio=radio)
    return cfgmod.build(cfgmod.load(text=text))


def fails(radios):
    try:
        build(radios)
    except cfgmod.ConfigError as e:
        return str(e)
    return None


p = build([("a", "uhd", 'serial = "3449ABC"\nargs = "num_send_frames=256"')])
r = p.radios[0]
check(r.serial == "3449ABC" and r.args == "num_send_frames=256,serial=3449ABC", f"serial given to the driver: {r.args}")
os.makedirs("/tmp/dvbtx-test", exist_ok=True)
cfgmod.write_engine_ini(p, "/tmp/dvbtx-test/devices_engine.ini", "/tmp/dvbtx-test")
check("args = num_send_frames=256,serial=3449ABC" in open("/tmp/dvbtx-test/devices_engine.ini").read(), "in engine.ini")
p = build([("a", "hackrf", 'args = "serial=457863c82b6a6a5f"')])
check(p.radios[0].serial == "457863c82b6a6a5f", "serial in args alone: the radio is bound too")
p = build([("a", "hackrf", 'serial = "457863c8"\nargs = "serial=457863c8"')])
check(p.radios[0].args == "serial=457863c8", "the same in both: kept once")
e = fails([("a", "hackrf", 'serial = "AAA"\nargs = "serial=BBB"')])
check(e is not None and "differ" in e, f"serial and args differ: {e}")
e = fails([("a", "uhd", 'serial = "3449ABC"'), ("b", "uhd", 'serial = "3449ABC"')])
check(e is not None and "the same uhd device" in e, f"two radios on one device: {e}")
check(fails([("a", "uhd", 'serial = "3449ABC"'), ("b", "soapy", 'serial = "3449ABC"')]) is None,
      "the same serial on two drivers: allowed")
e = fails([("a", "hackrf", 'serial = "45 78"')])
check(e is not None and "letters, digits" in e, f"bad serial: {e}")
check(build([("a", "hackrf", "")]).radios[0].serial == "", "no serial: unbound")
check(devices.same("hackrf", "0000000000000000457863c82b6a6a5f", "457863C82B6A6A5F"), "hackrf: the end of the serial")
check(not devices.same("uhd", "3449ABC", "49ABC"), "uhd: the whole serial")
check(devices.owner("hackrf", "0000000000000000457863c82b6a6a5f", {"6a6a5f": "r"}) == "r", "owner by the end")
e = fails([("a", "hackrf", 'serial = "0000000000000000457863c82b6a6a5f"'), ("b", "hackrf", 'serial = "2b6a6a5f"')])
check(e is not None and "the same hackrf device" in e, f"hackrf: the same device by the end of its serial: {e}")

# dvbtx devices --save, with a uhd_find_devices that prints two B2xx
import subprocess                       # noqa: E402
import tempfile                         # noqa: E402
tmp = tempfile.mkdtemp(prefix="dvbtx-devices-")
tool = os.path.join(tmp, "uhd_find_devices")
with open(tool, "w") as f:
    f.write("#!/bin/sh\ncat <<'X'\n" + UHD + "X\n")
os.chmod(tool, 0o755)
cfgp = os.path.join(tmp, "dvbtx.toml")
with open(cfgp, "w") as f:
    f.write(HEAD + RADIO.format(name="a", driver="uhd", extra='serial = "3449ABC"')
            + RADIO.format(name="b", driver="uhd", extra='args = "num_send_frames=256"') + CAR.format(radio="a"))
env = dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"])
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def dvbtx(*args):
    return subprocess.run([sys.executable, "-m", "dvbtx", "devices", "-c", cfgp, *args], cwd=root, env=env,
                          capture_output=True, text=True)


r = dvbtx()
check('serial = "3449ABC"' in r.stdout and "radio a" in r.stdout and 'serial = "30C5E1F"' in r.stdout,
      f"devices lists both, the first bound to radio a: {r.stdout!r}")
r = dvbtx("--save", "b")
saved = open(cfgp).read()
check(r.returncode == 0 and 'serial = "30C5E1F"' in saved, f"--save b: the free one saved ({r.stdout or r.stderr})")
check(cfgmod.build(cfgmod.load(cfgp)).radios[1].args == "num_send_frames=256,serial=30C5E1F", "and it builds")
r = dvbtx("--save", "a")
check(r.returncode == 0 and saved == open(cfgp).read(), f"--save a: its own device kept ({r.stdout or r.stderr})")
with open(cfgp, "w") as f:
    f.write(HEAD + RADIO.format(name="a", driver="uhd", extra="") + CAR.format(radio="a"))
r = dvbtx("--save", "a")
check(r.returncode != 0 and "--serial" in r.stderr, f"two free: --serial asked ({r.stderr.strip()})")
r = dvbtx("--save", "a", "--serial", "30C5E1F")
check(r.returncode == 0 and 'serial = "30C5E1F"' in open(cfgp).read(), f"--serial picks it ({r.stdout or r.stderr})")

if FAILS:
    print(f"FAILED: {len(FAILS)} check(s)")
    sys.exit(1)
print("all checks passed")
