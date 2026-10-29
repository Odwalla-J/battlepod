#!/usr/bin/env python3
"""Tests for MFD display streaming (docs/MFD_STREAMING_PROTOCOL.md).

    python tools/test_mfdstream.py            (or -v)

The codec and the module's decoder (tools/mfdstream.py); the shim's C
encoder against them, byte for byte (firestorm/build/streamtest.exe -
`make -C firestorm test` builds it; skipped if it is not there); and the
bridge carrying five streams to five receivers over real sockets, with a
stand-in for the shim (tools/mfdsource.py). Synthetic frames only - no game
data - but shaped like the MFDs': black, text, lines, a moving picture, and
whole-page changes. Standard library only.
"""
import os
import random
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mfdstream as ms  # noqa: E402
import modproto as mp  # noqa: E402
import mfdrecv  # noqa: E402

STREAMTEST = os.path.join(HERE, "..", "firestorm", "build", "streamtest.exe")


# ------------------------------------------------------------------ frames

from mfdsource import Frames  # noqa: E402


def send(dec, parts):
    """Feed parts; what came of the last one."""
    out = None
    for p in parts:
        out = dec.feed(p)
    return out


def covered(parts, w):
    """The pixels an update's rectangles cover."""
    px = set()
    for p in parts:
        body = p[ms.HEADER.size:]
        if ms.parse_header(p)["flags"] & ms.F_DEFLATE:
            body = ms.inflate(body)
        at = 0
        while at < len(body):
            x, y, rw, rh, codec, n = ms.RECT.unpack_from(body, at)
            at += ms.RECT.size + n
            px.update((yy * w + xx) for yy in range(y, y + rh) for xx in range(x, x + rw))
    return px


# ------------------------------------------------------------------ the codec

class RLA(unittest.TestCase):
    def roundtrip(self, fb, stride, x, y, w, h):
        data = ms.rla_encode(fb, stride, x, y, w, h)
        out = bytearray(fb)
        for r in range(h):                               # the rectangle wiped; the rest kept
            out[(y + r) * stride + x:(y + r) * stride + x + w] = bytes([0x55]) * w
        ms.rla_decode(data, out, stride, x, y, w, h)
        self.assertEqual(out, fb)
        return data

    def test_shapes(self):
        f = Frames(96, 64, seed=2)
        fb = f.page()
        for x, y, w, h in [(0, 0, 96, 64), (0, 0, 1, 1), (5, 0, 1, 64), (0, 7, 96, 1),
                           (17, 33, 40, 31), (95, 63, 1, 1), (0, 1, 96, 63)]:
            self.roundtrip(fb, 96, x, y, w, h)

    def test_long_runs(self):
        fb = bytearray(640 * 480)                        # black: one copy-above run after row 0
        data = self.roundtrip(fb, 640, 0, 0, 640, 480)
        self.assertLess(len(data), 120)
        fb = bytearray([200]) * (640 * 480)
        self.roundtrip(fb, 640, 0, 0, 640, 480)

    def test_noise(self):
        rnd = random.Random(3)
        fb = bytearray(rnd.randrange(256) for _ in range(64 * 32))
        self.roundtrip(fb, 64, 0, 0, 64, 32)
        self.roundtrip(fb, 64, 3, 5, 50, 20)

    def test_bad_data(self):
        fb = bytearray(16 * 16)
        for bad in (b"", b"\x1f", b"\xc0", b"\x00", b"\x3f\x00", b"\x5f\x5f\x00",
                    b"\x80" + b"\x80" * 300):
            with self.assertRaises(ValueError, msg=bad):
                ms.rla_decode(bad, fb, 16, 0, 0, 16, 16)
        with self.assertRaises(ValueError):          # a copy from above the screen
            ms.rla_decode(b"\x81", fb, 16, 0, 0, 2, 1)
        with self.assertRaises(ValueError):          # too much
            ms.rla_decode(b"\x5f\x5f", bytearray(16), 16, 0, 0, 16, 1)


class Stream(unittest.TestCase):
    W, H = 160, 120

    def pair(self, display=1, **kw):
        return (ms.Encoder(display, self.W, self.H, tile=16, stream=0x1234, **kw),
                ms.Decoder(display, self.W, self.H))

    def test_first_is_key_and_exact(self):
        e, d = self.pair()
        f = Frames(self.W, self.H).page()
        parts = e.update(f)
        self.assertTrue(ms.parse_header(parts[0])["flags"] & ms.F_KEY)
        self.assertEqual(send(d, parts), "frame")
        self.assertEqual(d.fb, f)

    def test_static_sends_nothing(self):
        e, d = self.pair()
        f = Frames(self.W, self.H).page()
        send(d, e.update(f))
        for _ in range(20):
            self.assertEqual(e.update(f), [])
        self.assertEqual(e.frame, 1)

    def test_small_change_only_its_tiles(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H)
        f = fr.page()
        send(d, e.update(f))
        g = bytearray(f)
        g[50 * self.W + 70] ^= 0xFF                      # one pixel: one 16x16 tile
        parts = e.update(g)
        self.assertEqual(len(parts), 1)
        self.assertEqual(covered(parts, self.W), {(yy * self.W + xx) for yy in range(48, 64)
                                                  for xx in range(64, 80)})
        self.assertLess(len(parts[0]), 120)
        self.assertEqual(send(d, parts), "frame")
        self.assertEqual(d.fb, g)
        h = bytearray(g)                                 # two apart: two rectangles
        h[0] ^= 1
        h[119 * self.W + 159] ^= 1
        parts = e.update(h)
        self.assertEqual(len(covered(parts, self.W)), 256 + 16 * 8)   # the bottom tile row is 8 high
        send(d, parts)
        self.assertEqual(d.fb, h)

    def test_changes_exact_over_time(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=4)
        f = fr.page()
        for t in range(60):
            if t % 15 == 14:
                f = fr.page()                            # a new page
            elif t % 3 == 0:
                f = fr.picture(f, 40, 30, 50, 40, t)     # the target view
            else:
                f = fr.nudge(f, 2)
            res = send(d, e.update(f))
            self.assertIn(res, ("frame", None))
            self.assertEqual(d.fb, f, t)
            self.assertEqual(d.frame, e.frame)

    def test_full_change(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=5)
        send(d, e.update(fr.page()))
        for f in (fr.page(), fr.noise(), bytearray(self.W * self.H), bytearray([255]) * (self.W * self.H),
                  fr.noise(0.3), fr.page()):
            send(d, e.update(f))
            self.assertEqual(d.fb, f)

    def test_parts_respect_the_limit(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=6)
        send(d, e.update(fr.page()))
        f = fr.noise()
        parts = e.update(f)
        self.assertGreater(len(parts), 3)
        for i, p in enumerate(parts):
            h = ms.parse_header(p)
            self.assertLessEqual(len(p) - ms.HEADER.size, ms.MAX_BODY)
            self.assertEqual(h["part"], i)
            self.assertEqual(bool(h["flags"] & ms.F_END), i == len(parts) - 1)
        send(d, parts)
        self.assertEqual(d.fb, f)

    def test_deflate(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=7)
        f = fr.page()
        parts = [ms.deflate_part(p) for p in e.update(f)]
        self.assertTrue(any(ms.parse_header(p)["flags"] & ms.F_DEFLATE for p in parts))
        send(d, parts)
        self.assertEqual(d.fb, f)
        for t in range(10):
            f = fr.nudge(f, 3)
            send(d, [ms.deflate_part(p) for p in e.update(f)])
            self.assertEqual(d.fb, f)
        d2 = ms.Decoder(1, self.W, self.H, deflate_ok=False)
        self.assertEqual(send(d2, [ms.deflate_part(p) for p in e.key()]), "sync")

    # ---- losing things

    def test_missing_update_is_noticed(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=8)
        f = fr.page()
        send(d, e.update(f))
        f = fr.nudge(f)
        e.update(f)                                      # lost on the way
        f = fr.nudge(f)
        self.assertEqual(send(d, e.update(f)), "sync")
        self.assertFalse(d.synced)
        f = fr.nudge(f)
        self.assertEqual(send(d, e.update(f)), "sync")   # still asking
        self.assertEqual(send(d, e.key()), "frame")      # the host answers
        self.assertEqual(d.fb, f)
        f = fr.nudge(f)
        self.assertEqual(send(d, e.update(f)), "frame")
        self.assertEqual(d.fb, f)

    def test_missing_part_is_noticed(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=9)
        send(d, e.update(fr.page()))
        f = fr.noise()
        parts = e.update(f)
        self.assertGreater(len(parts), 2)
        self.assertEqual(send(d, parts[:1] + parts[2:]), "sync")
        self.assertEqual(send(d, e.key()), "frame")
        self.assertEqual(d.fb, f)

    def test_missing_end_then_next_update(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=10)
        send(d, e.update(fr.page()))
        parts = e.update(fr.noise())
        send(d, parts[:-1])                              # the last part lost
        f = fr.page()
        self.assertEqual(send(d, e.update(f)), "sync")
        send(d, e.key())
        self.assertEqual(d.fb, f)

    def test_reconnect(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=11)
        f = fr.page()
        send(d, e.update(f))
        for _ in range(5):
            f = fr.nudge(f)
            send(d, e.update(f))
        d = ms.Decoder(1, self.W, self.H)               # rebooted, replugged
        f = fr.nudge(f)
        self.assertEqual(send(d, e.update(f)), "sync")
        self.assertEqual(d.why, ms.SYNC_NEW)
        send(d, e.key())
        self.assertEqual(d.fb, f)

    def test_new_stream_after_host_restart(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=12)
        send(d, e.update(fr.page()))
        e2 = ms.Encoder(1, self.W, self.H, tile=16, stream=0x9999)
        f = fr.page()
        parts = e2.update(f)                             # starts with a key frame
        self.assertEqual(send(d, parts), "frame")
        self.assertEqual(d.stream, 0x9999)
        self.assertEqual(d.fb, f)
        e3 = ms.Encoder(1, self.W, self.H, tile=16, stream=0x7777)
        e3.prev, e3.frame = bytes(f), 1                  # a delta from a stream it never saw
        self.assertEqual(send(d, e3.update(fr.nudge(f))), "sync")

    def test_key_while_synced_is_harmless(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=13)
        f = fr.page()
        send(d, e.update(f))
        self.assertEqual(send(d, e.key()), "frame")
        f = fr.nudge(f)
        self.assertEqual(send(d, e.update(f)), "frame")
        self.assertEqual(d.fb, f)

    def test_duplicate_update_ignored(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=14)
        send(d, e.update(fr.page()))
        f = fr.nudge(fr.page())
        parts = e.update(f)
        send(d, parts)
        self.assertEqual(send(d, parts), "ignored")
        self.assertTrue(d.synced)
        self.assertEqual(d.fb, f)

    # ---- five at once

    def test_five_independent(self):
        enc = [ms.Encoder(i, self.W, self.H, tile=16, stream=100 + i) for i in range(1, 6)]
        dec = [ms.Decoder(i, self.W, self.H) for i in range(1, 6)]
        frs = [Frames(self.W, self.H, seed=20 + i, bits=5 if i != 2 else 6) for i in range(5)]
        cur = [fr.page() for fr in frs]
        for t in range(12):
            everything = []
            for i in range(5):
                cur[i] = frs[i].nudge(cur[i], 1 + i) if t % 5 else frs[i].page()
                everything += enc[i].update(cur[i])
            random.Random(t).shuffle(everything)         # interleaved by display, in order within one
            everything.sort(key=lambda p: ms.parse_header(p)["display"])
            for p in everything:
                for d in dec:
                    d.feed(p)                            # everyone hears everything
            for i in range(5):
                self.assertEqual(dec[i].fb, cur[i], (t, i))

    # ---- what is not a frame

    def test_malformed(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=15)
        f = fr.page()
        good = e.update(f)
        send(d, good)
        p = e.key()[0]

        def hdr(**kw):
            h = ms.parse_header(p)
            h.update(kw)
            return ms.header(h["display"], h["flags"], h["stream"], h["frame"], h["part"],
                             h["width"], h["height"], h["time_us"])

        body = p[ms.HEADER.size:]
        cases = {
            "short header": p[:10],
            "wrong size": hdr(width=320) + body,
            "truncated body": p[:-7],
            "rect outside": hdr() + ms.RECT.pack(150, 0, 16, 16, ms.C_RAW, 256) + bytes(256),
            "rect zero size": hdr() + ms.RECT.pack(0, 0, 0, 16, ms.C_RAW, 0),
            "raw wrong length": hdr() + ms.RECT.pack(0, 0, 16, 16, ms.C_RAW, 200) + bytes(200),
            "unknown codec": hdr() + ms.RECT.pack(0, 0, 16, 16, 9, 4) + bytes(4),
            "rla short": hdr() + ms.RECT.pack(0, 0, 16, 16, ms.C_RLA, 1) + b"\x1f",
            "rla over": hdr() + ms.RECT.pack(0, 0, 16, 16, ms.C_RLA, 2) + b"\x7f\x7f",
            "rect header cut": hdr() + b"\x00\x01\x00",
            "bad deflate": hdr(flags=p[2] | ms.F_DEFLATE) + b"\xff\xff\xff\xff",
            "deflate bomb": hdr(flags=p[2] | ms.F_DEFLATE) + ms.deflate(bytes(ms.MAX_BODY * 3)),
            "too big": hdr() + bytes(ms.MAX_BODY + 1),
        }
        for name, bad in cases.items():
            dd = ms.Decoder(1, self.W, self.H)
            send(dd, e.key())
            self.assertTrue(dd.synced)
            self.assertEqual(dd.feed(bad), "sync", name)
            self.assertFalse(dd.synced, name)
            self.assertEqual(send(dd, e.key()), "frame", name)   # and it recovers
            self.assertEqual(dd.fb, f, name)
        # Another display's, or a later version's, are not for it.
        self.assertEqual(d.feed(hdr(display=4) + body), "ignored")
        self.assertEqual(d.feed(bytes([2]) + hdr()[1:] + body), "ignored")
        self.assertTrue(d.synced)

    def test_corruption_on_the_wire_is_dropped(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=16)
        f = fr.page()
        wire = b"".join(mp.pack(mp.FRAME, i, p) for i, p in enumerate(e.update(f)))
        rnd = random.Random(1)
        for trial in range(200):
            bad = bytearray(wire)
            for _ in range(rnd.randint(1, 3)):
                bad[rnd.randrange(len(bad))] ^= 1 << rnd.randrange(8)
            r = mp.Reader()
            dd = ms.Decoder(1, self.W, self.H)
            for t, s, p in r.feed(bytes(bad)):
                if t == mp.FRAME:
                    dd.feed(p)
            # Whatever got through the CRC is exactly right, or not shown.
            if dd.synced:
                self.assertEqual(dd.fb, f, trial)

    # ---- hard going

    def test_stress_rapid_pages(self):
        e, d = self.pair()
        fr = Frames(self.W, self.H, seed=17)
        for t in range(40):
            f = fr.page() if t % 2 else fr.noise(0.2)
            send(d, [ms.deflate_part(p) for p in e.update(f)])
            self.assertEqual(d.fb, f, t)

    def test_stress_full_size(self):
        W, H = 640, 480
        e = ms.Encoder(3, W, H, tile=16, stream=1)
        d = ms.Decoder(3, W, H)
        fr = Frames(W, H, seed=18, bits=5)
        f = fr.page()
        send(d, e.update(f))
        for t in range(3):
            f = fr.picture(fr.nudge(f, 4), 180, 120, 280, 200, t)
            send(d, e.update(f))
            self.assertEqual(d.fb, f)
        f = fr.page()
        send(d, e.update(f))
        self.assertEqual(d.fb, f)


# ------------------------------------------------------------------ the shim's encoder

@unittest.skipUnless(os.path.exists(STREAMTEST), "firestorm/build/streamtest.exe not built")
class CEncoder(unittest.TestCase):
    def run_c(self, w, h, tile, display, stream, commands):
        cmd = b"".join(b"F" + bytes(c) if c != "K" else b"K" for c in commands)
        out = subprocess.run([STREAMTEST, str(w), str(h), str(tile), str(display), str(stream)],
                             input=cmd, capture_output=True, check=True).stdout
        return [p for t, s, p in mp.Reader().feed(out) if t == mp.FRAME]

    def run_py(self, w, h, tile, display, stream, commands):
        e = ms.Encoder(display, w, h, tile=tile, stream=stream)
        out = []
        for i, c in enumerate(commands):
            out += e.key(i) if c == "K" else e.update(c, i)
        return out

    def check(self, w, h, tile, commands):
        c = self.run_c(w, h, tile, 2, 0xABCD, commands)
        p = self.run_py(w, h, tile, 2, 0xABCD, commands)
        self.assertEqual(len(c), len(p))
        for i, (a, b) in enumerate(zip(c, p)):
            self.assertEqual(a, b, "part %d differs" % i)
        d = ms.Decoder(2, w, h)
        last = None
        for part in c:
            d.feed(part)
        for cmd in commands:
            if cmd != "K":
                last = cmd
        self.assertEqual(d.fb, bytearray(last))

    def test_same_bytes_as_the_reference(self):
        fr = Frames(160, 120, seed=30)
        f = fr.page()
        cmds = [f]
        for t in range(25):
            f = fr.page() if t % 9 == 8 else (fr.picture(f, 10, 10, 40, 30, t) if t % 4 == 0 else fr.nudge(f, 2))
            cmds.append(f)
            if t in (5, 17):
                cmds.append("K")
        cmds += [f, f, fr.noise(), "K", fr.noise(0.1)]
        self.check(160, 120, 16, cmds)

    def test_odd_sizes_and_tiles(self):
        for w, h, tile in ((100, 70, 16), (64, 48, 8), (96, 64, 32), (33, 17, 16)):
            fr = Frames(w, h, seed=w)
            f = fr.page()
            self.check(w, h, tile, [f, fr.nudge(f, 3), fr.noise(), "K", fr.page()])

    def test_full_size(self):
        fr = Frames(640, 480, seed=31, bits=5)
        f = fr.page()
        g = fr.picture(fr.nudge(f, 6), 200, 100, 240, 180, 1)
        self.check(640, 480, 16, [f, g, fr.noise(0.5), "K"])


# ------------------------------------------------------------------ through the bridge

def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class FakeShim:
    """The shim's stream port, as firestorm/src/ddraw.c serves it: one
    client, a new stream for each, key frames on SYNC. show() is a frame."""

    def __init__(self, port, w=640, h=480):
        self.w, self.h = w, h
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(1)
        self.srv.settimeout(0.05)
        self.lock = threading.Lock()
        self.client = None
        self.enc = {}
        self.cur = {}
        self.seq = 0
        self.keys_sent = 0
        self.stop = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def send(self, payload):
        self.seq = (self.seq + 1) & 0xFF
        try:
            self.client.sendall(mp.pack(mp.FRAME, self.seq, payload))
        except OSError:
            self.client = None

    def show(self, d, frame):
        with self.lock:
            self.cur[d] = bytes(frame)
            if self.client:
                for p in self.enc[d].update(frame, mfdrecv.host_us()):
                    self.send(p)

    def restart(self):
        """The game restarted: the connection goes, a new stream follows."""
        with self.lock:
            if self.client:
                self.client.close()
            self.client = None

    def run(self):
        reader = mp.Reader()
        while not self.stop:
            if not self.client:
                try:
                    c, _ = self.srv.accept()
                except OSError:
                    continue
                c.settimeout(0.02)
                with self.lock:
                    stream = random.getrandbits(32)
                    self.enc = {d: ms.Encoder(d, self.w, self.h, stream=stream) for d in range(1, 6)}
                    self.client, reader = c, mp.Reader()
                    for d, f in self.cur.items():           # the first update: a key frame
                        for p in self.enc[d].update(f, mfdrecv.host_us()):
                            self.send(p)
                continue
            try:
                data = self.client.recv(4096)
            except socket.timeout:
                continue
            except (OSError, AttributeError):
                data = b""
            if not data:
                self.restart()
                continue
            for t, _, p in reader.feed(data):
                if t == mp.SYNC:
                    d = ms.parse_sync(p)[0]
                    with self.lock:
                        if d in self.enc and self.client:
                            self.keys_sent += 1
                            for part in self.enc[d].key(mfdrecv.host_us()):
                                self.send(part)

    def close(self):
        self.stop = True
        self.restart()
        self.thread.join()
        self.srv.close()


class Receivers:
    """Five headless receivers, polled by one thread."""

    def __init__(self, port, uids=("vmfd-1", "vmfd-2", "vmfd-3", "vmfd-4", "vmfd-5"), deflate=True):
        self.port = port
        self.rx = {u: mfdrecv.Receiver(u, "127.0.0.1:%d" % port, deflate=(deflate if u != "vmfd-2" else False))
                   for u in uids}
        self.paused = set()
        self.stop = False
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop:
            with self.lock:
                for u, r in self.rx.items():
                    if u not in self.paused:
                        r.poll()
            time.sleep(0.002)

    def replace(self, uid):
        with self.lock:
            self.rx[uid].close()
            self.rx[uid] = mfdrecv.Receiver(uid, "127.0.0.1:%d" % self.port)

    def close(self):
        self.stop = True
        self.thread.join()
        for r in self.rx.values():
            r.close()


def wait_for(cond, timeout=15.0):
    t = time.monotonic()
    while time.monotonic() - t < timeout:
        if cond():
            return True
        time.sleep(0.02)
    return cond()


class ThroughTheBridge(unittest.TestCase):
    """tools/bridge.py --game firestorm, with the stand-in shim's stream and
    its UDP control port (lamps in, key presses out), and five receivers."""

    def setUp(self):
        self.stream_port, self.module_port = free_port(), free_port()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", 0))
        self.udp.settimeout(0.05)
        self.shim = FakeShim(self.stream_port)
        self.gen = {d: Frames(640, 480, seed=40 + d, bits=6 if d in (2, 5) else 5) for d in range(1, 6)}
        self.cur = {d: self.gen[d].page() for d in range(1, 6)}
        for d, f in self.cur.items():
            self.shim.show(d, f)
        self.log = open(os.path.join(os.environ.get("TEMP", "."), "test_mfdstream_bridge.log"), "w")
        self.bridge = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "bridge.py"), "--game", "firestorm",
             "--firestorm-port", str(self.udp.getsockname()[1]), "--stream-port", str(self.stream_port),
             "--module-port", str(self.module_port), "--verbose"],
            stdout=self.log, stderr=subprocess.STDOUT)
        self.rx = Receivers(self.module_port)
        self.assertTrue(wait_for(self.all_exact, 20), self.state())

    def tearDown(self):
        self.rx.close()
        self.bridge.terminate()
        self.bridge.wait()
        self.shim.close()
        self.udp.close()
        self.log.close()

    def state(self):
        return {u: (r.dec.display, r.dec.synced, r.dec.frame, r.linked()) for u, r in self.rx.rx.items()}

    def exact(self, uid, d):
        r = self.rx.rx[uid]
        return r.dec.synced and r.dec.display == d and r.dec.fb == self.cur[d]

    def all_exact(self):
        return all(self.exact("vmfd-%d" % d, d) for d in range(1, 6))

    def show(self, d, f):
        self.cur[d] = bytearray(f)
        self.shim.show(d, f)

    def test_five_streams_exact(self):
        for t in range(15):
            for d in range(1, 6):
                g = self.gen[d]
                f = g.page() if t % 7 == 6 else g.nudge(self.cur[d], 1 + d)
                if d == 4:
                    f = g.picture(f, 200, 140, 120, 90, t)
                self.show(d, f)
            time.sleep(1 / 21)
        self.assertTrue(wait_for(self.all_exact), self.state())
        for u, r in self.rx.rx.items():
            self.assertEqual(r.stats.invalid, 0, u)

    def test_unchanged_frames_send_nothing(self):
        time.sleep(0.5)
        before = {u: r.stats.bytes for u, r in self.rx.rx.items()}
        for _ in range(20):
            for d in range(1, 6):
                self.shim.show(d, self.cur[d])
            time.sleep(0.02)
        time.sleep(0.5)
        self.assertEqual(before, {u: r.stats.bytes for u, r in self.rx.rx.items()})

    def test_receiver_reconnects(self):
        self.rx.replace("vmfd-3")                           # unplugged and back
        f = self.gen[3].nudge(self.cur[3], 4)
        self.show(3, f)
        self.assertTrue(wait_for(lambda: self.exact("vmfd-3", 3)), self.state())
        for t in range(5):
            self.show(3, self.gen[3].nudge(self.cur[3], 2))
            time.sleep(0.05)
        self.assertTrue(wait_for(self.all_exact), self.state())

    def test_shim_restart(self):
        self.shim.restart()
        for d in range(1, 6):
            self.show(d, self.gen[d].page())                # what the restarted game shows
        self.assertTrue(wait_for(self.all_exact, 20), self.state())

    def test_stalled_receiver_catches_up(self):
        self.rx.paused.add("vmfd-4")                        # its window dragged, its USB busy
        for t in range(30):
            for d in range(1, 6):
                self.show(d, self.gen[d].nudge(self.cur[d], 3) if t % 5 else self.gen[d].page())
            time.sleep(0.02)
        self.assertTrue(wait_for(lambda: all(self.exact("vmfd-%d" % d, d) for d in (1, 2, 3, 5))), self.state())
        self.rx.paused.discard("vmfd-4")
        self.assertTrue(wait_for(self.all_exact), self.state())

    def test_lamps_and_keys_still_work(self):
        hello, addr = None, None
        t = time.monotonic()
        while time.monotonic() - t < 10 and not addr:       # the bridge says hello to the "shim"
            try:
                hello, addr = self.udp.recvfrom(256)
            except socket.timeout:
                pass
        self.assertEqual(hello, b"hello")
        # A lamp, while pictures flow: MFD 3's top-left key (lamp 0x37) lit.
        for t in range(10):
            self.show(3, self.gen[3].page())
            if t == 3:
                self.udp.sendto(b"game 84 37 3C 73", addr)
        self.assertTrue(wait_for(lambda: self.rx.rx["vmfd-3"].levels[0] == 255, 5))
        # A key, back to the game as a press.
        while True:                                         # (drop the hellos)
            try:
                self.udp.recvfrom(256)
            except socket.timeout:
                break
        self.rx.rx["vmfd-3"].key(0, True)
        got = []
        t = time.monotonic()
        while time.monotonic() - t < 5 and b"press 37" not in got:
            try:
                got.append(self.udp.recvfrom(256)[0])
            except socket.timeout:
                pass
        self.assertIn(b"press 37", got)
        self.assertTrue(wait_for(self.all_exact), self.state())

    def test_stress_whole_pages(self):
        for t in range(12):
            for d in range(1, 6):
                self.show(d, self.gen[d].page() if t % 3 else self.gen[d].noise(0.15))
        self.assertTrue(wait_for(self.all_exact, 30), self.state())



if __name__ == "__main__":
    unittest.main()
