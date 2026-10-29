#!/usr/bin/env python3
"""A stand-in for the shim's MFD stream port: five displays of synthetic
MFD-like pictures (or a recording, played back), served the way the shim
serves them (docs/MFD_STREAMING_PROTOCOL.md, "The host side") - so the
bridge and receivers can be run and tested with no game.

    tools/mfdsource.py [--port 9110] [--fps 21] [--activity idle|walk|combat|stress]
    tools/mfdsource.py --play recording.mfds
    tools/mfdsource.py --activity stress --loop 60      (coded first: full rate)

--activity sets how much moves: idle (a number ticking), walk (a few
readouts), combat (MFD 4's target view moving every frame, readouts, a page
change now and then on every display), stress (whole pages every frame).
A recording is what tools/mfdrecv.py --record wrote. Standard library only.
"""
import argparse
import os
import random
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mfdstream as ms  # noqa: E402
import modproto as mp  # noqa: E402


class Frames:
    """MFD-like pictures: a page of text and lines on black, in the grey
    levels a 5- or 6-bit channel has, and ways to change it."""

    def __init__(self, w, h, seed=1, bits=6):
        self.w, self.h = w, h
        self.rnd = random.Random(seed)
        self.levels = [((v << (8 - bits)) | (v >> (2 * bits - 8))) & 0xFF for v in range(1 << bits)]

    def level(self):
        return self.rnd.choice(self.levels)

    def page(self):
        w, h, r = self.w, self.h, self.rnd
        f = bytearray(w * h)
        for _ in range(r.randint(3, 12)):                # text: rows of glyph cells
            y, x = r.randrange(0, h - 10), r.randrange(0, w - 8)
            v = self.level()
            for c in range(r.randint(1, 12)):
                for gy in range(7):
                    for gx in range(5):
                        if r.random() < 0.45 and x + c * 6 + gx < w:
                            f[(y + gy) * w + x + c * 6 + gx] = v
        for _ in range(r.randint(2, 6)):                 # boxes and rules
            y, x = r.randrange(h), r.randrange(w - 8)
            n = r.randint(4, w - x)
            f[y * w + x:y * w + x + n] = bytes([self.level()]) * n
            for yy in range(y, min(h, y + r.randint(2, 40))):
                f[yy * w + x] = f[y * w + x]
        return f

    def nudge(self, f, n=1):
        """A few small changes: a number ticking, a blip moving."""
        f = bytearray(f)
        for _ in range(n):
            x, y = self.rnd.randrange(self.w - 6), self.rnd.randrange(self.h - 8)
            for yy in range(y, y + 7):
                for xx in range(x, x + 5):
                    f[yy * self.w + xx] = self.level() if self.rnd.random() < 0.5 else 0
        return f

    def picture(self, f, x0, y0, w, h, t):
        """A live picture in a box - the target view - every pixel moving."""
        f = bytearray(f)
        lv, n = self.levels, len(self.levels)
        for y in range(y0, y0 + h):
            row = bytes(lv[((x * 3 + y * 5 + t * 7) ^ (x * y + t)) % n] for x in range(x0, x0 + w))
            f[y * self.w + x0:y * self.w + x0 + w] = row
        return f

    def noise(self, frac=1.0):
        return bytearray(self.rnd.randrange(256) if self.rnd.random() < frac else 0
                         for _ in range(self.w * self.h))


class Scene:
    """Five displays' pictures over time, at an activity level."""

    def __init__(self, activity, w=640, h=480, seed=1):
        self.activity, self.t = activity, 0
        # Displays 1, 3, 4 sit in 5-bit red and blue channels; 2 and 5 in 6-bit green.
        self.gen = [Frames(w, h, seed + i, 6 if i in (1, 4) else 5) for i in range(5)]
        self.cur = [g.page() for g in self.gen]

    def step(self):
        t, a = self.t, self.activity
        self.t += 1
        for i, g in enumerate(self.gen):
            f = self.cur[i]
            if a == "stress":
                f = g.page() if t % 2 else g.noise(0.3)
            elif a == "combat":
                if t % 40 == 8 * i:
                    f = g.page()                         # a page changes
                else:
                    f = g.nudge(f, 3)
                    if i == 3:
                        f = g.picture(f, 180, 120, 280, 200, t)
            elif a == "walk":
                if t % 3 == 0:
                    f = g.nudge(f, 1)
            elif t % 21 == 0:
                f = g.nudge(f, 1)
            self.cur[i] = f
        return self.cur


class Loop:
    """A display's frames, coded once and served round and round - for
    load tests at rates Python could not code live. Each part's stream id
    and frame number are written in as it goes out."""

    def __init__(self, display, frames, w, h):
        e = ms.Encoder(display, w, h, stream=0)
        e.update(frames[0])
        self.frames = [bytes(f) for f in frames]
        self.deltas = [None] * len(frames)
        for i in list(range(1, len(frames))) + [0]:
            self.deltas[i] = e.update(frames[i])
        self.keys = {}
        self.display, self.w, self.h = display, w, h
        self.i, self.frame, self.stream = 0, 1, 0

    def start(self, stream):
        self.stream, self.frame = stream, 1

    def patch(self, parts, t):
        return [bytes(p[:4]) + struct.pack(">II", self.stream, self.frame) + bytes(p[12:18]) +
                struct.pack(">I", t) + bytes(p[22:]) for p in parts]

    def update(self, _frame, t):
        self.i = (self.i + 1) % len(self.frames)
        parts = self.deltas[self.i]
        if not parts:
            return []
        self.frame += 1
        return self.patch(parts, t)

    def key(self, t):
        if self.i not in self.keys:
            self.keys[self.i] = ms.Encoder(self.display, self.w, self.h, stream=0).update(self.frames[self.i])
        return self.patch(self.keys[self.i], t)


def now_us():
    return int(time.perf_counter() * 1e6) & 0xFFFFFFFF


class Recording:
    """A file of FRAME payloads (u32 length, payload) - what mfdrecv.py
    --record writes - decoded back to whole frames for playback."""

    def __init__(self, path):
        self.dec = {}
        self.frames = []        # [(display, bytes)]
        with open(path, "rb") as f:
            data = f.read()
        at = 0
        while at + 4 <= len(data):
            n = struct.unpack_from(">I", data, at)[0]
            p = data[at + 4:at + 4 + n]
            at += 4 + n
            h = ms.parse_header(p)
            d = self.dec.setdefault(h["display"], ms.Decoder(h["display"], h["width"], h["height"]))
            if d.feed(p) == "frame":
                self.frames.append((h["display"], bytes(d.fb)))


class Source:
    def __init__(self, a):
        self.a = a
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", a.port))
        self.srv.listen(1)
        self.client = None
        self.seq = 0
        self.stats = {}

    def send(self, mtype, payload):
        self.seq = (self.seq + 1) & 0xFF
        try:
            self.client.sendall(mp.pack(mtype, self.seq, payload))
        except OSError:
            self.drop()

    def drop(self):
        if self.client:
            self.client.close()
        self.client = None

    def emit(self, i, parts, t_enc):
        s = self.stats.setdefault(i, [0, 0, 0.0])
        s[0] += 1
        s[1] += sum(len(p) for p in parts)
        s[2] += t_enc
        for p in parts:
            if self.client:
                self.send(mp.FRAME, p)

    def run(self):
        a = self.a
        print("mfdsource: tcp://127.0.0.1:%d, %s at %g fps" % (a.port, a.play or a.activity, a.fps), flush=True)
        if a.play:
            rec = Recording(a.play)
            shape = {}
            for d, f in rec.frames:
                shape.setdefault(d, rec.dec[d])
        scene = None if a.play else Scene(a.activity, a.width, a.height, a.seed)
        loops = None
        if a.loop:
            t0 = time.monotonic()
            frames = {d: [] for d in range(1, 6)}
            for _ in range(a.loop):
                for i, f in enumerate(scene.step()):
                    frames[i + 1].append(bytes(f))
            loops = {d: Loop(d, fr, a.width, a.height) for d, fr in frames.items()}
            print("mfdsource: %d frames a display coded in %.0f s" % (a.loop, time.monotonic() - t0), flush=True)
        enc, reader, step, last_log = {}, mp.Reader(), 0, time.monotonic()
        nxt = time.monotonic()
        while True:
            if not self.client:
                self.srv.settimeout(0.5)
                try:
                    c, _ = self.srv.accept()
                except socket.timeout:
                    continue
                c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                c.settimeout(None)
                self.client, reader = c, mp.Reader()
                stream = random.getrandbits(32)
                if loops:
                    enc = loops
                    for lp in loops.values():
                        lp.start(stream)
                elif a.play:
                    enc = {d: ms.Encoder(d, x.w, x.h, stream=stream) for d, x in shape.items()}
                else:
                    enc = {i + 1: ms.Encoder(i + 1, a.width, a.height, stream=stream) for i in range(5)}
                print("mfdsource: a client, stream %08X" % stream, flush=True)
            # what the client says
            self.client.setblocking(False)
            try:
                data = self.client.recv(4096)
                if not data:
                    self.drop()
                    continue
            except BlockingIOError:
                data = b""
            except OSError:
                self.drop()
                continue
            finally:
                if self.client:
                    self.client.setblocking(True)
            for t, _, p in reader.feed(data):
                if t == mp.HELLO_Q:
                    self.send(mp.HELLO, mp.hello("mfd-source", "mfdsource", "mfdsource 1", 0, 0, 0,
                                                 a.width, a.height, mp.SCREEN_GREY))
                elif t == mp.SYNC:
                    d = ms.parse_sync(p)[0]
                    if d in enc:
                        t0 = time.perf_counter()
                        parts = enc[d].key(now_us())
                        self.emit(d, parts, time.perf_counter() - t0)
            # a frame, on time
            now = time.monotonic()
            if now < nxt:
                time.sleep(min(nxt - now, 0.005))
                continue
            nxt = max(nxt + 1.0 / a.fps, now - 0.1)
            if a.play:
                if not rec.frames:
                    continue
                d, f = rec.frames[step % len(rec.frames)]
                todo = [(d, f)]
            elif loops:
                todo = [(d, None) for d in loops]
            else:
                todo = [(i + 1, f) for i, f in enumerate(scene.step())]
            step += 1
            for d, f in todo:
                t0 = time.perf_counter()
                parts = enc[d].update(f, now_us())
                self.emit(d, parts, time.perf_counter() - t0)
            if a.verbose and now - last_log > 5:
                last_log = now
                for d, (n, b, te) in sorted(self.stats.items()):
                    print("mfdsource: display %d: %d updates, %.1f KB, encode %.1f ms avg"
                          % (d, n, b / 1024, te * 1000 / max(n, 1)), flush=True)
                self.stats = {}


def main():
    ap = argparse.ArgumentParser(description="a stand-in for the shim's MFD stream")
    ap.add_argument("--port", type=int, default=9110)
    ap.add_argument("--fps", type=float, default=21)
    ap.add_argument("--activity", choices=("idle", "walk", "combat", "stress"), default="walk")
    ap.add_argument("--play", help="a recording from mfdrecv.py --record")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--loop", type=int, default=0,
                    help="code this many frames first and serve them round and round (for load tests)")
    ap.add_argument("--verbose", action="store_true")
    Source(ap.parse_args()).run()


if __name__ == "__main__":
    main()
