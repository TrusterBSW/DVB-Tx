# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos).
"""DVB-T2 (EN 302 755) parameter checks and exact TS rate, mirroring engine/src/dvbt2.c.

Keyword parameters (all optional, defaults = UK-like 32K profile):
  bandwidth        Hz (8e6) or MHz (8); 1.7, 5, 6, 7, 8, 10 MHz
  fft              "1k" "2k" "4k" "8k" "16k" "32k" (or 1024..32768)
  extended         bool, extended carrier mode (8K, 16K, 32K)
  guard            "1/128" "1/32" "1/16" "19/256" "1/8" "19/128" "1/4"
  pilot            "PP1".."PP8" (or 1..8)
  constellation    "qpsk" "16qam" "64qam" "256qam"
  rotation         bool (no effect on the rate)
  code_rate        "1/2" "3/5" "2/3" "3/4" "4/5" "5/6"
  frame            "normal" (64800) or "short" (16200)
  fec_blocks       FEC blocks per T2 frame, 0 = maximum that fits
  data_symbols     L_data, 0 = longest frame <= 250 ms
  ti_blocks        time interleaving blocks, 0 = fewest that fit the TI memory
  l1_constellation "bpsk" "qpsk" "16qam" "64qam" (L1-post)
  papr             must be 0 / "none"
  cell_id, network_id, t2_system_id, t2_frames, frequency: accepted, no effect on the rate

rate() returns the useful TS bit rate (188-byte packets), equal to dvbt2_ts_rate().
"""
from fractions import Fraction

DEFAULTS = dict(bandwidth=8e6, fft="32k", extended=True, guard="1/128", pilot="PP7",
                constellation="256qam", rotation=True, code_rate="5/6", frame="normal",
                fec_blocks=0, data_symbols=0, ti_blocks=0, l1_constellation="64qam", papr=0,
                cell_id=0, network_id=0x3085, t2_system_id=0x8001, t2_frames=0, frequency=0)

_FFTS = [1024, 2048, 4096, 8192, 16384, 32768]
_GIS = [(1, 128), (1, 32), (1, 16), (19, 256), (1, 8), (19, 128), (1, 4)]
# EN 302 755 table 58 (SISO): allowed pilot patterns [fft index][guard index], None = GI not allowed
_PP = [
    [None, None, {4, 5}, None, {2, 3}, None, {1}],
    [None, {7, 4}, {4, 5}, None, {2, 3}, None, {1}],
    [None, {7, 4}, {4, 5}, None, {2, 3}, None, {1}],
    [{7}, {7, 4}, {8, 4, 5}, {8, 4, 5}, {2, 3, 8}, {2, 3, 8}, {1, 8}],
    [{7}, {7, 4, 6}, {2, 8, 4, 5}, {2, 8, 4, 5}, {2, 3, 8}, {2, 3, 8}, {1, 8}],
    [{7}, {4, 6}, {2, 8, 4}, {2, 8, 4}, {2, 8}, {2, 8}, None],
]
# C_DATA, N_FC, C_FC [fft][extended][pp-1] (tables 48-50, SISO, no TR) as in gr-dtv
_CD = [
    [[(764, 568, 402), (768, 710, 654), (798, 710, 490), (804, 780, 707), (818, 780, 544), (0, 0, 0),
      (0, 0, 0), (0, 0, 0)]],
    [[(1522, 1136, 804), (1532, 1420, 1309), (1596, 1420, 980), (1602, 1562, 1415), (1632, 1562, 1088),
      (0, 0, 0), (1646, 1632, 1396), (0, 0, 0)]],
    [[(3084, 2272, 1609), (3092, 2840, 2619), (3228, 2840, 1961), (3234, 3124, 2831), (3298, 3124, 2177),
      (0, 0, 0), (3328, 3266, 2792), (0, 0, 0)]],
    [[(6208, 4544, 3218), (6214, 5680, 5238), (6494, 5680, 3922), (6498, 6248, 5662), (6634, 6248, 4354),
      (0, 0, 0), (6698, 6532, 5585), (6698, 0, 0)],
     [(6296, 4608, 3264), (6298, 5760, 5312), (6584, 5760, 3978), (6588, 6336, 5742), (6728, 6336, 4416),
      (0, 0, 0), (6788, 6624, 5664), (6788, 0, 0)]],
    [[(12418, 9088, 6437), (12436, 11360, 10476), (12988, 11360, 7845), (13002, 12496, 11324),
      (13272, 12496, 8709), (13288, 13064, 11801), (13416, 13064, 11170), (13406, 0, 0)],
     [(12678, 9280, 6573), (12698, 11600, 10697), (13262, 11600, 8011), (13276, 12760, 11563),
      (13552, 12760, 8893), (13568, 13340, 12051), (13698, 13340, 11406), (13688, 0, 0)]],
    [[(0, 0, 0), (24886, 22720, 20952), (0, 0, 0), (26022, 24992, 22649), (0, 0, 0), (26592, 26128, 23603),
      (26836, 0, 0), (26812, 0, 0)],
     [(0, 0, 0), (25412, 23200, 21395), (0, 0, 0), (26572, 25520, 23127), (0, 0, 0), (27152, 26680, 24102),
      (27404, 0, 0), (27376, 0, 0)]],
]
_NP2 = [16, 8, 4, 2, 1, 1]
_CP2 = [558, 1118, 2236, 4472, 8944, 22432]
_CPS = [(853, None), (1705, None), (3409, None), (6817, 6913), (13633, 13921), (27265, 27841)]
# Kbch (normal, short) per code rate
_KBCH = {"1/2": (32208, 7032), "3/5": (38688, 9552), "2/3": (43040, 10632), "3/4": (48408, 11712),
         "4/5": (51648, 12432), "5/6": (53840, 13152)}
_BITS = {"qpsk": 2, "16qam": 4, "64qam": 6, "256qam": 8}
_L1 = {"bpsk": 1, "qpsk": 2, "16qam": 4, "64qam": 6}
# elementary period T in microseconds, EN 302 755 table 65
_T = {1.7: Fraction(71, 131), 5: Fraction(7, 40), 6: Fraction(7, 48), 7: Fraction(1, 8), 8: Fraction(7, 64),
      10: Fraction(7, 80)}
M_TI = (1 << 19) + (1 << 15)


def _norm(params):
    unknown = set(params) - set(DEFAULTS)
    if unknown:
        raise ValueError("unknown DVB-T2 parameter(s): " + ", ".join(sorted(unknown)))
    p = dict(DEFAULTS)
    p.update({k: v for k, v in params.items() if v is not None})
    return p


def _bw_t(bw):
    try:
        bw = float(bw)
    except (TypeError, ValueError):
        raise ValueError("bandwidth must be a number (Hz or MHz)")
    mhz = bw / 1e6 if bw > 1000 else bw
    if abs(mhz - 1.7) < 0.02 or abs(mhz - 1.712) < 0.02:
        return _T[1.7]
    for k in (5, 6, 7, 8, 10):
        if abs(mhz - k) < 1e-6:
            return _T[k]
    raise ValueError("DVB-T2 bandwidth must be 1.7, 5, 6, 7, 8 or 10 MHz")


def _fft(v):
    if isinstance(v, str):
        s = v.strip().lower()
        if s.endswith("k") and s[:-1].isdigit():
            v = int(s[:-1]) * 1024
        elif s.isdigit():
            v = int(s)
        else:
            raise ValueError("fft must be 1k, 2k, 4k, 8k, 16k or 32k")
    if v not in _FFTS:
        raise ValueError("fft must be 1k, 2k, 4k, 8k, 16k or 32k")
    return _FFTS.index(v), v


def _frac(v, what):
    try:
        if isinstance(v, (tuple, list)):
            a, b = int(v[0]), int(v[1])
        else:
            a, b = (int(x) for x in str(v).replace(" ", "").split("/"))
    except (TypeError, ValueError):
        raise ValueError(f"{what} must be written like 3/5, got {v!r}")
    return a, b


def resolve(**params):
    """Check the parameters and return a dict with all derived values (ValueError if invalid)."""
    p = _norm(params)
    T = _bw_t(p["bandwidth"])
    fidx, N = _fft(p["fft"])
    ext = bool(p["extended"])
    if ext and fidx < 3:
        raise ValueError("extended carrier mode exists only for 8K, 16K and 32K")
    gi = _frac(p["guard"], "guard")
    if gi not in _GIS:
        raise ValueError("guard interval must be 1/128, 1/32, 1/16, 19/256, 1/8, 19/128 or 1/4")
    g = _GIS.index(gi)
    allowed = _PP[fidx][g]
    if allowed is None:
        raise ValueError(f"guard interval {gi[0]}/{gi[1]} is not allowed with FFT {N // 1024}K "
                         "(EN 302 755 table 67)")
    pp = p["pilot"]
    if isinstance(pp, str):
        s = pp.strip().upper()
        pp = int(s[2:]) if s.startswith("PP") and s[2:].isdigit() else (int(s) if s.isdigit() else 0)
    if pp not in range(1, 9):
        raise ValueError("pilot must be PP1..PP8")
    if pp not in allowed:
        raise ValueError(f"PP{pp} is not allowed with FFT {N // 1024}K and guard interval {gi[0]}/{gi[1]}"
                         f" (EN 302 755 table 58; allowed: {', '.join('PP%d' % x for x in sorted(allowed))})")
    con = str(p["constellation"]).lower().replace("-", "").replace(" ", "")
    if con not in _BITS:
        raise ValueError("constellation must be qpsk, 16qam, 64qam or 256qam")
    bits = _BITS[con]
    cr = "%d/%d" % _frac(p["code_rate"], "code_rate")
    if cr not in _KBCH:
        raise ValueError("DVB-T2 code rate must be 1/2, 3/5, 2/3, 3/4, 4/5 or 5/6")
    frame = str(p["frame"]).lower()
    if frame not in ("normal", "short", "64800", "16200"):
        raise ValueError("frame must be normal (64800) or short (16200)")
    short = frame in ("short", "16200")
    nldpc = 16200 if short else 64800
    kbch = _KBCH[cr][1 if short else 0]
    cells = nldpc // bits
    l1 = str(p["l1_constellation"]).lower().replace("-", "")
    if l1 not in _L1:
        raise ValueError("l1_constellation must be bpsk, qpsk, 16qam or 64qam")
    eta = _L1[l1]
    if p["papr"] not in (0, None, False, "none", "off"):
        raise ValueError("PAPR reduction is not supported")
    for k in ("cell_id", "network_id", "t2_system_id"):
        if not 0 <= int(p[k]) <= 0xffff:
            raise ValueError(f"{k} is a 16-bit value")
    if not 0 <= int(p["t2_frames"]) <= 255:
        raise ValueError("t2_frames must be 1..255")

    np2, cp2 = _NP2[fidx], _CP2[fidx]
    cps = _CPS[fidx][1 if ext else 0]
    cdata, nfc, cfc = _CD[fidx][1 if ext else 0][pp - 1]
    if not cdata:
        raise ValueError(f"PP{pp} is not available with FFT {N // 1024}K")
    if (gi == (1, 128) and pp == 7) or (gi == (1, 32) and pp == 4) or (gi == (1, 16) and pp == 2) or \
            (gi == (19, 256) and pp == 2):
        nfc = cfc = 0
    gis = N // gi[1] * gi[0]
    S = N + gis
    npunc_temp = (6 * (7032 - 350)) // 5
    npost_temp = 350 + 168 + 9000 - npunc_temp
    d = 2 * eta if np2 == 1 else eta * np2
    npost = -(-npost_temp // d) * d

    lfmin = np2 + (3 if fidx == 5 else 7)
    # T_F = (2048 + LF S) T <= 250 ms
    lfmax = int((Fraction(250000) / T - 2048) // S)
    if fidx == 5 and lfmax % 2:
        lfmax -= 1
    lfmax = min(lfmax, np2 + 4095)
    ld = int(p["data_symbols"] or 0)
    if ld:
        lf = np2 + ld
        if lf < lfmin:
            raise ValueError("too few data symbols (32K: at least 3)" if fidx == 5
                             else "too few data symbols (at least 7)")
        if lf > lfmax:
            raise ValueError(f"too many data symbols: the T2 frame would be longer than 250 ms "
                             f"(max {lfmax - np2})")
        if fidx == 5 and lf % 2:
            raise ValueError("32K: the number of symbols per frame (data symbols + 1) must be even")
    else:
        lf = lfmax
        if lf < lfmin:
            raise ValueError("frame too short")
    ldata = lf - np2
    mapped = np2 * cp2 + ((ldata - 1) * cdata + nfc if nfc else ldata * cdata)
    avail = mapped - 1840 - npost // eta - (nfc - cfc)
    nmax = min(avail // cells, 1023)
    if nmax < 1:
        raise ValueError("the T2 frame is too short for one FEC block")
    nfec = int(p["fec_blocks"] or 0)
    if nfec < 0 or nfec > nmax:
        raise ValueError(f"too many FEC blocks for the T2 frame (max {nmax})")
    nfec = nfec or nmax
    nti = int(p["ti_blocks"] or 0)
    if nti:
        if nti < 0 or nti > nfec or nti > 255:
            raise ValueError("ti_blocks must be between 1 and the number of FEC blocks")
        if -(-nfec // nti) * cells > M_TI:
            raise ValueError("time interleaver memory exceeded (EN 302 755 clause 6.5.1): use more TI blocks")
    else:
        nti = 1
        while -(-nfec // nti) * cells > M_TI:
            nti += 1
    frame_samples = 2048 + lf * S
    fs = Fraction(10 ** 6) / T
    return dict(fft=N, guard_samples=gis, n_p2=np2, data_symbols=ldata, fec_blocks=nfec, ti_blocks=nti,
                cells_per_fec=cells, kbch=kbch, frame_samples=frame_samples, sample_rate=fs,
                frame_seconds=Fraction(frame_samples) / fs, c_data=cdata, n_fc=nfc, c_fc=cfc, c_p2=cp2,
                carriers=cps, mapped_cells=mapped, dummy_cells=avail - nfec * cells,
                ts_rate=Fraction(nfec * (kbch - 80)) * fs / frame_samples,
                occupied_bandwidth=cps * fs / N, bits=bits, short=short, extended=ext, pilot=pp,
                guard=gi, code_rate=cr, l1_bits=eta)


def check(**params):
    """Raise ValueError with a clear message if the combination is invalid."""
    resolve(**params)


def rate(**params):
    """Exact useful TS bit rate in bit/s (float), equal to dvbt2_ts_rate()."""
    return float(resolve(**params)["ts_rate"])


def occupied_bandwidth(**params):
    """Bandwidth of the active carriers in Hz."""
    return float(resolve(**params)["occupied_bandwidth"])


def sample_rate(**params):
    """Elementary sample rate in Hz."""
    return float(resolve(**params)["sample_rate"])


if __name__ == "__main__":
    import sys
    kw = dict(a.split("=", 1) for a in sys.argv[1:])
    for k in ("extended", "rotation"):
        if k in kw:
            kw[k] = kw[k].lower() in ("1", "true", "yes", "on")
    r = resolve(**kw)
    for k, v in r.items():
        print(f"{k} = {float(v) if isinstance(v, Fraction) else v}")
