"""Programme guide: everything dropped in the EPG folder (or fetched from XMLTV URLs) becomes
events per service, then EIT present/following and schedule sections.

Accepted files:
  *.xml / *.xml.gz  XMLTV (<tv>): channels matched by service epg_id, else by display name
                    TSDuck EIT XML (<tsduck><EIT service_id=...>): matched by service_id
"""

import datetime as dt
import gzip
import hashlib
import io
import logging
import os
import re
import threading
import unicodedata
import urllib.request
import xml.etree.ElementTree as ET

log = logging.getLogger("dvbtx.epg")

UTC = dt.timezone.utc

_CONTENT = [
    (("film", "movie", "cinéma", "cinema", "téléfilm"), 0x10),
    (("série", "serie", "series", "feuilleton", "soap"), 0x15),
    (("info", "news", "journal", "actualité", "météo", "weather"), 0x20),
    (("jeu", "game show", "quiz", "divertissement", "entertainment", "variétés", "talk"), 0x30),
    (("sport", "football", "rugby", "tennis", "cyclisme", "motor"), 0x40),
    (("jeunesse", "enfant", "children", "kids", "dessin animé", "animation", "cartoon"), 0x50),
    (("musique", "music", "concert", "clip"), 0x60),
    (("culture", "art", "théâtre", "theatre", "opéra"), 0x70),
    (("magazine", "société", "society", "reportage"), 0x80),
    (("documentaire", "documentary", "découverte", "nature", "science", "histoire"), 0x90),
    (("loisirs", "leisure", "cuisine", "cooking", "voyage", "travel"), 0xA0),
]


def _norm(s):
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().lower()
    return re.sub(r"[^a-z0-9]+", "", s)


def content_code(categories):
    for cat in categories:
        c = cat.lower()
        for words, code in _CONTENT:
            if any(w in c for w in words):
                return code
    return None


def _xmltv_time(s, default_tz):
    s = s.strip()
    m = re.match(r"(\d{14})(?:\s*([+-]\d{4}))?", s) or re.match(r"(\d{12})(?:\s*([+-]\d{4}))?", s)
    if not m:
        raise ValueError(s)
    digits = m.group(1)
    fmt = "%Y%m%d%H%M%S" if len(digits) == 14 else "%Y%m%d%H%M"
    t = dt.datetime.strptime(digits, fmt)
    if m.group(2):
        sign = 1 if m.group(2)[0] == "+" else -1
        off = dt.timedelta(hours=int(m.group(2)[1:3]), minutes=int(m.group(2)[3:5]))
        return t.replace(tzinfo=dt.timezone(sign * off)).astimezone(UTC)
    return t.replace(tzinfo=default_tz).astimezone(UTC)


def _rating(value):
    m = re.search(r"(\d+)", value or "")
    if not m:
        return None
    age = int(m.group(1))
    return max(1, min(0x0F, age - 3)) if age >= 4 else None


def _event_id(start):
    return int(start.timestamp() // 60) & 0xFFFF


def _open(path_or_bytes):
    if isinstance(path_or_bytes, bytes):
        data = path_or_bytes
        if data[:2] == b"\x1f\x8b":
            data = gzip.decompress(data)
        return io.BytesIO(data)
    if str(path_or_bytes).endswith(".gz"):
        return gzip.open(path_or_bytes, "rb")
    return open(path_or_bytes, "rb")


def parse_xmltv(src, default_tz):
    """-> (channels {id: [display names]}, programmes {channel id: [event]})."""
    channels, progs = {}, {}
    with _open(src) as f:
        for _, el in ET.iterparse(f, events=("end",)):
            if el.tag == "channel":
                channels[el.get("id")] = [d.text or "" for d in el.findall("display-name")]
                el.clear()
            elif el.tag == "programme":
                try:
                    start = _xmltv_time(el.get("start"), default_tz)
                    stop = _xmltv_time(el.get("stop"), default_tz) if el.get("stop") else None
                except ValueError:
                    el.clear()
                    continue
                title = el.find("title")
                ev = {
                    "start": start,
                    "stop": stop,
                    "title": (title.text or "").strip() if title is not None else "",
                    "lang": title.get("lang") if title is not None else None,
                    "subtitle": (el.findtext("sub-title") or "").strip(),
                    "description": (el.findtext("desc") or "").strip(),
                    "content": content_code([c.text or "" for c in el.findall("category")]),
                }
                r = el.find("rating")
                if r is not None:
                    ev["rating"] = _rating(r.findtext("value"))
                progs.setdefault(el.get("channel"), []).append(ev)
                el.clear()
    return channels, progs


def parse_tsduck_eit(path):
    """-> {service_id: [event]} from a TSDuck XML file of EIT tables."""
    out = {}
    root = ET.parse(path).getroot()
    for eit in root.iter("EIT"):
        sid = int(eit.get("service_id", "0"), 0)
        for e in eit.findall("event"):
            try:
                start = dt.datetime.strptime(e.get("start_time"), "%Y-%m-%d %H:%M:%S").replace(tzinfo=UTC)
                h, m, s = (int(x) for x in e.get("duration", "00:00:00").split(":"))
            except (TypeError, ValueError):
                continue
            ev = {"start": start, "duration": h * 3600 + m * 60 + s, "event_id": int(e.get("event_id", "0"), 0)}
            se = e.find("short_event_descriptor")
            if se is not None:
                ev["lang"] = se.get("language_code")
                ev["title"] = (se.findtext("event_name") or "").strip()
                ev["subtitle"] = (se.findtext("text") or "").strip()
            ext = [x.findtext("text") or "" for x in e.findall("extended_event_descriptor")]
            if ext:
                ev["description"] = "".join(ext).strip()
                if ev.get("subtitle") and ev["description"].startswith(ev["subtitle"].rstrip(".")[:40]):
                    ev["subtitle"] = ""
            c = e.find("content_descriptor/content")
            if c is not None:
                ev["content"] = (int(c.get("content_nibble_level_1", "0"), 0) << 4) | int(c.get("content_nibble_level_2", "0"), 0)
            pr = e.find("parental_rating_descriptor/country")
            if pr is not None:
                ev["rating"] = int(pr.get("rating", "0"), 0) or None
                ev["country"] = pr.get("country_code")
            out.setdefault(sid, []).append(ev)
    return out


def _finish(events, max_days):
    """sort, compute durations, drop overlaps and the far future"""
    now = dt.datetime.now(UTC)
    horizon = now + dt.timedelta(days=max_days)
    evs = sorted((e for e in events if e.get("start")), key=lambda e: e["start"])
    out = []
    for i, e in enumerate(evs):
        if "duration" not in e or e["duration"] is None:
            stop = e.get("stop") or (evs[i + 1]["start"] if i + 1 < len(evs) else e["start"] + dt.timedelta(hours=1))
            e["duration"] = int((stop - e["start"]).total_seconds())
        if e["duration"] <= 0:
            continue
        end = e["start"] + dt.timedelta(seconds=e["duration"])
        if end <= now - dt.timedelta(hours=1) or e["start"] > horizon:
            continue
        if out and e["start"] < out[-1]["start"] + dt.timedelta(seconds=out[-1]["duration"]):
            # overlapping: shorten the previous one
            prev = out[-1]
            prev["duration"] = int((e["start"] - prev["start"]).total_seconds())
            if prev["duration"] <= 0:
                out.pop()
        e.setdefault("event_id", _event_id(e["start"]))
        out.append(e)
    ids = set()
    for e in out:                     # event ids unique within the service
        while e["event_id"] in ids:
            e["event_id"] = (e["event_id"] + 1) & 0xFFFF
        ids.add(e["event_id"])
    return out


class EpgStore:
    """Watches the EPG folder and URLs; .events(sid) gives the current list of a service."""

    def __init__(self, directory, urls, services, tz, days=7, url_refresh_s=3600):
        self.dir = directory
        self.urls = list(urls or [])
        self.services = services            # objects with .sid .name .epg_id
        self.tz = tz
        self.days = days
        self.url_refresh_s = url_refresh_s
        self._mtimes = None
        self._url_cache = {}                # url -> (fetched_at, bytes)
        self._events = {}
        self._digest = None
        self.lock = threading.Lock()
        self.errors = []

    def events(self, sid):
        with self.lock:
            return self._events.get(sid, [])

    def _snapshot(self):
        snap = {}
        if self.dir and os.path.isdir(self.dir):
            for n in sorted(os.listdir(self.dir)):
                p = os.path.join(self.dir, n)
                if os.path.isfile(p) and (n.endswith(".xml") or n.endswith(".xml.gz") or n.endswith(".xmltv")):
                    st = os.stat(p)
                    snap[p] = (st.st_mtime_ns, st.st_size)
        return snap

    def _fetch_urls(self, force=False):
        changed = False
        now = dt.datetime.now().timestamp()
        for u in self.urls:
            t, _ = self._url_cache.get(u, (0, None))
            if force or now - t >= self.url_refresh_s:
                try:
                    with urllib.request.urlopen(u, timeout=60) as r:
                        data = r.read()
                    old = self._url_cache.get(u, (0, None))[1]
                    self._url_cache[u] = (now, data)
                    changed |= data != old
                except OSError as e:
                    self.errors.append(f"{u}: {e}")
                    log.warning("EPG %s: %s", u, e)
                    self._url_cache.setdefault(u, (now, None))
                    self._url_cache[u] = (now, self._url_cache[u][1])
        return changed

    def refresh(self, force=False, remap=False):
        """Reloads if something changed; returns True if the events changed. remap: the channels changed,
        events matched again to them (URLs not fetched again)."""
        snap = self._snapshot()
        url_changed = False if remap else self._fetch_urls(force)
        if not force and not remap and not url_changed and snap == self._mtimes:
            return False
        self._mtimes = snap
        self.errors = []
        by_sid = {}
        xmltv_sources = []
        for p in snap:
            try:
                with _open(p) as f:
                    head = f.read(4096)
                if b"<tsduck" in head:
                    for sid, evs in parse_tsduck_eit(p).items():
                        by_sid.setdefault(sid, []).extend(evs)
                else:
                    xmltv_sources.append(p)
            except (OSError, ET.ParseError) as e:
                self.errors.append(f"{os.path.basename(p)}: {e}")
                log.warning("EPG file %s: %s", p, e)
        for u, (_, data) in self._url_cache.items():
            if data:
                xmltv_sources.append(data)
        for src in xmltv_sources:
            try:
                channels, progs = parse_xmltv(src, self.tz)
            except (OSError, ET.ParseError, EOFError) as e:
                self.errors.append(f"xmltv: {e}")
                continue
            names = {}
            for cid, dn in channels.items():
                for n in dn:
                    names.setdefault(_norm(n), cid)
            for s in self.services:
                cid = s.epg_id if s.epg_id in progs else None
                if not cid and not s.epg_id:
                    cid = names.get(_norm(s.name))
                    if cid not in progs:
                        cid = s.name if s.name in progs else None
                if cid:
                    by_sid.setdefault(s.sid, []).extend(dict(e) for e in progs[cid])
        result = {sid: _finish(evs, self.days) for sid, evs in by_sid.items()}
        digest = hashlib.sha1(repr(sorted((k, [(e["event_id"], e["start"], e["duration"], e.get("title")) for e in v])
                                          for k, v in result.items())).encode()).hexdigest()
        with self.lock:
            self._events = result
        changed = digest != self._digest
        self._digest = digest
        if changed:
            log.info("EPG: %d events for %d services", sum(len(v) for v in result.values()), len(result))
        return changed


def present_following(events, now):
    """(present, following, next_boundary) for a sorted event list."""
    present = following = None
    boundary = None
    for i, e in enumerate(events):
        end = e["start"] + dt.timedelta(seconds=e["duration"])
        if end <= now:
            continue
        if e["start"] <= now:
            present = e
            following = events[i + 1] if i + 1 < len(events) else None
            boundary = end
        else:
            following = e
            boundary = e["start"]
        break
    return present, following, boundary
