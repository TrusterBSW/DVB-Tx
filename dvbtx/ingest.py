"""Ingest: one ffmpeg process per input, turning any source into an SPTS on a local UDP port
read by the engine. Each process is supervised and restarted with a back-off; a crashing or
stalled source never touches the multiplex (the engine switches the service to its backup).

Source syntax (service 'source' / 'backup'):
  /path/file.mkv, file:///path        local file, looped (loop = true)
  dir:/path/to/folder                 every video file of a folder, as a looped playlist
  http(s)://..., *.m3u8               progressive or HLS stream
  m3u:/path/list.m3u#Channel name     entry of an M3U playlist (by name, tvg-name, tvg-id or #index)
  twitch://channel                    via streamlink (or yt-dlp)
  ytdlp:https://...                   anything yt-dlp can read (YouTube live, ...)
  htsp://user:pass@host:9982/Channel  tvheadend channel (through its HTTP streaming port)
  rtmp://host/app/key                 RTMP pull; rtmp://0.0.0.0:1935/live/key?listen to receive (OBS)
  rtsp://, srt://, udp://, rtp://     network streams (udp+direct:// skips ffmpeg)
  v4l2:/dev/video0?size=1280x720&fps=25&format=mjpeg&audio=alsa:hw:1,0   webcam / HDMI capture
  screen::0.0?size=1920x1080&fps=25&audio=pulse:default                   X11 screen
  lavfi:testsrc2=size=1280x720:rate=25 test pattern
  pipe:/path/fifo                     named pipe (any container ffmpeg understands)
  stdin: or -                         standard input of dvbtx
  exec:command args                   stdout of a command
"""

import collections
import glob
import logging
import os
import re
import shlex
import shutil
import signal
import subprocess
import threading
import time
import urllib.parse
import urllib.request

from . import config as cfgmod

log = logging.getLogger("dvbtx.ingest")


def _die_with_parent():
    """preexec: SIGTERM this child when dvbtx dies, even killed with SIGKILL"""
    try:
        import ctypes
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)   # PR_SET_PDEATHSIG
    except OSError:
        pass
    os.setsid()

VIDEO_EXT = (".mkv", ".mp4", ".ts", ".m2ts", ".mov", ".avi", ".webm", ".mpg", ".mpeg", ".flv", ".m4v", ".wmv")
_FF_CAPS = {}


def ffmpeg_caps(ffmpeg):
    if ffmpeg in _FF_CAPS:
        return _FF_CAPS[ffmpeg]
    caps = {"extension_picky": False, "readrate_catchup": False, "encoders": set(), "ok": False}
    try:
        h = subprocess.run([ffmpeg, "-hide_banner", "-h", "demuxer=hls"], capture_output=True, text=True, timeout=10)
        caps["extension_picky"] = "extension_picky" in h.stdout
        f = subprocess.run([ffmpeg, "-hide_banner", "-h", "full"], capture_output=True, text=True, timeout=10)
        caps["readrate_catchup"] = "-readrate_catchup" in f.stdout
        e = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True, timeout=10)
        caps["encoders"] = {l.split()[1] for l in e.stdout.splitlines() if len(l.split()) > 1 and l.startswith(" ")}
        caps["ok"] = True
    except (OSError, subprocess.SubprocessError):
        pass
    _FF_CAPS[ffmpeg] = caps
    return caps


def _query(src):
    """'scheme:path?k=v&k2=v2' -> (path, {k: v})"""
    if "?" in src:
        path, q = src.split("?", 1)
        return path, dict(urllib.parse.parse_qsl(q, keep_blank_values=True))
    return src, {}


def resolve_ytdlp(ytdlp, url, fmt, timeout=90):
    """yt-dlp -g: one URL (muxed or HLS) or two (video, audio)"""
    r = subprocess.run([ytdlp, "-q", "--no-warnings", "-g", "-f", fmt, url], capture_output=True, text=True,
                       timeout=timeout)
    urls = [u for u in r.stdout.splitlines() if u.startswith("http")]
    if r.returncode or not urls:
        raise ValueError(f"yt-dlp: {(r.stderr.strip().splitlines() or ['no URL'])[-1]}")
    return urls[:2]


def resolve_m3u(spec, timeout=20):
    """m3u:/path/or/url#Name -> stream URL"""
    body = spec[4:]
    ref = ""
    if "#" in body:
        body, ref = body.split("#", 1)
    if re.match(r"https?://", body):
        with urllib.request.urlopen(body, timeout=timeout) as r:
            text = r.read().decode("utf-8", "replace")
    else:
        with open(body, encoding="utf-8", errors="replace") as f:
            text = f.read()
    entries, info = [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXTINF"):
            info = line
        elif line and not line.startswith("#"):
            attrs = dict(re.findall(r'([\w-]+)="([^"]*)"', info or ""))
            title = (info or "").rsplit(",", 1)[-1].strip() if info else ""
            entries.append((title, attrs, line))
            info = None
    if not entries:
        raise ValueError(f"empty playlist {body}")
    if not ref:
        return entries[0][2]
    if ref.isdigit():
        return entries[int(ref)][2]
    norm = re.sub(r"\W+", "", ref.lower())
    for title, attrs, url in entries:
        for cand in (title, attrs.get("tvg-name", ""), attrs.get("tvg-id", "")):
            if re.sub(r"\W+", "", cand.lower()) == norm:
                return url
    raise ValueError(f"'{ref}' not found in {body}")


def htsp_to_http(src):
    u = urllib.parse.urlsplit(src)
    port = (u.port or 9982) - 1
    cred = ""
    if u.username:
        cred = urllib.parse.quote(u.username) + (":" + urllib.parse.quote(u.password or "") if u.password else "") + "@"
    ch = urllib.parse.unquote(u.path.lstrip("/"))
    path = f"/stream/channelnumber/{ch}" if ch.isdigit() else f"/stream/channelname/{urllib.parse.quote(ch)}"
    q = dict(urllib.parse.parse_qsl(u.query))
    q.setdefault("profile", "pass")
    return f"http://{cred}{u.hostname}:{port}{path}?{urllib.parse.urlencode(q)}"


class Recipe:
    """What to run for one input: optional producer command piped into ffmpeg."""

    def __init__(self):
        self.producer = None       # argv or shell string
        self.producer_shell = False
        self.inputs = []           # ffmpeg input arguments
        self.encode = False
        self.realtime_source = False
        self.use_stdin = False
        self.extra_audio = None
        self.vf = []
        self.playlist = None       # files of a folder, played one ffmpeg after the other
        self.maps = None           # stream selection forced by the recipe
        self.probe = []            # URLs/paths whose codecs may be checked before copying
        self.network = False       # fetched over HTTP: -re must be able to catch up after a slow fetch
        self.copyts = False        # separate video and audio inputs: their own timestamps keep them in sync


def recipe_for(src, opts, ing, work_dir, pl_index=0):
    """Parses a source string into a Recipe (pl_index: file to play for a folder)."""
    r = Recipe()
    ext_in = list(opts.get("ffmpeg_input", []))
    readrate = opts.get("readrate", "auto")
    loop = bool(opts.get("loop", True))
    ff = ing["ffmpeg"]
    caps = ffmpeg_caps(ff)

    def http_input(url):
        r.network = True
        a = ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_on_network_error", "1",
             "-reconnect_delay_max", "5", "-rw_timeout", "15000000"]
        if caps.get("extension_picky") and (".m3u8" in url or "m3u8" in url):
            a += ["-extension_picky", "0"]
        return a + ["-i", url]

    if src in ("-", "stdin:", "stdin"):
        r.inputs = ["-i", "pipe:0"]
        r.use_stdin = True
        r.realtime_source = True
    elif src.startswith("exec:"):
        r.producer = src[5:]
        r.producer_shell = True
        r.inputs = ["-i", "pipe:0"]
    elif src.startswith("twitch://"):
        chan, q = _query(src[9:])
        quality = q.get("quality", "best")
        if shutil.which(ing["streamlink"]):
            r.producer = [ing["streamlink"], "--stdout", "--loglevel", "warning", "--twitch-disable-ads",
                          f"https://www.twitch.tv/{chan}", quality]
            r.inputs = ["-i", "pipe:0"]
        else:
            urls = resolve_ytdlp(ing["ytdlp"], f"https://www.twitch.tv/{chan}", "best")
            r.inputs = http_input(urls[0])
    elif src.startswith("ytdlp:"):
        urls = resolve_ytdlp(ing["ytdlp"], src[6:], opts.get(
            "ytdlp_format", "bv*[vcodec^=avc1][height<=1080]+ba[acodec^=mp4a]/b[vcodec^=avc1]/bv*[height<=1080]+ba/b"))
        r.probe = list(urls)
        r.inputs = http_input(urls[0])
        if len(urls) == 2:                     # separate video and audio (YouTube DASH)
            r.inputs += http_input(urls[1])
            r.maps = ["0:v:0", "1:a:0"]
            # the two live playlists do not start at the same moment (seen: audio one 5 s segment
            # ahead): ffmpeg would start each one at 0 and the sound would be off by that much
            r.copyts = True
    elif src.startswith("m3u:"):
        u = resolve_m3u(src)
        r.inputs = http_input(u)
        r.probe = [u]
    elif src.startswith("htsp://") or src.startswith("tvh://"):
        r.inputs = http_input(htsp_to_http(src.replace("tvh://", "htsp://", 1)))
    elif re.match(r"https?://", src):
        r.inputs = http_input(src)
        r.probe = [src]
    elif src.startswith("rtmp://") or src.startswith("rtmps://"):
        path, q = _query(src)
        if "listen" in q:
            r.inputs = ["-listen", "1", "-timeout", "-1", "-i", path]
        else:
            r.inputs = ["-rw_timeout", "15000000", "-i", src]
        r.realtime_source = True
    elif src.startswith("rtsp://"):
        r.inputs = ["-rtsp_transport", opts.get("rtsp_transport", "tcp"), "-timeout", "15000000", "-i", src]
        r.realtime_source = True
    elif re.match(r"(srt|udp|rtp|tcp)://", src):
        url = src
        if src.startswith("udp://"):
            url += ("&" if "?" in src else "?") + "fifo_size=1000000&overrun_nonfatal=1&timeout=15000000"
        r.inputs = ["-i", url]
        r.realtime_source = True
    elif src.startswith("v4l2:"):
        dev, q = _query(src[5:])
        a = ["-f", "v4l2", "-thread_queue_size", "1024"]
        if q.get("format"):
            a += ["-input_format", q["format"]]
        if q.get("size"):
            a += ["-video_size", q["size"]]
        if q.get("fps"):
            a += ["-framerate", q["fps"]]
        r.inputs = a + ["-i", dev or "/dev/video0"]
        r.extra_audio = q.get("audio", "silence")
        r.encode = True
        r.realtime_source = True
    elif src.startswith("screen:") or src.startswith("x11:"):
        disp, q = _query(src.split(":", 1)[1])
        a = ["-f", "x11grab", "-thread_queue_size", "1024", "-framerate", q.get("fps", "25")]
        if q.get("size"):
            a += ["-video_size", q["size"]]
        r.inputs = a + ["-i", disp or os.environ.get("DISPLAY", ":0.0")]
        r.extra_audio = q.get("audio", "silence")
        r.encode = True
        r.realtime_source = True
    elif src.startswith("lavfi:") or src.startswith("testsrc:"):
        graph, q = _query(src.split(":", 1)[1])
        r.inputs = ["-re", "-f", "lavfi", "-i", graph or "testsrc2=size=1280x720:rate=25"]
        r.extra_audio = q.get("audio", "sine")
        r.encode = True
        readrate = False                 # -re already set
    elif src.startswith("pipe:"):
        r.inputs = ["-i", src[5:]]
        r.realtime_source = True
    else:
        path = src[7:] if src.startswith("file://") else (src[4:] if src.startswith("dir:") else src)
        if os.path.isdir(path):
            # one ffmpeg per file (the concat demuxer breaks on files of different formats); the
            # engine handles the timeline discontinuity and the new PMT at each change
            files = sorted(f for f in glob.glob(os.path.join(path, "*")) if f.lower().endswith(VIDEO_EXT))
            if not files:
                raise ValueError(f"no video file in {path}")
            r.playlist = files
            r.inputs = ["-i", files[pl_index % len(files)]]
            r.probe = [files[pl_index % len(files)]]
        elif os.path.exists(path):
            import stat as st_
            if st_.S_ISFIFO(os.stat(path).st_mode):
                r.inputs = ["-i", path]
                r.realtime_source = True
            else:
                r.inputs = (["-stream_loop", "-1"] if loop else []) + ["-i", path]
                r.probe = [path]
        else:
            raise ValueError(f"source {src!r}: file not found and unknown scheme")

    if readrate == "auto":
        readrate = not r.realtime_source
    if readrate:                               # -re before every input
        out = []
        for a in r.inputs:
            if a == "-i":
                out.append("-re")
                # each HLS segment costs a new connection: at the default catch-up rate (1.05) the lag
                # grows by a fraction of a second per segment until the service falls back to its backup
                if r.network and caps.get("readrate_catchup"):
                    out += ["-readrate_catchup", str(opts.get("readrate_catchup", 2))]
            out.append(a)
        r.inputs = out
        # the input options of the first input go before its -re: keep them in front

    r.inputs = ext_in + r.inputs
    if opts.get("encode_on"):
        r.encode = True
    return r


DVB_VIDEO = {"h264", "hevc", "mpeg2video", "mpeg1video"}
DVB_AUDIO = {"aac", "mp2", "mp3", "ac3", "eac3", "aac_latm"}
_PROBE_CACHE = {}


def probe_codecs(urls, ffprobe="ffprobe", timeout=20):
    """{'video': set, 'audio': set} of the codecs of the inputs (cached for local files)"""
    out = {"video": set(), "audio": set()}
    for u in urls:
        key = (u, os.path.getmtime(u)) if os.path.exists(u) else None
        if key and key in _PROBE_CACHE:
            res = _PROBE_CACHE[key]
        else:
            try:
                p = subprocess.run([ffprobe, "-v", "error", "-show_entries", "stream=codec_type,codec_name", "-of", "csv=p=0",
                                    u], capture_output=True, text=True, timeout=timeout)
                res = [l.split(",") for l in p.stdout.splitlines() if "," in l]
            except (OSError, subprocess.SubprocessError):
                res = []
            if key:
                _PROBE_CACHE[key] = res
        for cols in res:
            name, kind = cols[0], cols[1] if len(cols) > 1 else ""
            if kind in out:
                out[kind].add(name)
    return out


def _audio_input(spec):
    if spec in (None, "", "none"):
        return None
    if spec == "silence":
        return ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo"]
    if spec == "sine":
        return ["-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000"]
    kind, _, dev = spec.partition(":")
    if kind in ("alsa", "pulse", "pipewire", "jack"):
        return ["-f", "pulse" if kind == "pipewire" else kind, "-thread_queue_size", "1024", "-i", dev or "default"]
    return ["-i", spec]


def encode_args(svc_opts, rec, svc, ing):
    enc = svc_opts["encode"]
    vb = enc.get("video_bitrate", "auto")
    vbr = cfgmod.auto_video_bitrate(svc) if vb in (None, "", "auto") else cfgmod._bitrate(vb)
    vbr = int(vbr)
    codec = enc.get("video_codec", "libx264")
    caps = ffmpeg_caps(ing["ffmpeg"])
    if caps["ok"] and codec not in caps["encoders"]:
        raise ValueError(f"ffmpeg has no encoder {codec}")
    fps = enc.get("fps") or 25
    gop = max(1, int(float(fps) * float(enc.get("gop_s", 1.0))))
    vf = []
    if enc.get("deinterlace"):
        vf.append("yadif")
    if enc.get("size"):
        w, h = str(enc["size"]).lower().split("x")
        vf.append(f"scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,setsar=1")
    if enc.get("fps"):
        vf.append(f"fps={enc['fps']}")
    a = []
    hw = enc.get("hwaccel", "")
    if hw == "vaapi" or codec.endswith("_vaapi"):
        a = ["-vaapi_device", enc.get("device", "/dev/dri/renderD128")]
        vf += ["format=nv12", "hwupload"]
    elif enc.get("pix_fmt"):
        vf.append(f"format={enc['pix_fmt']}")
    if vf:
        a += ["-vf", ",".join(vf)]
    bufsize = int(vbr * min(1.0, float(svc_opts.get("mux_delay", 0.7)) * 0.7))
    a += ["-c:v", codec, "-b:v", str(vbr), "-maxrate", str(vbr), "-bufsize", str(bufsize), "-g", str(gop)]
    if codec in ("libx264", "libx265"):
        a += ["-preset", enc.get("preset", "veryfast"), "-sc_threshold", "0"]
        if enc.get("tune"):
            a += ["-tune", enc["tune"]]
        if codec == "libx264":
            a += ["-x264-params", "nal-hrd=vbr:repeat-headers=1"]
    elif codec.endswith("_nvenc"):
        a += ["-preset", enc.get("preset", "p4"), "-rc", "cbr"]
    a += ["-c:a", enc.get("audio_codec", "aac"), "-b:a", str(enc.get("audio_bitrate", "128k")),
          "-ar", str(enc.get("audio_rate", 48000)), "-ac", "2"]
    return a


def build_command(inp, plan, pl_index=0):
    """argv of ffmpeg (and of the producer) for one input; also returns the folder playlist."""
    c = plan.cfg
    ing = c["ingest"]
    o = inp.options
    rec = recipe_for(inp.source, o, ing, plan.work_dir, pl_index)
    ff = [ing["ffmpeg"], "-hide_banner", "-loglevel", ing.get("loglevel", "warning"), "-nostats"]
    if not rec.use_stdin and not rec.producer:
        ff.append("-nostdin")
    ff += ["-fflags", "+genpts+discardcorrupt"] + (["-copyts"] if rec.copyts else []) + rec.inputs
    maps = list(rec.maps or o.get("map") or [])
    extra = _audio_input(rec.extra_audio) if rec.extra_audio else None
    if extra:
        ff += extra
        maps = ["0:v:0", "1:a:0"]
    for m in maps:
        ff += ["-map", m]
    if not rec.encode and rec.probe and o.get("encode_on") is None and o.get("codec_check", True):
        codecs = probe_codecs(rec.probe, ing.get("ffprobe", "ffprobe"))
        bad_v = bool(codecs["video"]) and not codecs["video"] & DVB_VIDEO
        allowed_a = set(o.get("copy_audio_codecs") or DVB_AUDIO) & DVB_AUDIO
        bad_a = bool(codecs["audio"]) and not codecs["audio"] & allowed_a
        if bad_v:
            rec.encode = True
            rec.note = f"video {'/'.join(sorted(codecs['video']))} is not a DVB codec: encoded"
        elif bad_a:
            enc = o["encode"]
            rec.audio_only = ["-c:v", "copy", "-c:a", enc.get("audio_codec", "aac"), "-b:a",
                              str(enc.get("audio_bitrate", "128k")), "-ar", str(enc.get("audio_rate", 48000)), "-ac", "2"]
            rec.note = f"audio {'/'.join(sorted(codecs['audio']))} not in copy_audio_codecs: audio encoded"
    if rec.encode:
        ff += encode_args(o, rec, inp.service, ing) if inp.service is not None else _backup_encode(o)
    elif getattr(rec, "audio_only", None):
        ff += rec.audio_only
    else:
        ff += ["-c", "copy"]
    if getattr(rec, "note", None):
        log.info("input %s: %s", inp.name, rec.note)
    ff += list(o.get("ffmpeg_output", []))
    ff += ["-f", "mpegts", "-muxdelay", str(o.get("mux_delay", 0.7)), "-pcr_period", "20",
           "-mpegts_flags", "+resend_headers+system_b", "-max_interleave_delta", "0",
           f"udp://{inp.addr}?pkt_size=1316&buffer_size=4194304"]
    prod = None
    if rec.producer:
        prod = rec.producer if not rec.producer_shell else ["/bin/sh", "-c", rec.producer]
    return prod, ff, rec.use_stdin, rec.playlist


def _backup_encode(o):
    enc = o["encode"]
    vb = cfgmod._bitrate(enc.get("video_bitrate")) or 2e6
    return ["-c:v", enc.get("video_codec", "libx264"), "-preset", "veryfast", "-b:v", str(int(vb)),
            "-maxrate", str(int(vb)), "-bufsize", str(int(vb / 2)), "-g", "25", "-pix_fmt", "yuv420p",
            "-c:a", enc.get("audio_codec", "aac"), "-b:a", "128k", "-ar", "48000", "-ac", "2"]


class IngestProc:
    """Runs and supervises the process(es) of one input."""

    def __init__(self, inp, plan):
        self.inp = inp
        self.plan = plan
        self.ing = plan.cfg["ingest"]
        self.proc = None
        self.prod = None
        self.lines = collections.deque(maxlen=60)
        self.restarts = 0
        self.state = "stopped"
        self.started_at = 0.0
        self.next_start = 0.0
        self.backoff = float(self.ing["restart_min_s"])
        self.last_exit = None
        self.cmdline = ""
        self.error = ""
        self._stop = False
        self.pl_index = 0
        self.playlist = None
        self.disc_times = collections.deque(maxlen=100)   # "timestamp discontinuity" lines of ffmpeg

    def _reader(self, stream, tag):
        for raw in iter(stream.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip()
            if line:
                self.lines.append(f"{time.strftime('%H:%M:%S')} {tag}{line}")
                if "timestamp discontinuity" in line:
                    self.disc_times.append(time.monotonic())
        stream.close()

    def start(self):
        if self.inp.kind != "ffmpeg" or self._stop:
            return
        try:
            prod, ff, use_stdin, self.playlist = build_command(self.inp, self.plan, self.pl_index)
        except (ValueError, OSError) as e:
            self.error = str(e)
            self.state = "error"
            self.lines.append(f"{time.strftime('%H:%M:%S')} {e}")
            log.error("input %s: %s", self.inp.name, e)
            self.next_start = time.monotonic() + self.backoff
            self.backoff = min(self.backoff * 2, float(self.ing["restart_max_s"]))
            return
        self.error = ""
        self.cmdline = (" ".join(shlex.quote(a) for a in prod) + " | " if prod else "") + " ".join(shlex.quote(a) for a in ff)
        log.debug("input %s: %s", self.inp.name, self.cmdline)
        try:
            stdin = None
            if prod:
                self.prod = subprocess.Popen(prod, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                             preexec_fn=_die_with_parent)
                threading.Thread(target=self._reader, args=(self.prod.stderr, "[src] "), daemon=True).start()
                stdin = self.prod.stdout
            elif not use_stdin:
                stdin = subprocess.DEVNULL
            self.proc = subprocess.Popen(ff, stdin=stdin, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                         preexec_fn=_die_with_parent)
            if self.prod:
                self.prod.stdout.close()
            threading.Thread(target=self._reader, args=(self.proc.stderr, ""), daemon=True).start()
        except OSError as e:
            self.error = str(e)
            self.state = "error"
            self.next_start = time.monotonic() + self.backoff
            return
        self.state = "running"
        self.started_at = time.monotonic()

    def _kill(self, p):
        if p and p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                p.wait(3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                p.wait()

    def stop(self):
        self._stop = True
        self._kill(self.prod)
        self._kill(self.proc)
        self.state = "stopped"

    def restart(self):
        self._kill(self.prod)
        self._kill(self.proc)
        self.proc = self.prod = None
        self.next_start = 0
        self.state = "restarting"

    def poll(self):
        """called every second by the supervisor"""
        if self._stop or self.inp.kind != "ffmpeg":
            return
        now = time.monotonic()
        if self.proc is None:
            if now >= self.next_start:
                self.start()
            return
        rc = self.proc.poll()
        if rc is None and self.prod is not None and self.prod.poll() is not None:
            self._kill(self.proc)          # producer died: restart both
            rc = self.proc.poll()
        if rc is None:
            if now - self.started_at > 60:
                self.backoff = float(self.ing["restart_min_s"])
            return
        self._kill(self.prod)
        self.last_exit = rc
        self.proc = self.prod = None
        if self.playlist and rc == 0:            # end of a file of the folder: next one, right away
            self.pl_index = (self.pl_index + 1) % len(self.playlist)
            if self.pl_index or self.inp.options.get("loop", True):
                self.start()
                return
        self.restarts += 1
        self.state = "backoff"
        self.next_start = now + self.backoff
        log.warning("input %s: ffmpeg exited (%s), restart in %.0f s%s", self.inp.name, rc, self.backoff,
                    f": {self.lines[-1]}" if self.lines else "")
        self.backoff = min(self.backoff * 2, float(self.ing["restart_max_s"]))

    def status(self):
        return {"index": self.inp.index, "name": self.inp.name, "kind": self.inp.kind, "role": self.inp.role,
                "source": self.inp.source, "addr": self.inp.addr, "state": self.state, "restarts": self.restarts,
                "uptime": round(time.monotonic() - self.started_at) if self.state == "running" else 0,
                "last_exit": self.last_exit, "error": self.error, "log": list(self.lines)[-50:],
                "playing": self.playlist[self.pl_index % len(self.playlist)] if self.playlist else None,
                "cmd": self.cmdline}
