"""Engine test: services and inputs added, changed and removed while it runs.

A "ts" carrier (no radio) sends its multiplex to UDP; looping ffmpeg sources (stream copy of small
clips made here, little CPU) feed the inputs. Checks on the output, packet by packet: the PAT lists
the services after each change (new version), the services already there are not disturbed, and a
change of source (make before break) leaves no gap.

usage: python3 tests/live_channels_test.py [engine binary]
"""
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "engine", "build", "dvbtx-engine")
BASE = 23100                     # inputs on BASE + id, output on BASE + 99
OUT = BASE + 99
TS_RATE = 12_000_000


class Output(threading.Thread):
    """Receives the multiplex: PAT content, PMT PIDs, arrival times of every packet per PID."""

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        self.sock.bind(("127.0.0.1", OUT))
        self.lock = threading.Lock()
        self.pat = {}            # program_number -> PMT PID
        self.pat_version = None
        self.pat_versions = []
        self.last = {}           # PID -> last arrival time
        self.gap = {}            # PID -> longest silence since reset
        self.count = {}
        self.cc = {}
        self.cc_errors = {}
        self.stop = False

    def reset(self):
        with self.lock:
            self.gap, self.count, self.cc_errors = {}, {}, {}

    def run(self):
        while not self.stop:
            r, _, _ = select.select([self.sock], [], [], 0.2)
            if not r:
                continue
            data = self.sock.recv(65536)
            now = time.monotonic()
            with self.lock:
                for off in range(0, len(data) - 187, 188):
                    self.packet(data[off:off + 188], now)

    def packet(self, p, now):
        if p[0] != 0x47:
            return
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid == 0x1FFF:
            return
        if pid in self.last:
            self.gap[pid] = max(self.gap.get(pid, 0), now - self.last[pid])
        self.last[pid] = now
        self.count[pid] = self.count.get(pid, 0) + 1
        if p[3] & 0x10:
            cc = p[3] & 0x0F
            if pid in self.cc and cc != (self.cc[pid] + 1) & 15:
                self.cc_errors[pid] = self.cc_errors.get(pid, 0) + 1
            self.cc[pid] = cc
        if pid == 0 and p[1] & 0x40:
            s = p[5 + p[4]:]
            n = ((s[1] & 0x0F) << 8) | s[2]
            ver = (s[5] >> 1) & 0x1F
            progs = {}
            for i in range(8, 3 + n - 4, 4):
                prog = (s[i] << 8) | s[i + 1]
                if prog:
                    progs[prog] = ((s[i + 2] & 0x1F) << 8) | s[i + 3]
            if ver != self.pat_version:
                self.pat_versions.append(ver)
            self.pat_version, self.pat = ver, progs


class Engine:
    def __init__(self, ini):
        self.p = subprocess.Popen([ENGINE, ini], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.replies, self.stats, self.log = {}, None, []
        self.seq = 0
        self.cv = threading.Condition()
        threading.Thread(target=self._out, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()

    def _out(self):
        for line in self.p.stdout:
            try:
                d = json.loads(line)
            except ValueError:
                continue
            with self.cv:
                if d.get("type") == "reply":
                    self.replies[d["seq"]] = d
                elif d.get("type") == "stats":
                    self.stats = d
                self.cv.notify_all()

    def _err(self):
        for line in self.p.stderr:
            self.log.append(line.rstrip())

    def cmd(self, text, timeout=5):
        self.seq += 1
        seq = self.seq
        self.p.stdin.write(f"seq={seq} {text}\n")
        self.p.stdin.flush()
        with self.cv:
            self.cv.wait_for(lambda: seq in self.replies, timeout)
        return self.replies.get(seq, {"ok": False, "error": "no reply"})

    def service(self, sid):
        st = self.stats or {}
        for c in st.get("carriers", []):
            for s in c.get("services", []):
                if s["sid"] == sid:
                    return s
        return None

    def input_ids(self):
        return sorted(i["index"] for i in (self.stats or {}).get("inputs", []))

    def stop(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
            self.p.wait(5)
        except (OSError, subprocess.TimeoutExpired):
            self.p.kill()


def source(clip, port):
    return subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-re", "-stream_loop", "-1",
                             "-i", clip, "-c", "copy", "-f", "mpegts", f"udp://127.0.0.1:{port}?pkt_size=1316"],
                            stdin=subprocess.DEVNULL)


def make_clip(path, colour, freq):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                    f"testsrc2=size=320x180:rate=25,hue=h={colour}", "-f", "lavfi", "-i", f"sine=frequency={freq}",
                    "-t", "6", "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-b:v", "400k",
                    "-c:a", "aac", "-b:a", "64k", "-f", "mpegts", path], check=True)


FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def wait_for(pred, timeout):
    t = time.monotonic() + timeout
    while time.monotonic() < t:
        if pred():
            return True
        time.sleep(0.1)
    return False


def main():
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg needed")
    tmp = tempfile.mkdtemp(prefix="dvbtx-live-")
    clips = [os.path.join(tmp, f"{n}.ts") for n in "abcd"]
    for i, c in enumerate(clips):
        make_clip(c, i * 90, 400 + 200 * i)
    ini = os.path.join(tmp, "engine.ini")
    with open(ini, "w") as f:
        f.write(f"""[engine]
log_level = info
stats_interval_ms = 500
[input 0]
name = A
addr = 127.0.0.1:{BASE}
[input 1]
name = backup
addr = 127.0.0.1:{BASE + 1}
[carrier 0]
standard = ts
tsid = 1
ts_rate = {TS_RATE}
output = udp://127.0.0.1:{OUT}
[service 0]
carrier = 0
name = Un
sid = 257
pid_base = 256
primary = 0
backup = 1
""")
    out = Output()
    out.start()
    procs = [source(clips[0], BASE), source(clips[1], BASE + 1)]
    eng = Engine(ini)
    try:
        check(wait_for(lambda: out.pat.get(257) == 256 and (eng.service(257) or {}).get("source") == "primary", 10),
              "start: service 257 on its primary, in the PAT")
        v0 = out.pat_version

        # add an input and a service
        r = eng.cmd(f"input add id=2 addr=127.0.0.1:{BASE + 2} name=C")
        check(r["ok"], f"input add: {r}")
        procs.append(source(clips[2], BASE + 2))
        out.reset()
        r = eng.cmd("service add carrier=0 sid=258 pid_base=288 primary=2 backup=1 name=Deux")
        check(r["ok"], f"service add: {r}")
        check(wait_for(lambda: out.pat.get(258) == 288, 3), "PAT lists the new service")
        check(out.pat_version != v0, f"PAT version changed ({v0} -> {out.pat_version})")
        check(wait_for(lambda: (eng.service(258) or {}).get("source") == "primary", 8), "new service on its primary")
        check(wait_for(lambda: out.count.get(289, 0) > 100, 5), "new service's video flows")
        time.sleep(2)
        with out.lock:
            g = out.gap.get(257, 9)
        check(g < 0.5, f"service 257 undisturbed by the addition (longest video gap {g * 1000:.0f} ms)")

        # errors
        check(not eng.cmd("service add carrier=0 sid=258 pid_base=320 primary=2 name=X")["ok"], "duplicate sid refused")
        check(not eng.cmd("service add carrier=0 sid=259 pid_base=300 primary=2 name=X")["ok"], "overlapping PIDs refused")
        check(not eng.cmd("service add carrier=0 sid=259 pid_base=320 primary=9 name=X")["ok"], "unknown input refused")
        check(not eng.cmd(f"input add id=2 addr=127.0.0.1:{BASE + 5} name=X")["ok"], "existing input id refused")

        # change of source, make before break
        r = eng.cmd(f"input add id=3 addr=127.0.0.1:{BASE + 3} name=D")
        check(r["ok"], f"input add D: {r}")
        out.reset()
        r = eng.cmd("service set carrier=0 sid=257 primary=3")
        check(r["ok"], f"service set primary: {r}")
        time.sleep(1.5)
        check((eng.service(257) or {}).get("inputs", [None])[0] == 0, "old source kept while the new one is silent")
        procs.append(source(clips[3], BASE + 3))
        check(wait_for(lambda: (eng.service(257) or {}).get("inputs", [None])[0] == 3, 8), "new source in use once it delivers")
        r = eng.cmd("input del id=0")
        check(r["ok"], f"input del of the old source: {r}")
        time.sleep(2)
        with out.lock:
            g = out.gap.get(257, 9)      # its video (the audio of these clips comes in 350 ms bursts anyway)
        check(g < 0.2, f"no gap in the video while its source changed (longest {g * 1000:.0f} ms)")
        check((eng.service(257) or {}).get("source") == "primary", "still on primary (no detour by the backup)")
        check(wait_for(lambda: 0 not in eng.input_ids(), 5), "old input freed")
        check(not eng.cmd("service set carrier=0 sid=257 primary=0")["ok"], "removed input no longer usable")

        # rename, then remove a service
        check(eng.cmd("service set carrier=0 sid=258 name=Deux bis")["ok"], "rename")
        check(wait_for(lambda: (eng.service(258) or {}).get("name") == "Deux bis", 3), "new name in the statistics")
        v1 = out.pat_version
        out.reset()
        r = eng.cmd("service del carrier=0 sid=258")
        check(r["ok"], f"service del: {r}")
        check(wait_for(lambda: 258 not in out.pat and out.pat_version != v1, 3), "PAT without the removed service")
        check(eng.cmd("input del id=2")["ok"], "input del of its source")
        check(wait_for(lambda: 2 not in eng.input_ids() and eng.service(258) is None, 5), "service and input gone")
        backup_refs_ok = 1 in eng.input_ids()
        check(backup_refs_ok, "shared backup input kept")
        time.sleep(1)
        with out.lock:
            g = out.gap.get(257, 9)
            ccerr = sum(out.cc_errors.values())
        check(g < 0.5, f"service 257 undisturbed by the removal (longest video gap {g * 1000:.0f} ms)")
        check(ccerr == 0, f"no continuity error in the output ({ccerr})")

        # the backup takes over when the primary stops, the PMT slot of the removed service is reused
        r = eng.cmd(f"input add id=4 addr=127.0.0.1:{BASE + 4} name=E")
        check(r["ok"] and eng.cmd("service add carrier=0 sid=260 pid_base=288 primary=4 backup=1 name=Trois")["ok"],
              "service added in the place of the removed one")
        check(wait_for(lambda: (eng.service(260) or {}).get("source") == "backup", 8),
              "service without a running source goes to the backup")
        check(eng.p.poll() is None, "engine still running")
    finally:
        out.stop = True
        for p in procs:
            p.kill()
        eng.stop()
        log = [l for l in eng.log if " ERR " in l or " WRN " in l]
        if log:
            print("engine warnings:\n  " + "\n  ".join(log[-10:]))
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"{'FAILED: ' + str(len(FAILS)) if FAILS else 'all checks passed'}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
