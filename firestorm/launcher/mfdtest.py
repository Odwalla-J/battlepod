#!/usr/bin/env python3
"""MFD boards, for the launcher: find them, tell each which MFD it is, and
loop a test picture on each.

A board is a cockpit module with a framebuffer (docs/MODULE_PROTOCOL.md,
docs/MFD_STREAMING_PROTOCOL.md) on a serial port - firmware/esp32-mfd on an
Elecrow CrowPanel 7", behind a CH340 at 2 Mbaud. Its name says which MFD it
is ("mfd:1" .. "mfd:5", the names in layouts/firestorm-mfds) and lives in its
flash; NAME (0x17) sets it.

The test picture for each MFD is its page drawn here - its key labels, its
title, something moving - or, once recorded, a few seconds of the game's own
MFD picture (record(), from the shim's stream port while a mission runs; the
frames are kept beside the game, in profiles\\mfd-test, never in this
repository). Either loops on the board as the bridge would send it: an
update for each change, DEFLATE, a whole frame when the board asks.

usage: mfdtest.py scan | name PORT N | test [SECONDS] | record GAME_DIR [SECONDS]
(needs pyserial)
"""
import os
import random
import socket
import struct
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (HERE, os.path.normpath(os.path.join(HERE, "..", "..", "tools"))):
    if os.path.isfile(os.path.join(_p, "mfdstream.py")) and _p not in sys.path:
        sys.path.insert(0, _p)
import modproto as mp  # noqa: E402
import mfdstream as ms  # noqa: E402

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

BAUD = 2000000
W, H = 640, 480
NAME = 0x17             # host -> module: the name to keep (the MFD it is)
FPS = 10
LOOP = 40               # frames in a drawn loop: 4 s

# Each MFD's keys, as layouts/firestorm-mfds labels them: top row 1-4, bottom 5-8.
LABELS = {
    1: ["", "", "", "", "", "", "", ""],
    2: ["", "NEXT WP", "PREV WP", "OBJECTIVES", "", "", "", ""],
    3: ["MUTE", "NEAR EN", "XHAIR TGT", "ARMOR", "ZOOM", "JUMP", "FLUSH", "LT AMP"],
    4: ["NEAR FR", "NEXT FR", "PREV FR", "TGT MODE", "NEAR EN", "NEXT EN", "PREV EN", "XHAIR TGT"],
    5: ["GRP FIRE", "CHAIN", "NEXT WPN", "PREV WPN", "MODE", "GROUP 1", "GROUP 2", "GROUP 3"],
}
TITLES = {1: "PILOT", 2: "NAVIGATION", 3: "SYSTEMS", 4: "TARGET", 5: "WEAPONS"}

from mfdpaint import FONT, centred, fill, frame, text, text_w  # noqa: E402,F401


def page(mfd, t, note=""):
    """MFD mfd's test page at phase t (0-1): its labels, its title, something moving."""
    fb = bytearray(W * H)
    labels = LABELS.get(mfd, [""] * 8)
    for i in range(4):                                   # the soft-key strips, 40 px, four to a row
        for r, y in ((0, 0), (1, H - 40)):
            frame(fb, i * 160 + 2, y + 2, 156, 36, 160)
            centred(fb, i * 160 + 80, y + 13, labels[r * 4 + i] or "-", 2, 255 if labels[r * 4 + i] else 90)
    frame(fb, 0, 40, W, H - 80, 90, 1)
    title = TITLES.get(mfd, "NOT ASSIGNED")
    text(fb, 16, 52, title, 3)
    tag = "MFD %d" % mfd if mfd in TITLES else "MFD ?"
    text(fb, W - 16 - text_w(tag, 3), 52, tag, 3, 200)
    if note:
        centred(fb, W // 2, 412, note, 2, 160)
    k = int(t * 1000)
    if mfd == 1:
        for i, (name, val) in enumerate((("KILLS", 3 + k // 250), ("DEATHS", 1), ("SCORE", 120 + k))):
            x = 40 + i * 195
            frame(fb, x, 150, 170, 110, 200)
            centred(fb, x + 85, 165, name, 2)
            centred(fb, x + 85, 205, str(val), 4)
        centred(fb, W // 2, 320, "PILOT ONE   MADCAT PRIME", 2, 220)
    elif mfd == 2:
        heading = t * 360
        fill(fb, 40, 170, 560, 2, 200)
        for d in range(0, 360, 10):
            x = int(320 + ((d - heading + 180) % 360 - 180) * 5)
            if 40 <= x < 600:
                big = d % 30 == 0
                fill(fb, x, 172, 2, 18 if big else 9, 220)
                if big:
                    label = {0: "N", 90: "E", 180: "S", 270: "W"}.get(d, str(d // 10))
                    centred(fb, x, 196, label, 2)
        fill(fb, 318, 150, 4, 18, 255)
        centred(fb, W // 2, 260, "HEADING %03d" % int(heading), 3)
        centred(fb, W // 2, 320, "NAV POINT ALPHA   %d M" % (1500 - k), 2, 220)
    elif mfd == 3:
        import math
        for i, name in enumerate(("HEAT", "COOLANT", "JUMP", "ARMOR")):
            x = 70 + i * 135
            level = 0.5 + 0.45 * math.sin(2 * math.pi * (t + i / 4))
            frame(fb, x, 120, 70, 250, 160)
            fill(fb, x + 6, 120 + 6 + int((238) * (1 - level)), 58, int(238 * level), 230)
            centred(fb, x + 35, 380, name, 2)
    elif mfd == 4:
        import math
        cx = int(320 + 170 * math.cos(2 * math.pi * t))
        cy = int(240 + 90 * math.sin(4 * math.pi * t))
        fill(fb, 40, 240, 560, 1, 90)
        fill(fb, 320, 90, 1, 240, 90)
        frame(fb, cx - 40, cy - 40, 80, 80, 255, 3)
        fill(fb, cx - 12, cy, 25, 2, 255)
        fill(fb, cx, cy - 12, 2, 25, 255)
        centred(fb, W // 2, 350, "RANGE %d M" % (300 + int(250 * (1 + math.sin(2 * math.pi * t)))), 3)
        if int(t * 8) % 2:
            centred(fb, W // 2, 110, "TARGET LOCKED", 2)
    elif mfd == 5:
        weapons = ("MEDIUM LASER", "MEDIUM LASER", "SMALL LASER", "AC/5", "LRM 5", "LRM 5")
        sel = int(t * len(weapons)) % len(weapons)
        text(fb, 40, 110, "GROUP  WEAPON", 2, 200)
        text(fb, 520, 110, "AMMO", 2, 200)
        for i, wpn in enumerate(weapons):
            y = 140 + i * 36
            on = i == sel
            if on:
                fill(fb, 36, y - 6, 568, 30, 230)
            v = 0 if on else 255
            text(fb, 40, y, "%d %d %d" % (1, 2 if i > 2 else 0, 3 if i > 3 else 0), 2, v)
            text(fb, 136, y, wpn, 2, v)
            text(fb, 540, y, "-" if "LASER" in wpn else str(20 + i * 2), 2, v)
    else:
        centred(fb, W // 2, 220, "TELL THIS BOARD WHICH MFD IT IS", 2)
    return fb


def drawn_loop(mfd, note=""):
    return [page(mfd, i / LOOP, note) for i in range(LOOP)]


# ------------------------------------------------------------------ recordings

def rec_path(game, mfd):
    return os.path.join(game, "profiles", "mfd-test", "mfd%d.bin" % mfd)


def save_loop(path, frames):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"FSMFDLOOP1" + struct.pack(">HHH", W, H, len(frames)))
        f.write(zlib.compress(b"".join(bytes(x) for x in frames), 6))


def load_loop(path):
    try:
        with open(path, "rb") as f:
            d = f.read()
        if d[:10] != b"FSMFDLOOP1":
            return None
        w, h, n = struct.unpack(">HHH", d[10:16])
        raw = zlib.decompress(d[16:])
        if (w, h) != (W, H) or len(raw) != w * h * n:
            return None
        return [raw[i * w * h:(i + 1) * w * h] for i in range(n)]
    except (OSError, zlib.error, struct.error):
        return None


def recorded(game):
    return [n for n in range(1, 6) if game and os.path.exists(rec_path(game, n))]


def record(game, seconds=4, port=9110):
    """A few seconds of each MFD from the shim's stream (a mission running,
    [stream] enable = 1, no bridge connected), saved as each MFD's loop.
    Returns the MFDs recorded."""
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=1.0)
    except OSError:
        raise ValueError("no MFD stream on port %d: start a mission with the MFDs streaming "
                         "(the box on this page), and stop the bridge if it is running" % port)
    c.settimeout(0.1)
    reader, seq = mp.Reader(), [0]

    def send(t, p=b""):
        seq[0] = (seq[0] + 1) & 0xFF
        c.sendall(mp.pack(t, seq[0], p))

    decs = {d: ms.Decoder(display=d) for d in range(1, 6)}
    frames = {d: [] for d in range(1, 6)}
    send(mp.HELLO_Q, mp.hello_q())
    asked = {}
    start, next_sample, until = time.time(), time.time() + 1.0, time.time() + seconds + 8
    try:
        while time.time() < until:
            now = time.time()
            for d, dec in decs.items():
                if not dec.synced and now - asked.get(d, 0) > 0.3:
                    send(mp.SYNC, ms.sync(d, ms.SYNC_NEW, 0))
                    asked[d] = now
            try:
                data = c.recv(65536)
            except socket.timeout:
                data = b""
            if data == b"" and time.time() - start > 0.5 and not any(dec.synced for dec in decs.values()) \
                    and time.time() - start > 6:
                break
            for t, _, p in reader.feed(data):
                if t == mp.FRAME and len(p) > 1 and p[1] in decs:
                    decs[p[1]].feed(p)
            if now >= next_sample and any(dec.synced for dec in decs.values()):
                for d, dec in decs.items():
                    if dec.synced:
                        frames[d].append(bytes(dec.fb))
                next_sample += 1.0 / FPS
                if all(len(f) >= seconds * FPS for d, f in frames.items() if decs[d].synced) and \
                        sum(1 for dec in decs.values() if dec.synced) and \
                        max(len(f) for f in frames.values()) >= seconds * FPS:
                    break
    finally:
        c.close()
    done = []
    for d, f in frames.items():
        if len(f) >= FPS:
            save_loop(rec_path(game, d), f)
            done.append(d)
    if not done:
        raise ValueError("the stream sent no MFD pictures: is a mission running, with all five MFDs streaming?")
    return done


# ------------------------------------------------------------------ boards

class Board:
    """One board on a serial port."""

    def __init__(self, port):
        self.port, self.ser, self.reader, self.seq, self.info = port, None, mp.Reader(), 0, None

    def open(self):
        if serial is None:
            raise ValueError("the launcher needs pyserial for MFD boards")
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.write_timeout = self.port, BAUD, 0, 2
        s.dtr = s.rts = False                   # on an ESP32 board these reset it
        s.open()
        self.ser = s
        s.write(b"\x00")                        # ends any half packet it holds

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except OSError:
                pass
        self.ser = None

    def send(self, t, p=b""):
        self.seq = (self.seq + 1) & 0xFF
        self.ser.write(mp.pack(t, self.seq, p))

    def poll(self):
        data = self.ser.read(8192)
        return self.reader.feed(data) if data else []

    def hello(self, timeout=1.2):
        for _ in range(2):
            self.send(mp.HELLO_Q, mp.hello_q())
            end = time.time() + timeout / 2
            while time.time() < end:
                for t, _, p in self.poll():
                    if t == mp.HELLO:
                        self.info = mp.parse_hello(p)
                        return self.info
                time.sleep(0.02)
        return None

    @property
    def mfd(self):
        uid = (self.info or {}).get("uid", "")
        return int(uid[4]) if len(uid) == 5 and uid.startswith("mfd:") and uid[4] in "12345" else None

    def describe(self):
        i = self.info or {}
        return {"port": self.port, "uid": i.get("uid", ""), "firmware": i.get("firmware", ""), "mfd": self.mfd}


def ports():
    if serial is None:
        return []
    return sorted(p.device for p in serial.tools.list_ports.comports())


def scan():
    """The MFD boards on this PC's serial ports: [describe()]. A port another
    program holds (the bridge) is passed over."""
    found, lock = [], threading.Lock()

    def probe(port):
        b = Board(port)
        try:
            b.open()
            info = b.hello()
            if info and info.get("cls") == "mfd" and info.get("kind") == ms.SCREEN_GREY:
                with lock:
                    found.append(b.describe())
        except (OSError, ValueError, serial.SerialException if serial else OSError):
            pass
        finally:
            b.close()

    threads = [threading.Thread(target=probe, args=(p,), daemon=True) for p in ports()]
    for t in threads:
        t.start()
    for t in threads:
        t.join(4)
    return sorted(found, key=lambda d: (d["mfd"] or 9, d["port"]))


def assign(port, mfd):
    """Tell the board on port it is MFD mfd (1-5), or unassign it (0)."""
    b = Board(port)
    try:
        b.open()
        if not b.hello():
            raise ValueError("no MFD board answers on %s" % port)
        name = ("mfd:%d" % mfd) if mfd else "mfd-%s" % b.info["uid"][-6:].replace(":", "")
        b.send(NAME, name.encode())
        end = time.time() + 1.5
        while time.time() < end:
            for t, _, p in b.poll():
                if t == mp.HELLO:
                    b.info = mp.parse_hello(p)
                    if b.info["uid"] == name:
                        return b.describe()
            time.sleep(0.02)
        raise ValueError("the board on %s did not take the name %s (its firmware may be older)" % (port, name))
    finally:
        b.close()


class Player(threading.Thread):
    """A loop of frames on one board, as the bridge would send it: each
    change as an update, DEFLATE, two updates in flight at most, a whole
    frame whenever the board asks."""

    def __init__(self, port, display, frames):
        super().__init__(daemon=True)
        self.port, self.display = port, display
        self.stop_ = threading.Event()
        self.error = None
        self.shown = 0
        enc = ms.Encoder(display)
        self.key0 = [ms.deflate_part(p) for p in enc.update(frames[0])]
        self.steps = []                         # frame i -> i+1, round the loop
        for f in list(frames[1:]) + [frames[0]]:
            self.steps.append([ms.deflate_part(p) for p in enc.update(f)])
        self.black = ms.Encoder(display, stream=enc.stream)

    def stop(self):
        self.stop_.set()

    def run(self):
        b = Board(self.port)
        fno = 0

        def put(parts):
            nonlocal fno
            fno = (fno + 1) & 0xFFFFFFFF
            for p in parts:
                q = bytearray(p)
                struct.pack_into(">I", q, 8, fno)
                b.send(mp.FRAME, bytes(q))

        try:
            b.open()
            b.send(mp.HELLO_Q, mp.hello_q())
            put(self.key0)
            inflight, i, due, acked = 1, 0, time.time(), time.time()
            while not self.stop_.is_set():
                for t, _, p in b.poll():
                    if t == mp.FRAME_ACK:
                        inflight, acked = 0, time.time()
                        self.shown += 1
                    elif t == mp.SYNC:
                        i, inflight = 0, 1
                        put(self.key0)          # from the top, whole
                now = time.time()
                if inflight and now - acked > 1.0:
                    i, inflight, acked = 0, 1, now
                    put(self.key0)
                if now >= due and inflight < 2:
                    step = self.steps[i]
                    i = (i + 1) % len(self.steps)
                    if step:
                        put(step)
                        inflight += 1
                    due += 1.0 / FPS
                    if due < now - 0.5:
                        due = now
                time.sleep(0.005)
            # leave the board black, as it starts
            enc = ms.Encoder(self.display)
            put([ms.deflate_part(p) for p in enc.update(bytes(W * H))])
            time.sleep(0.3)
        except Exception as e:  # noqa: BLE001 - shown in the launcher, not lost
            self.error = str(e)
        finally:
            b.close()


class Tester:
    """Test pictures on every board found: each its MFD's loop."""

    def __init__(self):
        self.players = []

    @property
    def running(self):
        return any(p.is_alive() for p in self.players)

    def start(self, boards, game=None):
        self.stop()
        for d in boards:
            n = d["mfd"]
            frames = load_loop(rec_path(game, n)) if (game and n) else None
            if not frames:
                frames = drawn_loop(n or 0, "" if n else "THIS BOARD: %s" % d["uid"])
            p = Player(d["port"], n or 1, frames)
            p.start()
            self.players.append(p)

    def stop(self):
        for p in self.players:
            p.stop()
        for p in self.players:
            p.join(2)
        self.players = []

    def errors(self):
        return [("%s: %s" % (p.port, p.error)) for p in self.players if p.error]


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    if argv[0] == "scan":
        for d in scan():
            print(d)
    elif argv[0] == "name":
        print(assign(argv[1], int(argv[2])))
    elif argv[0] == "test":
        t = Tester()
        t.start(scan())
        time.sleep(float(argv[1]) if len(argv) > 1 else 10)
        for p in t.players:
            print(p.port, "frames shown:", p.shown, "error:", p.error)
        t.stop()
    elif argv[0] == "record":
        print("recorded MFDs:", record(argv[1], float(argv[2]) if len(argv) > 2 else 4))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
