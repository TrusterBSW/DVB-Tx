"""Previews of the channels for the web page, running only while someone looks at them.

Both read a copy of what goes on air, asked to the engine for the time of the preview ("monitor add
carrier=N addr=udp://127.0.0.1:PORT"), so they show what the TVs receive (backup clip included).

Thumbnails: every web.preview_thumb_every_s, while the page asks for them, one ffmpeg per multiplex
reads its copy just long enough to write one small JPEG per channel (-frames:v 1 of the first key frame,
-skip_frame nokey, decoded on the GPU when the transcodes have one) and stops: reading the whole
multiplex is what costs, not the decoding. They stop THUMB_IDLE_S after the last request.

Live: one ffmpeg per viewer of a channel writes fragmented MP4 straight into the HTTP answer, which
a <video> plays: the video copied when the browser can play it (H.264), else re-encoded small; the
sound in AAC. It stops when the viewer leaves.
"""

import logging
import os
import socket
import subprocess
import threading
import time

from . import gpu
from .ingest import _die_with_parent

log = logging.getLogger("dvbtx.preview")

THUMB_IDLE_S = 60           # thumbnails of a multiplex kept up to date that long after the last request
THUMB_SHOT_S = 30           # a shot still running after that: stopped (seen: 21 s for a key frame of a FAST channel)
VIDEO_STREAM_TYPES = {0x01, 0x02, 0x10, 0x1B, 0x24}
# ffmpeg stops probing once every stream is described; a source whose key frames (with the picture
# size) come seconds apart needs that long: give up sooner and ffmpeg ends without output
PROBE_US, PROBE_BYTES = "6000000", "40000000"
# decoder messages of a stream joined in the middle (until its first key frame): not worth a log line
NOISE = ("non-existing PPS", "no frame!", "decode_slice_header", "Last message repeated", "co located POCs")
H264 = 0x1B


def _hw_decode():
    """(input options, environment) to decode the thumbnails' key frames on the GPU the transcodes use,
    the frames copied back to memory for the small JPEGs: a 1080p key frame costs the CPU ~15 ms, for
    every channel of the multiplex. ([], {}): no usable adapter, the CPU (an unsupported codec also
    falls back to it)."""
    m = gpu.MANAGER
    if m is None or getattr(m, "state", "") != "ready":
        return [], {}
    for _, w in m.best("h264"):
        if w.api in ("vaapi", "qsv") and w.dev.get("render"):
            return ["-hwaccel", "vaapi", "-hwaccel_device", w.dev["render"]], ({"LIBVA_DRIVER_NAME": w.va} if w.va else {})
        if w.api == "nvenc":
            return ["-hwaccel", "cuda", "-hwaccel_device", str(w.dev["cuda"])], {"CUDA_DEVICE_ORDER": "PCI_BUS_ID"}
    return [], {}


def _free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _udp(port):
    return f"udp://127.0.0.1:{port}?fifo_size=1000000&overrun_nonfatal=1&timeout=10000000"


class Previews:
    def __init__(self, sup):
        self.sup = sup
        self.ffmpeg = sup.cfg["ingest"]["ffmpeg"]
        self.dir = os.path.join(sup.work, "preview")
        os.makedirs(self.dir, exist_ok=True)
        self.thumbs = {}            # carrier index -> {"proc", "port", "sids", "asked"}
        self.live = 0               # live previews running
        self.lock = threading.Lock()

    # ------------------------------------------------------------ engine copies of the output

    def _monitor(self, add, carrier, port):
        r = self.sup.engine_request(f"monitor {'add' if add else 'del'} carrier={carrier} addr=udp://127.0.0.1:{port}")
        if not r.get("ok") and add:
            raise RuntimeError(r.get("error") or "the engine refused the copy")

    def _service(self, sid):
        """(carrier, service, live stats) of a channel"""
        svc = next((s for s in self.sup.plan.services if s.sid == sid), None)
        if svc is None:
            return None, None, {}
        live = {}
        for c in (self.sup.stats or {}).get("carriers", []):
            if c.get("carrier") == svc.carrier.index:
                live = next((x for x in c.get("services", []) if x.get("sid") == sid), {})
        return svc.carrier, svc, live

    def _video_sids(self, car):
        """the channels of a multiplex that have a picture (stream type seen by the engine, else the SI type)"""
        live = {}
        for c in (self.sup.stats or {}).get("carriers", []):
            if c.get("carrier") == car.index:
                live = {x["sid"]: x for x in c.get("services", [])}
        out = []
        for s in car.services:
            vt = live.get(s.sid, {}).get("video_type")
            if vt in VIDEO_STREAM_TYPES or (vt is None and self.sup.si._stype(s) != 0x02):
                out.append(s.sid)
        return sorted(out)

    # ------------------------------------------------------------ thumbnails

    def thumbnail(self, sid):
        """JPEG bytes of the last picture of a channel (None: not yet), and keeps the thumbnails going"""
        car, svc, _ = self._service(sid)
        if svc is None:
            return None
        with self.lock:
            t = self.thumbs.get(car.index)
            if t is None:
                t = self.thumbs[car.index] = {"proc": None, "port": None, "next": 0, "carrier": car.index}
                log.info("thumbnails of %s: started, a picture per channel every %d s", car.name, self.every)
            t["asked"] = time.monotonic()
            if t["proc"] is None and t["next"] == 0:
                self._shot(car, t)                      # the first picture at once
        try:
            with open(os.path.join(self.dir, f"thumb-{sid}.jpg"), "rb") as f:
                return f.read()
        except OSError:
            return None

    @property
    def every(self):
        return max(2, int(self.sup.cfg["web"].get("preview_thumb_every_s", 30)))

    def _shot(self, car, t):
        """one ffmpeg reading the multiplex until it has one picture of each channel"""
        sids = self._video_sids(car)
        t["next"] = time.monotonic() + self.every
        if not sids:
            return
        port = _free_port()
        try:
            self._monitor(True, car.index, port)
        except RuntimeError as e:
            log.warning("thumbnails of %s: %s", car.name, e)
            return
        hw, env = _hw_decode()
        cmd = [self.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-skip_frame", "nokey",
               "-threads", "1"] + hw + ["-analyzeduration", PROBE_US, "-probesize", PROBE_BYTES, "-i", _udp(port)]
        for sid in sids:
            cmd += ["-map", f"0:p:{sid}:v:0", "-an", "-vf", "scale=320:-2", "-frames:v", "1", "-q:v", "6",
                    "-atomic_writing", "1", "-f", "image2", os.path.join(self.dir, f"thumb-{sid}.jpg")]
        try:
            # not _die_with_parent: its signal comes when the *thread* that started the child ends, here
            # the web request; if dvbtx dies, the engine copy stops and ffmpeg ends on its UDP timeout
            p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                 start_new_session=True, env=dict(os.environ, **env) if env else None)
        except OSError as e:
            self._monitor(False, car.index, port)
            log.warning("thumbnails of %s: %s", car.name, e)
            return
        threading.Thread(target=self._drain, args=(p, f"thumbnails {car.name}"), daemon=True).start()
        t.update(proc=p, port=port, started=time.monotonic())

    def _end_shot(self, t):
        p = t.get("proc")
        if p is None:
            return
        if p.poll() is None:
            _stop(p)
        elif p.returncode:
            log.warning("thumbnails of multiplex %d: ffmpeg stopped (%s)", t["carrier"],
                        " / ".join(getattr(p, "tail", [])) or f"exit {p.returncode}")
        self._monitor(False, t["carrier"], t["port"])
        t["proc"] = t["port"] = None

    def _stop_thumbs(self, ci):
        t = self.thumbs.pop(ci, None)
        if t:
            self._end_shot(t)

    def poll(self):
        """main loop, every second: a shot that ended frees its copy, the next one starts when due;
        thumbnails nobody asked for lately are stopped"""
        now = time.monotonic()
        with self.lock:
            for ci, t in list(self.thumbs.items()):
                if now - t["asked"] > THUMB_IDLE_S:
                    self._stop_thumbs(ci)
                    log.info("thumbnails of multiplex %d: stopped (page closed)", ci)
                    continue
                p = t.get("proc")
                if p is not None and (p.poll() is not None or now - t["started"] > THUMB_SHOT_S):
                    self._end_shot(t)
                if t.get("proc") is None and now >= t["next"]:
                    car = next((c for c in self.sup.plan.carriers if c.index == ci), None)
                    if car is None:
                        self.thumbs.pop(ci, None)
                    else:
                        self._shot(car, t)

    def stop(self):
        with self.lock:
            for ci in list(self.thumbs):
                self._stop_thumbs(ci)

    def _drain(self, p, what):
        p.tail = []
        for raw in iter(p.stderr.readline, b""):
            line = raw.decode("utf-8", "replace").strip()
            if line:
                log.debug("%s: %s", what, line)
                if not any(x in line for x in NOISE):
                    p.tail = (p.tail + [line])[-3:]

    # ------------------------------------------------------------ live

    def stream(self, sid, write, transcode=False):
        """runs a live preview of a channel, write(bytes) sends to the viewer (raises when it left)"""
        car, svc, live = self._service(sid)
        if svc is None:
            raise KeyError(sid)
        copy = live.get("video_type") == H264 and not transcode
        port = _free_port()
        self._monitor(True, car.index, port)
        cmd = [self.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-fflags", "nobuffer",
               "-analyzeduration", PROBE_US, "-probesize", PROBE_BYTES, "-i", _udp(port),
               "-map", f"0:p:{sid}:v:0?", "-map", f"0:p:{sid}:a:0?"]
        if copy:
            cmd += ["-c:v", "copy"]
        else:
            cmd += ["-vf", "yadif=deint=interlaced,scale=-2:360", "-c:v", "libx264", "-preset", "veryfast", "-tune",
                    "zerolatency", "-b:v", "900k", "-maxrate", "900k", "-bufsize", "900k", "-g", "50",
                    "-pix_fmt", "yuv420p"]
        # fragments of 0.2 s: the browser gets the picture as it comes, not in bursts of a second
        cmd += ["-c:a", "aac", "-b:a", "96k", "-ac", "2", "-f", "mp4", "-movflags",
                "frag_keyframe+empty_moov+default_base_moof", "-frag_duration", "200000", "pipe:1"]
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             preexec_fn=_die_with_parent)
        threading.Thread(target=self._drain, args=(p, f"live {svc.name}"), daemon=True).start()
        with self.lock:
            self.live += 1
        log.info("live preview of %s: started (%s)", svc.name, "video copied" if copy else "video re-encoded 360p")
        try:
            while True:
                chunk = p.stdout.read1(65536) if hasattr(p.stdout, "read1") else p.stdout.read(65536)
                if not chunk:
                    break
                write(chunk)
        finally:
            ended = p.poll() is not None
            _stop(p)
            self._monitor(False, car.index, port)
            with self.lock:
                self.live -= 1
            if ended and p.returncode:
                log.warning("live preview of %s: ffmpeg stopped (%s)", svc.name, " / ".join(getattr(p, "tail", [])))
            else:
                log.info("live preview of %s: stopped", svc.name)

    def raw(self, carrier, write):
        """the multiplex exactly as it goes on air (the engine's copy relayed as it comes, no ffmpeg),
        while it is read: for VLC and the like"""
        car = next((c for c in self.sup.plan.carriers if c.index == carrier), None)
        if car is None:
            raise KeyError(carrier)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(10)
        port = sock.getsockname()[1]
        self._monitor(True, car.index, port)
        with self.lock:
            self.live += 1
        log.info("raw stream of %s: started", car.name)
        buf = bytearray(65536)
        try:
            while True:
                n = sock.recv_into(buf)
                write(bytes(buf[:n]))
        except socket.timeout:
            pass
        finally:
            sock.close()
            self._monitor(False, car.index, port)
            with self.lock:
                self.live -= 1
            log.info("raw stream of %s: stopped", car.name)

    def status(self):
        return {"thumbnails": sorted(self.thumbs), "live": self.live}


def _stop(p):
    if p.poll() is None:
        try:
            os.killpg(p.pid, 15)
        except ProcessLookupError:
            pass
        try:
            p.wait(3)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, 9)
            p.wait()
