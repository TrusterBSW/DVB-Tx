"""Useful MPEG-TS bit rate of a channel, from its modulation parameters.

DVB-T  ETSI EN 300 744   DVB-C ITU-T J.83 annex A / EN 300 429
DVB-S  ETSI EN 300 421   DVB-S2 ETSI EN 302 307-1 (CCM, TS input, no ISSY/NPD), DVB-S2X EN 302 307-2
"""

from fractions import Fraction

DVBT_CONSTELLATIONS = {"qpsk": 2, "16qam": 4, "64qam": 6}
DVBT_CODE_RATES = ["1/2", "2/3", "3/4", "5/6", "7/8"]
DVBT_GUARDS = ["1/4", "1/8", "1/16", "1/32"]
DVBT_MODES = {"2k": 2048, "8k": 8192}
DVBT_BANDWIDTHS = [5_000_000, 6_000_000, 7_000_000, 8_000_000]

DVBC_CONSTELLATIONS = {"16qam": 4, "32qam": 5, "64qam": 6, "128qam": 7, "256qam": 8}

DVBS_CODE_RATES = ["1/2", "2/3", "3/4", "5/6", "7/8"]

DVBS2_MODULATIONS = {"qpsk": 2, "8psk": 3, "8apsk": 3, "16apsk": 4, "8+8apsk": 4, "32apsk": 5, "64apsk": 6,
                     "128apsk": 7, "256apsk": 8}
DVBS2_KBCH_NORMAL = {"1/4": 16008, "1/3": 21408, "2/5": 25728, "1/2": 32208, "3/5": 38688, "2/3": 43040,
                     "3/4": 48408, "4/5": 51648, "5/6": 53840, "8/9": 57472, "9/10": 58192}
DVBS2_KBCH_SHORT = {"1/4": 3072, "1/3": 5232, "2/5": 6312, "1/2": 7032, "3/5": 9552, "2/3": 10632,
                    "3/4": 11712, "4/5": 12432, "5/6": 13152, "8/9": 14232}
DVBS2_ALLOWED = {"qpsk": ["1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"],
                 "8psk": ["3/5", "2/3", "3/4", "5/6", "8/9", "9/10"],
                 "16apsk": ["2/3", "3/4", "4/5", "5/6", "8/9", "9/10"],
                 "32apsk": ["3/4", "4/5", "5/6", "8/9", "9/10"]}
# DVB-S2X (EN 302 307-2 tables 17a/17b, VL-SNR excepted): code rates by bits per symbol, (normal, short);
# the constellation variant follows from the rate (8APSK, 8+8APSK, 4+12+16APSK, ...)
DVBS2X_ALLOWED = {
    2: (["13/45", "9/20", "11/20"], ["11/45", "4/15", "14/45", "7/15", "8/15", "32/45"]),
    3: (["23/36", "25/36", "13/18", "100/180", "104/180"], ["7/15", "8/15", "26/45", "32/45"]),
    4: (["26/45", "3/5", "28/45", "23/36", "25/36", "13/18", "140/180", "154/180", "90/180", "96/180", "100/180",
         "18/30", "20/30"], ["26/45", "3/5", "7/15", "8/15", "32/45"]),
    5: (["2/3", "128/180", "132/180", "140/180"], ["2/3", "32/45"]),
    6: (["128/180", "132/180", "7/9", "4/5", "5/6"], []),
    7: (["135/180", "140/180"], []),
    8: (["116/180", "124/180", "128/180", "135/180", "20/30", "22/30"], []),
}
DVBS2X_KBCH_NORMAL = {"13/45": 18528, "9/20": 28968, "11/20": 35448, "26/45": 37248, "28/45": 40128,
                      "23/36": 41208, "25/36": 44808, "13/18": 46608, "7/9": 50208, "90/180": 32208,
                      "96/180": 34368, "100/180": 35808, "104/180": 37248, "116/180": 41568, "124/180": 44448,
                      "128/180": 45888, "132/180": 47328, "135/180": 48408, "140/180": 50208, "154/180": 55248,
                      "18/30": 38688, "20/30": 43008, "22/30": 47328}
DVBS2X_KBCH_SHORT = {"11/45": 3792, "4/15": 4152, "14/45": 4872, "7/15": 7392, "8/15": 8472, "26/45": 9192,
                     "32/45": 11352}
DVBS2_ROLLOFFS = (0.35, 0.25, 0.20, 0.15, 0.10, 0.05)   # the last three: DVB-S2X


class CapacityError(ValueError):
    pass


def _frac(s):
    try:
        return Fraction(str(s))
    except (ValueError, ZeroDivisionError) as e:
        raise CapacityError(f"bad fraction {s!r}") from e


def dvbt_rate(bandwidth=8_000_000, mode="8k", constellation="64qam", code_rate="7/8", guard="1/32"):
    """Useful bit rate (bit/s) of a non-hierarchical DVB-T channel."""
    if mode not in DVBT_MODES:
        raise CapacityError(f"DVB-T mode must be one of {', '.join(DVBT_MODES)}")
    if constellation not in DVBT_CONSTELLATIONS:
        raise CapacityError(f"DVB-T constellation must be one of {', '.join(DVBT_CONSTELLATIONS)}")
    if code_rate not in DVBT_CODE_RATES:
        raise CapacityError(f"DVB-T code rate must be one of {', '.join(DVBT_CODE_RATES)}")
    if guard not in DVBT_GUARDS:
        raise CapacityError(f"DVB-T guard interval must be one of {', '.join(DVBT_GUARDS)}")
    fft = DVBT_MODES[mode]
    ndata = 6048 if fft == 8192 else 1512
    fs = Fraction(int(bandwidth)) * 8 / 7
    tsym = (fft + fft * _frac(guard)) / fs
    bits = ndata * DVBT_CONSTELLATIONS[constellation] * _frac(code_rate)
    return float(bits * Fraction(188, 204) / tsym)


def dvbt_spacing(bandwidth=8_000_000, mode="8k"):
    return float(Fraction(int(bandwidth)) * 8 / 7 / DVBT_MODES[mode])


def dvbc_rate(symbol_rate=6_900_000, constellation="256qam"):
    if constellation not in DVBC_CONSTELLATIONS:
        raise CapacityError(f"DVB-C constellation must be one of {', '.join(DVBC_CONSTELLATIONS)}")
    return symbol_rate * DVBC_CONSTELLATIONS[constellation] * 188 / 204


def dvbc_bandwidth(symbol_rate, rolloff=0.15):
    return symbol_rate * (1 + rolloff)


def dvbs_rate(symbol_rate=27_500_000, code_rate="3/4"):
    if code_rate not in DVBS_CODE_RATES:
        raise CapacityError(f"DVB-S code rate must be one of {', '.join(DVBS_CODE_RATES)}")
    return float(symbol_rate * 2 * _frac(code_rate) * Fraction(188, 204))


def dvbs2_rate(symbol_rate=27_500_000, modulation="8psk", code_rate="3/4", pilots=True, frame="normal"):
    if modulation not in DVBS2_MODULATIONS:
        raise CapacityError(f"DVB-S2 modulation must be one of {', '.join(DVBS2_MODULATIONS)}")
    bits = DVBS2_MODULATIONS[modulation]
    short = frame != "normal"
    s2 = code_rate in DVBS2_ALLOWED.get(modulation, [])
    s2x = code_rate in DVBS2X_ALLOWED[bits][1 if short else 0]
    if not s2 and not s2x:
        raise CapacityError(f"DVB-S2 {modulation} code rates: {', '.join(DVBS2_ALLOWED.get(modulation, []))}; "
                            f"DVB-S2X, {frame} frames: {', '.join(DVBS2X_ALLOWED[bits][1 if short else 0]) or 'none'}")
    table = dict(DVBS2_KBCH_SHORT if short else DVBS2_KBCH_NORMAL)
    table.update(DVBS2X_KBCH_SHORT if short else DVBS2X_KBCH_NORMAL)
    if code_rate not in table:
        raise CapacityError(f"code rate {code_rate} does not exist with short frames")
    nldpc = 64800 if frame == "normal" else 16200
    slots = -(-(-(-nldpc // bits)) // 90)     # 128APSK: 9258 symbols in 103 slots
    pilot_blocks = (slots - 1) // 16 if pilots else 0
    symbols = 90 + slots * 90 + pilot_blocks * 36
    return (table[code_rate] - 80) / symbols * symbol_rate


def rate(standard, **p):
    """Useful TS rate of a carrier described by a dict of its parameters."""
    s = standard.lower()
    if s == "dvbt":
        return dvbt_rate(p.get("bandwidth", 8_000_000), p.get("mode", "8k"), p.get("constellation", "64qam"),
                         p.get("code_rate", "7/8"), p.get("guard", "1/32"))
    if s == "dvbc":
        return dvbc_rate(p.get("symbol_rate", 6_900_000), p.get("constellation", "256qam"))
    if s == "dvbs":
        return dvbs_rate(p.get("symbol_rate", 27_500_000), p.get("code_rate", "3/4"))
    if s == "dvbs2":
        return dvbs2_rate(p.get("symbol_rate", 27_500_000), p.get("modulation", "8psk"), p.get("code_rate", "3/4"),
                          p.get("pilots", True), p.get("frame", "normal"))
    if s == "dvbt2":
        from . import t2
        return t2.rate(**p)
    if s == "ts":
        return float(p["ts_rate"])
    raise CapacityError(f"unknown standard {standard!r}")


def dvbt_table(bandwidth=8_000_000, mode="8k"):
    """All non-hierarchical DVB-T rates for one bandwidth: rows (constellation, code rate) x guard."""
    rows = []
    for c in DVBT_CONSTELLATIONS:
        for cr in DVBT_CODE_RATES:
            rows.append((c, cr, [dvbt_rate(bandwidth, mode, c, cr, g) for g in DVBT_GUARDS]))
    return rows
