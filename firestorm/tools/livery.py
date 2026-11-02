#!/usr/bin/env python3
"""The console's camos and units, as pictures for the launcher's Pilots page.

The console's slots have two numbered lists - camo (1-16) and unit (1-17) -
with a small picture of each beside them. Those pictures live packed in the
game's resources; this reads them off the console's own screen instead: it
puts a pod in slot 1 (or uses the one the console filled in), picks each
number in turn with the shim's pointer (the control port's click and vkey,
so the real mouse is not touched), snapshots the console and crops the
picture. They go to profiles\\livery\\camo_NN.png and unit_NN.png beside the
game - game material, kept there, never in a repository.

    python livery.py --game C:\\path\\to\\MW4

About a minute. It starts the centre if the console is not running
(start-centre.ps1) and stops it again afterwards. Needs Pillow.
"""
import argparse
import glob
import os
import socket
import subprocess
import sys
import time

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CONSOLE = ("127.0.0.1", 9101)
# Slot 1 of the console's 800x600 menu (see ddraw.c, fill_main).
CAMO_LIST, UNIT_LIST = (383, 101), (453, 101)
CAMO_BOX, UNIT_BOX = (340, 85, 372, 117), (403, 83, 439, 119)
CAMOS, UNITS = 16, 17


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Console:
    def __init__(self, game):
        self.game = game
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def cmd(self, s, wait=0.08):
        self.sock.sendto(s.encode(), CONSOLE)
        time.sleep(wait)

    def log_has(self, text):
        p = os.path.join(self.game, "shim-ctcl1.log")
        return os.path.exists(p) and text in open(p, encoding="latin-1", errors="replace").read()

    def snap(self):
        for f in glob.glob(os.path.join(self.game, "snap-ctcl1-main1-*.bmp")):
            os.remove(f)
        self.cmd("snap", 0)
        until = time.time() + 5
        while time.time() < until:
            fs = glob.glob(os.path.join(self.game, "snap-ctcl1-main1-*.bmp"))
            if fs:
                time.sleep(0.2)
                im = Image.open(fs[0]).convert("RGB")
                os.remove(fs[0])
                return im
            time.sleep(0.1)
        raise RuntimeError("the console made no snapshot (is it running?)")

    def pick(self, at, n):
        """Number n in the list at `at`: open it, Home, Down, Enter."""
        self.cmd("click %d %d" % at, 0.4)
        self.cmd("vkey 24")
        for _ in range(n - 1):
            self.cmd("vkey 28", 0.06)
        self.cmd("vkey 0D", 0.6)


def main():
    ap = argparse.ArgumentParser(description="read the console's camo and unit pictures")
    ap.add_argument("--game", default=os.path.join(HERE, "..", "..", "..", "firestorm-run", "MW4"))
    a = ap.parse_args()
    game = os.path.abspath(a.game)
    out = os.path.join(game, "profiles", "livery")
    os.makedirs(out, exist_ok=True)
    ps1 = os.path.join(HERE, "start-centre.ps1")
    c = Console(game)

    started = False
    if not c.log_has("main1: first frame") or not subprocess.run(
            ["tasklist", "/fi", "imagename eq MW4.exe"], capture_output=True, text=True).stdout.count("MW4.exe"):
        log("starting the centre")
        for f in glob.glob(os.path.join(game, "shim-ctcl*.log")):
            os.remove(f)
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1, "-Game", game,
                        "-NoPrint"], capture_output=True, timeout=120)
        started = True
        until = time.time() + 60
        while time.time() < until and not c.log_has("main1: first frame"):
            time.sleep(0.5)
        time.sleep(4)
        until = time.time() + 20                     # the console fills its slots, if pilots are seated
        while time.time() < until and not c.log_has("the console's slot") and c.log_has("profiles:"):
            time.sleep(0.5)
        time.sleep(3)
    try:
        if not c.log_has("the console's slot 1"):
            log("putting the first pod in slot 1")
            c.cmd("click 178 92", 0.5)
            c.cmd("click 87 133", 0.5)
            c.cmd("click 117 117", 0.4)
            c.cmd("chars Livery", 0.4)
            c.cmd("enter", 0.6)
        for kind, at, box, n in (("camo", CAMO_LIST, CAMO_BOX, CAMOS), ("unit", UNIT_LIST, UNIT_BOX, UNITS)):
            for i in range(1, n + 1):
                c.pick(at, i)
                im = c.snap().crop(box)
                im.save(os.path.join(out, "%s_%02d.png" % (kind, i)))
            log("%d %ss" % (n, kind))
        c.pick(CAMO_LIST, 1)
        c.pick(UNIT_LIST, 1)
    finally:
        c.cmd("pointer off")
        if started:
            subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1, "-Game", game,
                            "-Stop"], capture_output=True, timeout=60)
    log("pictures in", out)


if __name__ == "__main__":
    sys.exit(main())
