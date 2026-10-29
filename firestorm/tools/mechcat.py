#!/usr/bin/env python3
"""mechcat: catalogue every 'Mech and variant by letting the game show them.

The variant files are packed in the engine's own format; nothing reads them
but the game. So this seats each 'Mech in turn in the pod on this PC (as the
launcher's pilot profiles do: profiles\\assign.ini, rewritten into the
console's setup by the shim), LOADs it, reads its statistics off the radar's
'Mech view, LAUNCHes, reads its weapons off MFD 5, and starts the pod afresh
for the next. Reading is Windows' own text recognition (Windows.Media.Ocr).

The result is profiles\\mechs.json in the game folder - never in this
repository - with, per 'Mech, its statistics, weapons and ammunition, and the
crops it was read from (profiles\\mechcat\\) for checking. It resumes where it
stopped; --only NAME does one 'Mech, --redo reads again what is there.

usage: mechcat.py --game C:\\path\\to\\MW4 [--only "Loki B"] [--limit N] [--redo]

Needs a centre on this PC (tools/start-centre.ps1) set up as in the README
(one pod, this PC's address, first in ctcl.ini [teslas]), Python with Pillow.
Takes about 30 s a 'Mech; the game's windows must stay visible.
"""
import argparse
import glob
import json
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "launcher"))
sys.path.insert(0, HERE)
from fslauncher import Ini, mech_table          # noqa: E402  (the console's 'Mech list)
import desk                                      # noqa: E402
from PIL import Image                            # noqa: E402

CTL = ("127.0.0.1", 9100)
OCR_PS = r'''
param([string]$Img)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$t) { $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Img)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$dec = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$bmp = Await ($dec.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
$res = Await ($engine.RecognizeAsync($bmp)) ([Windows.Media.Ocr.OcrResult])
$n = 0
foreach ($l in $res.Lines) { $n++; foreach ($w in $l.Words) { "{0}`t{1:F0}`t{2:F0}`t{3}" -f $n, $w.BoundingRect.X, $w.BoundingRect.Y, $w.Text } }
$stream.Dispose()
'''


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Centre:
    def __init__(self, game):
        self.game = game
        self.pod_log = os.path.join(game, "shim-ctcl2.log")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ps1 = os.path.join(HERE, "start-centre.ps1")
        self.ocr_script = os.path.join(game, "profiles", "mechcat", "ocr.ps1")
        os.makedirs(os.path.dirname(self.ocr_script), exist_ok=True)
        with open(self.ocr_script, "w", encoding="utf-8") as f:
            f.write(OCR_PS)

    # ---- the pod's log, the console's window
    def lines(self, pat):
        try:
            with open(self.pod_log, encoding="latin-1", errors="replace") as f:
                return sum(1 for l in f if re.search(pat, l))
        except OSError:
            return 0

    def wait_for(self, pat, more, seconds):
        until = time.time() + seconds
        while time.time() < until:
            if self.lines(pat) > more:
                return True
            time.sleep(0.25)
        return False

    def console(self):
        out = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "(Get-CimInstance Win32_Process -Filter \"Name='MW4.exe'\" | "
                              "Where-Object CommandLine -match 'ctcltype 1').ProcessId"],
                             capture_output=True, text=True).stdout.split()
        pid = int(out[0]) if out else None
        for w in desk.windows("BattleTech Firestorm"):
            if w[5] == pid:
                return w
        return None

    def click_console(self, x, y):
        w = self.console()
        if not w:
            raise RuntimeError("the console's window is gone")
        desk.front(w[0])
        desk.click(w[1] + x, w[2] + y)

    # ---- a fresh centre for each 'Mech: the console has no abort, and a
    # console that saw a match start will not LOAD again until it ends.
    def fresh(self, pilot="Catalog"):
        """Pod launcher and console started (start-centre.ps1, no clicks), the
        pod put in slot 1 - ready for LOAD. False if they did not come up."""
        self.stop()
        for f in glob.glob(os.path.join(self.game, "shim-ctcl*.log")):
            try:
                os.remove(f)
            except OSError:
                pass
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", self.ps1, "-Game",
                        self.game, "-NoPrint"], capture_output=True, text=True, timeout=120)
        con_log = os.path.join(self.game, "shim-ctcl1.log")
        until = time.time() + 40
        while time.time() < until:
            try:
                if "main1: first frame" in open(con_log, encoding="latin-1", errors="replace").read():
                    break
            except OSError:
                pass
            time.sleep(0.25)
        else:
            return False
        time.sleep(2.5)
        for x, y in ((181, 118), (90, 159), (120, 143)):   # slot 1's list, the pod, the name box
            self.click_console(x, y)
            time.sleep(0.6)
        desk.type_text(pilot)
        desk.scan(0x1C); time.sleep(0.05); desk.scan(0x1C, up=True)
        time.sleep(0.6)
        return True

    def stop(self):
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", self.ps1, "-Game",
                        self.game, "-Stop"], capture_output=True, timeout=60)

    # ---- reading
    def snap(self, kinds):
        for f in glob.glob(os.path.join(self.game, "snap-*.bmp")):
            try:
                os.remove(f)
            except OSError:
                pass
        self.sock.sendto(b"snap", CTL)
        until = time.time() + 5
        got = {}
        while time.time() < until and len(got) < len(kinds):
            for k in kinds:
                fs = sorted(glob.glob(os.path.join(self.game, "snap-ctcl2-%s*.bmp" % k)))
                if fs and k not in got:
                    time.sleep(0.1)
                    got[k] = fs[-1]
            time.sleep(0.05)
        return got

    def ocr_words(self, png):
        """[(line, x, y, word)] as Windows' OCR read them."""
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", self.ocr_script, png],
                           capture_output=True, text=True, timeout=60)
        out = []
        for l in r.stdout.splitlines():
            p = l.split("\t", 3)
            if len(p) == 4:
                out.append((int(p[0]), float(p[1]), float(p[2]), p[3].strip()))
        return out

    def ocr(self, png, row_h=None):
        """The text: a list of OCR's own lines; or, for an image made of rows
        row_h pixels apart, {row: text} with each word put in its row."""
        lines = {}
        for line, x, y, w in self.ocr_words(png):
            lines.setdefault(int(y // row_h) if row_h else line, []).append((x, w))
        text = {k: " ".join(w for x, w in sorted(v)) for k, v in sorted(lines.items())}
        return text if row_h else list(text.values())


# ---- what the screens say
STATS = [("tech", r"tech"), ("class", r"class"), ("accel", r"accel"), ("decel", r"decel"),
         ("turn_rate", r"turn"), ("torso_range", r"torso"), ("twist_speed", r"twist"),
         ("firepower", r"fire\s*power"), ("armor", r"armou?r"), ("speed", r"^speed"),
         ("heat_efficiency", r"heat"), ("tonnage", r"tonn")]
CLASSES = {"light": "Light", "medium": "Medium", "heavy": "Heavy", "asault": "Assault", "assault": "Assault"}


def number(s):
    s = s.replace(",", ".").replace("O", "0").replace("o", "0")
    s = re.sub(r"(\d)\s+(\d)", r"\1.\2", s)            # "0 44" was 0.44
    m = re.search(r"-?\d+(\.\d+)?", s)
    return float(m.group()) if m else None


def parse_stats(lines):
    out = {}
    for l in lines:
        if ":" not in l:
            continue
        k, v = l.split(":", 1)
        for name, pat in STATS:
            if name not in out and re.search(pat, k.strip(), re.I):
                if name == "tech":
                    out[name] = "Clan" if "clan" in v.lower() else "Inner Sphere" if "inner" in v.lower() else v.strip()
                elif name == "class":
                    out[name] = CLASSES.get(v.strip().lower(), v.strip())
                else:
                    out[name] = number(v)
                break
    return out


def weapon_name(s):
    """OCR reads the 1 and 0 of AC10 as I and O: after a weapon type a size is
    digits, so they are put back."""
    return re.sub(r"(AC|LRM|SRM|MRM|ATM|STREAK|SSRM|PPC|LBX|LB)([0-9IOLS]+)(?=REAR|\b)",
                  lambda m: m.group(1) + m.group(2).replace("I", "1").replace("L", "1").replace("O", "0").replace("S", "5"),
                  s)


def parse_weapons(rows, nrows):
    """rows: {row number: text} - the name, then the ammunition if any."""
    out = []
    for i in range(nrows):
        t = rows.get(i, "").strip()
        m = re.match(r"^(.*?)(?:\s+(\d+))?\s*-?$", t)
        name = re.sub(r"[^A-Z0-9]", "", (m.group(1) if m else t).upper())
        out.append({"name": weapon_name(name) or "?", "ammo": int(m.group(2)) if m and m.group(2) else None})
    return out


def stats_image(radar_bmp, png):
    """The 'Mech view's statistics panel, turned upright, doubled."""
    im = Image.open(radar_bmp).convert("RGB")
    c = im.crop((465, 58, 632, 424)).rotate(270, expand=True)
    c.resize((c.width * 2, c.height * 2)).save(png)


def weapons_image(mfd_bmp, png):
    """MFD 5 (the right head's green): the weapon table, one row per line - the
    name and ammunition columns side by side, so they read as one line."""
    g = Image.open(mfd_bmp).convert("RGB").split()[1].crop((640, 0, 1280, 480))
    rows = []
    y = 76                                              # the first row; one every 25 pixels
    while y + 25 <= 470:
        name = g.crop((166, y + 1, 555, y + 24))
        px = name.load()
        inked = sum(1 for yy in range(name.height) if any(px[xx, yy] > 100 for xx in range(0, name.width, 2)))
        if inked < 6:                                   # blank, or only the table's bottom border
            break
        rows.append((name, g.crop((560, y + 1, 630, y + 24))))
        y += 25
    if not rows:
        return 0
    # Light text on a grey bar becomes black on white, padded, so the last
    # letter of a name is not lost to the bar's edge.
    out = Image.new("L", (30 + 390 + 80 + 70 + 30, ROW_H * len(rows)), 255)
    ink = lambda im: im.point(lambda v: 0 if v > 150 else 255)
    for i, (n, a) in enumerate(rows):
        out.paste(ink(n), (30, ROW_H * i + 8))
        out.paste(ink(a), (30 + 390 + 80, ROW_H * i + 8))
    out = out.resize((out.width * 2, out.height * 2), Image.LANCZOS)
    out.save(png)
    return len(rows)


ROW_H = 40          # the weapons image's row pitch, before it is doubled


def pretty_weapon(n):
    """CLANULTRAAC5 -> Clan Ultra AC/5 (the launcher does the same in app.js)."""
    n = weapon_name(n)
    rear = n.endswith("REAR")                   # mounted facing back
    n = n[:-4] if rear else n
    clan = n.startswith("CLAN")
    s = n[4:] if clan else n
    named = {"MACHINEGUN": "Machine Gun", "GAUSSRIFLE": "Gauss Rifle", "LIGHTGAUSSRIFLE": "Light Gauss Rifle",
             "FLAMER": "Flamer", "PPC": "PPC", "ERPPC": "ER PPC", "NARC": "NARC Beacon", "NARCBEACON": "NARC Beacon",
             "TAG": "TAG", "AMS": "Anti-Missile System", "LONGTOM": "Long Tom", "HEAVYGAUSSRIFLE": "Heavy Gauss Rifle",
             "THUNDERBOLT": "Thunderbolt"}
    if s in named:
        s = named[s]
    else:
        for pat, rep in ((r"^ULTRAAC(\d+)$", r"Ultra AC/\1"), (r"^LBXAC(\d+)$", r"LB \1-X AC"),
                         (r"^RAC(\d+)$", r"Rotary AC/\1"), (r"^AC(\d+)$", r"AC/\1"),
                         (r"^(?:SSRM|STREAKSRM)(\d+)$", r"Streak SRM \1"), (r"^(LRM|SRM|MRM|ATM)(\d+)$", r"\1 \2")):
            s = re.sub(pat, rep, s)
        s = re.sub(r"^ER(?=[A-Z])", "ER ", s)
        s = re.sub(r"(LARGE|MEDIUM|SMALL)?(PULSE)?LASER$",
                   lambda m: " ".join(x for x in ((m.group(1) or "").title(), "Pulse" if m.group(2) else "", "Laser") if x), s)
    return ("Clan " if clan else "") + s + (" (rear)" if rear else "")


def report(game, cat):
    """profiles\\MECHS.md: every 'Mech and variant, as the game showed them."""
    lines = ["# BattleTech: Firestorm - 'Mechs and variants", "",
             "Read off the game's own screens by `tools/mechcat.py` (the radar's 'Mech view and MFD 5's "
             "weapon table), %s. Ratings (firepower, armour, speed, heat efficiency) are the game's own "
             "0-100 figures; speeds in m/s, turn and twist in degrees per second." % time.strftime("%Y-%m-%d"), ""]
    for v in cat.values():                     # names read before a correction existed
        for w in v.get("weapons", []):
            w["name"] = weapon_name(w["name"])
    with open(os.path.join(game, "profiles", "mechs.json"), "w", encoding="utf-8") as f:
        json.dump(cat, f, indent=1)
    by = {}
    for key, v in cat.items():
        by.setdefault(v["chassis"], []).append((v["index"], key, v))
    for chassis in sorted(by, key=str.lower):
        rows = sorted(by[chassis])
        st0 = rows[0][2].get("stats", {})
        lines += ["## %s" % chassis, "",
                  "%s t, %s, %s." % (st0.get("tonnage", "?"), st0.get("class", "?"), st0.get("tech", "?")), "",
                  "| Variant | Firepower | Armor | Speed | Heat eff. | Weapons |", "|---|---|---|---|---|---|"]
        for _, key, v in rows:
            st = v.get("stats", {})
            counts = []
            for w in v.get("weapons", []):
                for c in counts:
                    if c[0] == w["name"]:
                        c[1] += 1
                        break
                else:
                    counts.append([w["name"], 1])
            weapons = ", ".join("%dx %s" % (n, pretty_weapon(w)) for w, n in counts) or "(none read)"
            name = (v["variant"][len(chassis):].strip() if v["variant"] else "Stock")
            fmt = lambda x: "" if x is None else ("%g" % x if isinstance(x, float) else str(x))
            lines.append("| %s | %s | %s | %s | %s | %s |" % (name, fmt(st.get("firepower")), fmt(st.get("armor")),
                                                            fmt(st.get("speed")), fmt(st.get("heat_efficiency")), weapons))
        lines.append("")
    path = os.path.join(game, "profiles", "MECHS.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    log("wrote %s (%d 'Mechs)" % (path, len(cat)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--report", action="store_true", help="only write profiles\\MECHS.md from what has been read")
    ap.add_argument("--game", required=True)
    ap.add_argument("--only", help="one 'Mech, as the launcher names it (e.g. \"Loki B\" or \"Loki\")")
    ap.add_argument("--limit", type=int, default=0, help="at most this many this run")
    ap.add_argument("--redo", action="store_true", help="read again what is already in the catalogue")
    a = ap.parse_args()

    game = os.path.abspath(a.game)
    prof = os.path.join(game, "profiles")
    crops = os.path.join(prof, "mechcat")
    os.makedirs(crops, exist_ok=True)
    cat_path = os.path.join(prof, "mechs.json")
    cat = json.load(open(cat_path, encoding="utf-8")) if os.path.exists(cat_path) else {}
    if a.report:
        report(game, cat)
        return
    table = mech_table(game)
    todo = [(i + 1, c, v) for i, (c, v) in enumerate(table)
            if (a.redo or (v or c) not in cat) and (not a.only or (v or c) == a.only)]
    if a.limit:
        todo = todo[:a.limit]
    log("%d of %d to read" % (len(todo), len(table)))
    if not todo:
        return

    ctcl = Ini(os.path.join(game, "ctcl.ini"))
    tes = [k for k, v in ctcl.items("teslas") if not k.startswith(("#", "*"))]
    pod_ip = tes[0]
    assign = os.path.join(prof, "assign.ini")
    keep = open(assign, encoding="latin-1").read() if os.path.exists(assign) else None

    c = Centre(game)
    try:
        for idx, chassis, variant in todo:
            key = variant or chassis
            t0 = time.time()
            with open(assign, "w", encoding="latin-1") as f:
                f.write("[%s]\nname=Catalog\nmech=%s\nmech_index=%d\n" % (pod_ip, key, idx))
            loaded = False
            for attempt in range(2):
                if not c.fresh():
                    log("%s: the centre did not start (try %d)" % (key, attempt + 1))
                    continue
                lamps = c.lines(r"RIO from game: 84")
                c.click_console(672, 580)               # LOAD
                if c.wait_for(r"RIO from game: 84", lamps, 25):
                    loaded = True
                    break
                log("%s: LOAD did not take (try %d)" % (key, attempt + 1))
            if not loaded:
                log("%s: skipped" % key)
                continue
            time.sleep(2.5)                             # the 'Mech view is drawn
            safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", key)
            spng = os.path.join(crops, safe + "-stats.png")
            shot = c.snap(["radar"])
            if "radar" in shot:
                stats_image(shot["radar"], spng)
            missions = c.lines(r"mfd\d+: first frame")
            c.click_console(672, 580)                   # LAUNCH
            ok = c.wait_for(r"mfd\d+: first frame", missions, 30)
            time.sleep(3)                               # the weapon table is filled
            wpng = os.path.join(crops, safe + "-weapons.png")
            nrows = 0
            if ok:
                shot = c.snap(["mfd"])
                if "mfd" in shot:
                    nrows = weapons_image(shot["mfd"], wpng)
            stats = parse_stats(c.ocr(spng)) if os.path.exists(spng) else {}
            weapons = parse_weapons(c.ocr(wpng, ROW_H * 2), nrows) if nrows else []
            cat[key] = {"chassis": chassis, "variant": variant, "index": idx, "stats": stats,
                        "weapons": weapons, "rows_seen": nrows, "read": time.strftime("%Y-%m-%d")}
            with open(cat_path + ".tmp", "w", encoding="utf-8") as f:
                json.dump(cat, f, indent=1)
            os.replace(cat_path + ".tmp", cat_path)
            log("%-28s %4.1fs  %s t, %s, %d weapons%s" % (key, time.time() - t0, stats.get("tonnage"),
                stats.get("class"), len(weapons), "" if len(weapons) == nrows else " (rows %d)" % nrows))
    finally:
        if keep is None:
            os.remove(assign)
        else:
            with open(assign, "w", encoding="latin-1") as f:
                f.write(keep)
        c.stop()


if __name__ == "__main__":
    main()
