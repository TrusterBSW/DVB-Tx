"""Radios plugged in, as their own tools see them (hackrf_info, uhd_find_devices, SoapySDRUtil): their
serial numbers, for the `serial` key of a [[radio]] (dvbtx devices, /api/devices, the web page).
edgeQAM boards (FPGA DVB-T modulators on the network) are found by mDNS (avahi-browse _dvbmod._udp):
their host name stands for a serial."""
import re
import shutil
import subprocess

# what each driver's tool prints for one device
TOOLS = {"hackrf": ["hackrf_info"], "uhd": ["uhd_find_devices"], "soapy": ["SoapySDRUtil", "--find"],
         "edgeqam": ["avahi-browse", "-rpt", "_dvbmod._udp"]}
# receive-only Soapy modules, or radios better driven by the native plugins
SOAPY_SKIP = {"audio", "rtlsdr", "airspy", "airspyhf", "sdrplay", "remote", "hackrf", "uhd"}
TIMEOUT_S = 20
MDNS_TIMEOUT_S = 5

SERIAL_RE = re.compile(r"[A-Za-z0-9_-]{1,63}")
BOARD_ID_RE = re.compile(r"[A-Za-z0-9_.-]{1,63}")        # an edgeQAM board: its TXT id, host or service name


def _blocks(text, start):
    """the text cut where a line matches start"""
    out, cur = [], None
    for line in text.splitlines():
        if re.match(start, line.strip()):
            cur = []
            out.append(cur)
        elif cur is not None:
            cur.append(line.strip())
    return out


def _kv(lines, sep):
    d = {}
    for line in lines:
        k, s, v = line.partition(sep)
        if s:
            d[k.strip().lower()] = v.strip()
    return d


def parse(driver, text):
    """[{"serial", "label"}] from the tool's output"""
    found = []
    if driver == "hackrf":
        for b in _blocks(text, r"Found HackRF"):
            d = _kv(b, ":")
            sn = d.get("serial number", "").replace("0x", "").replace(" ", "")
            if sn:
                found.append({"serial": sn, "label": d.get("board id number", "HackRF")})
    elif driver == "uhd":
        for b in _blocks(text, r"-- UHD Device \d+"):
            d = _kv(b, ":")
            if d.get("serial"):
                found.append({"serial": d["serial"],
                              "label": " ".join(x for x in (d.get("product") or d.get("type"), d.get("name")) if x)})
    elif driver == "soapy":
        for b in _blocks(text, r"Found device \d+"):
            d = _kv(b, "=")
            if d.get("driver") in SOAPY_SKIP or not d.get("serial"):
                continue
            found.append({"serial": d["serial"], "label": d.get("label") or d.get("driver", "")})
    elif driver == "edgeqam":
        # avahi-browse -rpt: =;iface;IPv4;name;_dvbmod._udp;local;host.local;address;port;"k=v" "k=v"...
        for line in text.splitlines():
            f = line.split(";", 9)
            if len(f) < 10 or f[0] != "=" or f[2] != "IPv4" or f[4] != "_dvbmod._udp":
                continue
            txt = dict(kv.split("=", 1) for kv in re.findall(r'"([^"]*)"', f[9]) if "=" in kv)
            if txt.get("std", "dvbt") != "dvbt":
                continue
            try:
                ports = [int(p) for p in txt.get("ports", f[8]).split(",") if p.strip()]
                api = int(txt.get("api", 8080))
            except ValueError:
                continue
            name = re.sub(r"\\(\d{3})", lambda m: chr(int(m.group(1))), f[3])   # avahi escapes: \032 = space
            host = f[6].removesuffix(".local").split(".")[0]
            # its serial: the TXT id (its Ethernet MAC, unique and stable), else its host name (which avahi
            # renames plutoplus-2 when two boards share one, in the order they booted)
            found.append({"serial": txt.get("id") or host, "host": host, "label": name, "address": f[7],
                          "ports": ports[:int(txt.get("nmux", len(ports)))], "api": api,
                          "freq1": int(txt.get("freq1", 0) or 0), "txt": txt})
    seen, out = set(), []
    for f in found:
        if (BOARD_ID_RE if driver == "edgeqam" else SERIAL_RE).fullmatch(f["serial"]) and f["serial"] not in seen:
            seen.add(f["serial"])
            out.append(f)
    return out


def find(driver):
    """(devices, error): the devices of this driver plugged in, or why none could be listed"""
    cmd = TOOLS.get(driver)
    if cmd is None:
        return [], f"no tool lists the {driver} radios"
    if not shutil.which(cmd[0]):
        return [], f"{cmd[0]} is not installed"
    timeout = MDNS_TIMEOUT_S if driver == "edgeqam" else TIMEOUT_S
    try:
        r = subprocess.run(cmd + (["-t"] if driver == "edgeqam" else []), capture_output=True, text=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return [], f"{cmd[0]} did not answer within {timeout} s"
    except OSError as e:
        return [], f"{cmd[0]}: {e}"
    return parse(driver, r.stdout + "\n" + r.stderr), None


def serial_of(args):
    """the serial=... of an args string, or None"""
    m = re.search(r"(?:^|,)\s*serial\s*=\s*([^,\s]*)", args or "")
    return m.group(1) if m else None


def with_serial(args, serial):
    """args with serial=... set (added, or replaced)"""
    rest = [a for a in (args or "").split(",") if a.strip() and not re.match(r"\s*serial\s*=", a)]
    return ",".join(rest + [f"serial={serial}"] if serial else rest)


def same(driver, a, b):
    """two serials of the same device: libhackrf opens a HackRF by the end of its serial (hackrf_info prints
    32 digits, the user may keep the last ones); the others compare whole, without case"""
    a, b = (a or "").lower(), (b or "").lower()
    if not a or not b:
        return False
    if driver == "hackrf":
        return a.endswith(b) or b.endswith(a)
    return a == b


def board_matches(d, key):
    """an edgeQAM board found by mDNS (parse) answers to key: its TXT id, its host name or its service name"""
    key = (key or "").lower()
    return bool(key) and key in {(d.get(k) or "").lower() for k in ("serial", "host", "label")}


def owner(driver, serial, bound, dev=None):
    """bound: {serial: radio name}; the radio bound to that device (dev: an edgeQAM board as parse found
    it, bound by its id, host or service name), or None"""
    return next((n for sn, n in bound.items()
                 if (board_matches(dev, sn) if driver == "edgeqam" and dev else same(driver, sn, serial))), None)
