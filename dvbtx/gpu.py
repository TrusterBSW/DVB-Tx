"""GPUs for the transcodes of the ingest: found at every start, learnt when they change, chosen per input.

Detection (instant): the display adapters of the PCI bus, their vendor (Intel, AMD, NVIDIA), kernel
driver and render node; an adapter without a usable driver is listed with the reason (no driver,
passed through to a VM, nouveau without NVENC).

Learning: for each usable adapter, every way ffmpeg can use it (VAAPI with each installed VA driver,
with or without the low-power encoder; QSV; NVENC; the whole chain on the GPU, decoding on it with
the frames copied to memory, or only the encoding on it) transcodes a 1080p H.264 test clip to 720p at a
capped bit rate, for each codec (H.264, HEVC, MPEG-2). A way counts if ffmpeg succeeds, the output
has the codec and size asked for, and its bit rate stays under the cap: a multiplex takes any rate
under a channel's share, not above it, so an encoder without rate control (constant quality only,
Intel without its HuC firmware) is refused. The speed
(frames per second) ranks them.

The results are kept with a fingerprint of the adapters, their drivers and ffmpeg: the learning
runs again (in the background, the transcodes use the CPU meanwhile) only when that fingerprint
changes, or when asked (`dvbtx gpu learn`, button of the web page).
"""

import glob
import hashlib
import json
import logging
import os
import re
import subprocess
import tempfile
import threading
import time

log = logging.getLogger("dvbtx.gpu")

VENDORS = {"0x8086": "intel", "0x1002": "amd", "0x10de": "nvidia"}
# codec of the configuration -> family the GPU encoders are named after
FAMILIES = {"libx264": "h264", "h264": "h264", "libx265": "hevc", "hevc": "hevc", "mpeg2video": "mpeg2",
            "mpeg2": "mpeg2"}
PROBE_CODEC = {"h264": "h264", "hevc": "hevc", "mpeg2": "mpeg2video"}
TEST_RATE = {"h264": 4_000_000, "hevc": 3_000_000, "mpeg2": 6_000_000}
TEST_SECONDS = 8
LEARN_VERSION = 3               # a change of the tests makes every machine learn again
CPU_VALUES = ("off", "none", "cpu", "no", "false")

MANAGER = None          # set by the supervisor: the ingest asks it for a GPU


def _read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


_PCI_NAMES = {}


def _pci_name(vendor, device):
    """name of a PCI device from pci.ids (the vendor name if the device is not listed)"""
    key = (vendor, device)
    if key in _PCI_NAMES:
        return _PCI_NAMES[key]
    name = ""
    for path in ("/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids", "/usr/share/pci.ids"):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                in_vendor = False
                for line in f:
                    if not line.strip() or line.startswith("#"):
                        continue
                    if not line.startswith("\t"):
                        if in_vendor:
                            break
                        in_vendor = line[:4].lower() == vendor
                        if in_vendor:
                            name = line[4:].strip()
                    elif in_vendor and not line.startswith("\t\t") and line[1:5].lower() == device:
                        name = line[5:].strip()
                        break
            break
        except OSError:
            continue
    _PCI_NAMES[key] = name
    return name


def detect():
    """display adapters: [{id, vendor, name, pci, pci_id, driver, render, cuda, usable, reason}]"""
    devs = []
    nvidia = 0
    for p in sorted(glob.glob("/sys/bus/pci/devices/*")):
        if not _read(os.path.join(p, "class")).startswith("0x03"):
            continue
        vid, did = _read(os.path.join(p, "vendor")), _read(os.path.join(p, "device"))
        vendor = VENDORS.get(vid, "other")
        drv_link = os.path.join(p, "driver")
        drv = os.path.basename(os.path.realpath(drv_link)) if os.path.exists(drv_link) else ""
        render = sorted(glob.glob(os.path.join(p, "drm", "renderD*")))
        d = {"vendor": vendor, "name": _pci_name(vid[2:], did[2:]) or f"{vendor} {did}", "pci": os.path.basename(p),
             "pci_id": f"{vid[2:]}:{did[2:]}", "driver": drv,
             "driver_version": _read(f"/sys/module/{drv}/version") if drv else "",
             "render": "/dev/dri/" + os.path.basename(render[0]) if render else "", "cuda": None,
             "usable": False, "reason": ""}
        if vendor == "nvidia":
            # CUDA numbers the NVIDIA cards in PCI order with CUDA_DEVICE_ORDER=PCI_BUS_ID (set for ffmpeg)
            d["cuda"] = nvidia
            nvidia += 1
        if not drv:
            d["reason"] = "no driver loaded (blacklisted?)"
        elif drv == "vfio-pci":
            d["reason"] = "given to a virtual machine (vfio-pci)"
        elif vendor == "nvidia" and drv != "nvidia":
            d["reason"] = f"driver {drv}: NVENC needs the NVIDIA driver"
        elif vendor in ("intel", "amd") and not d["render"]:
            d["reason"] = f"driver {drv} without a render node"
        elif vendor == "other":
            d["reason"] = "unknown vendor"
        else:
            d["usable"] = True
        d["id"] = f"cuda:{d['cuda']}" if vendor == "nvidia" else (d["render"] or d["pci"])
        devs.append(d)
    return devs


def _va_drivers():
    """VA-API drivers installed (names for LIBVA_DRIVER_NAME)"""
    names = set()
    for pat in ("/usr/lib/*/dri/*_drv_video.so", "/usr/lib/dri/*_drv_video.so", "/usr/lib64/dri/*_drv_video.so",
                "/usr/lib/*/dri-nonfree/*_drv_video.so"):
        for f in glob.glob(pat):
            names.add(os.path.basename(f)[:-len("_drv_video.so")])
    return sorted(names)


def _ffmpeg_version(ffmpeg):
    try:
        r = subprocess.run([ffmpeg, "-hide_banner", "-version"], capture_output=True, text=True, timeout=10)
        return (r.stdout.splitlines() or [""])[0]
    except (OSError, subprocess.SubprocessError):
        return ""


def fingerprint(devs, ffmpeg):
    """what the learning depends on: adapters, drivers (and their firmware options), VA drivers, ffmpeg"""
    parts = [[d["pci"], d["pci_id"], d["driver"], d["driver_version"], d["render"]] for d in devs]
    params = {m: _read(f"/sys/module/{m}/parameters/enable_guc") for m in ("i915", "xe")}
    doc = {"learning": LEARN_VERSION, "devs": parts, "va": _va_drivers(), "ffmpeg": _ffmpeg_version(ffmpeg), "kernel": os.uname().release,
           "nvidia": (_read("/proc/driver/nvidia/version").splitlines() or [""])[0], "guc": params}
    return hashlib.sha1(json.dumps(doc, sort_keys=True).encode()).hexdigest()[:16]


class Way:
    """one way of using one adapter for one codec family"""

    # mode: "full" decoding, filters and encoding on the GPU, the frames never leave it; "dec" decoded on
    # the GPU and copied to memory (a change of the input, such as a file looping, only re-creates
    # filters on frames in memory: ffmpeg 7.1 cannot do it on GPU frames), filters and encoding on it;
    # "enc" decoding and filters on the CPU, only the encoding on the GPU
    MODES = {"full": "GPU decode+scale", "dec": "GPU decode (copied), GPU scale", "enc": "CPU decode, GPU encode"}

    def __init__(self, dev, api, mode, va=None, low_power=False):
        self.dev, self.api, self.mode, self.va, self.low_power = dev, api, mode, va, low_power

    @property
    def key(self):
        return ":".join([self.api, self.dev["id"], self.mode, self.va or "-",
                         "lp" if self.low_power else "-"])

    @classmethod
    def from_key(cls, key, devs):
        parts = key.split(":")          # the device id may contain ':' (cuda:0)
        api, mode, va, lp = parts[0], parts[-3], parts[-2], parts[-1]
        dev_id = ":".join(parts[1:-3])
        dev = next((d for d in devs if d["id"] == dev_id), None)
        if dev is None:
            return None
        if mode not in cls.MODES:
            return None
        return cls(dev, api, mode, None if va == "-" else va, lp == "lp")

    def label(self):
        """for the web page and the logs: 'Intel UHD 630 (VAAPI iHD, whole chain)'"""
        api = {"vaapi": "VAAPI", "qsv": "QSV", "nvenc": "NVENC"}[self.api]
        if self.va:
            api += f" {self.va}"
        if self.low_power:
            api += " low-power"
        return f"{self.dev['name']} ({api}, {self.MODES[self.mode]})"

    def env(self):
        e = {}
        if self.va:
            e["LIBVA_DRIVER_NAME"] = self.va
        if self.api == "nvenc":
            e["CUDA_DEVICE_ORDER"] = "PCI_BUS_ID"
        return e

    def input_args(self):
        """options before the -i of the video input (and global device options)"""
        r = self.dev["render"]
        if self.api == "vaapi":
            if self.mode == "full":
                return ["-hwaccel", "vaapi", "-hwaccel_device", r, "-hwaccel_output_format", "vaapi"]
            if self.mode == "dec":
                return ["-init_hw_device", f"vaapi=va:{r}", "-hwaccel", "vaapi", "-hwaccel_device", "va",
                        "-filter_hw_device", "va"]
            return ["-vaapi_device", r]
        if self.api == "qsv":
            return ["-init_hw_device", f"vaapi=va:{r}", "-init_hw_device", "qsv=qs@va", "-filter_hw_device", "qs"]
        n = str(self.dev["cuda"])
        if self.mode == "full":
            return ["-init_hw_device", f"cuda=cu:{n}", "-filter_hw_device", "cu", "-hwaccel", "cuda",
                    "-hwaccel_device", "cu", "-hwaccel_output_format", "cuda"]
        if self.mode == "dec":
            return ["-init_hw_device", f"cuda=cu:{n}", "-filter_hw_device", "cu", "-hwaccel", "cuda",
                    "-hwaccel_device", "cu"]
        return []

    def filters(self, deinterlace=False, size=None, fps=None):
        """video filter chain (size: (w, h) or None); frames in memory are uploaded (hwupload lets frames
        already on the GPU through, so a codec the GPU does not decode still works)"""
        vf = []
        gpu_filters = self.mode in ("full", "dec")
        if gpu_filters and self.api == "vaapi":
            vf += ["format=nv12|vaapi", "hwupload"]
            if deinterlace:
                vf.append("deinterlace_vaapi")
            if size:
                w, h = size
                vf.append(f"scale_vaapi=w={w}:h={h}:force_original_aspect_ratio=decrease:format=nv12")
                vf.append(f"pad_vaapi=w={w}:h={h}:x=(ow-iw)/2:y=(oh-ih)/2")
            else:
                vf.append("scale_vaapi=format=nv12")
        elif gpu_filters and self.api == "nvenc":
            vf += ["format=nv12|cuda", "hwupload"]
            if deinterlace:
                vf.append("yadif_cuda")
            if size:
                w, h = size
                vf.append(f"scale_cuda=w={w}:h={h}:force_original_aspect_ratio=decrease:force_divisible_by=2")
        else:
            if deinterlace:
                vf.append("yadif")
            if size:
                w, h = size
                vf.append(f"scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,setsar=1")
            vf.append("format=nv12")
            if self.api == "vaapi":
                vf.append("hwupload")
            elif self.api == "qsv":
                vf += ["hwupload=extra_hw_frames=64", "format=qsv"]
        if fps:
            vf.append(f"fps={fps}")
        return vf

    def encoder(self, family):
        return f"{family}_{'nvenc' if self.api == 'nvenc' else self.api}"

    def rate_args(self):
        """options for a capped variable bit rate, like libx264's (-b:v / -maxrate / -bufsize given by the
        caller): the multiplex takes any rate under its share, not one above it"""
        if self.api == "vaapi":
            return ["-rc_mode", "VBR"] + (["-low_power", "1"] if self.low_power else [])
        if self.api == "qsv":
            return ["-look_ahead", "0"]
        return ["-preset", "p4", "-rc", "vbr"]


def ways(dev, family, va_drivers):
    """every way worth trying for one usable adapter and codec family"""
    out = []
    if dev["vendor"] in ("intel", "amd") and dev["render"]:
        vas = [v for v in (("iHD", "i965") if dev["vendor"] == "intel" else ("radeonsi",)) if v in va_drivers] or [None]
        for va in vas:
            for lp in ((False, True) if dev["vendor"] == "intel" and family != "mpeg2" else (False,)):
                out += [Way(dev, "vaapi", m, va, lp) for m in Way.MODES]
        if dev["vendor"] == "intel":
            out.append(Way(dev, "qsv", "enc"))
    if dev["vendor"] == "nvidia" and dev["driver"] == "nvidia" and family in ("h264", "hevc"):
        out += [Way(dev, "nvenc", m) for m in Way.MODES]
    return out


def _test_clip(ffmpeg, folder):
    # like a broadcast: High profile, CABAC, B frames (an ultrafast clip decodes too cheaply on the CPU
    # and hides what decoding on the GPU saves)
    path = os.path.join(folder, "gpu-test-1080p-high.ts")
    if not os.path.exists(path):
        tmp = path + ".tmp"
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                        "testsrc2=size=1920x1080:rate=25", "-t", str(TEST_SECONDS), "-c:v", "libx264", "-preset",
                        "veryfast", "-profile:v", "high", "-bf", "2", "-b:v", "8M", "-pix_fmt", "yuv420p", "-g", "25",
                        "-f", "mpegts", tmp], check=True, capture_output=True, timeout=300)
        os.replace(tmp, path)
    return path


def _probe(ffprobe, path):
    r = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries",
                        "stream=codec_name,width,height,nb_read_packets", "-of", "json", path],
                       capture_output=True, text=True, timeout=30)
    s = (json.loads(r.stdout or "{}").get("streams") or [{}])[0]
    return s


def test(way, family, clip, ffmpeg, ffprobe, folder):
    """one test transcode: {"ok", "fps", "error"}"""
    target = TEST_RATE[family]
    out = os.path.join(folder, "gpu-test-out.ts")
    vf = way.filters(size=(1280, 720))
    # the clip twice (-stream_loop 1): a source that loops or changes re-creates the filters, a way that
    # breaks there (ffmpeg 7.1 with the frames kept on the GPU) is refused
    cmd = ([ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-stream_loop", "1"] + way.input_args() +
           ["-i", clip, "-an", "-vf", ",".join(vf), "-c:v", way.encoder(family)] + way.rate_args() +
           ["-b:v", str(target), "-maxrate", str(target), "-bufsize", str(target // 2), "-g", "25",
            "-f", "mpegts", out])
    env = dict(os.environ, **way.env())
    # the speed is measured from ffmpeg's progress reports once frames come out: the start of the
    # device (CUDA: a second or two) would otherwise weigh on a short clip
    cmd[1:1] = ["-progress", "pipe:1", "-stats_period", "0.5"]
    t0 = time.monotonic()
    marks = []                          # (time, frames) of each progress report
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        err_lines = []
        threading.Thread(target=lambda: err_lines.extend(p.stderr), daemon=True).start()
        for line in p.stdout:
            if line.startswith("frame="):
                marks.append((time.monotonic(), int(line.split("=", 1)[1] or 0)))
            if time.monotonic() - t0 > 60:
                p.kill()
        _, status, ru = os.wait4(p.pid, 0)            # its own CPU time (the GPU should spare it)
        rc = p.returncode = os.waitstatus_to_exitcode(status)
        cpu_s = ru.ru_utime + ru.ru_stime
    except (OSError, subprocess.SubprocessError) as e:
        return {"ok": False, "error": str(e)}
    t_end = time.monotonic()
    if t_end - t0 > 60:
        return {"ok": False, "error": "too slow (more than 60 s)"}
    stderr = "".join(err_lines)
    if rc:
        lines = [l.strip() for l in err_lines if l.strip() and "Terminating thread" not in l and "Task finished" not in l]
        err = lines[0] if lines else f"ffmpeg exit {rc}"
        if rc < 0 or "Assertion" in stderr:
            err = (re.search(r"Assertion .*", stderr) or re.search(r".+", err)).group(0)
        return {"ok": False, "error": re.sub(r"^\[[^\]]*@ 0x[0-9a-f]+\]\s*", "", err)[:200]}
    try:
        s = _probe(ffprobe, out)
        pk = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "packet=pts_time,size",
                             "-of", "csv=p=0", out], capture_output=True, text=True, timeout=30).stdout
        pkts = sorted((float(t), int(z)) for t, z in (l.split(",")[:2] for l in pk.splitlines() if l.count(",") >= 1)
                      if t not in ("", "N/A"))
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        return {"ok": False, "error": f"output unreadable: {e}"}
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass
    if s.get("codec_name") != PROBE_CODEC[family]:
        return {"ok": False, "error": f"output codec {s.get('codec_name')}"}
    w, h = int(s.get("width") or 0), int(s.get("height") or 0)
    if not (1200 <= w <= 1280 and 660 <= h <= 720):
        return {"ok": False, "error": f"output size {w}x{h}"}
    frames = int(s.get("nb_read_packets") or 0)
    # the rate must stay under its cap (an encoder without rate control, constant quality only, ignores
    # it): the average, and the busiest second
    rate = sum(z for _, z in pkts) * 8 / (2 * TEST_SECONDS)
    peak, j, acc = 0, 0, 0
    for t, z in pkts:
        acc += z
        while pkts[j][0] <= t - 1.0:
            acc -= pkts[j][1]
            j += 1
        peak = max(peak, acc * 8)
    if rate > 1.25 * target or peak > 1.75 * target or rate < 0.05 * target:
        return {"ok": False, "error": f"bit rate not held: {rate / 1e6:.1f} Mbit/s on average, {peak / 1e6:.1f} at most "
                                      f"for a cap of {target / 1e6:.0f}"}
    # a GOP or so is lost where the clip loops (B frames), even on the CPU; a way that stops there gives half
    if frames < 2 * TEST_SECONDS * 25 * 0.85:
        return {"ok": False, "error": f"{frames} frames out of {2 * TEST_SECONDS * 25} (stopped where the clip loops?)"}
    first = next(((t, f) for t, f in marks if f > 0), None)
    if first and frames - first[1] >= 25 and t_end - first[0] > 0.2:
        fps = (frames - first[1]) / (t_end - first[0])
    else:
        fps = frames / (t_end - t0)
    return {"ok": True, "fps": round(fps, 1), "rate": round(rate), "cpu_ms": round(1000 * cpu_s / max(1, frames), 2)}


def learn(devs, ffmpeg, ffprobe, folder, progress=None):
    """tests every way of every usable adapter: {dev id: {family: [{"way", "label", ok, fps, error}]}}"""
    os.makedirs(folder, exist_ok=True)
    clip = _test_clip(ffmpeg, folder)
    va = _va_drivers()
    res = {}
    for d in devs:
        if not d["usable"]:
            continue
        res[d["id"]] = {}
        for fam in ("h264", "hevc", "mpeg2"):
            rows = []
            for w in ways(d, fam, va):
                r = test(w, fam, clip, ffmpeg, ffprobe, folder)
                r.update(way=w.key, label=w.label())
                rows.append(r)
                if progress:
                    progress(d, fam, r)
            res[d["id"]][fam] = rank(rows)
    return res


def rank(rows):
    """the way used first: the one that costs the CPU least, among those at least half as fast as the
    fastest (and 2x real time at 25 frames/s); then by speed"""
    ok = [r for r in rows if r.get("ok")]
    if not ok:
        return rows
    top = max(r["fps"] for r in ok)
    fast = [r for r in ok if r["fps"] >= max(0.5 * top, 50)] or ok
    first = min(fast, key=lambda r: (r.get("cpu_ms", 0), -r["fps"]))
    rest = sorted((r for r in rows if r is not first), key=lambda r: (not r.get("ok"), -r.get("fps", 0)))
    return [first] + rest


def default_cache():
    for folder in ("/var/lib/dvbtx", os.path.expanduser("~/.cache/dvbtx")):
        try:
            os.makedirs(folder, exist_ok=True)
            if os.access(folder, os.W_OK):
                return os.path.join(folder, "gpu.json")
        except OSError:
            continue
    return os.path.join(tempfile.gettempdir(), "dvbtx-gpu.json")


def _why(fams):
    """the reason that says most about an adapter that cannot be used: the encoder's own refusal"""
    errs = [r.get("error", "") for rows in fams.values() for r in rows if not r.get("ok")]
    if not errs and not any(fams.values()):
        return "no encoder for this codec on this card"
    for pat in ("RC mode", "entrypoint", "bit rate", "MFX", "Assertion"):
        for e in errs:
            if pat in e:
                return e
    return errs[0] if errs else ""


class Manager:
    """the adapters, their learnt results, and the transcodes running on each"""

    def __init__(self, ing):
        self.mode = str(ing.get("gpu", "auto") if ing.get("gpu") is not False else "off").strip()
        self.ffmpeg = ing.get("ffmpeg", "ffmpeg")
        self.ffprobe = ing.get("ffprobe", "ffprobe")
        self.cache_path = ing.get("gpu_cache") or default_cache()
        self.folder = os.path.dirname(self.cache_path)
        self.devices = []
        self.results = {}
        self.fingerprint = ""
        self.learnt_at = None
        self.state = "off"
        self.progress = ""
        self.load = {}                  # device id -> transcodes running
        self.lock = threading.Lock()
        self.thread = None

    def allowed(self, dev_id):
        if self.mode.lower() in CPU_VALUES:
            return False
        if self.mode.lower() in ("", "auto", "on", "true", "yes"):
            return True
        return dev_id in [x.strip() for x in self.mode.split(",")]

    def start(self):
        """detection at start: the learning only when the adapters changed (in the background)"""
        if self.mode.lower() in CPU_VALUES:
            self.state = "off"
            return
        self.devices = detect()
        if not any(d["usable"] for d in self.devices):
            self.state = "none"
            for d in self.devices:
                log.info("GPU %s: not used (%s)", d["name"], d["reason"])
            return
        self.fingerprint = fingerprint(self.devices, self.ffmpeg)
        cached = self.cached()
        if cached.get("fingerprint") == self.fingerprint:
            self.results, self.learnt_at = cached.get("results", {}), cached.get("learnt_at")
            self.state = "ready"
            self._log_choice()
        else:
            log.info("GPU: %s: learning in the background (transcodes on the CPU meanwhile)",
                     "adapters, drivers or ffmpeg changed since the last learning" if cached else "never learnt")
            self.learn_async()

    def cached(self):
        try:
            with open(self.cache_path) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def save(self, results):
        self.results, self.learnt_at = results, time.time()
        try:
            tmp = self.cache_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"fingerprint": self.fingerprint, "learnt_at": self.learnt_at, "devices": self.devices,
                           "results": results}, f, indent=1)
            os.replace(tmp, self.cache_path)
        except OSError as e:
            log.warning("GPU: results not saved in %s: %s", self.cache_path, e)

    def learn_async(self):
        with self.lock:
            if self.state == "learning":
                return False
            self.state = "learning"
        self.thread = threading.Thread(target=self._learn, daemon=True)
        self.thread.start()
        return True

    def _learn(self):
        try:
            self.devices = detect()
            self.fingerprint = fingerprint(self.devices, self.ffmpeg)

            def prog(d, fam, r):
                self.progress = f"{d['name']} {fam}: {r['label']} {'%.0f img/s' % r['fps'] if r['ok'] else 'no'}"
            self.save(learn(self.devices, self.ffmpeg, self.ffprobe, self.folder, prog))
            self.state = "ready"
            self._log_choice()
        except Exception as e:                       # noqa: BLE001 - the TV goes on with the CPU
            log.error("GPU learning failed: %s", e)
            self.state = "error"
        finally:
            self.progress = ""

    def _log_choice(self):
        for d in self.devices:
            if not d["usable"]:
                log.info("GPU %s: not used (%s)", d["name"], d["reason"])
                continue
            fams = self.results.get(d["id"], {})
            best = {f: next((r for r in rows if r.get("ok")), None) for f, rows in fams.items()}
            ok = [f"{f} {b['fps']:.0f} img/s" for f, b in best.items() if b]
            if ok:
                log.info("GPU %s: %s (%s)", d["name"], ", ".join(ok), best.get("h264", {}).get("label", "") if best.get("h264") else "")
            else:
                why = _why(fams)
                log.warning("GPU %s: no way to encode under a capped bit rate (%s): transcodes on the CPU%s", d["name"],
                            why, "; Intel: the HuC firmware (options i915 enable_guc=2) or the non-free VA driver "
                            "(intel-media-va-driver-non-free) are often missing" if d["vendor"] == "intel" else "")

    def best(self, family, api=None):
        """[(fps, Way)] that work for that codec family, fastest first"""
        out = []
        for d in self.devices:
            if not d["usable"] or not self.allowed(d["id"]):
                continue
            for r in self.results.get(d["id"], {}).get(family, []):
                if r.get("ok") and (api is None or r["way"].startswith(api + ":")):
                    w = Way.from_key(r["way"], self.devices)
                    if w:
                        out.append((r["fps"], w))
                    break                 # rows are sorted: the fastest way of this adapter
        return sorted(out, key=lambda x: -x[0])

    def acquire(self, family, api=None, exclude=()):
        """a Way for one more transcode (None: the CPU): the adapter with the most speed left"""
        if self.state != "ready":
            return None
        with self.lock:
            cands = [(fps / (1 + self.load.get(w.dev["id"], 0)), w) for fps, w in self.best(family, api)
                     if w.key not in exclude]
            if not cands:
                return None
            w = max(cands, key=lambda x: x[0])[1]
            self.load[w.dev["id"]] = self.load.get(w.dev["id"], 0) + 1
            return w

    def release(self, way):
        if way is None:
            return
        with self.lock:
            n = self.load.get(way.dev["id"], 0)
            self.load[way.dev["id"]] = max(0, n - 1)

    def status(self):
        return {"mode": self.mode, "state": self.state, "progress": self.progress, "learnt_at": self.learnt_at,
                "cache": self.cache_path, "fingerprint": self.fingerprint,
                "devices": [dict(d, load=self.load.get(d["id"], 0),
                                 results=self.results.get(d["id"], {})) for d in self.devices]}
