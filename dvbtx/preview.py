"""Previews of the channels for the web page, running only while someone looks at them.

Both read a copy of what goes on air, asked to the engine for the time of the preview ("monitor add
carrier=N addr=udp://127.0.0.1:PORT"), so they show what the TVs receive (backup clip included).

Thumbnails: one ffmpeg per multiplex decodes only the key frames (-skip_frame nokey: a few per
second for all its channels) and writes a small JPEG per channel every few seconds. It starts with
the first thumbnail asked and stops THUMB_IDLE_S after the last one.

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

from .ingest import _die_with_parent

log = logging.getLogger("dvbtx.preview")

THUMB_IDLE_S = 30           # thumbnails of a multiplex kept up to date that long after the last request
THUMB_EVERY_S = 4           # a new picture per channel every ...
VIDEO_STREAM_TYPES = {0x01, 0x02, 0x10, 0x1B, 0x24}
H264 = 0x1B


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
            sids = self._video_sids(car)
            if t and (t["proc"].poll() is not None or t["sids"] != sids):
                self._stop_thumbs(car.index)           # died, or the channels changed
                t = None
            if t is None:
                t = self._start_thumbs(car, sids)
            if t:
                t["asked"] = time.monotonic()
        try:
            with open(os.path.join(self.dir, f"thumb-{sid}.jpg"), "rb") as f:
                return f.read()
        except OSError:
            return None

    def _start_thumbs(self, car, sids):
        if not sids:
            return None
        port = _free_port()
        try:
            self._monitor(True, car.index, port)
        except RuntimeError as e:
            log.warning("thumbnails of %s: %s", car.name, e)
            return None
        cmd = [self.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-skip_frame", "nokey",
               "-analyzeduration", "1500000", "-probesize", "8000000", "-i", _udp(port)]
        # the first key frame at once, then one every THUMB_EVERY_S (fps=1/N would wait for its first window)
        pick = f"select='isnan(prev_selected_t)+gte(t-prev_selected_t\\,{THUMB_EVERY_S})',scale=320:-2"
        for sid in sids:
            cmd += ["-map", f"0:p:{sid}:v:0", "-an", "-vf", pick, "-fps_mode", "passthrough", "-q:v", "6",
                    "-update", "1", "-atomic_writing", "1", "-f", "image2", os.path.join(self.dir, f"thumb-{sid}.jpg")]
        try:
            # not _die_with_parent: its signal comes when the *thread* that started the child ends, here
            # the web request; if dvbtx dies, the engine copy stops and ffmpeg ends on its UDP timeout
            p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                 start_new_session=True)
        except OSError as e:
            self._monitor(False, car.index, port)
            log.warning("thumbnails of %s: %s", car.name, e)
            return None
        threading.Thread(target=self._drain, args=(p, f"thumbnails {car.name}"), daemon=True).start()
        t = {"proc": p, "port": port, "sids": sids, "asked": time.monotonic(), "carrier": car.index}
        self.thumbs[car.index] = t
        log.info("thumbnails of %s: started (%d channels)", car.name, len(sids))
        return t

    def _stop_thumbs(self, ci):
        t = self.thumbs.pop(ci, None)
        if not t:
            return
        _stop(t["proc"])
        self._monitor(False, ci, t["port"])

    def poll(self):
        """main loop, every second: thumbnails nobody asked for lately are stopped"""
        with self.lock:
            for ci, t in list(self.thumbs.items()):
                if time.monotonic() - t["asked"] > THUMB_IDLE_S or t["proc"].poll() is not None:
                    self._stop_thumbs(ci)
                    log.info("thumbnails of multiplex %d: stopped", ci)

    def stop(self):
        with self.lock:
            for ci in list(self.thumbs):
                self._stop_thumbs(ci)

    def _drain(self, p, what):
        for raw in iter(p.stderr.readline, b""):
            line = raw.decode("utf-8", "replace").strip()
            if line:
                log.debug("%s: %s", what, line)

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
               "-analyzeduration", "1000000", "-probesize", "5000000", "-i", _udp(port),
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
            _stop(p)
            self._monitor(False, car.index, port)
            with self.lock:
                self.live -= 1
            log.info("live preview of %s: stopped", svc.name)

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
