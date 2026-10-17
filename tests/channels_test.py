"""Channel operations on the configuration and the TOML file (no transmitter needed).

usage: python3 tests/channels_test.py
"""
import os
import sys
import tempfile
import tomllib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from dvbtx import channels as ch          # noqa: E402
from dvbtx import config as cfgmod        # noqa: E402

SAMPLE = '''# test configuration
work_dir = "/tmp/dvbtx-test"

[network]
name = "Test"

[[radio]]
name = "r"
driver = "null"

[[carrier]]
name = "Mux A"
radio = "r"
frequency = 482_000_000
tsid = 1
  # first channel
  [[carrier.service]]
  name = "Un"
  source = "/srv/un.ts"   # a file
  backup = "/srv/backup.ts"

  [[carrier.service]]
  name = "Deux"
  source = "udp://239.1.1.1:1234"
  [carrier.service.encode]
  video_bitrate = "3M"

  [[carrier.service]]
  name = "Trois"
  source = "lavfi:testsrc2"

[[carrier]]
name = "Mux B"
radio = "r"
frequency = 490_000_000
tsid = 2
  [[carrier.service]]
  name = "Quatre"
  source = "/srv/quatre.ts"

[epg]
dir = ""
'''

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def load(path):
    cfg = cfgmod.load(path)
    return cfg, cfgmod.build(cfg)


def main():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "dvbtx.toml")
    with open(path, "w") as f:
        f.write(SAMPLE)
    cfg, plan = load(path)
    sids = {s.name: s.sid for s in plan.services}
    check(sids == {"Un": 0x101, "Deux": 0x102, "Trois": 0x103, "Quatre": 0x201}, f"default sids {sids}")

    # designation
    check(ch.find(plan, "2").name == "Deux", "find by number")
    check(ch.find(plan, "0x0103").name == "Trois", "find by service_id")
    check(ch.find(plan, "quatre").name == "Quatre", "find by name, any case")
    try:
        ch.find(plan, "Cinq")
        check(False, "unknown channel refused")
    except ch.ChannelError:
        check(True, "unknown channel refused")

    # remove the second: the third keeps its sid, number and PIDs
    trois = ch.find(plan, "Trois")
    new, nplan, sid = ch.remove(cfg, plan, "Deux")
    ch.persist(path, cfg, new, removed=(0, 1))
    cfg, plan = load(path)
    t = ch.find(plan, "Trois")
    check((t.sid, t.lcn, t.pid_base) == (trois.sid, trois.lcn, trois.pid_base),
          f"removal keeps the others ({t.sid:#x} {t.lcn} {t.pid_base:#x})")
    text = open(path).read()
    check("# test configuration" in text and "# a file" in text and "# first channel" in text, "comments kept")
    check("Deux" not in text and "video_bitrate" not in text, "removed block and its sub-table gone")
    check(text.count("[[carrier.service]]") == 3, "three services left in the file")

    # add to the first multiplex, with options (which one must be said: there are two)
    try:
        ch.add(cfg, plan, {"name": "Cinq", "source": "x.ts"})
        check(False, "multiplex required when there are several")
    except ch.ChannelError:
        check(True, "multiplex required when there are several")
    new, nplan, sid = ch.add(cfg, plan, {"carrier": "mux a", "name": "Cinq", "source": "https://example.org/live.m3u8",
                                         "backup": "/srv/backup.ts", "ytdlp_format": "b"})
    ch.persist(path, cfg, new)
    cfg, plan = load(path)
    c = ch.find(plan, "Cinq")
    check(c.carrier.index == 0 and c.sid == 0x102 and c.lcn == 2, f"added: carrier {c.carrier.index}, "
          f"sid {c.sid:#x} (the free one), number {c.lcn}")
    raw = tomllib.load(open(path, "rb"))["carrier"][0]["service"][-1]
    check(raw.get("ytdlp_format") == "b" and raw["name"] == "Cinq", "options written")
    check(open(path).read().index("Cinq") < open(path).read().index("Mux B"), "added inside its multiplex")

    # rename, change of source, backup removed
    new, nplan, sid = ch.update(cfg, plan, "Cinq", {"name": "Cinq HD", "source": "udp://239.1.1.5:1234",
                                                     "backup": ""})
    ch.persist(path, cfg, new)
    cfg, plan = load(path)
    c = ch.find(plan, "Cinq HD")
    check(c.source == "udp://239.1.1.5:1234" and c.backup == "", "renamed, new source, no backup")

    # renumber: refused when taken, swapped on demand
    try:
        ch.update(cfg, plan, "Cinq HD", {"lcn": 1})
        check(False, "taken number refused")
    except ch.ChannelError as e:
        check("swap" in str(e), f"taken number refused ({e})")
    new, nplan, sid = ch.update(cfg, plan, "Cinq HD", {"lcn": 1, "swap": True})
    ch.persist(path, cfg, new)
    cfg, plan = load(path)
    check(ch.find(plan, "Cinq HD").lcn == 1 and ch.find(plan, "Un").lcn == 2, "numbers swapped")

    # validation by the configuration builder
    for bad, what in (({"name": 'a"b', "source": "x.ts"}, "quote in a name"),
                      ({"name": "X", "source": "x.ts", "lcn": 9, "sid": 0x201}, "sid already used"),
                      ({"name": "X", "source": "exec:rm -rf /"}, "exec: source from the API"),
                      ({"name": "X", "source": "x.ts", "ffmpeg_output": ["-y", "/etc/x"]}, "raw ffmpeg arguments from the API")):
        try:
            ch.add(cfg, plan, dict(bad, carrier=0), allow_exec=False)
            check(False, f"{what} refused")
        except ch.ChannelError as e:
            check(True, f"{what} refused ({e})")

    # hand-edited file meanwhile: not overwritten
    with open(path, "a") as f:
        f.write('\n')
    before = open(path).read().replace('name = "Quatre"', 'name = "Quatre bis"')
    with open(path, "w") as f:
        f.write(before)
    new, nplan, sid = ch.update(cfg, plan, "Un", {"name": "Un bis"})
    try:
        ch.persist(path, cfg, new)
        check(False, "file edited meanwhile: refused")
    except ch.ChannelError:
        check(open(path).read() == before, "file edited meanwhile: left untouched")
    print("all checks passed" if not FAILS else f"FAILED: {len(FAILS)}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
