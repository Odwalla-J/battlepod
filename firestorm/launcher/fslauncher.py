#!/usr/bin/env python3
"""Firestorm launcher: set up a PC as a pod or the console, and start it.

One window for what was otherwise shim.ini, options.ini and ctcl.ini by hand:
which monitor shows what, the controls and their bindings, frame pacing,
the console's list of pods, and printing. It edits those files in place
(keeping their comments and any key it does not know) and starts the
programs a role needs, the way tools/start-centre.ps1 does.

The window is a small web page served on 127.0.0.1 and shown in an Edge
app window (Edge is part of Windows 10 and 11); nothing outside the
standard library is needed. The server stops when the window closes.

usage: fslauncher.py [--game DIR] [--port N] [--no-window]

--game is the Firestorm folder (MW4.exe, fsrun.exe and the shim's
ddraw.dll); by default the folder this file is in if MW4.exe is there, else
../firestorm-run/MW4 beside the repository.
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from ctypes import wintypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)        # its own modules (mfdtest.py), in a packaged copy too
import career  # noqa: E402
import bt3  # noqa: E402
START = int(time.time())
# What the page may borrow from the game folder: its fonts and pictures.
GAME_FILES = re.compile(r"^(hsh/(Mechs/[^/\\]+\.bmp|fonts/[^/\\]+\.ttf|logo\.bmp|mr_background\.bmp)"
                        r"|profiles/livery/(camo|unit)_\d\d\.png)$", re.I)
# The console's camo and unit lists (its slot's two numbered lists). Their
# pictures are read off the console by tools/livery.py into profiles\livery.
CAMOS, UNITS = 16, 17
WEB = os.path.join(HERE, "web")
MODES = [(512, 384), (640, 480), (800, 600), (1024, 768), (1280, 1024), (1600, 1200)]
# The game knows only these (MW4.exe 0x426FC2); for any other size the shim
# makes the mission that size and widens the cockpit's view to fit it
# (docs/WIDESCREEN.md). These are offered too, with each monitor's own size.
WIDE = [(1280, 720), (1366, 768), (1600, 900), (1920, 1080), (1920, 1200), (2560, 1080), (2560, 1440)]


def mode_list(monitors, current):
    """[value, label] for the Resolution list: the game's sizes, then wide ones."""
    sizes = list(MODES) + [m for m in WIDE if m not in MODES]
    for m in monitors:
        if (m["w"], m["h"]) not in sizes and 640 <= m["w"] <= 4096 and 480 <= m["h"] <= 4096:
            sizes.append((m["w"], m["h"]))
    try:
        cw, ch = (int(v) for v in current.split("x"))
        if (cw, ch) not in sizes:
            sizes.append((cw, ch))
    except ValueError:
        pass
    native = {(m["w"], m["h"]) for m in monitors}
    out = []
    for w, h in sorted(sizes, key=lambda s: ((s[0], s[1]) not in MODES, s[0], s[1])):
        shape = next((n for n, r in (("4:3", 4 / 3), ("5:4", 1.25), ("16:9", 16 / 9), ("16:10", 1.6),
                                     ("21:9", 2560 / 1080)) if abs(w / h - r) < 0.02), "%.2f:1" % (w / h))
        out.append(["%dx%d" % (w, h), "%d x %d  %s%s" % (w, h, shape, ", monitor" if (w, h) in native else "")])
    return out

# The pod's analog controls a USB controller's axis can be ([joy] in shim.ini),
# with the shim's defaults: the cockpit's own controllers (firmware/rp2040-controls).
JOY_AXES = {"throttle": "Firestorm Throttle.X", "pedal_left": "Firestorm Pedals.X", "pedal_right": "Firestorm Pedals.Y",
            "stick_x": "Firestorm Stick.X", "stick_y": "Firestorm Stick.Y"}
JOY_BINDS = {"fire1": "Firestorm Stick.B1", "fire2": "Firestorm Stick.B2", "fire3": "Firestorm Stick.B3",
             "button4": "Firestorm Stick.B4", "hat_up": "Firestorm Stick.HatUp", "hat_down": "Firestorm Stick.HatDown",
             "hat_left": "Firestorm Stick.HatLeft", "hat_right": "Firestorm Stick.HatRight",
             "throttle_button": "Firestorm Throttle.B1", "panic": "Firestorm Throttle.B2"}


def joy_get(shim, key, default):
    v = shim.get("joy", key, default)
    return "" if v.strip().lower() == "none" else v


def joy_set(shim, key, value, default):
    """None when it is the default (the key goes), "none" to turn a default off."""
    value = (value or "").strip()
    shim.set("joy", key, None if value == default else value or "none")

# Cockpit actions the shim binds ([keys] / [pad] / [joy]), with its defaults.
ACTIONS = [
    ("throttle_up", "Throttle up", "R", "none"),
    ("throttle_down", "Throttle down", "F", "none"),
    ("throttle_full", "Throttle full", "none", "DUp"),
    ("throttle_zero", "Throttle stop", "none", "DDown"),
    ("pedal_left", "Turn left (pedal)", "A", "none"),
    ("pedal_right", "Turn right (pedal)", "D", "none"),
    ("twist_left", "Torso twist left", "J", "none"),
    ("twist_right", "Torso twist right", "L", "none"),
    ("aim_up", "Aim up", "I", "none"),
    ("aim_down", "Aim down", "K", "none"),
    ("fire1", "Fire group 1", "Space 1", "RT"),
    ("fire2", "Fire group 2", "2", "LT"),
    ("fire3", "Fire group 3", "3", "RB"),
    ("fire_all", "Fire all groups", "4 Backslash", "A"),
    ("throttle_button", "Throttle button", "F11", "LB"),
    ("panic", "Panic button", "F12", "none"),
    ("mfd_prev", "Select previous MFD", "F9", "DLeft"),
    ("mfd_next", "Select next MFD", "F10", "DRight"),
    ("mouse", "Free / capture mouse", "ScrollLock", "none"),
    ("hat_up", "Hat forward (look down)", "Up", "none"),
    ("hat_down", "Hat back (look behind)", "Down", "none"),
    ("hat_left", "Hat left (look left)", "Left", "none"),
    ("hat_right", "Hat right (look right)", "Right", "none"),
    ("button4", "Stick button 4 (unused by the game)", "none", "none"),
]
PAD_BUTTONS = ["none", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start",
               "LS", "RS", "DUp", "DDown", "DLeft", "DRight"]
# The cockpit's windows: shim.ini section, title, the game's own size.
WINDOWS = [("mfd1", "MFD 1", 320, 240), ("mfd2", "MFD 2", 320, 240), ("mfd3", "MFD 3", 320, 240),
           ("mfd4", "MFD 4", 320, 240), ("mfd5", "MFD 5", 320, 240), ("radar", "Radar", 640, 480),
           ("plasma", "Plasma sign", 672, 192), ("mfd_wall", "MFD wall", 1600, 240)]


# ------------------------------------------------------------ ini files
class Ini:
    """An ini file edited in place: lines kept, keys updated or added."""

    def __init__(self, path):
        self.path = path
        self.lines = []
        if os.path.exists(path):
            with open(path, encoding="latin-1") as f:
                self.lines = f.read().splitlines()

    def _find(self, sec):
        """(header index, end index) of the first [sec], or None."""
        start = None
        for i, l in enumerate(self.lines):
            m = re.match(r"\s*\[(.+?)\]", l)
            if m:
                if start is not None:
                    return start, i
                if m.group(1).strip().lower() == sec.lower():
                    start = i
        return (start, len(self.lines)) if start is not None else None

    def get(self, sec, key, default=None):
        r = self._find(sec)
        if not r:
            return default
        for l in self.lines[r[0] + 1:r[1]]:
            m = re.match(r"\s*([^;#=\s][^=]*?)\s*=\s*(.*)$", l)
            if m and m.group(1).lower() == key.lower():
                return m.group(2).strip()
        return default

    def items(self, sec):
        r = self._find(sec)
        out = []
        if r:
            for l in self.lines[r[0] + 1:r[1]]:
                m = re.match(r"\s*([^;#=\s][^=]*?)\s*=\s*(.*)$", l)
                if m:
                    out.append((m.group(1), m.group(2).strip()))
        return out

    def set(self, sec, key, value):
        """Set sec.key; value None removes it."""
        r = self._find(sec)
        if not r:
            if value is None:
                return
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines += ["[%s]" % sec, "%s=%s" % (key, value)]
            return
        for i in range(r[0] + 1, r[1]):
            m = re.match(r"\s*([^;#=\s][^=]*?)\s*=", self.lines[i])
            if m and m.group(1).lower() == key.lower():
                if value is None:
                    del self.lines[i]
                else:
                    self.lines[i] = "%s=%s" % (key, value)
                return
        if value is None:
            return
        end = r[1]
        while end > r[0] + 1 and not self.lines[end - 1].strip():
            end -= 1
        self.lines.insert(end, "%s=%s" % (key, value))

    def replace_section(self, sec, pairs):
        r = self._find(sec)
        body = ["%s=%s" % kv for kv in pairs]
        if r:
            self.lines[r[0] + 1:r[1]] = body + ([""] if r[1] < len(self.lines) else [])
        else:
            self.lines += ["[%s]" % sec] + body

    def save(self):
        if os.path.exists(self.path):
            shutil.copyfile(self.path, self.path + ".bak")
        with open(self.path, "w", encoding="latin-1", newline="\r\n") as f:
            f.write("\n".join(self.lines).rstrip("\n") + "\n")


# ------------------------------------------------------------ the PC
def monitors():
    """Monitors as the shim numbers them: 1 the primary, then the rest."""
    user32 = ctypes.windll.user32

    class MONITORINFOEX(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                    ("dwFlags", wintypes.DWORD), ("szDevice", ctypes.c_wchar * 32)]

    found = []
    PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                              ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

    def cb(hm, dc, r, lp):
        mi = MONITORINFOEX()
        mi.cbSize = ctypes.sizeof(mi)
        user32.GetMonitorInfoW(hm, ctypes.byref(mi))
        m, w = mi.rcMonitor, mi.rcWork
        found.append({"primary": bool(mi.dwFlags & 1), "device": mi.szDevice,
                      "x": m.left, "y": m.top, "w": m.right - m.left, "h": m.bottom - m.top,
                      "work": [w.left, w.top, w.right - w.left, w.bottom - w.top]})
        return True

    user32.SetProcessDPIAware()
    user32.EnumDisplayMonitors(None, None, PROC(cb), 0)
    found.sort(key=lambda m: not m["primary"])          # stable: the rest keep Windows' order
    for i, m in enumerate(found):
        m["n"] = i + 1
    return found


def pads():
    """Which XInput pads are connected."""
    for dll in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
        try:
            x = getattr(ctypes.windll, dll)
            break
        except OSError:
            x = None
    if not x:
        return []
    state = (ctypes.c_byte * 16)()
    return [i for i in range(4) if x.XInputGetState(i, ctypes.byref(state)) == 0]


class JOYCAPSA(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("szPname", ctypes.c_char * 32)] + \
               [(n, wintypes.UINT) for n in ("wXmin", "wXmax", "wYmin", "wYmax", "wZmin", "wZmax", "wNumButtons",
                                             "wPeriodMin", "wPeriodMax", "wRmin", "wRmax", "wUmin", "wUmax",
                                             "wVmin", "wVmax", "wCaps", "wMaxAxes", "wNumAxes", "wMaxButtons")] + \
               [("szRegKey", ctypes.c_char * 32), ("szOEMVxD", ctypes.c_char * 260)]


class JOYINFOEX(ctypes.Structure):
    _fields_ = [(n, wintypes.DWORD) for n in ("dwSize", "dwFlags", "dwXpos", "dwYpos", "dwZpos", "dwRpos",
                                              "dwUpos", "dwVpos", "dwButtons", "dwButtonNumber", "dwPOV",
                                              "dwReserved1", "dwReserved2")]


def joy_name(vid, pid, n):
    """The name Windows shows for a controller (its OEMName), as the shim reads it."""
    import winreg
    key = r"System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_%04X&PID_%04X" % (vid, pid)
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            with winreg.OpenKey(root, key) as k:
                return winreg.QueryValueEx(k, "OEMName")[0]
        except OSError:
            pass
    return "controller %d" % n


def joysticks():
    """USB controllers as the shim's [joy] sees them (winmm): number, USB ids,
    name, and each axis from 0 to 1000, the buttons held, the hat."""
    try:
        w = ctypes.windll.winmm
    except OSError:
        return []
    out = []
    for i in range(min(w.joyGetNumDevs(), 16)):
        c, st = JOYCAPSA(), JOYINFOEX()
        st.dwSize, st.dwFlags = ctypes.sizeof(st), 0xFF          # JOY_RETURNALL
        if w.joyGetDevCapsA(i, ctypes.byref(c), ctypes.sizeof(c)) or w.joyGetPosEx(i, ctypes.byref(st)):
            continue
        axes = {}
        for n, lo, hi, v in (("X", c.wXmin, c.wXmax, st.dwXpos), ("Y", c.wYmin, c.wYmax, st.dwYpos),
                             ("Z", c.wZmin, c.wZmax, st.dwZpos), ("R", c.wRmin, c.wRmax, st.dwRpos),
                             ("U", c.wUmin, c.wUmax, st.dwUpos), ("V", c.wVmin, c.wVmax, st.dwVpos)):
            if len(axes) < c.wNumAxes and hi > lo:
                axes[n] = max(0, min(1000, (v - lo) * 1000 // (hi - lo)))
        out.append({"n": i, "ids": "%04X:%04X" % (c.wMid, c.wPid), "name": joy_name(c.wMid, c.wPid, i),
                    "axes": axes, "buttons": [b + 1 for b in range(min(c.wNumButtons, 32)) if st.dwButtons >> b & 1],
                    "nbuttons": c.wNumButtons, "hat": st.dwPOV if (c.wCaps & 0x10) and st.dwPOV < 36000 else None,
                    "has_hat": bool(c.wCaps & 0x10)})
    return out


def local_addresses():
    try:
        return sorted({a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)})
    except OSError:
        return []


def processes():
    """The centre's programs that are running, with their command lines."""
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='MW4.exe' or Name='Launcher.exe' or "
          "Name='mw4print.exe' or ((Name='python.exe' or Name='pythonw.exe') and CommandLine like '%bridge.py%')\" | Select-Object ProcessId,Name,CommandLine | "
          "ConvertTo-Json -Compress")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                             timeout=15, creationflags=0x08000000).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return []
    if not out:
        return []
    data = json.loads(out)
    data = data if isinstance(data, list) else [data]
    res = []
    for p in data:
        cl = p.get("CommandLine") or ""
        role = {"Launcher.exe": "pod launcher", "mw4print.exe": "report printer", "fsrun.exe": "starter"}.get(p["Name"])
        if p["Name"].lower().startswith("python"):
            role = "MFD bridge"
        if p["Name"] == "MW4.exe":
            m = re.search(r"-ctcltype (\d)", cl)
            role = {"1": "console", "2": "pod", "3": "camera"}.get(m.group(1) if m else "", "game")
        res.append({"pid": p["ProcessId"], "name": p["Name"], "role": role})
    return res


# ------------------------------------------------------------ the launcher
class Launcher:
    def __init__(self, game):
        self.game = game
        self.lock = threading.Lock()
        self.last_ping = None
        self.message = ""
        self.pilots = Pilots(game)
        self.careers = career.Careers(game)
        self.bt3 = bt3.BT3(os.path.dirname(os.path.dirname(game)))
        self.mfd_boards = []
        self.mfd_tester = None

    # ---- pilots: kept on the console; a pod asks the console's launcher
    def role(self):
        return self.ini("launcher.ini").get("launcher", "role", "pod")

    def console_addr(self):
        a = self.ini("launcher.ini").get("launcher", "console", "")
        if a and ":" not in a:
            a += ":%d" % POD_PORT
        return a

    def remote(self, path, body=None):
        import urllib.request
        addr = self.console_addr()
        if not addr:
            raise ValueError("this pod does not know the console's address yet (This PC page)")
        req = urllib.request.Request("http://%s%s" % (addr, path), method="POST" if body is not None else "GET",
                                     data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=4) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            raise ValueError(json.loads(e.read() or b"{}").get("error", str(e)))
        except OSError as e:
            raise ValueError("cannot reach the console's launcher at %s (%s)" % (addr, getattr(e, "reason", e)))

    def pilots_view(self):
        if self.role() == "pod":
            v = self.remote("/pod/roster")
            v.update(local=False, console=self.console_addr())
            return v
        teslas = [t for t in self.state_teslas()]
        roster = self.pilots.roster()
        return {"local": True, "roster": roster, "seats": self.pilots.seats(),
                "mechs": self.pilots.mechs(), "pods": teslas, "you": None, "careers": self.career_of(roster)}

    # ---- careers (career.py): each match's record, from the pod that flew it,
    # kept on the console and summed into ranks and leaderboards.
    def career_of(self, roster):
        """Each profile's career, by its id."""
        return {c["profile"]: c for c in self.careers.summary(roster) if c["profile"]}

    def career_view(self, since=None, game=None):
        if self.role() == "pod":
            q = "&".join(k + "=" + v for k, v in (("since", since), ("game", game)) if v)
            return self.remote("/pod/career" + ("?" + q if q else ""))
        roster = self.pilots.roster()
        return {"pilots": self.careers.summary(roster, since, game), "recent": self.careers.recent(game=game),
                "ranks": career.rank_table(), "since": since or "", "game": game or ""}

    def bt3_report(self):
        """A finished 3.0 battle into the careers, once (career.add ignores it again)."""
        try:
            lines = self.bt3.career_entries() if self.bt3.available else []
        except (OSError, ValueError):
            lines = []
        if not lines or getattr(self, "_bt3_reported", None) == lines[0]["match"]:
            return
        try:
            for e in lines:
                if self.role() == "pod":
                    self.remote("/pod/career", e)
                else:
                    self.careers.add(e, self.my_pod(), self.pilots.roster())
            self._bt3_reported = lines[0]["match"]
        except ValueError:
            pass

    def pod_records(self):
        """Where this pod's MW4 writes its match records (c:\\mw4files, as the shim has it)."""
        return self.path("mw4files", self.ini("shim.ini").get("shim", "pod_files", "ctcl2") or "ctcl2")

    def career_sync(self):
        """Report this pod's finished matches to the console, each once, and
        bring back everyone's rank for the plasma sign. Records left from
        before - or from while this launcher was closed - go too."""
        self.bt3_report()
        if self.role() not in ("pod", "both"):
            return
        sent_path = self.path("profiles", "career-sent.txt")
        try:
            with open(sent_path, encoding="utf-8") as f:
                sent = set(l.strip() for l in f if l.strip())
        except OSError:
            sent = set()
        folder = self.pod_records()
        try:
            names = sorted(n for n in os.listdir(folder) if n.lower().endswith(".pr"))
        except OSError:
            names = []
        done = []
        for n in names:
            mid = os.path.splitext(n)[0]
            f = os.path.join(folder, n)
            if mid in sent or time.time() - os.path.getmtime(f) < 15:      # still being written
                continue
            try:
                rec = career.read_record(f)
            except OSError:
                continue
            e = career.entry_of(rec) if rec else None
            if e:
                try:
                    if self.role() == "pod":
                        self.remote("/pod/career", e)
                    else:
                        self.careers.add(e, self.my_pod(), self.pilots.roster())
                except ValueError:
                    break                       # the console is away: the rest next time
            done.append(mid)
        if done:
            os.makedirs(os.path.dirname(sent_path), exist_ok=True)
            with open(sent_path, "a", encoding="utf-8") as f:
                f.writelines(m + "\n" for m in done)
        try:
            text = self.remote("/pod/ranks")["ini"] if self.role() == "pod" else \
                self.careers.ranks_ini(self.pilots.roster())
            career.write_if_changed(self.path("profiles", "ranks.ini"), text)
        except (ValueError, KeyError, OSError):
            pass

    def career_loop(self):
        while True:
            try:
                self.career_sync()
            except Exception as e:              # never the launcher's end
                print("careers: %s" % e, flush=True)
            time.sleep(15)

    def my_pod(self):
        mine = [t["ip"] for t in self.state_teslas() if t["ip"] in local_addresses()]
        return mine[0] if mine else "127.0.0.1"

    def state_teslas(self):
        ctcl = self.ini("ctcl.ini")
        r = ctcl._find("teslas")
        out = []
        for l in (ctcl.lines[r[0] + 1:r[1]] if r else []):
            m = re.match(r"\s*([#*]?)\s*([0-9A-Za-z_.-]+)\s*=\s*([^,]*)", l)
            if m and m.group(1) != "#":
                out.append({"ip": m.group(2), "slot": m.group(3).strip()})
        return out

    def pilot_save(self, body):
        if self.role() == "pod":
            return self.remote("/pod/profile", body)
        return {"profile": self.pilots.save_profile(body)}

    def pilot_delete(self, body):
        if self.role() == "pod":
            raise ValueError("pilots are removed at the console")
        self.pilots.delete_profile(body["id"])
        return {}

    def pilot_seat(self, body):
        if self.role() == "pod":                       # the console seats whoever asks, by their address
            return self.remote("/pod/seat", {"id": body.get("id")})
        return {"profile": self.pilots.seat(body["pod"], body.get("id"))}

    def path(self, *p):
        return os.path.join(self.game, *p)

    def ini(self, name):
        return Ini(self.path(name))

    def relocate(self):
        """Follow the game folder when it moves, and this PC when its address does.

        ctcl.ini names fsrun.exe by full path in [Games] 1=, and on a PC that
        is both console and pod, [teslas] names the pod by this PC's address;
        a copied folder or a new DHCP lease leaves either pointing nowhere."""
        ctcl, fixed = self.ini("ctcl.ini"), []
        cmd = ctcl.get("Games", "1", "")
        m = re.match(r'\s*"?(.*?fsrun\.exe)"?(.*)$', cmd, re.I)
        if m and os.path.normcase(os.path.abspath(m.group(1))) != os.path.normcase(self.path("fsrun.exe")):
            ctcl.set("Games", "1", self.path("fsrun.exe") + m.group(2))
            fixed.append("the pod's path")
        mine = [a for a in local_addresses() if not a.startswith("127.")]
        r = ctcl._find("teslas")
        if self.role() == "both" and mine and r:
            pods = [i for i in range(r[0] + 1, r[1]) if re.match(r"\s*[0-9.]+\s*=", ctcl.lines[i])]
            ips = [re.match(r"\s*([0-9.]+)", ctcl.lines[i]).group(1) for i in pods]
            if len(pods) == 1 and ips[0] not in mine and not ips[0].startswith("127."):
                old = ips[0]
                new = next((a for a in mine if a.rsplit(".", 1)[0] == old.rsplit(".", 1)[0]), mine[0])
                ctcl.lines[pods[0]] = ctcl.lines[pods[0]].replace(old, new)
                fixed.append("this PC's address (%s, was %s)" % (new, old))
        if fixed:
            ctcl.save()
            self.message = "ctcl.ini follows %s." % " and ".join(fixed)

    # ---- reading
    def state(self):
        shim, opts, ctcl, me = self.ini("shim.ini"), self.ini("options.ini"), self.ini("ctcl.ini"), self.ini("launcher.ini")
        role = me.get("launcher", "role", "pod")
        pod_sec = "main.ctcl2"

        def main(key, default):
            return shim.get(pod_sec, key, shim.get("main", key, default))

        wins = {}
        for sec, title, w, h in WINDOWS:
            wins[sec] = {k: shim.get(sec, k) for k in ("x", "y", "w", "h", "borderless")}
        binds = []
        for name, label, k, p in ACTIONS:
            binds.append({"name": name, "label": label, "keys": shim.get("keys", name, k),
                          "pad": shim.get("pad", name, p), "default_keys": k, "default_pad": p,
                          "joy": joy_get(shim, name, JOY_BINDS.get(name, ""))})
        teslas = []
        r = ctcl._find("teslas")
        for l in (ctcl.lines[r[0] + 1:r[1]] if r else []):
            # "#" turns a pod off; "*" marks a special station (cameraship, live camera).
            m = re.match(r"\s*([#*]?)\s*([0-9A-Za-z_.-]+)\s*=\s*([^,]*)", l)
            if m:
                teslas.append({"ip": m.group(2), "name": m.group(3).strip(),
                               "enabled": m.group(1) != "#", "mark": m.group(1)})
        games = ctcl.get("Games", "1", "")
        mons = monitors()
        return {
            "game": self.game,
            "found": os.path.exists(self.path("MW4.exe")),
            "shim_installed": os.path.exists(self.path("ddraw.dll")) and os.path.exists(self.path("fsrun.exe")),
            "role": role,
            "monitors": mons,
            "pads": pads(),
            "addresses": local_addresses(),
            "modes": mode_list(mons, "%sx%s" % (opts.get("graphics options", "screenwidth", "800"),
                                                     opts.get("graphics options", "screenheight", "600"))),
            "pad_buttons": PAD_BUTTONS,
            "joysticks": joysticks(),
            "windows_meta": [{"sec": s, "title": t, "w": w, "h": h} for s, t, w, h in WINDOWS],
            "settings": {
                "mode": "%sx%s" % (opts.get("graphics options", "screenwidth", "800"),
                                   opts.get("graphics options", "screenheight", "600")),
                "bitdepth": opts.get("graphics options", "bitdepth", "16"),
                "main_fullscreen": main("fullscreen", "0") == "1",
                "main_monitor": int(main("monitor", "1") or 1),
                "main_x": main("x", "0"), "main_y": main("y", "0"),
                "main_w": main("w", ""), "main_h": main("h", ""),
                "keep_aspect": shim.get("main", "keep_aspect", "1") == "1",
                "mfd_layout": shim.get("mfd", "layout", "windows"),
                "mfd_tint": shim.get("mfd", "tint", "FFFFFF"),
                "show_surface": shim.get("mfd", "show_surface", "1") == "1",
                "wall_cols": shim.get("mfd_wall", "cols", "5"),
                "radar_rotate": shim.get("radar", "rotate", "0"),
                "plasma": shim.get("plasma", "enabled", "1") == "1",
                "windows": wins,
                "pad_enabled": shim.get("pad", "enabled", "1") == "1",
                "pad_index": shim.get("pad", "index", "0"),
                "pad_deadzone": shim.get("pad", "deadzone", "7000"),
                "pad_invert_y": shim.get("pad", "invert_y", "0") == "1",
                "ramp_ms": shim.get("keys", "ramp_ms", "600"),
                "capture_mouse": shim.get("shim", "capture_mouse", ""),
                "binds": binds,
                "joy_enabled": shim.get("joy", "enabled", "1") == "1",
                "joy_deadzone": shim.get("joy", "deadzone", "4"),
                "joy_axes": {a: joy_get(shim, a, d) for a, d in JOY_AXES.items()},
                "vsync": shim.get("shim", "vsync", "1"),
                "max_fps": shim.get("shim", "max_fps", ""),
                "gpu_present": shim.get("main", "gpu_present", "1") == "1",
                "frame_stats": shim.get("shim", "frame_stats", "0") == "1",
                "print_to_file": shim.get("print", "to_file", ""),
                "print_folder": shim.get("print", "folder", "reports"),
                "start_printer": me.get("launcher", "start_printer", "1") == "1",
                "console_addr": me.get("launcher", "console", ""),
                "teslas": teslas,
                "pod_command": games,
            },
            "processes": processes(),
            "mechs": self.pilots.mechs(),
            "message": self.message,
        }

    # ---- writing
    def save(self, s):
        shim, opts, ctcl, me = self.ini("shim.ini"), self.ini("options.ini"), self.ini("ctcl.ini"), self.ini("launcher.ini")
        if not shim.lines:
            shim.lines = ["; firestorm-shim settings for this machine, kept by the launcher (launcher/fslauncher.py).",
                          "; Sections can be specialised by role: [main.ctcl2] for a pod, [main.ctcl1] the console.", ""]
        me.set("launcher", "role", s["role"])
        me.set("launcher", "start_printer", "1" if s.get("start_printer", True) else "0")
        me.set("launcher", "console", (s.get("console_addr") or "").strip() or None)

        w, h = s["mode"].split("x")
        opts.set("graphics options", "screenwidth", w)
        opts.set("graphics options", "screenheight", h)
        opts.set("graphics options", "bitdepth", s.get("bitdepth", "16"))

        pod = "main.ctcl2"
        if s["main_fullscreen"]:
            shim.set(pod, "fullscreen", "1")
            shim.set(pod, "monitor", str(s["main_monitor"]))
        else:
            shim.set(pod, "fullscreen", None)
            shim.set(pod, "monitor", None)
        for k in ("x", "y", "w", "h"):
            v = str(s.get("main_" + k, "")).strip()
            shim.set(pod, k, v if v else None)
        shim.set("main", "keep_aspect", None if s["keep_aspect"] else "0")
        shim.set("main", "gpu_present", None if s["gpu_present"] else "0")

        shim.set("mfd", "layout", "wall" if s["mfd_layout"] == "wall" else None)
        shim.set("mfd", "tint", s["mfd_tint"] if s["mfd_tint"].upper() != "FFFFFF" else None)
        shim.set("mfd", "show_surface", None if s["show_surface"] else "0")
        shim.set("mfd_wall", "cols", str(s["wall_cols"]) if s["mfd_layout"] == "wall" else None)
        shim.set("radar", "rotate", s["radar_rotate"] if s["radar_rotate"] in ("90", "270") else None)
        shim.set("plasma", "enabled", None if s["plasma"] else "0")
        for sec, win in s["windows"].items():
            for k in ("x", "y", "w", "h"):
                v = str(win.get(k) or "").strip()
                shim.set(sec, k, v if v else None)
            shim.set(sec, "borderless", "1" if win.get("borderless") in (True, "1", 1) else None)

        shim.set("pad", "enabled", None if s["pad_enabled"] else "0")
        shim.set("pad", "index", str(s["pad_index"]) if str(s["pad_index"]) != "0" else None)
        shim.set("pad", "deadzone", str(s["pad_deadzone"]) if str(s["pad_deadzone"]) != "7000" else None)
        shim.set("pad", "invert_y", "1" if s["pad_invert_y"] else None)
        shim.set("keys", "ramp_ms", str(s["ramp_ms"]) if str(s["ramp_ms"]) != "600" else None)
        shim.set("shim", "capture_mouse", s["capture_mouse"] if s["capture_mouse"] in ("0", "1") else None)
        for b in s["binds"]:
            shim.set("keys", b["name"], b["keys"] if b["keys"] != b["default_keys"] else None)
            shim.set("pad", b["name"], b["pad"] if b["pad"] != b["default_pad"] else None)
            joy_set(shim, b["name"], b.get("joy"), JOY_BINDS.get(b["name"], ""))
        shim.set("joy", "enabled", None if s.get("joy_enabled", True) else "0")
        shim.set("joy", "deadzone", str(s.get("joy_deadzone", "4")) if str(s.get("joy_deadzone", "4")) != "4" else None)
        for a, d in JOY_AXES.items():
            joy_set(shim, a, s.get("joy_axes", {}).get(a), d)

        shim.set("shim", "vsync", s["vsync"] if s["vsync"] != "1" else None)
        shim.set("shim", "max_fps", str(s["max_fps"]).strip() or None)
        shim.set("shim", "frame_stats", "1" if s["frame_stats"] else None)
        shim.set("print", "to_file", s["print_to_file"] if s["print_to_file"] in ("0", "1") else None)
        shim.set("print", "folder", s["print_folder"] if s["print_folder"] not in ("", "reports") else None)

        if s.get("teslas") is not None:
            ctcl.replace_section("teslas", [(("#" if not t["enabled"] else "*" if t.get("mark") == "*" else "") + t["ip"].strip(),
                                             "%s, %s" % (t["name"].strip() or "Pilot", t["ip"].strip()))
                                            for t in s["teslas"] if t["ip"].strip()])
        # The launcher starts the pod with ctcl.ini's [Games] 1=: through fsrun, from here.
        if s["role"] in ("pod", "both"):
            cmd = ctcl.get("Games", "1", "")
            want = '%s -mechview 1 -tmfds 1 -3dt -ctcltype 2 /gosnoblade' % self.path("fsrun.exe")
            if "fsrun.exe" not in cmd.lower():
                ctcl.set("Games", "1", want)
        for f in (shim, opts, ctcl, me):
            f.save()
        self.message = "Saved."
        return True

    # ---- running
    def start(self, what, args=None):
        args = args or {}
        self.bridge_note = ""
        game, fsrun = self.game, self.path("fsrun.exe")
        hidden = subprocess.STARTUPINFO()
        hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        hidden.wShowWindow = 0

        def run(argv, show=False, minimised=False):
            si = None
            if not show:
                si = subprocess.STARTUPINFO()
                si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                si.wShowWindow = 7 if minimised else 0
            subprocess.Popen(argv, cwd=game, startupinfo=si, creationflags=0x08000000)

        me = self.ini("launcher.ini")
        if what == "stop":
            subprocess.run(["taskkill", "/F", "/IM", "MW4.exe", "/IM", "Launcher.exe", "/IM", "mw4print.exe",
                            "/IM", "fsrun.exe"], capture_output=True, creationflags=0x08000000)
            self.bridge_stop()
            self.message = "Stopped."
            return
        if not os.path.exists(fsrun):
            self.message = "fsrun.exe and ddraw.dll are not in %s: install the shim first (make install)." % game
            return
        # A second copy of a role would fight the first for its ports: start
        # each afresh. Only what is being started - a console started beside
        # a running pod leaves the pod alone.
        roles = {"pod": ("pod", "pod launcher", "MFD bridge"), "console": ("console", "report printer")}.get(
            what, ("pod", "pod launcher", "MFD bridge", "console", "report printer", "camera", "game"))
        old = [p["pid"] for p in processes() if p["role"] in roles]
        if old:
            subprocess.run(["taskkill", "/F"] + sum([["/PID", str(p)] for p in old], []),
                           capture_output=True, creationflags=0x08000000)
            time.sleep(1.5)                     # until Windows lets go of ddraw.dll and the ports
        if what in ("pod", "both"):
            pod_log = self.path("shim-ctcl2.log")
            pod_was = os.path.getmtime(pod_log) if os.path.exists(pod_log) else 0
            run([fsrun, "-exe", "Launcher.exe"])
            if self.bridge_wanted():
                self.bridge_start()
        if what in ("console", "both"):
            def console():
                if what == "both":              # the pod first, as start-centre.ps1 does: until it draws
                    until = time.time() + 30
                    while time.time() < until:
                        try:
                            if os.path.getmtime(pod_log) > pod_was:
                                with open(pod_log, encoding="latin-1", errors="replace") as f:
                                    if "main1: first frame" in f.read():
                                        break
                        except OSError:
                            pass
                        time.sleep(0.25)
                    time.sleep(1)
                if me.get("launcher", "start_printer", "1") == "1":
                    run([fsrun, "-exe", "mw4print.exe"], minimised=True)
                run([fsrun, "-ctcltype", "1", "/gosnoblade"])
            if what == "both":
                threading.Thread(target=console, daemon=True).start()
            else:
                console()
        if what == "test":
            if self.bridge_wanted():
                self.bridge_start()
            ps1 = os.path.join(os.path.dirname(HERE), "tools", "start-centre.ps1")
            argv = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1, "-Game", game,
                    "-Mission", "-Pilot", args.get("pilot") or "Pilot"]
            if me.get("launcher", "start_printer", "1") != "1":
                argv.append("-NoPrint")
            subprocess.Popen(argv, creationflags=0x08000000)
        self.message = {"pod": "Pod started: the launcher is waiting for the console.",
                        "console": "Console started.",
                        "both": "Pod started; the console follows once the pod is up (a few seconds).",
                        "test": "Starting a test match on this PC (about 35 s)..."}.get(what, "") + self.bridge_note

    # ---- MFD boards (mfdtest.py): find them, tell each which MFD it is, test pictures
    def mfd_mod(self):
        import mfdtest
        if self.mfd_tester is None:
            self.mfd_tester = mfdtest.Tester()
        return mfdtest

    def mfd_state(self):
        mt = self.mfd_mod()
        shim = self.ini("shim.ini")
        me = self.ini("launcher.ini")
        return {"available": mt.serial is not None, "boards": self.mfd_boards,
                "testing": self.mfd_tester.running, "errors": self.mfd_tester.errors(),
                "recorded": mt.recorded(self.game), "stream": shim.get("stream", "enable", "0") == "1",
                "bridge": self.bridge_running(), "bridge_auto": me.get("mfd", "bridge", "1") == "1",
                "bridge_ports": self.bridge_ports(), "bridge_found": self.bridge_files() is not None,
                "bridge_log": self.bridge_log_tail()}

    def mfd_scan(self):
        mt = self.mfd_mod()
        self.mfd_tester.stop()
        if mt.serial is None:
            raise ValueError("this launcher has no pyserial, which MFD boards need")
        self.bridge_stop()                      # it holds the boards' ports
        self.mfd_boards = mt.scan()
        self.remember_boards()
        self.message = "Found %d MFD board%s." % (len(self.mfd_boards), "" if len(self.mfd_boards) == 1 else "s")
        return self.mfd_state()

    def mfd_assign(self, body):
        """A board is MFD n now; one that was MFD n before is no MFD."""
        mt = self.mfd_mod()
        port, n = body["port"], int(body.get("mfd") or 0)
        testing = self.mfd_tester.running
        self.mfd_tester.stop()
        self.bridge_stop()
        for b in self.mfd_boards:
            if n and b["port"] != port and b["mfd"] == n:
                b.update(mt.assign(b["port"], 0))
        done = mt.assign(port, n)
        for b in self.mfd_boards:
            if b["port"] == port:
                b.update(done)
        self.remember_boards()
        self.message = "%s is %s." % (port, "MFD %d" % n if n else "no MFD")
        if testing:
            self.mfd_tester.start(self.mfd_boards, self.game)
        return self.mfd_state()

    def mfd_test(self, body):
        self.mfd_mod()
        if body.get("on"):
            if not self.mfd_boards:
                raise ValueError("no MFD boards found yet: Find boards first")
            self.bridge_stop()
            self.mfd_tester.start(self.mfd_boards, self.game)
            self.message = "Test pictures on %d board%s." % (len(self.mfd_boards), "" if len(self.mfd_boards) == 1 else "s")
        else:
            self.mfd_tester.stop()
            self.message = "Test pictures stopped."
        return self.mfd_state()

    def mfd_record(self, body):
        mt = self.mfd_mod()
        port = int(self.ini("shim.ini").get("stream", "port", "9110") or 9110)
        again = self.bridge_running()           # the stream takes one client: the bridge steps aside
        self.bridge_stop()
        if again:
            time.sleep(0.5)                     # until the shim sees it go
        try:
            done = mt.record(self.game, float(body.get("seconds") or 4), port)
        finally:
            if again:
                self.bridge_start()
        self.message = "Recorded MFD %s from the game." % ", ".join(map(str, done))
        return self.mfd_state()

    def mfd_forget(self):
        mt = self.mfd_mod()
        for n in mt.recorded(self.game):
            os.remove(mt.rec_path(self.game, n))
        self.message = "Recordings deleted: the test pictures are drawn ones again."
        return self.mfd_state()

    def mfd_stream(self, body):
        """[stream] enable: the shim serves the MFD pictures, for the bridge (and recording)."""
        shim = self.ini("shim.ini")
        shim.set("stream", "enable", "1" if body.get("on") else None)
        shim.save()
        self.message = "The game streams its MFDs (from the next start)." if body.get("on") else "MFD streaming off."
        return self.mfd_state()

    # ---- the MFD bridge (battlepod's tools/bridge.py): the game's MFD
    # pictures, from its stream port, to the boards. Started with the pod
    # when boards are assigned; a program of its own, so it runs on when
    # this page closes, and is found again by its command line.
    bridge_note = ""

    def bridge_files(self, game="firestorm"):
        """(bridge.py, profile, layout) for a game - Firestorm's five MFDs,
        or BattleTech 3.0's panel (battlepod) - beside this launcher in a
        packaged copy, else in the battlepod tree round it."""
        top = os.path.normpath(os.path.join(HERE, "..", ".."))
        prof, lay = ("firestorm-5.07", "firestorm-mfds") if game == "firestorm" else ("battletech-13.1.8", "milestone-mfd")
        for script, profile, layout in (
                (os.path.join(HERE, "bridge.py"), os.path.join(HERE, prof), os.path.join(HERE, lay)),
                (os.path.join(top, "tools", "bridge.py"), os.path.join(top, "profiles", prof),
                 os.path.join(top, "layouts", lay))):
            if all(os.path.isfile(f) for f in (script, profile, layout)):
                return script, profile, layout
        return None

    def remember_boards(self):
        """The ports of the boards that are an MFD: what the bridge opens."""
        me = self.ini("launcher.ini")
        ports = [b["port"] for b in self.mfd_boards if b.get("mfd")]
        me.set("mfd", "ports", ",".join(ports) or None)
        me.save()

    def bridge_ports(self):
        return [p.strip() for p in self.ini("launcher.ini").get("mfd", "ports", "").split(",") if p.strip()]

    def bridge_wanted(self):
        return (self.ini("launcher.ini").get("mfd", "bridge", "1") == "1" and bool(self.bridge_ports())
                and self.bridge_files() is not None)

    def bridge_pids(self):
        return [p["pid"] for p in processes() if p["role"] == "MFD bridge"]

    def bridge_running(self):
        return bool(self.bridge_pids())

    def bridge_start(self, game="firestorm"):
        import mfdtest
        files = self.bridge_files(game)
        self.bridge_note = ""
        if files is None:
            raise ValueError("the MFD bridge (bridge.py) is not with this launcher")
        if not self.bridge_ports():
            raise ValueError("no board is an MFD yet: Find boards, then give each its MFD")
        self.bridge_stop()
        if self.mfd_tester is not None:
            self.mfd_tester.stop()              # the test pictures hold the same ports
        script, profile, layout = files
        shim = self.ini("shim.ini")
        if game == "firestorm" and shim.get("stream", "enable", "0") != "1":
            self.bridge_note = " The game does not stream its MFDs (MFD boards page): the boards stay dark."
        argv = [sys.executable, script, "--game", game, "--profile", profile, "--layout", layout]
        if game == "firestorm":
            argv += ["--stream-port", shim.get("stream", "port", "9110") or "9110"]
        else:
            argv += ["--pod-port", str(BT3_RIO_PORT)]
        for port in self.bridge_ports():
            argv += ["--serial", "%s@%d" % (port, mfdtest.BAUD)]
        log = open(self.path("bridge.log"), "w", encoding="utf-8")
        # A process group of its own, no window: it outlives this launcher.
        subprocess.Popen(argv, cwd=os.path.dirname(script), stdout=log, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, creationflags=0x08000000 | 0x00000200)
        log.close()

    def bridge_stop(self):
        pids = self.bridge_pids()
        if pids:
            subprocess.run(["taskkill", "/F"] + sum([["/PID", str(p)] for p in pids], []),
                           capture_output=True, creationflags=0x08000000)
            time.sleep(0.3)                     # until Windows lets go of its serial ports

    def bridge_log_tail(self, n=8):
        try:
            with open(self.path("bridge.log"), encoding="utf-8", errors="replace") as f:
                return [l.rstrip() for l in f.readlines()[-n:]]
        except OSError:
            return []

    def mfd_bridge(self, body):
        """Start or stop the bridge now; or whether it starts with the pod."""
        if "auto" in body:
            me = self.ini("launcher.ini")
            me.set("mfd", "bridge", None if body["auto"] else "0")
            me.save()
            self.message = "The bridge starts with the pod." if body["auto"] else "The bridge is started by hand."
        if "on" in body:
            if body["on"]:
                self.bridge_start()
                self.message = "MFD bridge started on %s.%s" % (", ".join(self.bridge_ports()), self.bridge_note)
            else:
                self.bridge_stop()
                self.message = "MFD bridge stopped."
        return self.mfd_state()

    def logs(self):
        out = {}
        p = self.path("bridge.log")
        if os.path.exists(p):
            out["bridge"] = self.bridge_log_tail(12)
        for role in ("ctcl2", "ctcl1", "launcher", "mw4print"):
            p = self.path("shim-%s.log" % role)
            if os.path.exists(p):
                with open(p, encoding="latin-1", errors="replace") as f:
                    out[role] = [l.rstrip() for l in f.readlines()[-12:]]
        return out


# ------------------------------------------------------------ pilots
# The console's list of 'Mechs, which a pod goes by: every chassis in
# alphabetical order, each followed by its Resource\Variants files; a 'Mech's
# index is its place in it from 1 (read from a running console's memory, and
# rebuilt from these 65 chassis and the variant files exactly).
CHASSIS = [
    "Annihilator", "Archer", "Arctic Wolf", "Ares", "Argus", "AssassinII", "Atlas", "Avatar", "Awesome",
    "Battlemaster", "BattlemasterIIC", "Behemoth", "BehemothII", "Black Hawk", "Black Knight", "Black Lanner",
    "Brigand", "Bushwacker", "Catapult", "Cauldron-Born", "Chimera", "Commando", "Cougar", "Cyclops", "Daishi",
    "Deimos", "Dragon", "Fafnir", "Flea", "Gladiator", "Grizzly", "Hauptmann", "Hellhound", "Hellspawn",
    "Highlander", "HollanderII", "Hunchback", "Kodiak", "Loki", "Longbow", "Mad Cat", "Mad Cat MKII", "Masakari",
    "Mauler", "Nova Cat", "Osiris", "Owens", "Puma", "Raven", "Rifleman", "Ryoken", "Shadow Cat", "Solitaire",
    "Sunder", "Templar", "Thanatos", "Thor", "Uller", "Urbanmech", "Uziel", "Victor", "Vulture", "Warhammer",
    "Wolfhound", "Zeus",
]
NAME_MAX = 15            # the console's name box, and what fits the plasma sign large


def mech_table(game):
    """[(chassis, variant file name or "")] in the console's order."""
    d = os.path.join(game, "resource", "Variants")
    files = [f[:-4] for f in os.listdir(d) if f.lower().endswith(".mw4")] if os.path.isdir(d) else []
    ch = sorted(CHASSIS, key=str.lower)
    out = []
    for c in ch:
        out.append((c, ""))
        mine = [f for f in files if f.lower().startswith(c.lower() + " ") and
                max((c2 for c2 in ch if f.lower().startswith(c2.lower() + " ")), key=len) == c]
        out += [(c, f) for f in sorted(mine, key=str.lower)]
    return out


def mech_images(game):
    """chassis -> its picture in the game's hsh\\Mechs (the battle report's
    portraits), matched loosely: "Arctic Wolf" is arctic wolf.bmp, "Mad Cat
    MKII" mad cat mk.ii.bmp. Behemoth, Longbow and Victor have none."""
    d = os.path.join(game, "hsh", "Mechs")
    if not os.path.isdir(d):
        return {}
    norm = lambda s: re.sub(r"[^a-z0-9]", "", s.lower())
    files = {norm(f[:-4]): f for f in os.listdir(d) if f.lower().endswith(".bmp")}
    return {c: "hsh/Mechs/" + files[norm(c)] for c in CHASSIS if norm(c) in files}


class Pilots:
    """The roster (profiles\\profiles.json) and who sits in which pod
    (profiles\\assign.ini, which the console's shim reads at LOAD)."""

    def __init__(self, game):
        self.game = game
        self.dir = os.path.join(game, "profiles")
        self.lock = threading.Lock()

    def _path(self, name):
        return os.path.join(self.dir, name)

    def roster(self):
        try:
            with open(self._path("profiles.json"), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return []

    def _write_roster(self, r):
        os.makedirs(self.dir, exist_ok=True)
        tmp = self._path("profiles.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(r, f, indent=1)
        os.replace(tmp, self._path("profiles.json"))

    def mechs(self):
        t = mech_table(self.game)
        groups = {}
        for c, v in t:
            groups.setdefault(c, [])
            if v:
                groups[c].append({"file": v, "label": v[len(c):].strip()})
        return {"chassis": [c for c in sorted(CHASSIS, key=str.lower)], "variants": groups,
                "images": mech_images(self.game), "specs": self.specs(), "livery": self.livery()}

    def livery(self):
        """The camo and unit pictures tools/livery.py read off the console:
        a /game/ path for each number that has one."""
        d = os.path.join(self.dir, "livery")

        def pics(kind, n):
            return {i: "profiles/livery/%s_%02d.png" % (kind, i) for i in range(1, n + 1)
                    if os.path.exists(os.path.join(d, "%s_%02d.png" % (kind, i)))}
        return {"camos": CAMOS, "units": UNITS, "camo": pics("camo", CAMOS), "unit": pics("unit", UNITS)}

    def specs(self):
        """What tools/mechcat.py read off the game's own screens, by 'Mech
        name ("Loki", "Loki B"): statistics and weapons. Empty until it runs."""
        try:
            with open(self._path("mechs.json"), encoding="utf-8") as f:
                cat = json.load(f)
        except (OSError, ValueError):
            return {}
        return {k: {"stats": v.get("stats", {}), "weapons": v.get("weapons", [])} for k, v in cat.items()}

    def index_of(self, chassis, variant):
        t = mech_table(self.game)
        try:
            return t.index((chassis, variant or "")) + 1
        except ValueError:
            return 0

    def seats(self):
        ini = Ini(self._path("assign.ini"))
        out = {}
        for l in ini.lines:
            m = re.match(r"\s*\[(.+?)\]", l)
            if m:
                ip = m.group(1)
                out[ip] = {"profile": ini.get(ip, "profile"), "name": ini.get(ip, "name"), "mech": ini.get(ip, "mech")}
        return out

    def save_profile(self, p):
        name = (p.get("name") or "").strip()[:NAME_MAX]
        if not name:
            raise ValueError("a pilot needs a name")
        if not re.match(r"^[ -~]+$", name):
            raise ValueError("names are plain letters, digits and punctuation")
        chassis = p.get("chassis") or "Loki"
        if chassis not in CHASSIS:
            raise ValueError("no 'Mech called %s" % chassis)
        variant = p.get("variant") or ""
        if variant and not self.index_of(chassis, variant):
            raise ValueError("no %s variant %s" % (chassis, variant))
        with self.lock:
            r = self.roster()
            if any(x["name"].lower() == name.lower() and x["id"] != p.get("id") for x in r):
                raise ValueError("there is already a pilot called %s" % name)
            if p.get("id"):
                cur = next((x for x in r if x["id"] == p["id"]), None)
                if not cur:
                    raise ValueError("no such pilot")
            else:
                cur = {"id": "%x" % int(time.time() * 1000), "created": time.strftime("%Y-%m-%d")}
                r.append(cur)
            def number(key, top):
                try:
                    v = int(p.get(key) or 1)
                except (TypeError, ValueError):
                    raise ValueError("%s is a number" % key)
                if not 1 <= v <= top:
                    raise ValueError("%s is 1 to %d" % (key, top))
                return v
            cur.update(name=name, chassis=chassis, variant=variant, notes=(p.get("notes") or "")[:200],
                       camo=number("camo", CAMOS), unit=number("unit", UNITS))
            self._write_roster(r)
            # A pilot already seated takes their new 'Mech into the next match.
            for ip, s in self.seats().items():
                if s["profile"] == cur["id"]:
                    self._seat(ip, cur)
            return cur

    def delete_profile(self, pid):
        with self.lock:
            r = [x for x in self.roster() if x["id"] != pid]
            self._write_roster(r)
            for ip, s in self.seats().items():
                if s["profile"] == pid:
                    self._seat(ip, None)

    def _seat(self, ip, prof):
        os.makedirs(self.dir, exist_ok=True)
        ini = Ini(self._path("assign.ini"))
        if not ini.lines:
            ini.lines = ["; Who sits in which pod, by its address: written by the launcher, read by the",
                         "; console's shim when a match loads (README, Profiles).", ""]
        # Remove the pod's section, and the pilot's seat anywhere else.
        for sec in [ip] + ([s for s, v in self.seats().items() if prof and v["profile"] == prof["id"]]):
            r = ini._find(sec)
            if r:
                del ini.lines[r[0]:r[1]]
        if prof:
            mech = prof["variant"] or prof["chassis"]
            # chassis_pos, camo and unit are for the console's own lists, which
            # its shim fills in at start (README, Profiles): the chassis' place
            # among the 65 as the console lists them, alphabetically.
            ini.lines += ["[%s]" % ip, "profile=%s" % prof["id"], "name=%s" % prof["name"], "mech=%s" % mech,
                          "mech_index=%d" % self.index_of(prof["chassis"], prof["variant"]),
                          "chassis_pos=%d" % (sorted(CHASSIS, key=str.lower).index(prof["chassis"]) + 1),
                          "camo=%d" % prof.get("camo", 1), "unit=%d" % prof.get("unit", 1), ""]
        ini.save()

    def seat(self, ip, pid):
        with self.lock:
            prof = next((x for x in self.roster() if x["id"] == pid), None) if pid else None
            if pid and not prof:
                raise ValueError("no such pilot")
            self._seat(ip, prof)
            return prof


# ------------------------------------------------------------ http
def make_handler(app):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            app.last_ping = time.time()
            path = self.path.split("?")[0]
            if path == "/api/state":
                return self.send(200, app.state())
            if path == "/api/processes":
                return self.send(200, {"processes": processes(), "message": app.message, "pads": pads()})
            if path == "/api/logs":
                return self.send(200, app.logs())
            if path == "/api/mfd":
                return self.send(200, app.mfd_state())
            if path == "/api/joy":
                return self.send(200, {"joysticks": joysticks()})
            if path == "/api/ping":
                return self.send(200, {"ok": True})
            if path == "/api/pilots":
                try:
                    return self.send(200, app.pilots_view())
                except ValueError as e:
                    return self.send(200, {"error": str(e), "local": False, "console": app.console_addr()})
            if path == "/api/bt3":
                v = app.bt3.view()
                if v.get("battle") and v["battle"].get("over"):
                    threading.Thread(target=app.bt3_report, daemon=True).start()
                return self.send(200, v)
            if path == "/api/career":
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                since = (q.get("since") or [""])[0]
                game = (q.get("game") or [""])[0]
                try:
                    return self.send(200, app.career_view(since if re.match(r"^\d{4}-\d\d-\d\d$", since) else None,
                                                          game if game in ("fs", "bt3") else None))
                except ValueError as e:
                    return self.send(200, {"error": str(e)})
            # The game's own fonts and pictures, from the game folder - never
            # copied anywhere - and only these.
            if path.startswith("/game/"):
                rel = urllib.parse.unquote(path[6:])
                if not GAME_FILES.match(rel):
                    return self.send(404, {"error": "not found"})
                f = os.path.join(app.game, *rel.split("/"))
                if not os.path.isfile(f):
                    return self.send(404, {"error": "not found"})
                ctype = {".bmp": "image/bmp", ".png": "image/png"}.get(os.path.splitext(f)[1].lower(), "font/ttf")
                with open(f, "rb") as fh:
                    data = fh.read()
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "max-age=86400")
                self.end_headers()
                return self.wfile.write(data)
            name = "index.html" if path == "/" else path.lstrip("/")
            f = os.path.normpath(os.path.join(WEB, name))
            if not f.startswith(WEB) or not os.path.isfile(f):
                return self.send(404, {"error": "not found"})
            ctype = {".html": "text/html", ".js": "text/javascript", ".css": "text/css",
                     ".svg": "image/svg+xml", ".png": "image/png"}.get(os.path.splitext(f)[1], "application/octet-stream")
            with open(f, "rb") as fh:
                data = fh.read()
            if name == "index.html":            # this run's scripts and styles, never a cached older copy
                data = data.replace(b'.js"', b'.js?v=%d"' % START).replace(b'.css"', b'.css?v=%d"' % START)
            return self.send(200, data, ctype + ("; charset=utf-8" if ctype.startswith("text") else ""))

        def do_POST(self):
            app.last_ping = time.time()
            # Only the page we served: refuse requests from other sites.
            origin = self.headers.get("Origin", "")
            if origin and not origin.startswith("http://127.0.0.1:"):
                return self.send(403, {"error": "forbidden"})
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            try:
                if self.path == "/api/save":
                    app.save(body)
                elif self.path == "/api/start":
                    app.start(body.get("what"), body)
                elif self.path.startswith("/api/mfd/"):
                    fn = {"/api/mfd/scan": lambda b: app.mfd_scan(), "/api/mfd/assign": app.mfd_assign,
                          "/api/mfd/test": app.mfd_test, "/api/mfd/record": app.mfd_record,
                          "/api/mfd/forget": lambda b: app.mfd_forget(), "/api/mfd/stream": app.mfd_stream,
                          "/api/mfd/bridge": app.mfd_bridge}.get(self.path)
                    if not fn:
                        return self.send(404, {"error": "not found"})
                    out = fn(body)
                    out.update(ok=True, message=app.message)
                    return self.send(200, out)
                elif self.path.startswith("/api/bt3/"):
                    b = app.bt3
                    if self.path == "/api/bt3/save":
                        b.save(body)
                        return self.send(200, {"ok": True})
                    if self.path == "/api/bt3/start":
                        # The panel on the MFD boards, when there are some (MFD boards page).
                        human = ""
                        if app.bridge_wanted() and app.bridge_files("battlepod"):
                            app.bridge_start("battlepod")
                            human = "--rio-port 127.0.0.1:%d" % BT3_RIO_PORT
                        else:
                            app.bridge_stop()
                        msg = b.start(body, human)
                        if human:
                            msg += " The cockpit's panel goes to the MFD board%s." % ("s" if len(app.bridge_ports()) > 1 else "")
                        return self.send(200, {"ok": True, "message": msg})
                    if self.path == "/api/bt3/join":
                        return self.send(200, {"ok": True, "message": b.join(body)})
                    if self.path == "/api/bt3/stop":
                        b.stop()
                        app.bridge_stop()
                        return self.send(200, {"ok": True})
                    return self.send(404, {"error": "not found"})
                elif self.path in ("/api/pilot", "/api/pilot/delete", "/api/seat"):
                    fn = {"/api/pilot": app.pilot_save, "/api/pilot/delete": app.pilot_delete,
                          "/api/seat": app.pilot_seat}[self.path]
                    out = fn(body)
                    out["ok"] = True
                    return self.send(200, out)
                else:
                    return self.send(404, {"error": "not found"})
            except ValueError as e:                     # the user's to fix: say so plainly
                return self.send(400, {"error": str(e)})
            except Exception as e:                      # shown in the window, not lost
                app.message = "Error: %s" % e
                return self.send(500, {"error": str(e)})
            return self.send(200, {"ok": True, "message": app.message})
    return H


# The console's launcher answers its pods on the network: the roster, a new
# or changed pilot, and "seat this pilot in me" - the pod being whoever asks.
POD_PORT = 8790
# BattleTech 3.0's cockpit sends its panel (Remote I/O) to the bridge here.
BT3_RIO_PORT = 5180


def make_pod_handler(app):
    def pod_of(ip):
        """The pod a request comes from. One on this same PC asks from
        127.0.0.1: it is the pod listed under one of our addresses."""
        if ip.startswith("127.") or ip in local_addresses():
            mine = [t["ip"] for t in app.state_teslas() if t["ip"] in local_addresses()]
            return mine[0] if mine else ip
        return ip

    class P(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path, _, query = self.path.partition("?")
            p = app.pilots
            if path == "/pod/career":
                q = urllib.parse.parse_qs(query)
                since, game = (q.get("since") or [""])[0], (q.get("game") or [""])[0]
                return self.send(200, app.career_view(since if re.match(r"^\d{4}-\d\d-\d\d$", since) else None,
                                                      game if game in ("fs", "bt3") else None))
            if path == "/pod/ranks":
                return self.send(200, {"ini": app.careers.ranks_ini(p.roster())})
            if path != "/pod/roster":
                return self.send(404, {"error": "not found"})
            roster = p.roster()
            return self.send(200, {"roster": roster, "seats": p.seats(), "mechs": p.mechs(),
                                   "pods": app.state_teslas(), "you": pod_of(self.client_address[0]),
                                   "careers": app.career_of(roster)})

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n > 65536:
                return self.send(413, {"error": "too big"})
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
                if self.path == "/pod/profile":
                    return self.send(200, {"ok": True, "profile": app.pilots.save_profile(body)})
                if self.path == "/pod/seat":
                    return self.send(200, {"ok": True, "profile": app.pilots.seat(pod_of(self.client_address[0]), body.get("id"))})
                if self.path == "/pod/career":
                    added = app.careers.add(body, pod_of(self.client_address[0]), app.pilots.roster())
                    return self.send(200, {"ok": True, "added": added})
                return self.send(404, {"error": "not found"})
            except ValueError as e:
                return self.send(400, {"error": str(e)})
    return P


def edge():
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA")):
        if base:
            p = os.path.join(base, "Microsoft", "Edge", "Application", "msedge.exe")
            if os.path.exists(p):
                return p
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    default = HERE if os.path.exists(os.path.join(HERE, "MW4.exe")) else (
        os.path.dirname(HERE) if os.path.exists(os.path.join(os.path.dirname(HERE), "MW4.exe")) else
        os.path.normpath(os.path.join(HERE, "..", "..", "firestorm-run", "MW4")))
    ap.add_argument("--game", default=default)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--no-window", action="store_true", help="serve only; open the printed address yourself")
    ap.add_argument("--page", default="", help="the page to open on: bt3 for BattleTech 3.0")
    a = ap.parse_args()

    app = Launcher(os.path.abspath(a.game))
    if os.path.exists(app.path("MW4.exe")):
        app.relocate()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(app))
    url = "http://127.0.0.1:%d/" % srv.server_address[1]
    if re.match(r"^\w+$", a.page):
        url += "#" + a.page
    print("Firestorm launcher: %s (game: %s)" % (url, app.game), flush=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    # On the console, the pods' way in to the roster ([launcher] pod_port).
    if app.role() in ("console", "both"):
        port = int(app.ini("launcher.ini").get("launcher", "pod_port", str(POD_PORT)))
        try:
            pods = ThreadingHTTPServer(("0.0.0.0", port), make_pod_handler(app))
            threading.Thread(target=pods.serve_forever, daemon=True).start()
            print("pilots for the pods: port %d" % port, flush=True)
        except OSError as e:
            app.message = "Cannot offer pilots to the pods on port %d: %s" % (port, e)

    # On a pod, its finished matches to the console's careers.
    if app.role() in ("pod", "both") or app.bt3.available:
        threading.Thread(target=app.career_loop, daemon=True).start()

    if not a.no_window:
        e = edge()
        profile = os.path.join(os.environ.get("LOCALAPPDATA", HERE), "FirestormLauncher", "window")
        if e:
            subprocess.Popen([e, "--app=" + url, "--user-data-dir=" + profile, "--window-size=1280,860",
                              "--no-first-run", "--disable-features=Translate"])
        else:
            import webbrowser
            webbrowser.open(url)
    # The page pings every 2 s; once it has and then stops for 10 s, the window is closed.
    try:
        while True:
            time.sleep(1)
            if not a.no_window and app.last_ping and time.time() - app.last_ping > 10:
                break
    except KeyboardInterrupt:
        pass
    srv.shutdown()


if __name__ == "__main__":
    main()
