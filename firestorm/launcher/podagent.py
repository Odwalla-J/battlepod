"""podagent: a pod PC run unattended, from the console.

On a pod, `Firestorm.exe --auto` (what Windows starts at sign-in, once the
first-time setup has run) starts the launcher with no window and this beside
it. It owns the pod's screens and its games, and plays whichever the console
plays:

  Ready      every monitor black, "Ready" in large white letters on the main
             one, no cursor - what a pod shows when nobody is in it, and with
             no game running until the console starts one
  Firestorm  while the console's Firestorm is up: the pod side of Firestorm
             (Launcher.exe, which starts the pod's MW4 and waits for the
             console's LOAD) - while it waits the shim paints the same Ready
             on the main view ([ready] in shim.ini) - and when the console
             closes, no game and Ready again
  3.0        when the console's 3.0 battle tells it to join: the cockpit,
             full screen, until the battle ends - and Ready again

A pod is in service ("auto", following the console) or out of it ("ready":
Ready, whatever the console plays). It keeps what should be running
running: a game that falls over is started again. The console finds it and
drives it over the centre's network:

  UDP 5198   this pod's beacon, broadcast every 2 s and on any change:
             {"t": "pod", "name", "num", "mode", "state", "detail", "port"}
  UDP 5197   the console's "who" (broadcast); answered at once - and it is
             how a pod learns the console's address, with nothing typed,
             and whether the console is up ("fs"), should a /firestorm be
             missed
  TCP 8795   commands: GET /status; POST /mode {"mode": "auto" | "ready"},
             /firestorm {"run", "fresh"} (the console up or
             not), /bt3/join {"host", "seat", "name"}, /identify,
             /settings (the launcher's window over the Ready screen),
             /restart, /power {"action": "restart" | "shutdown"}

Only addresses on the centre's own network are listened to.
"""
import ctypes
import ipaddress
import json
import os
import re
import socket
import subprocess
import threading
import time
from ctypes import wintypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BEACON_PORT = 5198          # pods -> console
WHO_PORT = 5197             # console -> pods
AGENT_PORT = 8795

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM)
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.DefWindowProcW.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]
user32.CreateWindowExW.restype = wintypes.HWND
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_uint]
gdi32.CreateFontW.restype = wintypes.HFONT
# Handles are pointer-sized: without these, a 64-bit Python passes and
# returns them as C ints, and a large one fails or is cut short.
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.GetStockObject.restype = wintypes.HGDIOBJ
gdi32.SetBkMode.argtypes = [wintypes.HDC, ctypes.c_int]
gdi32.SetTextColor.argtypes = [wintypes.HDC, wintypes.COLORREF]
user32.BeginPaint.restype = wintypes.HDC
user32.BeginPaint.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.EndPaint.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.FillRect.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.HBRUSH]
user32.DrawTextW.argtypes = [wintypes.HDC, wintypes.LPCWSTR, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.InvalidateRect.argtypes = [wintypes.HWND, ctypes.c_void_p, wintypes.BOOL]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.PostMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]
user32.SetCursor.argtypes = [wintypes.HANDLE]
kernel32.GetModuleHandleW.restype = wintypes.HMODULE
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.Process32FirstW.argtypes = kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", ctypes.c_uint), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH),
                ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR)]


class PAINTSTRUCT(ctypes.Structure):
    _fields_ = [("hdc", wintypes.HDC), ("fErase", wintypes.BOOL), ("rcPaint", wintypes.RECT),
                ("fRestore", wintypes.BOOL), ("fIncUpdate", wintypes.BOOL), ("rgbReserved", ctypes.c_byte * 32)]


WM_PAINT, WM_ERASEBKGND, WM_SETCURSOR, WM_CLOSE, WM_APP = 0x000F, 0x0014, 0x0020, 0x0010, 0x8000
WS_POPUP, WS_EX_TOPMOST, WS_EX_TOOLWINDOW = 0x80000000, 0x00000008, 0x00000080
SW_HIDE, SW_SHOWNOACTIVATE = 0, 4
HWND_TOPMOST = wintypes.HWND(-1)
SWP_NOACTIVATE, SWP_SHOWWINDOW = 0x0010, 0x0040


class ReadyScreen:
    """Black windows over every monitor, the main one saying Ready. Their own
    thread runs their message loop; show() and hide() are safe from any."""

    def __init__(self):
        self.text, self.sub, self.flash = "Ready", "", 0.0
        self.wins = []                  # (hwnd, is_main)
        self.shown = False
        self.ready = threading.Event()
        self._proc = WNDPROC(self._wndproc)
        threading.Thread(target=self._run, daemon=True).start()
        self.ready.wait(5)

    def _wndproc(self, h, msg, wp, lp):
        if msg == WM_ERASEBKGND:
            return 1
        if msg == WM_SETCURSOR:
            user32.SetCursor(None)      # a pod has no mouse: no arrow on its screens
            return 1
        if msg == WM_CLOSE:
            return 0                    # only this program closes them
        if msg == WM_PAINT:
            ps = PAINTSTRUCT()
            dc = user32.BeginPaint(h, ctypes.byref(ps))
            rc = wintypes.RECT()
            user32.GetClientRect(h, ctypes.byref(rc))
            user32.FillRect(dc, ctypes.byref(rc), gdi32.GetStockObject(4))      # BLACK_BRUSH
            if any(w == h and main for w, main in self.wins):
                self._paint_text(dc, rc)
            user32.EndPaint(h, ctypes.byref(ps))
            return 0
        if msg == WM_APP:               # show (wp 1) or hide (wp 0), from another thread
            for w, main in self.wins:
                if wp:
                    user32.SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_SHOWWINDOW | 0x0001 | 0x0002)
                    user32.InvalidateRect(w, None, True)
                else:
                    user32.ShowWindow(w, SW_HIDE)
            return 0
        if msg == WM_APP + 1:           # the text changed
            for w, main in self.wins:
                if main:
                    user32.InvalidateRect(w, None, True)
            return 0
        return user32.DefWindowProcW(h, msg, wp, lp)

    def _paint_text(self, dc, rc):
        w, h = rc.right - rc.left, rc.bottom - rc.top
        gdi32.SetBkMode(dc, 1)          # TRANSPARENT
        flashing = time.time() < self.flash
        big = gdi32.CreateFontW(-int(h * 0.22), 0, 0, 0, 800, 0, 0, 0, 0, 0, 0, 4, 0, "Segoe UI")
        old = gdi32.SelectObject(dc, big)
        gdi32.SetTextColor(dc, 0x00FFFFFF if not flashing else 0x0000C8FF)
        r = wintypes.RECT(0, 0, w, int(h * 0.92))
        user32.DrawTextW(dc, self.text, -1, ctypes.byref(r), 0x1 | 0x4 | 0x20)    # centre, vcentre, one line
        gdi32.SelectObject(dc, old)
        gdi32.DeleteObject(big)
        if self.sub:
            small = gdi32.CreateFontW(-max(14, int(h * 0.028)), 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 4, 0, "Segoe UI")
            old = gdi32.SelectObject(dc, small)
            gdi32.SetTextColor(dc, 0x00707070)
            r = wintypes.RECT(0, int(h * 0.86), w, int(h * 0.96))
            user32.DrawTextW(dc, self.sub, -1, ctypes.byref(r), 0x1 | 0x4 | 0x20)
            gdi32.SelectObject(dc, old)
            gdi32.DeleteObject(small)

    def _run(self):
        try:
            user32.SetProcessDPIAware()
        except AttributeError:
            pass
        inst = kernel32.GetModuleHandleW(None)
        wc = WNDCLASSW(0, self._proc, 0, 0, inst, None, None, None, None, "BattleTechReady")
        user32.RegisterClassW(ctypes.byref(wc))
        self.main_index = 0
        for n, m in enumerate(monitors()):
            hw = user32.CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "BattleTechReady", "Ready", WS_POPUP,
                                        m["x"], m["y"], m["w"], m["h"], None, None, inst, None)
            self.wins.append((hw, n == self.main_monitor_index()))
        self.ready.set()
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    main_monitor = 1                    # set before construction: one-based, as the launcher counts them

    def main_monitor_index(self):
        return max(0, self.main_monitor - 1)

    def _post(self, msg, wp=0):
        if self.wins:
            user32.PostMessageW(self.wins[0][0], msg, wp, 0)

    def show(self, text=None, sub=None):
        changed = False
        if text is not None and text != self.text:
            self.text, changed = text, True
        if sub is not None and sub != self.sub:
            self.sub, changed = sub, True
        if not self.shown:
            self.shown = True
            self._post(WM_APP, 1)
        elif changed:
            self._post(WM_APP + 1)

    def hide(self):
        if self.shown:
            self.shown = False
            self._post(WM_APP, 0)

    def identify(self, seconds=6):
        """The pod says which it is, in amber, for a few seconds."""
        self.flash = time.time() + seconds
        self._post(WM_APP + 1)

        def back():
            time.sleep(seconds + 0.1)
            self._post(WM_APP + 1)
        threading.Thread(target=back, daemon=True).start()


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


def exe_names():
    """Every running program's file name, lower case - a snapshot, quick
    enough to take every second (the launcher's own list asks PowerShell)."""
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    out = []
    if not snap or snap == wintypes.HANDLE(-1).value:
        return out
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = kernel32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        out.append(e.szExeFile.lower())
        ok = kernel32.Process32NextW(snap, ctypes.byref(e))
    kernel32.CloseHandle(snap)
    return out


# The launcher's own, given to this module when it imports it (fslauncher.py):
# every monitor as it lists them, and where Edge is.
monitors = edge = None


def lan(ip):
    """An address on the centre's own network, or this PC."""
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False
    return a.is_private or a.is_loopback or a.is_link_local


def pod_mode(mode):
    """A pod's mode: "auto" (in service: the console's game, whichever it
    is) or "ready" (out of service). The games' own modes, from before
    pods followed the console, are in service."""
    return "ready" if mode == "ready" else "auto"


def own_ip():
    """This PC's address on the centre's network (the one a route out uses)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


class PodAgent:
    def __init__(self, app):
        self.app = app                  # the launcher (fslauncher.Launcher) on this PC
        me = app.ini("launcher.ini")
        self.name = me.get("pod", "name", "") or socket.gethostname()
        self.num = int(me.get("pod", "number", "0") or 0)
        self.mode = pod_mode(me.get("pod", "mode", "auto"))
        self.state, self.detail = "starting", ""
        self.game_up = False            # the pod's MW4 is drawing
        self.fs_live = False            # the console is up: Firestorm's pod side runs (in Firestorm mode)
        self.console_seen = False       # this run of the pod's MW4 has taken a console (it takes one only)
        self.console = None             # the console's address, as its "who" gave it
        self.joined = None              # this pod's 3.0 cockpit, while in a battle
        self.lock = threading.RLock()
        self.changed = threading.Event()
        self.settings_proc = None
        ReadyScreen.main_monitor = int(app.ini("shim.ini").get("main.ctcl2", "monitor", "1") or 1)
        self.screen = ReadyScreen()
        # Keep the display awake and the PC from sleeping while this runs.
        kernel32.SetThreadExecutionState(0x80000000 | 0x00000002 | 0x00000001)

    # ---- what the console sees
    def status(self):
        return {"t": "pod", "v": 1, "name": self.name, "num": self.num, "mode": self.mode, "state": self.state,
                "detail": self.detail, "port": AGENT_PORT, "console_seen": self.console_seen,
                "game_up": self.game_up, "fs_live": self.fs_live, "host": socket.gethostname(), "ip": own_ip(),
                "games": ["firestorm"] + (["bt3"] if self.app.bt3.available else [])}

    def set_state(self, state, detail=""):
        if (state, detail) != (self.state, self.detail):
            self.state, self.detail = state, detail
            self.changed.set()

    def sub(self):
        game = "BattleTech: Firestorm" if self.mode == "auto" and self.fs_live else ""
        return "   ".join(x for x in (self.name, game) if x)

    def in_battle(self):
        """This pod's 3.0 cockpit is running."""
        if self.joined and self.joined.poll() is not None:
            self.joined = None
        return self.joined is not None

    # ---- the games
    def firestorm_procs(self):
        """The pod side's programs that are running: Launcher.exe and the
        pod's MW4 (a pod PC runs no console)."""
        return [n for n in exe_names() if n in ("launcher.exe", "mw4.exe")]

    def stop_firestorm(self):
        self.game_up = False
        if self.firestorm_procs() or self.app.bridge_running():
            self.app.stop_pod()
        self.console_seen = False

    def stop_bt3(self):
        if self.joined and self.joined.poll() is None:
            self.joined.kill()
        self.joined = None
        self.app.bt3.stop_joined()

    def set_mode(self, mode, restart=False):
        """In service ("auto") or out of it ("ready"). The same mode again
        changes nothing, unless restart: then whatever game should be
        running is started afresh."""
        if mode not in ("auto", "ready", "firestorm", "bt3"):
            raise ValueError("no such mode: %s" % mode)
        mode = pod_mode(mode)
        with self.lock:
            if mode == self.mode and not restart:
                return
            self.mode = mode
            me = self.app.ini("launcher.ini")
            me.set("pod", "mode", mode)
            me.save()
            self.screen.show("Ready", self.sub())
            self.set_state("switching", "")
            self.stop_bt3()
            self.stop_firestorm()
            if mode == "auto" and self.fs_live:
                self.start_firestorm()
            self.set_state("ready", self.waiting())
        self.changed.set()

    def waiting(self):
        return "out of service" if self.mode == "ready" else "" if self.fs_live else "waiting for the console"

    def fs_run(self, run, fresh=False):
        """The console's Firestorm started (run) or closed. Started:
        Firestorm's pod side up - afresh if a console has used this run of
        it already (its MW4 takes one console a run) - and a 3.0 cockpit
        left. Closed: no game, Ready."""
        with self.lock:
            self.fs_live = run
            if self.mode != "auto":
                return
            if run:
                if self.in_battle():
                    self.stop_bt3()
                if not self.firestorm_procs():
                    self.set_state("switching", "starting Firestorm")
                    self.start_firestorm()
                elif fresh and self.console_seen:
                    self.set_state("switching", "starting Firestorm afresh")
                    self.stop_firestorm()
                    self.start_firestorm()
            else:
                self.stop_firestorm()
                if not self.in_battle():        # a 3.0 battle already its game: left alone
                    self.screen.show("Ready", self.sub())
                    self.set_state("ready", self.waiting())
        self.changed.set()

    def start_firestorm(self):
        """The pod side, with the shim told to show Ready while it waits."""
        shim = self.app.ini("shim.ini")
        shim.set("ready", "text", "Ready")
        shim.set("ready", "sub", self.sub())
        shim.save()
        self.app.start("pod")
        self.fs_started = time.time()

    def join(self, host, seat, name=""):
        """This pod's cockpit, in the 3.0 battle the console has started."""
        with self.lock:
            if self.mode != "auto":
                raise ValueError("this pod is out of service")
            if not self.app.bt3.available:
                raise ValueError("BattleTech 3.0 is not installed on this pod")
            self.fs_live = False                # the console's Firestorm is closed by now
            self.stop_firestorm()
            self.stop_bt3()
            os.environ["BATTLEPOD_NO_HOLD"] = "1"           # the cockpit ends with its battle
            try:
                self.app.bt3.join({"game.join_host": host, "game.join_seat": str(seat)})
            finally:
                os.environ.pop("BATTLEPOD_NO_HOLD", None)
            self.joined = self.app.bt3.joined
            self.set_state("battle", "seat %d%s" % (seat, ", " + name if name else ""))
            self.joined_at = time.time()
            self.screen.hide()

    # ---- keeping it so
    def watch(self):
        """Every second: is what should be running running, and what is on screen."""
        fs_fail = 0
        while True:
            try:
                with self.lock:
                    battle = self.in_battle()
                    if self.settings_proc and self.settings_proc.poll() is None:
                        self.screen.hide()          # the launcher's window, for whoever is at the pod
                    elif battle:
                        self.screen.hide()          # the 3.0 cockpit, full screen
                    elif self.mode == "auto" and self.fs_live:
                        procs = self.firestorm_procs()
                        if not procs:
                            fs_fail += 1
                            if fs_fail >= 8:        # it fell over (or never came): again
                                self.set_state("restarting", "the game stopped; starting it again")
                                self.start_firestorm()
                                fs_fail = 0
                        else:
                            fs_fail = 0
                        idle, up = self.firestorm_screen()
                        if up != self.game_up:
                            self.game_up = up
                            self.changed.set()
                        if up:
                            self.screen.hide()   # the shim says Ready on the main view while it waits
                            self.set_state("battle" if not idle else "ready", "" if idle else "in a match")
                        else:
                            self.screen.show("Ready", self.sub())
                    else:
                        if self.firestorm_procs():  # left from a console that went without a word
                            self.stop_firestorm()
                        self.screen.show("Ready", self.sub())
                        self.set_state("ready", self.waiting())
            except Exception as e:                  # never this loop's end
                print("podagent: %s" % e, flush=True)
            time.sleep(1)

    def firestorm_screen(self):
        """(idle, up): whether the pod's main view is its waiting screen, and
        whether the pod's MW4 is drawing at all - from the shim's log."""
        log = self.app.path("shim-ctcl2.log")
        try:
            if os.path.getmtime(log) < getattr(self, "fs_started", 0) - 2:
                return True, False                  # a log from before this start
            with open(log, encoding="latin-1", errors="replace") as f:
                text = f.read()
        except OSError:
            return True, False
        seen = "WSAAccept" in text            # a console connected to this run's MW4
        if seen != self.console_seen:
            self.console_seen = seen
            self.changed.set()
        if not re.search(r"main\d+: first frame", text):
            return True, False
        said = re.findall(r"pod: (waiting|in a mission)", text)    # the shim's (ready_present)
        return not said or said[-1] == "waiting", True

    # ---- the network
    def beacon(self):
        """Ours every 2 s and at once on any change; the console's "who" answered."""
        out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        out.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        inp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        inp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        inp.bind(("", WHO_PORT))
        inp.settimeout(0.25)
        last = 0
        while True:
            now = time.time()
            if now - last >= 2 or self.changed.is_set():
                self.changed.clear()
                data = json.dumps(self.status()).encode()
                for dest in ("255.255.255.255", self.console):
                    if dest:
                        try:
                            out.sendto(data, (dest, BEACON_PORT))
                        except OSError:
                            pass
                last = now
            try:
                msg, (ip, port) = inp.recvfrom(2048)
                if lan(ip):
                    q = json.loads(msg)
                    if q.get("t") == "who":
                        self.learn_console(ip, q.get("port"))
                        if "fs" in q and bool(q["fs"]) != self.fs_live:     # a /firestorm missed
                            threading.Thread(target=self.fs_run, args=(bool(q["fs"]),), daemon=True).start()
                        out.sendto(json.dumps(self.status()).encode(), (ip, BEACON_PORT))
            except (socket.timeout, ValueError, OSError):
                pass

    def learn_console(self, ip, port):
        """The console's address, for the pilot roster and careers: kept, so
        nobody has to type it."""
        addr = ip + (":%d" % port if port else "")
        if self.console != ip:
            self.console = ip
            me = self.app.ini("launcher.ini")
            if me.get("launcher", "console", "") != addr:
                me.set("launcher", "console", addr)
                me.save()

    def open_settings(self):
        """The launcher's window over the Ready screen, for whoever is at the pod."""
        e = edge()
        if not e or (self.settings_proc and self.settings_proc.poll() is None):
            return
        self.screen.hide()
        prof = os.path.join(os.environ.get("LOCALAPPDATA", "."), "FirestormLauncher", "window")
        self.settings_proc = subprocess.Popen([e, "--app=" + self.app.url, "--user-data-dir=" + prof,
                                               "--start-maximized", "--no-first-run"])

        def back():
            self.settings_proc.wait()
            self.changed.set()
        threading.Thread(target=back, daemon=True).start()

    def power(self, action):
        flag = {"restart": "/r", "shutdown": "/s"}.get(action)
        if not flag:
            raise ValueError("restart or shutdown")
        self.stop_firestorm()
        self.stop_bt3()
        subprocess.Popen(["shutdown", flag, "/t", "3", "/c", "BattleTech: the console asked"], creationflags=0x08000000)

    def serve(self):
        agent = self

        class H(BaseHTTPRequestHandler):
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
                if not lan(self.client_address[0]):
                    return self.send(403, {"error": "forbidden"})
                if self.path == "/status":
                    return self.send(200, agent.status())
                return self.send(404, {"error": "not found"})

            def do_POST(self):
                if not lan(self.client_address[0]):
                    return self.send(403, {"error": "forbidden"})
                n = int(self.headers.get("Content-Length") or 0)
                try:
                    body = json.loads(self.rfile.read(min(n, 65536)) or b"{}")
                    p = self.path
                    if p == "/mode":
                        threading.Thread(target=agent.set_mode, args=(body.get("mode"),), daemon=True).start()
                    elif p == "/firestorm":
                        threading.Thread(target=agent.fs_run, args=(bool(body.get("run")), bool(body.get("fresh"))),
                                         daemon=True).start()
                    elif p == "/bt3/join":
                        agent.join(str(body.get("host") or self.client_address[0]), int(body.get("seat") or 2),
                                   str(body.get("name") or ""))
                    elif p == "/bt3/leave":
                        agent.stop_bt3()
                    elif p == "/identify":
                        agent.screen.identify()
                    elif p == "/settings":
                        agent.open_settings()
                    elif p == "/restart":
                        threading.Thread(target=agent.set_mode, args=(agent.mode, True), daemon=True).start()
                    elif p == "/power":
                        agent.power(body.get("action"))
                    else:
                        return self.send(404, {"error": "not found"})
                    agent.changed.set()
                    return self.send(200, dict(agent.status(), ok=True))
                except (ValueError, TypeError) as e:
                    return self.send(400, {"error": str(e)})

        srv = ThreadingHTTPServer(("0.0.0.0", AGENT_PORT), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()

    def run(self):
        """Start: the Ready screen, the network, and the mode it was last in."""
        self.screen.show("Ready", self.sub())
        self.serve()
        threading.Thread(target=self.beacon, daemon=True).start()
        threading.Thread(target=self.set_mode, args=(self.mode, True), daemon=True).start()
        threading.Thread(target=self.watch, daemon=True).start()


class PodDirectory:
    """On the console: every pod on the centre's network, from their beacons,
    and the way to tell them what to do. Asks "who" every few seconds, which
    also tells each pod where the console is."""

    def __init__(self, pod_port, fs_live=lambda: False):
        self.pod_port = pod_port        # where pods reach this console's launcher (roster, careers)
        self.fs_live = fs_live          # whether the Firestorm console is up, for the pods' Firestorm
        self.pods = {}                  # ip -> (status, last seen)
        self.lock = threading.Lock()
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        inp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        inp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            inp.bind(("", BEACON_PORT))
        except OSError as e:
            print("pods: cannot listen on UDP %d: %s" % (BEACON_PORT, e), flush=True)
            return
        inp.settimeout(0.5)
        out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        out.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        last = 0
        while True:
            if time.time() - last >= 4:
                self.who(out)
                last = time.time()
            try:
                msg, (ip, port) = inp.recvfrom(4096)
                st = json.loads(msg)
                if st.get("t") == "pod" and lan(ip):
                    with self.lock:
                        self.pods[ip] = (st, time.time())
            except (socket.timeout, ValueError, OSError):
                pass

    def who(self, out=None):
        s = out or socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            s.sendto(json.dumps({"t": "who", "port": self.pod_port, "fs": bool(self.fs_live())}).encode(),
                     ("255.255.255.255", WHO_PORT))
        except OSError:
            pass

    def list(self):
        now = time.time()
        with self.lock:
            items = list(self.pods.items())
        out = []
        for ip, (st, seen) in items:
            d = dict(st, ip=ip, age=round(now - seen, 1), online=now - seen < 7)
            out.append(d)
        out.sort(key=lambda d: (d.get("num") or 999, d.get("name") or "", d["ip"]))
        return out

    def forget(self, ip):
        with self.lock:
            self.pods.pop(ip, None)

    def command(self, ip, path, body=None):
        import urllib.request
        with self.lock:
            st = self.pods.get(ip, ({}, 0))[0]
        port = st.get("port") or AGENT_PORT
        req = urllib.request.Request("http://%s:%d%s" % (ip, port, path), method="POST",
                                     data=json.dumps(body or {}).encode(), headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=6) as r:
                st = json.loads(r.read())
        except urllib.error.HTTPError as e:
            raise ValueError(json.loads(e.read() or b"{}").get("error", str(e)))
        except OSError as e:
            raise ValueError("%s does not answer (%s)" % (ip, getattr(e, "reason", e)))
        with self.lock:
            if st.get("t") == "pod":
                self.pods[ip] = ({k: v for k, v in st.items() if k != "ok"}, time.time())
        return st
