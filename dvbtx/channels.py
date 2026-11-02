"""Channels (services) added, removed, renamed, renumbered or given another source.

The same operations serve the running transmitter (web API, `dvbtx channel` talking to it, SIGHUP
after a manual edit of the file) and the configuration file alone when dvbtx is not running:
  - on the configuration dict: the new [[carrier.service]] lists are validated by config.build(),
    exactly as at start-up;
  - on the TOML file: only the [[carrier.service]] blocks concerned are rewritten, the rest of the
    file (comments, layout) is kept, and the result is read back to check it says what was meant.

A channel is designated by its number (LCN), its service_id (0x0101, sid:257) or its name.
Structural changes (add, remove, renumber) write sid / lcn / pid_base explicitly in every service
that relied on the defaults: these follow the position in the file, and TVs identify a channel by
its service_id, so a removal must not shift the others.
"""

import copy
import json
import os
import re
import tomllib

from . import config as cfgmod


class ChannelError(ValueError):
    pass


# keys that only change the SI tables (SDT, NIT, EIT): applied without touching the stream
SI_KEYS = {"name", "lcn", "provider", "visible", "type", "epg_id"}
# keys of the engine's service settings: a change re-creates the service (brief interruption)
ENGINE_KEYS = {"sid", "pid_base", "switch_timeout_ms", "return_ms", "max_late_ms", "early_ms", "pcr_max_ms",
               "pmt_period_ms", "priority"}
# keys read by the supervisor only (choice among the sources of a service): applied without touching anything
SUPERVISOR_KEYS = {"failover_s", "source_start_s", "preferred_retry_s", "unstable_switches", "unstable_window_s"}
# sources that run a command or read the transmitter's own input: refused from the web API unless
# web.allow_exec_sources (the configuration file and the offline CLI may use them)
EXEC_RE = re.compile(r"^(exec:|pipe:|stdin:?$|-$)")
# options that hand raw arguments to ffmpeg (an output file is one of them): same rule
RAW_KEYS = {"ffmpeg_input", "ffmpeg_output"}


def _check_options(keys, allow_exec):
    bad = sorted(set(keys) & RAW_KEYS)
    if bad and not allow_exec:
        raise ChannelError(f"{', '.join(bad)}: raw ffmpeg arguments, allowed in the configuration file only "
                           "(or set web.allow_exec_sources = true)")


# ------------------------------------------------------------------ designating a channel

def find(plan, ref):
    """Service of the plan designated by LCN (digits), service_id (0x..., sid:N) or name."""
    r = str(ref).strip()
    if not r:
        raise ChannelError("no channel given")
    m = re.fullmatch(r"(?:sid:)?(0x[0-9a-fA-F]+)|sid:(\d+)", r)
    if m:
        sid = int(m.group(1), 16) if m.group(1) else int(m.group(2))
        hit = [s for s in plan.services if s.sid == sid]
        if not hit:
            raise ChannelError(f"no channel with service_id 0x{sid:04X}")
        return hit[0]
    if r.isdigit():
        hit = [s for s in plan.services if s.lcn == int(r)]
        if hit:
            return hit[0]
    hit = [s for s in plan.services if s.name.casefold() == r.casefold()]
    if len(hit) == 1:
        return hit[0]
    if len(hit) > 1:
        raise ChannelError(f"several channels are named {r!r}: give the number or the service_id")
    raise ChannelError(f"no channel {r!r} (number, name, or service_id such as 0x0101)")


def find_carrier(plan, ref):
    if ref is None or ref == "":
        if len(plan.carriers) == 1:
            return plan.carriers[0]
        raise ChannelError("several multiplexes: say which one (carrier index or name)")
    r = str(ref).strip()
    if r.isdigit() and int(r) < len(plan.carriers):
        return plan.carriers[int(r)]
    hit = [c for c in plan.carriers if c.name.casefold() == r.casefold()]
    if not hit:
        raise ChannelError(f"no multiplex {r!r}")
    return hit[0]


def describe(s):
    """JSON view of a service of the plan"""
    return {"carrier": s.carrier.index, "carrier_name": s.carrier.name, "lcn": s.lcn, "sid": s.sid,
            "name": s.name, "source": s.source, "sources": list(s.sources or [s.source]), "active": s.active,
            "backup": s.backup, "pid_base": s.pid_base,
            "visible": s.visible, "provider": s.provider}


# ------------------------------------------------------------------ operations on the configuration

def _raw(cfg, svc):
    return cfg["carrier"][svc.carrier.index]["service"][svc.local_index]


def _pin_all(cfg, plan):
    """sid, lcn, pid_base written in every service that relied on the defaults"""
    for s in plan.services:
        raw = _raw(cfg, s)
        raw.setdefault("sid", s.sid)
        raw.setdefault("lcn", s.lcn)
        raw.setdefault("pid_base", s.pid_base)


def _free_sid(plan, car):
    used = {s.sid for s in plan.services}
    for n in range(1, 256):
        sid = (car.tsid << 8) | n
        if sid not in used and sid:
            return sid
    sid = 1
    while sid in used:
        sid += 1
    return sid


def _free_lcn(plan):
    used = {s.lcn for s in plan.services}
    n = 1
    while n in used:
        n += 1
    return n


def _free_pid_base(car):
    used = [s.pid_base for s in car.services]
    for b in range(0x100, 0x1FC1, 0x20):
        if all(b + 32 <= u or u + 32 <= b for u in used):
            return b
    raise ChannelError(f"{car.name}: no free PID range left")


def _check_name(name):
    if not isinstance(name, str) or not re.fullmatch(r"[^\"\\\x00-\x1f]{1,60}", name.strip()):
        raise ChannelError(f"name {name!r}: 1-60 characters, no quotes or backslashes")
    return name.strip()


def _check_source(src, allow_exec):
    if not isinstance(src, str) or not src.strip():
        raise ChannelError("source required")
    src = src.strip()
    if not allow_exec and EXEC_RE.match(src):
        raise ChannelError(f"source {src!r} runs a command or reads standard input: allowed in the configuration "
                           "file only (or set web.allow_exec_sources = true)")
    return src


def _check_sources(v, allow_exec):
    """a source, or a list of sources in order of preference (written as a string when there is one)"""
    lst = [v] if isinstance(v, str) else list(v) if isinstance(v, (list, tuple)) else []
    lst = [_check_source(x, allow_exec) for x in lst if not (isinstance(x, str) and not x.strip())]
    if not lst:
        raise ChannelError("source required")
    if len(set(lst)) != len(lst):
        raise ChannelError("the same source is given twice")
    return lst[0] if len(lst) == 1 else lst


def _build(cfg):
    try:
        return cfgmod.build(cfg)
    except cfgmod.ConfigError as e:
        raise ChannelError(str(e)) from e


def add(cfg, plan, spec, allow_exec=True):
    """New configuration with one more service. spec: name, source, and optionally carrier, lcn, sid,
    backup, and any service option. Returns (cfg, plan, sid)."""
    spec = dict(spec)
    _check_options(spec, allow_exec)
    car = find_carrier(plan, spec.pop("carrier", None))
    raw = {"name": _check_name(spec.pop("name", "")), "source": _check_sources(spec.pop("source", ""), allow_exec)}
    for k, v in spec.items():
        if v is None or v == "":
            continue
        if k == "backup":
            v = _check_source(v, allow_exec) if v is not False else False
        raw[k] = v
    new = copy.deepcopy(cfg)
    _pin_all(new, plan)
    raw.setdefault("lcn", _free_lcn(plan))
    raw["lcn"] = int(raw["lcn"])
    if isinstance(raw.get("sid"), str):
        raw["sid"] = int(raw["sid"], 0)
    raw.setdefault("sid", _free_sid(plan, car))
    raw.setdefault("pid_base", _free_pid_base(car))
    new["carrier"][car.index].setdefault("service", []).append(raw)
    return new, _build(new), raw["sid"]


def remove(cfg, plan, ref):
    s = find(plan, ref)
    new = copy.deepcopy(cfg)
    _pin_all(new, plan)
    del new["carrier"][s.carrier.index]["service"][s.local_index]
    return new, _build(new), s.sid


def update(cfg, plan, ref, changes, allow_exec=True):
    """changes: name, lcn (swap=True gives the old number to the channel that had it), source, backup
    ("" or false: none), and service options (None removes one). Returns (cfg, plan, sid)."""
    s = find(plan, ref)
    changes = dict(changes)
    _check_options(changes, allow_exec)
    swap = bool(changes.pop("swap", False))
    new = copy.deepcopy(cfg)
    raw = _raw(new, s)
    if "lcn" in changes:
        _pin_all(new, plan)
        lcn = int(changes.pop("lcn"))
        other = next((o for o in plan.services if o.lcn == lcn and o is not s), None)
        if other is not None:
            if not swap:
                raise ChannelError(f"number {lcn} is used by {other.name} (swap them with --swap)")
            _raw(new, other)["lcn"] = s.lcn
        raw["lcn"] = lcn
    for k, v in changes.items():
        if k == "name":
            raw["name"] = _check_name(v)
        elif k == "source":
            raw["source"] = _check_sources(v, allow_exec)
        elif k == "backup":
            raw["backup"] = _check_source(v, allow_exec) if v not in ("", None, False) else False
        elif k in ("sid", "carrier"):
            raise ChannelError(f"{k} cannot be changed: remove the channel and add it again")
        elif v is None:
            raw.pop(k, None)
        else:
            raw[k] = v
    return new, _build(new), s.sid


# ------------------------------------------------------------------ the TOML file

# written 482_000_000 like in the sample configuration
GROUPED_KEYS = {"frequency", "bandwidth", "symbol_rate", "rate", "center", "lnb_lo", "ts_rate"}
HEX_KEYS = {"sid", "pid_base", "onid", "network_id"}


def toml_value(v, key=None):
    if key in HEX_KEYS and isinstance(v, int) and not isinstance(v, bool):
        return f"0x{v:04X}"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return f"{v:_}" if key in GROUPED_KEYS and abs(v) >= 100_000 else str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(toml_value(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{ " + ", ".join(f"{_key(k)} = {toml_value(x)}" for k, x in v.items()) + " }"
    raise ChannelError(f"cannot write {v!r} in TOML")


def _key(k):
    return k if re.fullmatch(r"[A-Za-z0-9_-]+", k) else json.dumps(k)


_HDR = re.compile(r"^\s*(\[\[?)\s*([A-Za-z0-9_.\"' -]+?)\s*\]\]?\s*(#.*)?$")


class TomlFile:
    """[[carrier.service]] blocks of a dvbtx configuration file, edited in place."""

    def __init__(self, path):
        self.path = path
        with open(path, encoding="utf-8") as f:
            self.text = f.read()

    def _scan(self):
        """carriers: list of {start, end, services: [{start, end, keys_end, indent}]} (line numbers)"""
        lines = self.text.split("\n")
        cars, cur_car, cur_svc = [], None, None
        for i, line in enumerate(lines):
            m = _HDR.match(line)
            if not m:
                continue
            arr, name = m.group(1) == "[[", re.sub(r"\s+", "", m.group(2))
            if cur_svc and not (not arr and name.startswith("carrier.service.")):
                cur_svc["end"] = i
                cur_svc = None
            if arr and name == "carrier":
                if cur_car:
                    cur_car["end"] = i
                cur_car = {"start": i, "end": len(lines), "services": [], "keys_end": None}
                cars.append(cur_car)
            elif arr and name == "carrier.service" and cur_car:
                cur_svc = {"start": i, "end": len(lines), "keys_end": None,
                           "indent": re.match(r"\s*", line).group(0)}
                cur_car["services"].append(cur_svc)
            elif not arr and name.startswith("carrier.service.") and cur_svc:
                if cur_svc["keys_end"] is None:
                    cur_svc["keys_end"] = i
            elif cur_car and not name.startswith("carrier."):
                cur_car["end"] = i
                cur_car = None
        for c in cars:
            for s in c["services"]:
                if s["keys_end"] is None:
                    s["keys_end"] = s["end"]
        return lines, cars

    @staticmethod
    def _trim(lines, a, b):
        """last line of a block that belongs to it: trailing blank lines and comments go with the next"""
        while b > a + 1 and (not lines[b - 1].strip() or lines[b - 1].lstrip().startswith("#")):
            b -= 1
        return b

    def set_keys(self, ci, si, values, remove=()):
        lines, cars = self._scan()
        blk = cars[ci]["services"][si]
        a, b = blk["start"], self._trim(lines, blk["start"], blk["keys_end"])
        ind = blk["indent"]
        for k, v in values.items():
            pat = re.compile(rf"^\s*{re.escape(_key(k))}\s*=")
            hit = next((j for j in range(a + 1, b) if pat.match(lines[j])), None)
            new = f"{ind}{_key(k)} = {toml_value(v, k)}"
            if hit is None:
                lines.insert(b, new)
                b += 1
            else:
                lines[hit] = new
        for k in remove:
            pat = re.compile(rf"^\s*{re.escape(_key(k))}\s*=")
            for j in range(b - 1, a, -1):
                if pat.match(lines[j]):
                    del lines[j]
                    b -= 1
        self.text = "\n".join(lines)

    def delete(self, ci, si):
        lines, cars = self._scan()
        blk = cars[ci]["services"][si]
        a, b = blk["start"], self._trim(lines, blk["start"], blk["end"])
        while a > 0 and lines[a - 1].lstrip().startswith("#"):      # its own comment just above
            a -= 1
        if a > 0 and not lines[a - 1].strip() and (b >= len(lines) or not lines[b].strip()):
            a -= 1                                                  # one blank line less
        del lines[a:b]
        self.text = "\n".join(lines)

    def append(self, ci, values, indent=None):
        lines, cars = self._scan()
        car = cars[ci]
        if car["services"]:
            last = car["services"][-1]
            at = self._trim(lines, last["start"], last["end"])
            ind = last["indent"] if indent is None else indent
        else:
            at = self._trim(lines, car["start"], car["end"])
            ind = "  " if indent is None else indent
        block = ["", f"{ind}[[carrier.service]]"]
        sub = {}
        for k, v in values.items():
            if isinstance(v, dict):
                sub[k] = v
            else:
                block.append(f"{ind}{_key(k)} = {toml_value(v, k)}")
        for k, v in sub.items():
            block.append(f"{ind}{_key(k)} = {toml_value(v)}")
        lines[at:at] = block
        self.text = "\n".join(lines)

    def services(self):
        return [c.get("service", []) for c in tomllib.loads(self.text).get("carrier", [])]

    def save(self):
        """atomic when the folder is writable (kept as name.bak before), in place otherwise"""
        d = os.path.dirname(os.path.abspath(self.path))
        if os.access(d, os.W_OK):
            try:
                with open(self.path, "rb") as f, open(self.path + ".bak", "wb") as g:
                    g.write(f.read())
            except OSError:
                pass
            st = os.stat(self.path)
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(self.text)
            try:
                os.chown(tmp, st.st_uid, st.st_gid)
            except OSError:
                pass
            os.chmod(tmp, st.st_mode & 0o7777)
            os.replace(tmp, self.path)
        else:
            with open(self.path, "w", encoding="utf-8") as f:
                f.write(self.text)


def _file_services(path):
    with open(path, "rb") as f:
        user = tomllib.load(f)
    return [c.get("service", []) for c in user.get("carrier", [])]


def persist(path, old_cfg, new_cfg, removed=None):
    """Writes to the file the [[carrier.service]] changes between two configurations (removed: (carrier,
    position) of a removed service). The services of the file must be those of old_cfg (no --set /
    --service on top of them). Raises ChannelError, the file then left as it was."""
    have = _file_services(path)
    old = [list(c.get("service", [])) for c in old_cfg["carrier"]]
    new = [c.get("service", []) for c in new_cfg["carrier"]]
    if len(have) != len(old) or len(new) != len(old):
        raise ChannelError("the multiplexes of the file differ from those running")
    f = TomlFile(path)
    for ci in range(len(old)):
        if have[ci] != old[ci] and not _same_but_pins(have[ci], old[ci]):
            raise ChannelError(f"the channels of multiplex {ci} in the file differ from those running "
                               "(edited by hand, or set on the command line): file left as it is")
    if removed:
        f.delete(*removed)
        del old[removed[0]][removed[1]]
    for ci in range(len(old)):
        o, n = old[ci], new[ci]
        if len(n) < len(o):
            raise ChannelError("internal: services lost")
        for si, x in enumerate(n):
            if si < len(o):
                y = o[si]
                setk = {k: v for k, v in x.items() if y.get(k) != v}
                rem = [k for k in y if k not in x]
                if setk or rem:
                    f.set_keys(ci, si, setk, rem)
            else:
                f.append(ci, x)
    if f.services() != new:
        raise ChannelError("could not edit the file safely (unusual layout): file left as it is")
    f.save()


def _same_but_pins(file_list, run_list):
    """the running list = the file's + sid/lcn/pid_base pinned by an earlier change of this run"""
    if len(file_list) != len(run_list):
        return False
    for a, b in zip(file_list, run_list):
        extra = {k for k in b if k not in a}
        if extra - {"sid", "lcn", "pid_base"} or any(a[k] != b[k] for k in a if k in b) or set(a) - set(b):
            return False
    return True
