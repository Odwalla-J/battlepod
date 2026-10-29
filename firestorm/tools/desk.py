"""Drive the desktop for start-centre.ps1 and tests: click, type with hardware
scan codes (GameOS reads DirectInput, which sees scan codes, not virtual
keys), bring a window forward.

  desk.py click X Y [X Y ...]       left click at screen points
  desk.py type TEXT                 type with scan codes
  desk.py key NAME                  one key: enter, tab, esc, f1.., space
  desk.py front TITLE [INDEX]       bring the INDEX'th window with this title forward
  desk.py wins TITLE                list windows with this title: hwnd, rect, pid
"""
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)

INPUT_MOUSE, INPUT_KEYBOARD = 0, 1
KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE = 0x2, 0x8
MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x2, 0x4


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def send(inp):
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def scan(code, up=False, ext=False):
    i = INPUT(type=INPUT_KEYBOARD)
    i.u.ki = KEYBDINPUT(0, code, KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0) | (1 if ext else 0), 0, 0)
    send(i)


SC = {c: s for s, c in zip(
    [0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
     0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C], "abcdefghijklmnopqrstuvwxyz")}
SC.update({str(d): 0x02 + (d - 1) if d else 0x0B for d in range(10)})
SC.update({" ": 0x39, "-": 0x0C, ".": 0x34, "_": 0x0C})
NAMED = {"enter": 0x1C, "tab": 0x0F, "esc": 0x01, "space": 0x39, "backspace": 0x0E,
         **{"f%d" % i: 0x3A + i for i in range(1, 11)}, "f11": 0x57, "f12": 0x58}
LSHIFT = 0x2A


def type_text(t):
    for ch in t:
        shift = ch.isupper() or ch == "_"
        code = SC.get(ch.lower())
        if code is None:
            continue
        if shift:
            scan(LSHIFT)
        scan(code)
        time.sleep(0.03)
        scan(code, up=True)
        if shift:
            scan(LSHIFT, up=True)
        time.sleep(0.05)


def click(x, y):
    user32.SetCursorPos(x, y)
    time.sleep(0.08)
    for f in (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP):
        i = INPUT(type=INPUT_MOUSE)
        i.u.mi = MOUSEINPUT(0, 0, 0, f, 0, 0)
        send(i)
        time.sleep(0.06)
    time.sleep(0.25)


def windows(title):
    found = []
    EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value == title and user32.IsWindowVisible(h):
            r = wintypes.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            found.append((h, r.left, r.top, r.right, r.bottom, pid.value))
        return True
    user32.EnumWindows(EnumProc(cb), 0)
    return found


def front(h):
    # Windows refuses SetForegroundWindow from a background process unless an
    # Alt press is in flight; this is the usual way round it.
    scan(0x38)
    user32.SetForegroundWindow(h)
    scan(0x38, up=True)
    time.sleep(0.3)


def main(a):
    if a[0] == "click":
        pts = list(map(int, a[1:]))
        for i in range(0, len(pts), 2):
            click(pts[i], pts[i + 1])
    elif a[0] == "type":
        type_text(" ".join(a[1:]))
    elif a[0] == "key":
        k = NAMED[a[1].lower()]
        scan(k)
        time.sleep(0.05)
        scan(k, up=True)
    elif a[0] == "wins":
        for w in windows(a[1]):
            print(*w)
    elif a[0] == "front":
        w = windows(a[1])
        idx = int(a[2]) if len(a) > 2 else 0
        if len(a) > 3 and a[3] == "x":          # front TITLE X x: the one whose left edge is X
            w = [x for x in w if x[1] == idx]
            idx = 0
        if w:
            front(w[idx][0])
            print("front", w[idx])


if __name__ == "__main__":
    main(sys.argv[1:])
