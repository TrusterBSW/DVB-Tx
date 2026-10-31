"""The configuration file edited from the web page: radios, multiplexes, channels, profiles.

Every change is checked (the whole file must still build, with the rules of the start-up) and written
to the file at once, its comments and layout kept. The transmitter does not change until it is asked
to apply the file (Supervisor.apply_config): so several settings can be prepared and applied
together. The difference between the file and what runs is listed (pending changes).

Profiles are copies of the file beside it, <file>.<name> (dvbtx.toml.night): saving one copies the
file, loading one copies it over the file; only the file itself is ever applied.
"""

import copy
import os
import re
import tomllib

from . import channels as chmod
from . import config as cfgmod
from .channels import ChannelError, TomlFile, _HDR, _key, toml_value

RADIO_KEYS = ("name",) + tuple(cfgmod.RADIO_DEFAULTS)
CARRIER_KEYS = set(cfgmod.CARRIER_COMMON_KEYS) | {k for ks in cfgmod.STD_KEYS.values() for k in ks}
PROFILE_RE = re.compile(r"[A-Za-z0-9_-]{1,40}")


class Tables(TomlFile):
    """[[radio]] and [[carrier]] tables of the file (not their sub-tables) edited in place"""

    def _tables(self, name):
        """blocks of the array of tables 'name': [{start, keys_end, end}] (line numbers)"""
        lines = self.text.split("\n")
        out, cur = [], None
        for i, line in enumerate(lines):
            m = _HDR.match(line)
            if not m:
                continue
            arr, hname = m.group(1) == "[[", re.sub(r"\s+", "", m.group(2))
            if cur and cur["keys_end"] is None:
                cur["keys_end"] = i
            if cur and not hname.startswith(name + "."):
                cur["end"] = i
                cur = None
            if arr and hname == name:
                cur = {"start": i, "keys_end": None, "end": len(lines)}
                out.append(cur)
        if cur and cur["keys_end"] is None:
            cur["keys_end"] = len(lines)
        return lines, out

    def set_table_keys(self, name, i, values, remove=()):
        lines, blocks = self._tables(name)
        blk = blocks[i]
        a, b = blk["start"], self._trim(lines, blk["start"], blk["keys_end"])
        for k, v in values.items():
            pat = re.compile(rf"^\s*{re.escape(_key(k))}\s*=")
            hit = next((j for j in range(a + 1, b) if pat.match(lines[j])), None)
            new = f"{_key(k)} = {toml_value(v, k)}"
            if hit is None:
                lines.insert(b, new)
                b += 1
            else:
                # its comment kept (one without quotes after the value: never part of a string)
                c = re.search(r"\s+#[^\"']*$", lines[hit])
                lines[hit] = new + (c.group(0) if c else "")
        for k in remove:
            pat = re.compile(rf"^\s*{re.escape(_key(k))}\s*=")
            for j in range(b - 1, a, -1):
                if pat.match(lines[j]):
                    del lines[j]
                    b -= 1
        self.text = "\n".join(lines)

    def delete_table(self, name, i):
        lines, blocks = self._tables(name)
        blk = blocks[i]
        a, b = blk["start"], self._trim(lines, blk["start"], blk["end"])
        while a > 0 and lines[a - 1].lstrip().startswith("#"):     # its own comment just above
            a -= 1
        if a > 0 and not lines[a - 1].strip() and (b >= len(lines) or not lines[b].strip()):
            a -= 1
        del lines[a:b]
        self.text = "\n".join(lines)

    def append_table(self, name, values, before=None):
        """a new [[name]] after the last one (else before the first [[before]], else at the end)"""
        lines, blocks = self._tables(name)
        if blocks:
            at = self._trim(lines, blocks[-1]["start"], blocks[-1]["end"])
        else:
            at = len(lines)
            if before:
                _, nxt = self._tables(before)
                if nxt:
                    at = nxt[0]["start"]
                    while at > 0 and lines[at - 1].lstrip().startswith("#"):
                        at -= 1
        block = ["", f"[[{name}]]"] + [f"{_key(k)} = {toml_value(v, k)}" for k, v in values.items()]
        if at < len(lines) and lines[at].strip():
            block.append("")
        lines[at:at] = block
        self.text = "\n".join(lines)

    def doc(self):
        return tomllib.loads(self.text)


def _strip_services(cars):
    return [{k: v for k, v in c.items() if k != "service"} for c in cars]


class Draft:
    """the configuration file as edited; validate(text) builds it like the start-up does"""

    def __init__(self, path, validate):
        self.path = path
        self.validate = validate          # text -> (cfg, plan), raises ConfigError

    def text(self):
        with open(self.path, encoding="utf-8") as f:
            return f.read()

    def doc(self):
        return tomllib.loads(self.text())

    def _commit(self, t, check):
        """t: the edited Tables; check(doc) -> True when the file says what was meant"""
        try:
            doc = t.doc()
        except tomllib.TOMLDecodeError as e:
            raise ChannelError(f"could not edit the file safely: {e}") from e
        if not check(doc):
            raise ChannelError("could not edit the file safely (unusual layout): file left as it is")
        try:
            self.validate(t.text)
        except cfgmod.ConfigError as e:
            raise ChannelError(str(e)) from e
        t.save()
        return {"saved": True}

    # ------------------------------------------------------------ radios and multiplexes

    def _clean(self, kind, values):
        allowed = RADIO_KEYS if kind == "radio" else CARRIER_KEYS
        bad = [k for k in values if k not in allowed]
        if bad:
            raise ChannelError(f"unknown {kind} setting(s): {', '.join(bad)}")
        return values

    def update(self, kind, i, changes):
        """changes: {key: value, key: None (removed)}; a new standard drops the parameters of the old one"""
        name = "radio" if kind == "radio" else "carrier"
        self._clean(kind, changes)
        doc = self.doc()
        tabs = doc.get(name, [])
        if not 0 <= i < len(tabs):
            raise ChannelError(f"no {kind} {i}")
        cur = tabs[i]
        sets = {k: v for k, v in changes.items() if v is not None and cur.get(k) != v}
        rem = [k for k, v in changes.items() if v is None and k in cur]
        if kind == "carrier":
            std = str(changes.get("standard") or cur.get("standard", "dvbt")).lower()
            if std != str(cur.get("standard", "dvbt")).lower():
                keep = set(cfgmod.STD_KEYS.get(std, ())) | set(cfgmod.CARRIER_COMMON_KEYS)
                rem += [k for k in cur if k not in keep and k != "service" and k not in sets and k not in rem]
            if "frequency" in sets and "channel" in cur and "channel" not in sets:
                rem.append("channel")
        if not sets and not rem:
            return {"saved": False, "unchanged": True}
        old_name = cur.get("name")
        t = Tables(self.path)
        t.set_table_keys(name, i, sets, rem)
        want = dict(cur)
        want.update(sets)
        for k in rem:
            want.pop(k, None)
        if kind == "radio" and "name" in sets and old_name:
            # the multiplexes that name this radio follow it
            for ci, c in enumerate(doc.get("carrier", [])):
                if c.get("radio") == old_name:
                    t.set_table_keys("carrier", ci, {"radio": sets["name"]})
        return self._commit(t, lambda d: {k: v for k, v in d[name][i].items()} == want)

    def add(self, kind, values):
        name = "radio" if kind == "radio" else "carrier"
        values = {k: v for k, v in self._clean(kind, values).items() if v is not None}
        doc = self.doc()
        n = len(doc.get(name, []))
        t = Tables(self.path)
        t.append_table(name, values, before="carrier" if kind == "radio" else None)
        return self._commit(t, lambda d: len(d.get(name, [])) == n + 1 and
                            {k: v for k, v in d[name][n].items()} == values)

    def remove(self, kind, i):
        name = "radio" if kind == "radio" else "carrier"
        doc = self.doc()
        tabs = doc.get(name, [])
        if not 0 <= i < len(tabs):
            raise ChannelError(f"no {kind} {i}")
        if kind == "radio":
            rname = tabs[i].get("name", f"radio{i}")
            users = [c.get("name", f"Mux {ci + 1}") for ci, c in enumerate(doc.get("carrier", []))
                     if c.get("radio", 0) in (rname, i)]
            if users:
                raise ChannelError(f"radio {rname} carries {', '.join(users)}: move or remove them first")
        t = Tables(self.path)
        t.delete_table(name, i)
        want = [x for j, x in enumerate(tabs) if j != i]
        return self._commit(t, lambda d: d.get(name, []) == want)

    # ------------------------------------------------------------ channels (on the file only)

    def channels(self, fn):
        """fn(cfg, plan) -> (new_cfg, removed) with the channel functions of channels.py, applied to the file"""
        text = self.text()
        cfg, plan = self.validate(text)
        new_cfg, removed = fn(cfg, plan)
        chmod.persist(self.path, cfg, new_cfg, removed)
        return {"saved": True}

    # ------------------------------------------------------------ profiles

    def profiles(self):
        d, base = os.path.dirname(os.path.abspath(self.path)), os.path.basename(self.path)
        cur = self.text()
        out = []
        for f in sorted(os.listdir(d)):
            if f.startswith(base + ".") and PROFILE_RE.fullmatch(f[len(base) + 1:]) and f[len(base) + 1:] not in ("bak", "tmp"):
                p = os.path.join(d, f)
                try:
                    with open(p, encoding="utf-8") as fh:
                        txt = fh.read()
                except OSError:
                    continue
                out.append({"name": f[len(base) + 1:], "mtime": os.path.getmtime(p), "same": txt == cur})
        return out

    def _profile_path(self, name):
        if not PROFILE_RE.fullmatch(name or "") or name in ("bak", "tmp"):
            raise ChannelError("profile name: 1-40 letters, digits, - or _")
        return f"{self.path}.{name}"

    def save_profile(self, name):
        p = self._profile_path(name)
        _write(p, self.text(), like=self.path)
        return {"saved": True, "profile": name}

    def load_profile(self, name):
        p = self._profile_path(name)
        try:
            with open(p, encoding="utf-8") as f:
                txt = f.read()
        except OSError as e:
            raise ChannelError(f"profile {name}: {e}") from e
        try:
            self.validate(txt)
        except cfgmod.ConfigError as e:
            raise ChannelError(f"profile {name} is not valid: {e}") from e
        _write(self.path + ".bak", self.text(), like=self.path)
        _write(self.path, txt, like=self.path)
        return {"saved": True, "profile": name}

    def delete_profile(self, name):
        try:
            os.unlink(self._profile_path(name))
        except OSError as e:
            raise ChannelError(f"profile {name}: {e}") from e
        return {"deleted": True}

    def restore(self, text):
        """the file back to a given text (the configuration running: pending changes dropped)"""
        _write(self.path + ".bak", self.text(), like=self.path)
        _write(self.path, text, like=self.path)
        return {"saved": True}


def _write(path, text, like):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    try:
        st = os.stat(like)
        os.chmod(tmp, st.st_mode & 0o7777)
        os.chown(tmp, st.st_uid, st.st_gid)
    except OSError:
        pass
    os.replace(tmp, path)


def diff(old, new):
    """pending changes between the configuration running (old doc) and the file (new doc):
    [{"what": radio|carrier|channel|settings, "op": added|removed|changed, "name", "keys": {k: [old, new]}}]"""
    out = []

    def keys(a, b, skip=()):
        return {k: [a.get(k), b.get(k)] for k in sorted(set(a) | set(b)) if k not in skip and a.get(k) != b.get(k)}

    for kind in ("radio", "carrier"):
        a, b = old.get(kind, []), new.get(kind, [])
        for i in range(max(len(a), len(b))):
            x, y = (a[i] if i < len(a) else None), (b[i] if i < len(b) else None)
            nm = (y or x).get("name", f"{kind} {i}")
            if x is None:
                out.append({"what": kind, "op": "added", "name": nm, "index": i})
            elif y is None:
                out.append({"what": kind, "op": "removed", "name": nm, "index": i})
            else:
                k = keys(x, y, ("service",))
                if k:
                    out.append({"what": kind, "op": "changed", "name": nm, "index": i, "keys": k})

    def svcs(doc):
        return [(ci, s) for ci, c in enumerate(doc.get("carrier", [])) for s in c.get("service", [])]
    a, b = svcs(old), svcs(new)
    used = set()

    def match(s):                       # same service_id, else same name
        for j, (ci, x) in enumerate(a):
            if j not in used and s.get("sid") is not None and x.get("sid") == s.get("sid"):
                return j
        for j, (ci, x) in enumerate(a):
            if j not in used and x.get("name") == s.get("name"):
                return j
        return None
    for ci, s in b:
        j = match(s)
        if j is None:
            out.append({"what": "channel", "op": "added", "name": s.get("name"), "carrier": ci})
            continue
        used.add(j)
        oci, o = a[j]
        # sid, lcn and pid_base written in the file by an earlier change only pin what was used anyway
        k = {x: v for x, v in keys(o, s).items() if not (x in ("sid", "lcn", "pid_base") and v[0] is None)}
        if oci != ci:
            k["carrier"] = [oci, ci]
        if k:
            out.append({"what": "channel", "op": "changed", "name": s.get("name"), "carrier": ci, "keys": k})
    for j, (ci, s) in enumerate(a):
        if j not in used:
            out.append({"what": "channel", "op": "removed", "name": s.get("name"), "carrier": ci})
    rest_a = {k: v for k, v in old.items() if k not in ("radio", "carrier")}
    rest_b = {k: v for k, v in new.items() if k not in ("radio", "carrier")}
    for sec in sorted(set(rest_a) | set(rest_b)):
        if rest_a.get(sec) != rest_b.get(sec):
            out.append({"what": "settings", "op": "changed", "name": sec,
                        "keys": keys(rest_a.get(sec) or {}, rest_b.get(sec) or {}) if isinstance(rest_a.get(sec, {}), dict)
                        and isinstance(rest_b.get(sec, {}), dict) else {}})
    return out


def needs_restart(old, new):
    """True when applying changes more than the channels (radios, multiplexes, other settings)"""
    a, b = copy.deepcopy(old), copy.deepcopy(new)
    a["carrier"], b["carrier"] = _strip_services(a.get("carrier", [])), _strip_services(b.get("carrier", []))
    a.pop("defaults", None)
    b.pop("defaults", None)
    return a != b
