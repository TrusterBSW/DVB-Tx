"""Load of the whole machine for the dashboard gauges: CPU of all its cores, and each graphics card,
whoever uses them (dvbtx or not), sampled every few seconds in the background.

CPU: /proc/stat. Intel and AMD cards: the busy time of each engine (render, video...) summed over
every process that has the card open (/proc/PID/fdinfo, drm-engine-*); the card's load is its
busiest engine. NVIDIA: nvidia-smi (GPU and encoder utilisation)."""

import glob
import os
import re
import shutil
import subprocess
import threading
import time

EVERY_S = 2.0


class SystemLoad:
    def __init__(self, devices_fn):
        self.devices_fn = devices_fn          # () -> gpu.detect()-like list of cards
        self.cpu = None
        self.cores = os.cpu_count() or 1
        self.gpus = []
        self._prev_cpu = None
        self._prev_eng = {}
        self._stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def stop(self):
        self._stop = True

    def _run(self):
        while not self._stop:
            try:
                self._sample()
            except Exception:                 # noqa: BLE001 - a gauge must never stop the TV
                pass
            time.sleep(EVERY_S)

    def _sample(self):
        with open("/proc/stat") as f:
            v = [int(x) for x in f.readline().split()[1:]]
        idle, total = v[3] + (v[4] if len(v) > 4 else 0), sum(v[:8])
        if self._prev_cpu:
            dt = total - self._prev_cpu[1]
            self.cpu = round(100 * (1 - (idle - self._prev_cpu[0]) / dt), 1) if dt > 0 else self.cpu
        self._prev_cpu = (idle, total)
        devs = [d for d in (self.devices_fn() or []) if d.get("usable")]
        drm = self._drm_engines({d["pci"] for d in devs if d["vendor"] in ("intel", "amd")})
        now = time.monotonic()
        out = []
        for d in devs:
            if d["vendor"] == "nvidia":
                out.append(dict(_nvidia(d), id=d["id"], name=d["name"]))
                continue
            eng = drm.get(d["pci"], {})
            prev = self._prev_eng.get(d["pci"])
            busy = {}
            if prev:
                dt = now - prev[0]
                busy = {k: min(100.0, round(100 * (ns - prev[1].get(k, ns)) / 1e9 / dt, 1)) for k, ns in eng.items()} if dt > 0 else {}
            self._prev_eng[d["pci"]] = (now, eng)
            out.append({"id": d["id"], "name": d["name"], "busy": max(busy.values()) if busy else None, "engines": busy})
        self.gpus = out

    @staticmethod
    def _drm_engines(pcis):
        """{pci: {engine: busy ns}} summed over every DRM client of the machine"""
        out = {p: {} for p in pcis}
        if not pcis:
            return out
        for fdinfo in glob.glob("/proc/[0-9]*/fdinfo"):
            pid = fdinfo.split("/")[2]
            try:
                fds = os.listdir(f"/proc/{pid}/fd")
            except OSError:
                continue
            seen = set()
            for fd in fds:
                try:
                    if not os.readlink(f"/proc/{pid}/fd/{fd}").startswith("/dev/dri/"):
                        continue
                    txt = open(f"{fdinfo}/{fd}").read()
                except OSError:
                    continue
                pdev = re.search(r"drm-pdev:\s*(\S+)", txt)
                cid = re.search(r"drm-client-id:\s*(\d+)", txt)
                if not pdev or pdev.group(1) not in out or not cid or (pdev.group(1), cid.group(1)) in seen:
                    continue
                seen.add((pdev.group(1), cid.group(1)))      # a client may hold several fds
                acc = out[pdev.group(1)]
                for name, ns in re.findall(r"drm-engine-([\w-]+):\s*(\d+) ns", txt):
                    acc[name] = acc.get(name, 0) + int(ns)
        return out

    def status(self):
        return {"cpu": self.cpu, "cores": self.cores, "gpus": self.gpus}


def _nvidia(d):
    if not shutil.which("nvidia-smi"):
        return {"busy": None, "engines": {}}
    try:
        r = subprocess.run(["nvidia-smi", f"--id={d['pci']}", "--query-gpu=utilization.gpu,utilization.encoder,utilization.decoder",
                            "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=5)
        g, e, dec = (float(x) for x in r.stdout.strip().split(",")[:3])
        return {"busy": max(g, e, dec), "engines": {"gpu": g, "encoder": e, "decoder": dec}}
    except (OSError, ValueError, subprocess.SubprocessError):
        return {"busy": None, "engines": {}}
