#!/usr/bin/env python3
"""mechbust: a portrait of a 'Mech from the turning model on the radar.

When a pilot is LOADed but not launched, the pod's radar shows the 'Mech in
3D, turning, beside its statistics. For the 'Mechs the game has no portrait
of (hsh\\Mechs has none of Behemoth, Behemoth II, Longbow or Victor) this
seats each in turn as tools/mechcat.py does, LOADs it, snaps the radar
through a whole turn, and keeps the frame that faces front: cut to the
'Mech's head and shoulders and set on white, as the game's own portraits are.

The result goes in the game folder - never this repository - as
profiles\\portraits\\<chassis>.png, where the launcher looks after
hsh\\Mechs, and every frame of the turn in profiles\\mechcat\\bust\\ for
checking (--frame N picks another as the front).

usage: mechbust.py --game C:\\path\\to\\MW4 [--only Victor,Longbow] [--frames 48]
                   [--keep-frames] [--frame NAME=N]

Needs what mechcat.py needs: a centre on this PC (tools/start-centre.ps1) and
the game's windows visible; it clicks the console with the real mouse.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mechcat                                   # noqa: E402  (the centre, seating, snapping)
from mechcat import Centre, log, Ini, mech_table  # noqa: E402
from fslauncher import CHASSIS                    # noqa: E402
from PIL import Image, ImageChops, ImageOps        # noqa: E402

MISSING = ["Behemoth", "BehemothII", "Longbow", "Victor"]


def upright(radar_bmp):
    """The radar's picture as its screen stands: the device is drawn on its side."""
    return Image.open(radar_bmp).convert("RGB").rotate(270, expand=True)


def fresh(c):
    """The centre started with the seat assign.ini gives: the console puts the
    pilot in slot 1 itself now (src/ddraw.c), and mechcat's clicks into the
    slot would clear it. False if the console did not come up seated."""
    c.stop()
    for f in glob.glob(os.path.join(c.game, "shim-ctcl*.log")):
        try:
            os.remove(f)
        except OSError:
            pass
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", c.ps1, "-Game",
                    c.game, "-NoPrint"], capture_output=True, text=True, timeout=120)
    log1 = os.path.join(c.game, "shim-ctcl1.log")
    until = time.time() + 60
    while time.time() < until:
        try:
            text = open(log1, encoding="latin-1", errors="replace").read()
            if "main1: first frame" in text and "the console's slot 1" in text:
                time.sleep(2)
                return True
        except OSError:
            pass
        time.sleep(0.3)
    return False


def grab(hwnd):
    """The window's client area as it is now, through PrintWindow - drawn by
    the window itself, so another window over it does not matter."""
    import ctypes
    from ctypes import wintypes
    u, g = ctypes.windll.user32, ctypes.windll.gdi32
    r = wintypes.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    if w <= 0 or h <= 0:
        return None
    hdc = u.GetDC(hwnd)
    mdc = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(mdc, bmp)
    u.PrintWindow(hwnd, mdc, 3)                 # client area, full content
    buf = ctypes.create_string_buffer(w * h * 4)

    class BMI(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                    ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]
    bmi = BMI(ctypes.sizeof(BMI), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    g.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    u.ReleaseDC(hwnd, hdc)
    return Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")


def radar_window(c):
    """The pod's radar window: "Radar" in the pod's process (not the console's)."""
    import desk
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-CimInstance Win32_Process -Filter \"Name='MW4.exe'\" | "
                          "Where-Object CommandLine -match 'ctcltype 2').ProcessId"],
                         capture_output=True, text=True).stdout.split()
    pid = int(out[0]) if out else None
    for title in ("Radar", "radar", "Radar 1"):
        for w in desk.windows(title):
            if pid is None or w[5] == pid:
                return w[0]
    return None


def capture_window(c, seconds):
    """The radar window, as fast as it can be read, for `seconds`."""
    h = radar_window(c)
    if not h:
        log("no radar window found")
        return []
    out, until = [], time.time() + seconds
    while time.time() < until:
        im = grab(h)
        if im:
            out.append(im)
    return out


VIEW = (64, 64, 430, 428)           # the radar window's 'Mech panel, inside its frame


def asymmetry(im):
    """How far the 'Mech's silhouette is from its own mirror image, 0 = none."""
    m = im.convert("L").point(lambda v: 255 if v > 12 else 0)
    bb = m.getbbox()
    if not bb:
        return 9.0
    sub = m.crop(bb)
    d = ImageChops.difference(sub, sub.transpose(Image.FLIP_LEFT_RIGHT))
    return sum(d.histogram()[128:]) / max(1, sum(sub.histogram()[128:]))


def front(ims, early):
    """The front: the most symmetric of the first `early` frames - the first
    second after LOAD. The model turns in 15-degree steps, a turn in about
    2.3 s, and comes square-on to the front about half a second in and to
    the back a second later. The back is as symmetric (often more: the arms
    differ), so only the time can say which is which."""
    best = None
    for n, im in enumerate(ims[:early]):
        s = asymmetry(im.crop(VIEW))
        if best is None or s < best[0]:
            best = (s, n)
    return best[1] if best else None


def make_bust(im):
    """A portrait as the game's are: 800 x 250, white, the 'Mech from the top
    of its head to its waist, centred and filling the height."""
    v = im.crop(VIEW).convert("RGB")
    # The black round the 'Mech, flood-filled from the edges, becomes white;
    # dark parts of the 'Mech itself are not connected to it and stay.
    bg = v.convert("L").point(lambda x: 255 if x <= 10 else 0)
    from PIL import ImageDraw
    for corner in ((0, 0), (bg.width - 1, 0), (0, bg.height - 1), (bg.width - 1, bg.height - 1)):
        if bg.getpixel(corner) == 255:
            ImageDraw.floodfill(bg, corner, 128)
    mask = bg.point(lambda x: 255 if x == 128 else 0)
    white = Image.new("RGB", v.size, (255, 255, 255))
    v = Image.composite(white, v, mask)
    bb = ImageOps.invert(mask).getbbox()
    if not bb:
        return None
    x0, y0, x1, y1 = bb
    top = v.crop((x0, y0, x1, y0 + int((y1 - y0) * 0.48)))
    scale = min(250.0 / top.height, 760.0 / top.width)
    top = top.resize((max(1, int(top.width * scale)), max(1, int(top.height * scale))), Image.LANCZOS)
    out = Image.new("RGB", (800, 250), (255, 255, 255))
    out.paste(top, ((800 - top.width) // 2, 250 - top.height))
    return out


class Watch:
    """The radar window grabbed in the background, timed, from before LOAD -
    so the moment the 'Mech first appears is seen."""

    def __init__(self, c):
        import threading
        self.h = radar_window(c)
        self.frames = []
        self.go = bool(self.h)
        self.t = threading.Thread(target=self.run, daemon=True)
        if self.go:
            self.t.start()

    def run(self):
        while self.go:
            im = grab(self.h)
            if im:
                self.frames.append((time.time(), im))

    def stop(self):
        self.go = False
        if self.t.is_alive():
            self.t.join(2)
        return self.frames


def shows_mech(im):
    """The panel has a 'Mech in it (not the black of before LOAD)."""
    m = im.crop(VIEW).convert("L").point(lambda v: 255 if v > 12 else 0)
    return sum(m.histogram()[128:]) > 400


def turn(frames):
    """[(seconds since the 'Mech appeared, image)], one per pose: the frames
    from its first appearance on, repeats dropped."""
    t0 = next((t for t, im in frames if shows_mech(im)), None)
    if t0 is None:
        return []
    out, prev = [], None
    for t, im in frames:
        if t < t0:
            continue
        g = im.crop(VIEW).convert("L")
        if prev is not None and ImageChops.difference(g, prev).getbbox() is None:
            continue
        prev = g
        out.append((t - t0, im))
    return out


def front_of(poses):
    """(index, period): the front. The model appears facing away and turns at
    a steady rate - Atlas and Victor alike, first pose again 2.33 s later -
    so the front is half a turn from the first pose: of the poses near that
    time, the most symmetric. The period is measured, as the time until the
    first pose comes round again."""
    first = poses[0][1].crop(VIEW).convert("L")
    count, period = None, 2.33
    for k in range(3, len(poses)):
        if ImageChops.difference(poses[k][1].crop(VIEW).convert("L"), first).getbbox() is None:
            count, period = k, poses[k][0]
            break
    # The first pose is square-on to the back, and the turn is `count` poses:
    # the front is half that many on. With an odd count it falls between two,
    # and the more symmetric of those is taken - only those: symmetry alone
    # is fooled by a 'Mech built lopsided (the Atlas's hip launcher).
    if count:
        near = sorted({count // 2, (count + 1) // 2})
    else:
        mid = min(range(len(poses)), key=lambda k: abs(poses[k][0] - period / 2))
        near = [mid]
    near = [k for k in near if 0 <= k < len(poses)]
    n = min(near, key=lambda k: asymmetry(poses[k][1].crop(VIEW)))
    return n, period


def capture(c, frames, gap):
    """`frames` snapshots of the radar, `gap` seconds apart, upright."""
    out = []
    for _ in range(frames):
        t = time.time()
        shot = c.snap(["radar"])
        if "radar" in shot:
            try:
                out.append(upright(shot["radar"]))
            except OSError:
                pass
        time.sleep(max(0.0, gap - (time.time() - t)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--only", default=",".join(MISSING), help="chassis, comma-separated")
    ap.add_argument("--frames", type=int, default=48)
    ap.add_argument("--gap", type=float, default=0.25, help="seconds between snapshots")
    ap.add_argument("--seconds", type=float, default=3.0, help="how long to watch once the 'Mech appears")
    ap.add_argument("--delay", type=float, default=0.0, help="seconds after LOAD before watching")
    ap.add_argument("--keep-frames", action="store_true", help="keep every frame, not only the front one")
    a = ap.parse_args()

    game = os.path.abspath(a.game)
    prof = os.path.join(game, "profiles")
    bust = os.path.join(prof, "mechcat", "bust")
    os.makedirs(bust, exist_ok=True)
    table = mech_table(game)
    want = [s.strip() for s in a.only.split(",") if s.strip()]
    todo = []
    for name in want:
        for i, (ch, v) in enumerate(table):
            if ch == name and not v:            # the chassis' stock 'Mech
                todo.append((i + 1, ch))
                break
        else:
            log("%s: not in the console's list" % name)

    ctcl = Ini(os.path.join(game, "ctcl.ini"))
    pod_ip = [k for k, v in ctcl.items("teslas") if not k.startswith(("#", "*"))][0]
    assign = os.path.join(prof, "assign.ini")
    keep = open(assign, encoding="latin-1").read() if os.path.exists(assign) else None
    c = Centre(game)
    try:
        for idx, chassis in todo:
            with open(assign, "w", encoding="latin-1") as f:
                f.write("[%s]\nname=Catalog\nmech=%s\nmech_index=%d\nchassis_pos=%d\n"
                        % (pod_ip, chassis, idx, sorted(CHASSIS, key=str.lower).index(chassis) + 1))
            poses = None
            for attempt in range(2):
                if not fresh(c):
                    log("%s: the centre did not start (try %d)" % (chassis, attempt + 1))
                    continue
                w = Watch(c)                    # from before LOAD, to see the 'Mech appear
                time.sleep(0.5)
                c.click_console(672, 580)       # LOAD
                until = time.time() + 30
                while time.time() < until and not any(shows_mech(im) for t, im in w.frames[-5:]):
                    time.sleep(0.2)
                time.sleep(a.seconds)           # a turn and more
                poses = turn(w.stop())
                if poses:
                    break
                log("%s: the 'Mech did not appear (try %d)" % (chassis, attempt + 1))
            if not poses:
                log("%s: skipped" % chassis)
                continue
            safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", chassis)
            for f in glob.glob(os.path.join(bust, safe + "-*.png")):
                os.remove(f)
            n, period = front_of(poses)
            for k, (t, im) in enumerate(poses):
                if a.keep_frames or k == n:
                    im.save(os.path.join(bust, "%s-%02d.png" % (safe, k)))
            pic = make_bust(poses[n][1])
            if pic:
                os.makedirs(os.path.join(prof, "portraits"), exist_ok=True)
                pic.save(os.path.join(prof, "portraits", chassis + ".png"))
            log("%s: %d poses a turn of %.2f s; pose %d (%.2f s) faces front -> profiles\\portraits\\%s.png"
                % (chassis, len(poses), period, n, poses[n][0], chassis))
    finally:
        if keep is None:
            if os.path.exists(assign):
                os.remove(assign)
        else:
            with open(assign, "w", encoding="latin-1") as f:
                f.write(keep)
        c.stop()


if __name__ == "__main__":
    main()
