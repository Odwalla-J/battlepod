#!/usr/bin/env python3
"""Lay out a copy of Firestorm that runs by double-clicking Firestorm.exe.

    DEST\\Firestorm.exe   starts the launcher (src/firestorm.c), with the game's vwe.ico
    DEST\\python\\         an embeddable Python from python.org, nothing installed
    DEST\\launcher\\       fslauncher.py and its page, and the MFD bridge
    DEST\\tools\\          start-centre.ps1 and desk.py, for the launcher's test match
    DEST\\game\\           a copy of --game with this build's ddraw.dll and fsrun.exe
    DEST\\BattleTech 3.0.exe the same starter, opening the launcher on BattleTech 3.0
    DEST\\battletech\\     BattleTech 3.0 on battlepod's emulator: cockpit.exe and
                         battlepod.exe, the centre (tools\\hub.py ...), and a copy
                         of the release's Game Files (--bt3-files, or the one the
                         launcher finds) - if battlepod is built

The copy is for yourself: it carries your game files. Everything in it is
found relative to Firestorm.exe, so the folder can be moved or copied to
another PC (the launcher puts ctcl.ini right on its first start there).

Run again, it refreshes the programs - Firestorm.exe, the shim, the launcher,
the tools - and leaves game\\ alone, settings, pilots and all; --fresh-game
copies the game over again (your settings in it are lost).

usage: package.py --game DIR --dest DIR [--fresh-game] [--bt3-files DIR]
(make package GAME=... DEST=... builds first and runs this)
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")
PYTHON = "3.13.9"
PY_URL = "https://www.python.org/ftp/python/%s/python-%s-embed-amd64.zip" % (PYTHON, PYTHON)

# Left behind in a working copy by running it: logs, display snapshots, backups.
JUNK_PREFIX = ("shim-", "snap-")
JUNK_SUFFIX = (".log", ".bak", ".out")


def junk(names):
    return [n for n in names if n.lower().startswith(JUNK_PREFIX) or n.lower().endswith(JUNK_SUFFIX)]


def embeddable_python(dest):
    if os.path.exists(os.path.join(dest, "pythonw.exe")):
        return
    cache = os.path.join(BUILD, "python-embed", os.path.basename(PY_URL))
    if not os.path.exists(cache):
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        print("downloading %s" % PY_URL, flush=True)
        with urllib.request.urlopen(PY_URL) as r, open(cache + ".part", "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(cache + ".part", cache)
    with open(cache, "rb") as f:
        print("python %s, sha256 %s" % (PYTHON, hashlib.sha256(f.read()).hexdigest()))
    with zipfile.ZipFile(cache) as z:
        z.extractall(dest)


def set_icon(exe, ico):
    """Give exe the icon in ico (the game's own vwe.ico), as Explorer shows it."""
    import ctypes
    import struct
    from ctypes import wintypes
    data = open(ico, "rb").read()
    reserved, kind, n = struct.unpack_from("<HHH", data, 0)
    if reserved or kind != 1 or not n:
        return False
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    k.BeginUpdateResourceW.restype = wintypes.HANDLE
    k.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD,
                                  ctypes.c_char_p, wintypes.DWORD]
    k.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    h = k.BeginUpdateResourceW(exe, False)
    if not h:
        return False
    group = struct.pack("<HHH", 0, 1, n)
    for i in range(n):          # each image as an RT_ICON (3), listed in one RT_GROUP_ICON (14)
        w, ht, colours, res, planes, bits, size, off = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        k.UpdateResourceW(h, 3, i + 1, 0, data[off:off + size], size)
        group += struct.pack("<BBBBHHIH", w, ht, colours, res, planes, bits, size, i + 1)
    k.UpdateResourceW(h, 14, 1, 0, group, len(group))
    return bool(k.EndUpdateResourceW(h, False))


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def battletech(dest, files, fresh):
    """BattleTech 3.0 beside Firestorm: battlepod's emulator (the repository
    round this one: make && make cockpit), its centre, and the release's
    Game Files - 6 MB, the person's own, as game\\ is."""
    repo = os.path.dirname(ROOT)
    build = os.path.join(repo, "build")
    exes = [os.path.join(build, f) for f in ("cockpit.exe", "battlepod.exe")]
    if not all(os.path.exists(f) for f in exes):
        print("no BattleTech 3.0: battlepod is not built (make && make cockpit in %s)" % repo)
        return
    sys.path.insert(0, os.path.join(ROOT, "launcher"))
    import bt3
    if not files:
        files = bt3.BT3(os.path.dirname(repo)).game_files()
    bt = os.path.join(dest, "battletech")
    os.makedirs(os.path.join(bt, "tools"), exist_ok=True)
    for f in exes:
        copy(f, os.path.join(bt, os.path.basename(f)))
    for f in ("hub.py", "mapsend.py", "console_view.py"):
        copy(os.path.join(repo, "tools", f), os.path.join(bt, "tools", f))
    target = os.path.join(bt, "Game Files")
    if fresh and os.path.exists(target):
        shutil.rmtree(target)
    if os.path.exists(target):
        print("battletech\\Game Files is already there: keeping it")
    elif bt3.BT3.is_game_files(files):
        print("copying BattleTech 3.0's Game Files from %s" % files, flush=True)
        shutil.copytree(files, target)
    else:
        print("BattleTech 3.0 without its release: set its Game Files folder in the launcher")
    # The settings, as this PC has them, the first time; the copy's own after.
    ini = os.path.join(bt, "battletech.ini")
    mine = os.path.join(build, "battletech.ini")
    if not os.path.exists(ini) and os.path.exists(mine):
        lines = [l for l in open(mine, encoding="utf-8").read().splitlines() if not l.lower().startswith("files=")]
        with open(ini, "w", encoding="utf-8", newline="\r\n") as f:
            f.write("\n".join(lines) + "\n")
    starter = os.path.join(dest, "BattleTech 3.0.exe")
    copy(os.path.join(dest, "Firestorm.exe"), starter)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True, help="the Firestorm folder to copy (MW4.exe)")
    ap.add_argument("--dest", required=True, help="where the copy goes")
    ap.add_argument("--fresh-game", action="store_true", help="copy the game again over any already there")
    ap.add_argument("--bt3-files", default="", help="BattleTech 3.0: the release's Game Files folder")
    a = ap.parse_args()

    game, dest = os.path.abspath(a.game), os.path.abspath(a.dest)
    if not os.path.exists(os.path.join(game, "MW4.exe")):
        sys.exit("package: no MW4.exe in %s" % game)
    if os.path.normcase(dest).startswith(os.path.normcase(game) + os.sep):
        sys.exit("package: %s is inside the game folder" % dest)
    for f in ("ddraw.dll", "fsrun.exe", "Firestorm.exe"):
        if not os.path.exists(os.path.join(BUILD, f)):
            sys.exit("package: build/%s is not built; run make package" % f)

    target = os.path.join(dest, "game")
    if a.fresh_game and os.path.exists(target):
        shutil.rmtree(target)
    if not os.path.exists(target):
        print("copying %s" % game, flush=True)
        shutil.copytree(game, target, ignore=lambda d, names: junk(names) if d == game else [])
    else:
        print("game\\ is already there: keeping it (--fresh-game copies it again)")

    for f in ("ddraw.dll", "fsrun.exe"):
        copy(os.path.join(BUILD, f), os.path.join(target, f))
    copy(os.path.join(BUILD, "Firestorm.exe"), os.path.join(dest, "Firestorm.exe"))
    ico = os.path.join(target, "vwe.ico")
    if os.path.exists(ico) and not set_icon(os.path.join(dest, "Firestorm.exe"), ico):
        print("Firestorm.exe keeps the plain icon: vwe.ico could not be added")

    launcher = os.path.join(dest, "launcher")
    if os.path.exists(launcher):
        shutil.rmtree(launcher)
    shutil.copytree(os.path.join(ROOT, "launcher"), launcher,
                    ignore=shutil.ignore_patterns("__pycache__", "*.cmd"))
    for f in ("start-centre.ps1", "desk.py"):
        copy(os.path.join(HERE, f), os.path.join(dest, "tools", f))
    # The MFD boards page (launcher/mfdtest.py) and the bridge it starts:
    # battlepod's bridge with the stream's modules, Firestorm's profile and
    # layout, and pyserial - pure Python - for the bundled Python, which has
    # nothing else.
    for f in ("bridge.py", "mfdstream.py", "modproto.py", "mfdpaint.py"):
        copy(os.path.join(ROOT, "..", "tools", f), os.path.join(launcher, f))
    for f in ("firestorm-5.07", "battletech-13.1.8"):
        copy(os.path.join(ROOT, "..", "profiles", f), os.path.join(launcher, f))
    for f in ("firestorm-mfds", "milestone-mfd"):
        copy(os.path.join(ROOT, "..", "layouts", f), os.path.join(launcher, f))
    try:
        import serial
        shutil.copytree(os.path.dirname(serial.__file__), os.path.join(launcher, "serial"),
                        ignore=shutil.ignore_patterns("__pycache__"))
    except ImportError:
        print("no pyserial here (pip install pyserial): the MFD boards page will not reach boards")

    battletech(dest, a.bt3_files, a.fresh_game)
    embeddable_python(os.path.join(dest, "python"))
    print("done: %s" % os.path.join(dest, "Firestorm.exe"))


if __name__ == "__main__":
    main()
