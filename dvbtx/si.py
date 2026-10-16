"""DVB SI sections (ETSI EN 300 468) built by the supervisor and carouselled by the engine:
NIT (with the EACEM logical channel numbers), SDT, EIT present/following and schedule,
and the local_time_offset_descriptor of the TOT."""

import datetime as dt
import struct
import unicodedata

# ---------------------------------------------------------------- CRC / helpers

_CRC_TABLE = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) if _c & 0x80000000 else (_c << 1)
    _CRC_TABLE.append(_c & 0xFFFFFFFF)


def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((crc >> 24) ^ b) & 0xFF]
    return crc


def long_section(table_id, ext, version, sec_num, last_sec, body, current=True):
    """Section with syntax indicator: header + body + CRC."""
    length = 5 + len(body) + 4
    if length > 4093:
        raise ValueError("section too long")
    hdr = bytes([table_id, 0xF0 | (length >> 8), length & 0xFF, ext >> 8, ext & 0xFF,
                 0xC0 | ((version & 0x1F) << 1) | (1 if current else 0), sec_num, last_sec])
    s = hdr + body
    return s + struct.pack(">I", crc32(s))


def desc(tag, payload):
    payload = bytes(payload)
    if len(payload) > 255:
        payload = payload[:255]
    return bytes([tag, len(payload)]) + payload


def loop(descs):
    d = b"".join(descs)
    return bytes([0xF0 | (len(d) >> 8), len(d) & 0xFF]) + d


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def mjd_utc(t):
    """5 bytes: MJD + BCD hh mm ss of an aware or UTC naive datetime."""
    if t.tzinfo is not None:
        t = t.astimezone(dt.timezone.utc).replace(tzinfo=None)
    mjd = (t.date() - dt.date(1858, 11, 17)).days
    return struct.pack(">H", mjd) + bytes([bcd(t.hour), bcd(t.minute), bcd(t.second)])


def bcd_duration(seconds):
    seconds = max(0, min(int(seconds), 99 * 3600 + 3599))
    return bytes([bcd(seconds // 3600), bcd(seconds // 60 % 60), bcd(seconds % 60)])


# ---------------------------------------------------------------- text

_6937_DIACRITIC = {
    "̀": 0xC1, "́": 0xC2, "̂": 0xC3, "̃": 0xC4, "̄": 0xC5, "̆": 0xC6,
    "̇": 0xC7, "̈": 0xC8, "̊": 0xCA, "̧": 0xCB, "̋": 0xCD, "̨": 0xCE,
    "̌": 0xCF,
}
_6937_SPECIAL = {
    "Œ": 0xEA, "œ": 0xFA, "Æ": 0xE1, "æ": 0xF1, "Ø": 0xE9, "ø": 0xF9, "ß": 0xFB, "Đ": 0xE2, "đ": 0xF2,
    "Ł": 0xE8, "ł": 0xF8, "Þ": 0xEC, "þ": 0xFC, "ı": 0xF5, "¡": 0xA1, "¢": 0xA2, "£": 0xA3, "$": 0x24,
    "¥": 0xA5, "#": 0x23, "§": 0xA7, "«": 0xAB, "°": 0xB0, "±": 0xB1, "²": 0xB2, "³": 0xB3, "×": 0xB4,
    "µ": 0xB5, "¶": 0xB6, "·": 0xB7, "÷": 0xB8, "»": 0xBB, "¼": 0xBC, "½": 0xBD, "¾": 0xBE, "¿": 0xBF,
    "’": 0x27, "‘": 0x27, "“": 0x22, "”": 0x22, "–": 0x2D, "—": 0x2D, "…": 0x2E, "€": 0x45,
    " ": 0xA0, "©": 0xD3, "®": 0xD2, "™": 0xD4, "♪": 0xD5,
}


def _iso6937(s):
    out = bytearray()
    for ch in s:
        o = ord(ch)
        if ch in _6937_SPECIAL:
            if ch == "…":
                out += b"..."
            elif ch == "€":
                out += b"EUR"
            else:
                out.append(_6937_SPECIAL[ch])
            continue
        if 0x20 <= o < 0x7F and ch not in "$#":
            out.append(o)
            continue
        if ch in "\n\r":
            out.append(0x8A)                     # DVB CR/LF control code
            continue
        d = unicodedata.normalize("NFD", ch)
        if len(d) >= 2 and d[1] in _6937_DIACRITIC and ord(d[0]) < 0x80:
            out.append(_6937_DIACRITIC[d[1]])
            out.append(ord(d[0]))
            continue
        base = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore")
        out += base if base else b"?"
    return bytes(out)


_PREFIX = {  # EN 300 468 table A.3 / A.4
    "iso8859-5": b"\x01", "iso8859-6": b"\x02", "iso8859-7": b"\x03", "iso8859-8": b"\x04", "iso8859-9": b"\x05",
    "iso8859-10": b"\x06", "iso8859-11": b"\x07", "iso8859-13": b"\x09", "iso8859-14": b"\x0a", "iso8859-15": b"\x0b",
    "iso8859-1": b"\x10\x00\x01", "iso8859-2": b"\x10\x00\x02", "iso8859-3": b"\x10\x00\x03", "iso8859-4": b"\x10\x00\x04",
}
_ALIASES = {"latin1": "iso8859-1", "latin9": "iso8859-15", "latin5": "iso8859-9", "tnt": "iso8859-9"}


def dvb_text(s, charset="iso8859-9", maxlen=255):
    """Encodes a string as a DVB text field (EN 300 468 annex A), truncated to maxlen bytes.
    Default ISO 8859-9 with its 0x05 prefix, as the French DTT broadcasts it (ISO 6937, the
    "default" table, showed broken accents on a TV); pure ASCII text is sent without prefix."""
    s = s or ""
    charset = _ALIASES.get(charset, charset)
    if charset != "iso6937" and charset != "utf8" and all(ord(ch) < 0x80 for ch in s):
        b, prefix = s.encode("ascii"), b""
    elif charset == "iso6937":
        b = _iso6937(s)
        prefix = b""
    elif charset == "utf8":
        b = s.encode("utf-8")
        prefix = b"\x15"
    elif charset in _PREFIX:
        # typographic characters missing from the 8859 tables
        t = s.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"').replace("…", "...") \
             .replace("–", "-").replace("—", "-").replace("œ", "oe").replace("Œ", "OE").replace("€", "EUR")
        b = t.encode(charset.replace("iso", "iso_"), "replace")
        prefix = _PREFIX[charset]
    else:
        raise ValueError(f"unknown charset {charset}")
    room = maxlen - len(prefix)
    if len(b) > room:
        b = b[:room]
        if charset == "utf8":                    # do not cut a character in half
            b = b.decode("utf-8", "ignore").encode("utf-8")
        elif charset == "iso6937" and b and 0xC1 <= b[-1] <= 0xCF:
            b = b[:-1]                           # dangling diacritic
    return prefix + b if b else b""


def lang_code(lang):
    # ISO 639-2/B codes, as broadcast by the French DTT ("fre", not "fra")
    lang = (lang or "fre").lower()
    two = {"fr": "fre", "fra": "fre", "en": "eng", "de": "ger", "deu": "ger", "es": "spa", "it": "ita",
           "nl": "dut", "nld": "dut", "pt": "por"}
    lang = two.get(lang, lang)
    return lang[:3].ljust(3).encode("ascii", "replace")


# ---------------------------------------------------------------- service types

SERVICE_TYPES = {
    "tv": 0x01, "sd": 0x01, "mpeg2": 0x01, "mpeg2hd": 0x11, "radio": 0x02, "teletext": 0x03,
    "h264sd": 0x16, "h264": 0x19, "hd": 0x19, "advanced_hd": 0x19, "hevc": 0x1F, "uhd": 0x1F,
    "advanced_radio": 0x0A, "data": 0x0C,
}


def service_type_from_streams(vtype, atypes):
    """DVB service_type guessed from the PMT stream types reported by the engine."""
    if vtype in (0x1B,):
        return 0x19
    if vtype in (0x24,):
        return 0x1F
    if vtype in (0x01, 0x02):
        return 0x01
    if vtype is None and atypes:
        return 0x0A if any(t in (0x0F, 0x11) for t in atypes) else 0x02
    return 0x19


# ---------------------------------------------------------------- delivery system descriptors

_T_BW = {8_000_000: 0, 7_000_000: 1, 6_000_000: 2, 5_000_000: 3}
_T_CONST = {"qpsk": 0, "16qam": 1, "64qam": 2}
_T_CR = {"1/2": 0, "2/3": 1, "3/4": 2, "5/6": 3, "7/8": 4}
_T_GI = {"1/32": 0, "1/16": 1, "1/8": 2, "1/4": 3}
_T_MODE = {"2k": 0, "8k": 1, "4k": 2}


def terrestrial_delivery(freq_hz, bandwidth, constellation, code_rate, guard, mode, unspecified=False):
    f = 0xFFFFFFFF if unspecified else int(round(freq_hz / 10))
    b1 = (_T_BW.get(int(bandwidth), 0) << 5) | 0x10 | 0x08 | 0x04 | 0x03
    b2 = (_T_CONST[constellation] << 6) | (0 << 3) | _T_CR[code_rate]
    b3 = (_T_CR[code_rate] << 5) | (_T_GI[guard] << 3) | (_T_MODE[mode] << 1) | 0
    return desc(0x5A, struct.pack(">I", f) + bytes([b1, b2, b3]) + b"\xff\xff\xff\xff")


_C_MOD = {"16qam": 1, "32qam": 2, "64qam": 3, "128qam": 4, "256qam": 5}


def _bcd_digits(value, ndigits):
    s = f"{int(value):0{ndigits}d}"[-ndigits:]
    out = bytearray()
    for i in range(0, ndigits, 2):
        out.append((int(s[i]) << 4) | int(s[i + 1]))
    return bytes(out)


def cable_delivery(freq_hz, symbol_rate, constellation):
    # frequency: 8 BCD digits in 100 Hz (MHz with 4 decimals); symbol rate: 7 digits in 100 sym/s + FEC inner
    f = _bcd_digits(round(freq_hz / 100), 8)
    sr = _bcd_digits(round(symbol_rate / 100) * 10, 8)     # 7 digits then 4 bits FEC_inner
    sr = sr[:3] + bytes([(sr[3] & 0xF0) | 0x0F])            # FEC inner: 1111 = no convolutional code
    return desc(0x44, f + b"\xff\xf2" + bytes([_C_MOD[constellation]]) + sr)


_S_FEC = {"1/2": 1, "2/3": 2, "3/4": 3, "5/6": 4, "7/8": 5, "8/9": 6, "3/5": 7, "4/5": 8, "9/10": 9}


def satellite_delivery(freq_hz, symbol_rate, code_rate, polarization="h", s2=False, modulation="qpsk",
                       rolloff=0.35, orbital=19.2, east=True):
    f = _bcd_digits(round(freq_hz / 10000), 8)              # GHz with 5 decimals = 10 kHz units
    pos = _bcd_digits(round(orbital * 10), 4)
    pol = {"h": 0, "v": 1, "l": 2, "r": 3}[polarization[0].lower()]
    ro = {0.35: 0, 0.25: 1, 0.20: 2}.get(rolloff, 0)
    mt = {"auto": 0, "qpsk": 1, "8psk": 2, "16qam": 3}.get(modulation, 0)
    b = (0x80 if east else 0) | (pol << 5) | ((ro if s2 else 0) << 3) | ((1 if s2 else 0) << 2) | mt
    sr = _bcd_digits(round(symbol_rate / 100) * 10, 8)
    sr = sr[:3] + bytes([(sr[3] & 0xF0) | _S_FEC.get(code_rate, 0xF)])
    return desc(0x43, f + pos + bytes([b]) + sr)


_T2_BW = {8_000_000: 0, 7_000_000: 1, 6_000_000: 2, 5_000_000: 3, 10_000_000: 4, 1_712_000: 5}
_T2_GI = {"1/32": 0, "1/16": 1, "1/8": 2, "1/4": 3, "1/128": 4, "19/128": 5, "19/256": 6}
_T2_MODE = {"2k": 0, "8k": 1, "4k": 2, "1k": 3, "16k": 4, "32k": 5}


def t2_delivery(t2_system_id, freq_hz, bandwidth, guard, fft, cell_id=0, plp_id=0, unspecified=False):
    """T2_delivery_system_descriptor (extension 0x04) with one cell, SISO, no TFS."""
    f = 0xFFFFFFFF if unspecified else int(round(freq_hz / 10))
    bw = _T2_BW.get(int(bandwidth), 0)
    flags = (0 << 14) | (bw << 10) | (0 << 8) | (_T2_GI[guard] << 5) | (_T2_MODE[fft] << 2) | (0 << 1) | 0
    body = bytes([0x04, plp_id & 0xFF]) + struct.pack(">HH", t2_system_id & 0xFFFF, flags)
    body += struct.pack(">HI", cell_id & 0xFFFF, f) + bytes([0])         # cell, centre frequency, no subcell
    return desc(0x7F, body)


# ---------------------------------------------------------------- tables

def nit_sections(network_id, network_name, transport_streams, version, charset="iso8859-9", lcn_pds=0x28,
                 actual=True):
    """transport_streams: list of dict(tsid, onid, delivery=bytes, services=[(sid, type, lcn, visible)])."""
    first_loop = loop([desc(0x40, dvb_text(network_name, charset))])
    ts_entries = []
    for ts in transport_streams:
        descs = []
        if ts.get("delivery"):
            descs.append(ts["delivery"])
        svcs = ts["services"]
        if svcs:
            descs.append(desc(0x41, b"".join(struct.pack(">HB", s[0], s[1]) for s in svcs)))
            lcns = [s for s in svcs if s[2] is not None]
            if lcns:
                if lcn_pds:
                    descs.append(desc(0x5F, struct.pack(">I", lcn_pds)))
                # EACEM logical_channel_descriptor: 4 bytes per service (at most 63 per descriptor)
                for i in range(0, len(lcns), 63):
                    descs.append(desc(0x83, b"".join(
                        struct.pack(">HH", s[0], (0x8000 if s[3] else 0) | 0x7C00 | (s[2] & 0x3FF))
                        for s in lcns[i:i + 63])))
        d = b"".join(descs)
        ts_entries.append(struct.pack(">HH", ts["tsid"], ts["onid"]) + bytes([0xF0 | (len(d) >> 8), len(d) & 0xFF]) + d)
    # split the transport stream loop over several sections if needed
    sections, cur = [], []
    room = 1021 - 5 - 4 - len(first_loop) - 2

    def flush():
        body_ts = b"".join(cur)
        sections.append(first_loop + bytes([0xF0 | (len(body_ts) >> 8), len(body_ts) & 0xFF]) + body_ts)

    size = 0
    for e in ts_entries:
        if cur and size + len(e) > room:
            flush()
            cur, size = [], 0
        cur.append(e)
        size += len(e)
    flush()
    tid = 0x40 if actual else 0x41
    return [long_section(tid, network_id, version, i, len(sections) - 1, b) for i, b in enumerate(sections)]


def sdt_sections(tsid, onid, services, version, charset="iso8859-9", actual=True):
    """services: list of dict(sid, type, name, provider, eit_schedule, eit_pf, running, free_ca)."""
    entries = []
    for s in services:
        prov = dvb_text(s.get("provider", ""), charset, 80)
        name = dvb_text(s["name"], charset, 80)
        sd = desc(0x48, bytes([s["type"], len(prov)]) + prov + bytes([len(name)]) + name)
        d = sd
        flags = (0x02 if s.get("eit_schedule") else 0) | (0x01 if s.get("eit_pf", True) else 0)
        rs = s.get("running", 4)
        hdr = struct.pack(">HB", s["sid"], 0xFC | flags) + bytes([(rs << 5) | ((1 if s.get("free_ca") else 0) << 4) | (len(d) >> 8), len(d) & 0xFF])
        entries.append(hdr + d)
    sections, cur, size = [], [], 0
    room = 1021 - 8 - 3 - 4
    for e in entries:
        if cur and size + len(e) > room:
            sections.append(cur)
            cur, size = [], 0
        cur.append(e)
        size += len(e)
    sections.append(cur)
    tid = 0x42 if actual else 0x46
    out = []
    for i, c in enumerate(sections):
        body = struct.pack(">HB", onid, 0xFF) + b"".join(c)
        out.append(long_section(tid, tsid, version, i, len(sections) - 1, body))
    return out


def event_descriptors(ev, charset="iso8859-9"):
    """Descriptors of one event: short_event (title only), extended_event (the whole description,
    sub-title first), content, parental rating."""
    lang = lang_code(ev.get("lang"))
    out = []
    name = dvb_text(ev.get("title", ""), charset, 240)
    out.append(desc(0x4D, lang + bytes([len(name)]) + name + b"\x00"))
    sub = (ev.get("subtitle") or "").strip()
    long_text = (ev.get("description") or "").strip()
    if sub and not long_text.startswith(sub):
        long_text = f"{sub}. {long_text}" if long_text else sub
    if long_text:
        full = dvb_text(long_text, charset, 4000)
        prefix = full[:3] if full[:1] == b"\x10" else (full[:1] if full[:1] and full[0] < 0x20 else b"")
        body = full[len(prefix):]
        chunk = 240 - len(prefix)
        parts = [prefix + body[i:i + chunk] for i in range(0, len(body), chunk)][:16]
        for n, p in enumerate(parts):
            out.append(desc(0x4E, bytes([(n << 4) | (len(parts) - 1)]) + lang + b"\x00" + bytes([len(p)]) + p))
    if ev.get("content") is not None:
        out.append(desc(0x54, bytes([ev["content"] & 0xFF, 0x00])))
    if ev.get("rating") is not None:
        out.append(desc(0x55, (ev.get("country") or "FRA").encode()[:3] + bytes([ev["rating"] & 0xFF])))
    for raw in ev.get("raw_descriptors", ()):
        out.append(raw)
    return out


def _event_bytes(ev, running, charset):
    d = b"".join(event_descriptors(ev, charset))
    start = ev["start"]
    dur = ev["duration"]
    return (struct.pack(">H", ev["event_id"] & 0xFFFF) + mjd_utc(start) + bcd_duration(dur)
            + bytes([(running << 5) | (len(d) >> 8), len(d) & 0xFF]) + d)


def _eit_body(tsid, onid, seg_last, last_tid, events):
    return struct.pack(">HHBB", tsid, onid, seg_last, last_tid) + b"".join(events)


def eit_pf(sid, tsid, onid, present, following, version, charset="iso8859-9", actual=True):
    """Two sections: present (0) and following (1); each may be None (empty section)."""
    tid = 0x4E if actual else 0x4F
    out = []
    for n, (ev, rs) in enumerate(((present, 4), (following, 1))):
        events = [_event_bytes(ev, rs, charset)] if ev else []
        body = _eit_body(tsid, onid, 1, tid, events)
        if len(body) > 4084:
            events = [_event_bytes(dict(ev, description=""), rs, charset)]
            body = _eit_body(tsid, onid, 1, tid, events)
        out.append(long_section(tid, sid, version, n, 1, body))
    return out


def eit_schedule(sid, tsid, onid, events, now_utc, days, versions, charset="iso8859-9", actual=True):
    """EIT schedule sections of one service. events: sorted list with aware 'start'.
    versions(table_id) -> version number. Returns the list of sections."""
    base_tid = 0x50 if actual else 0x60
    midnight = now_utc.replace(hour=0, minute=0, second=0, microsecond=0)
    horizon = midnight + dt.timedelta(days=days)
    # 3-hour segments since midnight UTC of today; 32 segments (4 days) per table
    segs = {}
    for ev in events:
        st = ev["start"]
        end = st + dt.timedelta(seconds=ev["duration"])
        if end <= now_utc or st >= horizon:
            continue
        idx = max(0, int((st - midnight).total_seconds() // (3 * 3600)))
        segs.setdefault(idx, []).append(_event_bytes(ev, 0, charset))
    if not segs:
        return []
    last_seg = max(segs)
    ntables = last_seg // 32 + 1
    last_tid = base_tid + ntables - 1
    out = []
    for t in range(ntables):
        tid = base_tid + t
        tab_segs = []
        last_in_table = min(last_seg, t * 32 + 31)
        for seg in range(t * 32, last_in_table + 1):
            evs = segs.get(seg, [])
            # pack events of the segment in up to 8 sections
            secs, cur, size = [], [], 0
            for e in evs:
                if cur and size + len(e) > 4084 - 6:
                    secs.append(cur)
                    cur, size = [], 0
                cur.append(e)
                size += len(e)
            secs.append(cur)
            tab_segs.append(secs[:8])
        last_sec = (len(tab_segs) - 1) * 8 + len(tab_segs[-1]) - 1
        ver = versions(tid)
        for i, secs in enumerate(tab_segs):
            seg_first = i * 8
            seg_last = seg_first + len(secs) - 1
            for k, evs in enumerate(secs):
                body = _eit_body(tsid, onid, seg_last, last_tid, evs)
                out.append(long_section(tid, sid, ver, seg_first + k, last_sec, body))
    return out


def local_time_offset_descriptor(country, tzname, now_utc=None, region=0):
    """0x58 descriptor for the TOT: current offset, next change and next offset (from zoneinfo)."""
    from zoneinfo import ZoneInfo
    tz = ZoneInfo(tzname)
    now = now_utc or dt.datetime.now(dt.timezone.utc)
    cur = now.astimezone(tz).utcoffset()
    # find the next change within a year (hour steps, then minute refine)
    t = now
    change = None
    for _ in range(366 * 24):
        t2 = t + dt.timedelta(hours=1)
        if t2.astimezone(tz).utcoffset() != cur:
            lo, hi = t, t2
            while (hi - lo) > dt.timedelta(seconds=1):
                mid = lo + (hi - lo) / 2
                if mid.astimezone(tz).utcoffset() == cur:
                    lo = mid
                else:
                    hi = mid
            change = hi.replace(microsecond=0)
            break
        t = t2
    nxt = change.astimezone(tz).utcoffset() if change else cur

    def off(td):
        m = int(td.total_seconds() // 60)
        pol = 1 if m < 0 else 0
        m = abs(m)
        return pol, bytes([bcd(m // 60), bcd(m % 60)])

    pol, cur_b = off(cur)
    _, nxt_b = off(nxt)
    change = change or (now + dt.timedelta(days=365))
    body = country.upper().encode()[:3] + bytes([(region << 2) | 0x02 | pol]) + cur_b + mjd_utc(change) + nxt_b
    return desc(0x58, body)
