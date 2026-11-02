"""Power profiles of the whole machine, for a transmitter running on battery (dvbtx power):

save       nothing on air: every saving the hardware offers (at boot, and when the last dvbtx unit stops)
transmit   a dvbtx unit runs: what the real-time work needs (no autosuspended radio, the processor
           allowed to speed up), the other savings kept

Opt-in: dvbtx-power.service applies save at boot, the dvbtx units call "dvbtx power hook start|stop"
(ExecStartPre / ExecStopPost, as root), which does nothing while that service is not enabled. Settings:
[power] of the configuration file.

Measured on an i3-8100T (4 multiplexes on two radios, 75 % of the processor): 22.1 W with the deep
C-states blocked and the performance governor, 13.9 W with transmit (EPP balance_power) and the
engine's cpu_latency_us = 100, no underrun; idle 1.37 W -> 0.62 W with save (package power, RAPL).
"""

import glob
import os
import shutil
import subprocess
import time

RUN = "/run/dvbtx-power"
UNIT = "dvbtx-power.service"


def _w(value, *patterns):
    """value written to every file matching the patterns that exists and accepts it"""
    ok = False
    for pat in patterns:
        for f in glob.glob(pat):
            try:
                with open(f, "w") as fh:
                    fh.write(str(value))
                ok = True
            except OSError:
                pass
    return ok


def _r(path, default="-"):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def _cpu(epp, turbo):
    for p in glob.glob("/sys/devices/system/cpu/cpufreq/policy*"):
        if "powersave" in _r(p + "/scaling_available_governors", "").split():
            _w("powersave", p + "/scaling_governor")
        if epp in _r(p + "/energy_performance_available_preferences", "").split():
            _w(epp, p + "/energy_performance_preference")
    _w(0 if turbo else 1, "/sys/devices/system/cpu/intel_pstate/no_turbo")
    _w(1 if turbo else 0, "/sys/devices/system/cpu/cpufreq/boost")


def _aspm(policy):
    """PCIe link power; refused when the firmware keeps it (ACPI FADT)"""
    f = "/sys/module/pcie_aspm/parameters/policy"
    if os.path.exists(f) and not _w(policy, f) and policy == "powersupersave":
        _w("powersave", f)


def _usb(control):
    """auto: autosuspend, except keyboards and mice (HID: slow to wake up); on: never suspended"""
    for d in glob.glob("/sys/bus/usb/devices/*"):
        if not os.path.exists(d + "/power/control"):
            continue
        hid = any(_r(i, "") == "03" for i in glob.glob(d + "/*:*/bInterfaceClass"))
        _w("on" if control == "auto" and hid else control, d + "/power/control")


def _common():
    """the savings kept in both profiles"""
    _w("auto", "/sys/bus/pci/devices/*/power/control")
    _w("med_power_with_dipm", "/sys/class/scsi_host/host*/link_power_management_policy")
    _w(1, "/sys/module/snd_hda_intel/parameters/power_save", "/sys/module/snd_ac97_codec/parameters/power_save")
    _w(0, "/proc/sys/kernel/nmi_watchdog")
    _w(1500, "/proc/sys/vm/dirty_writeback_centisecs")
    iw = shutil.which("iw")
    if iw:
        for n in glob.glob("/sys/class/net/*/wireless"):
            subprocess.run([iw, "dev", os.path.basename(os.path.dirname(n)), "set", "power_save", "on"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def apply(profile, pw):
    """save | transmit, with the [power] settings pw"""
    _common()
    _cpu(pw[f"{profile}_epp"], bool(pw[f"{profile}_turbo"]))
    _aspm(pw[f"{profile}_aspm"])
    _usb("on" if profile == "transmit" else "auto")   # a radio closed (its multiplexes stopped) and opened
    cmd = pw.get(f"{profile}_command")                 # again: never suspended meanwhile
    if cmd:
        subprocess.run(cmd, shell=True)
    os.makedirs(RUN, exist_ok=True)
    with open(RUN + "/profile", "w") as f:
        f.write(profile)
    print(f"power profile: {profile}")


def on_air():
    try:
        return sorted(os.listdir(RUN + "/on"))
    except OSError:
        return []


def enabled():
    r = subprocess.run(["systemctl", "-q", "is-enabled", UNIT], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    return r.returncode == 0


def hook(action, unit, pw):
    """called by a dvbtx unit as it starts / stops: transmit while any is on air, save after the last"""
    if not enabled():
        return
    os.makedirs(RUN + "/on", exist_ok=True)
    mark = os.path.join(RUN, "on", os.path.basename(unit or "dvbtx"))
    if action == "start":
        open(mark, "w").close()
        apply("transmit", pw)
    else:
        try:
            os.unlink(mark)
        except FileNotFoundError:
            pass
        if not on_air():
            apply("save", pw)


def status():
    p = "/sys/devices/system/cpu/cpufreq/policy0"
    print(f"profile:   {_r(RUN + '/profile', '?')}   (dvbtx units on air: {' '.join(on_air()) or 'none'};"
          f" {UNIT} {'enabled' if enabled() else 'not enabled'})")
    print(f"governor:  {_r(p + '/scaling_governor')}   EPP: {_r(p + '/energy_performance_preference')}")
    nt = _r("/sys/devices/system/cpu/intel_pstate/no_turbo", "")
    turbo = ("off" if nt == "1" else "on") if nt else {"0": "off", "1": "on"}.get(
        _r("/sys/devices/system/cpu/cpufreq/boost", ""), "-")
    print(f"turbo:     {turbo}   ASPM: {_r('/sys/module/pcie_aspm/parameters/policy')}")
    usb = {}
    for f in glob.glob("/sys/bus/usb/devices/*/power/control"):
        v = _r(f)
        usb[v] = usb.get(v, 0) + 1
    print("usb:       " + (", ".join(f"{n} {v}" for v, n in sorted(usb.items())) or "-"))
    e = "/sys/class/powercap/intel-rapl:0/energy_uj"
    a = _r(e, "")
    if a.isdigit():
        time.sleep(2)
        b = _r(e, "")
        if b.isdigit() and int(b) >= int(a):
            print(f"package:   {(int(b) - int(a)) / 2e6:.2f} W   (processor, RAPL, over 2 s)")
