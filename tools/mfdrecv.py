#!/usr/bin/env python3
"""A virtual MFD receiver: the module the ESP32-S3 MFDs will be, in a
window - a 640x480 screen fed by FRAME messages and eight lit keys -
speaking docs/MODULE_PROTOCOL.md to tools/bridge.py over TCP. Every pixel
it shows came through the stream (docs/MFD_STREAMING_PROTOCOL.md); it
never sees the game.

    tools/mfdrecv.py --uid vmfd-3                 one MFD (the layout says which)
    tools/mfdrecv.py --all                        five, vmfd-1 .. vmfd-5, in a row
    tools/mfdrecv.py --uid vmfd-3 --headless --stats out.json --seconds 60

The window shows the picture 1:1 (--scale 2 for twice, --scale 0.5 for
half); --tint 60ff80 colours it like a phosphor. Keys: click, or 1-8 (1-4
above the screen, 5-8 below). F2 shows the statistics line; --stats writes
them as JSON every five seconds and at the end; --record keeps every FRAME
it was sent, for tools/mfdsource.py --play. Without a link it says NO LINK
and keeps trying; out of step it shows SYNC until a whole frame comes.

The decoding is tools/mfdstream.py's Decoder - nothing else touches the
framebuffer, so this is what firmware has to do. Standard library only
(tkinter for the window).
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mfdstream as ms  # noqa: E402
import modproto as mp  # noqa: E402

W, H = 640, 480
KEY_H = 44


def host_us():
    """The host's microsecond clock, as the shim stamps frames: the
    performance counter, which perf_counter reads on Windows."""
    return int(time.perf_counter() * 1e6) & 0xFFFFFFFF


class Stats:
    """What one receiver saw, over its whole run and the last second."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.frames = 0             # updates completed (each one changed pixels)
        self.keys = 0               # of which whole frames
        self.parts = 0
        self.bytes = 0              # FRAME payloads, as they came
        self.pixels = 0             # pixels those updates rewrote
        self.update_bytes = []      # bytes per completed update
        self.decode_ms = []
        self.latency_ms = []
        self.invalid = 0            # bad packets and bad FRAMEs
        self.syncs = 0              # times it asked for a whole frame
        self.ignored = 0
        self.window = []            # (time, bytes) for the per-second rates
        self.peak_bps = 0.0
        self._cur = 0

    def part(self, n, pixels):
        self.parts += 1
        self.bytes += n
        self._cur += n
        self.pixels += pixels
        t = time.monotonic()
        self.window.append((t, n))
        while self.window and self.window[0][0] < t - 1.0:
            self.window.pop(0)
        if t - self.t0 > 1.0:
            self.peak_bps = max(self.peak_bps, sum(b for _, b in self.window))

    def done(self, key, decode_ms, latency_ms):
        self.frames += 1
        self.keys += key
        self.update_bytes.append(self._cur)
        self._cur = 0
        self.decode_ms.append(decode_ms)
        if latency_ms is not None:
            self.latency_ms.append(latency_ms)

    def rate(self):
        t = time.monotonic()
        recent = [b for tt, b in self.window if tt > t - 1.0]
        return sum(recent)

    def summary(self):
        el = max(time.monotonic() - self.t0, 1e-6)

        def pct(v, q):
            if not v:
                return None
            v = sorted(v)
            return round(v[min(len(v) - 1, int(q * len(v)))], 3)

        ub = self.update_bytes
        return {
            "seconds": round(el, 1),
            "updates": self.frames, "updates_per_s": round(self.frames / el, 2),
            "key_frames": self.keys, "parts": self.parts,
            "bytes": self.bytes, "avg_bytes_per_s": round(self.bytes / el), "peak_bytes_per_s": round(self.peak_bps),
            "bytes_per_update": {"mean": round(sum(ub) / len(ub)) if ub else None,
                                 "max": max(ub) if ub else None},
            "compression": round(self.pixels / self.bytes, 1) if self.bytes else None,
            "decode_ms": {"mean": round(sum(self.decode_ms) / len(self.decode_ms), 3) if self.decode_ms else None,
                          "p95": pct(self.decode_ms, 0.95), "max": round(max(self.decode_ms), 3) if self.decode_ms else None},
            "latency_ms": {"mean": round(sum(self.latency_ms) / len(self.latency_ms), 2) if self.latency_ms else None,
                           "p50": pct(self.latency_ms, 0.5), "p95": pct(self.latency_ms, 0.95),
                           "max": round(max(self.latency_ms), 2) if self.latency_ms else None},
            "invalid": self.invalid, "sync_requests": self.syncs, "ignored": self.ignored,
        }


def rect_pixels(payload):
    """The pixels a FRAME part rewrites (for the compression ratio)."""
    try:
        body = payload[ms.HEADER.size:]
        if payload[2] & ms.F_DEFLATE:
            body = ms.inflate(body)
        at, n = 0, 0
        while at + ms.RECT.size <= len(body):
            x, y, w, h, c, ln = ms.RECT.unpack_from(body, at)
            at += ms.RECT.size + ln
            n += w * h
        return n
    except (ValueError, struct.error, Exception):
        return 0


class Receiver:
    """The module, without a window: the link, the decoder, the keys' lamps."""

    def __init__(self, uid, bridge="127.0.0.1:5181", deflate=True, record=None, link_kbps=0):
        self.uid = uid
        # --link-kbps: read no faster than a slow link would deliver (USB Full
        # Speed CDC is about 1000 KB/s), so the bridge's pacing can be tried.
        self.link = link_kbps * 1024
        self.budget, self.budget_t = 0.0, time.monotonic()
        host, port = bridge.rsplit(":", 1)
        self.addr = (host, int(port))
        self.deflate = deflate
        self.sock = None
        self.reader = mp.Reader()
        self.seq = 0
        self.heard = 0.0
        self.tried = 0.0
        self.t0 = time.monotonic()
        self.dec = ms.Decoder(None, W, H, deflate_ok=deflate)
        self.levels = [0] * 8
        self.labels = {}
        self.stats = Stats()
        self.sync_at = 0.0
        self.dirty = False
        self.record = open(record, "ab") if record else None
        self.part_t = None          # when the update being built began to arrive

    def ms(self):
        return int((time.monotonic() - self.t0) * 1000)

    def linked(self):
        return self.sock is not None and time.monotonic() - self.heard < 2.0

    def send(self, mtype, payload=b""):
        if not self.sock:
            return
        self.seq = (self.seq + 1) & 0xFF
        try:
            self.sock.sendall(mp.pack(mtype, self.seq, payload))
        except OSError:
            self.close()

    def close(self):
        if self.sock:
            self.sock.close()
        self.sock = None
        self.reader = mp.Reader()

    def connect(self):
        try:
            s = socket.create_connection(self.addr, timeout=0.2)
        except OSError:
            return
        s.setblocking(False)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 * 1024)   # a module's buffer is small
        self.sock = s
        self.heard = time.monotonic()

    def key(self, k, down):
        self.send(mp.KEY, mp.key(k + 1, down, self.ms()))

    def poll(self):
        """Read whatever has come; True if the picture changed."""
        if not self.sock and time.monotonic() - self.tried > 1.0:
            self.tried = time.monotonic()
            self.connect()
        if self.sock:
            try:
                while True:
                    n = 1 << 16
                    if self.link:
                        t = time.monotonic()
                        self.budget = min(self.budget + (t - self.budget_t) * self.link, self.link * 0.01 + 4096)
                        self.budget_t = t
                        n = int(self.budget)
                        if n < 64:
                            break
                    data = self.sock.recv(n)
                    if self.link:
                        self.budget -= len(data)
                    if not data:
                        self.close()
                        break
                    self.heard = time.monotonic()
                    bad = self.reader.bad
                    for m in self.reader.feed(data):
                        self.handle(*m)
                    self.stats.invalid += self.reader.bad - bad
            except BlockingIOError:
                pass
            except OSError:
                self.close()
        changed, self.dirty = self.dirty, False
        return changed

    def ask_sync(self):
        t = time.monotonic()
        if t - self.sync_at > 0.25:                 # once a quarter second while out of step
            self.sync_at = t
            self.stats.syncs += 1
            self.send(mp.SYNC, ms.sync(self.dec.display or 0, self.dec.why or 0, self.dec.frame))

    def handle(self, mtype, seq, p):
        if mtype == mp.HELLO_Q:
            self.send(mp.HELLO, mp.hello("mfd", self.uid, "mfdrecv 1", 8, 8, 8, W, H, mp.SCREEN_GREY,
                                         ms.CAP_DEFLATE if self.deflate else 0, ms.MAX_BODY))
            self.dec = ms.Decoder(None, W, H, deflate_ok=self.deflate)   # a fresh start
            self.dirty = True
        elif mtype == mp.PING:
            self.send(mp.PONG, struct.pack(">I", self.ms() & 0xFFFFFFFF))
        elif mtype == mp.LED and len(p) >= 2:
            if p[0] < 8:
                self.levels[p[0]] = p[1]
        elif mtype == mp.LEDS and p:
            for i, level in enumerate(p[1:]):
                if p[0] + i < 8:
                    self.levels[p[0] + i] = level
        elif mtype == mp.CLEAR:
            self.labels.clear()
        elif mtype == mp.TEXT:
            field, x, y, size, attr, text = mp.parse_text(p)
            if 101 <= field <= 108:                 # a key's label
                self.labels[field - 101] = text
        elif mtype == mp.FRAME:
            self.frame(p)

    def frame(self, p):
        if self.record:
            self.record.write(struct.pack(">I", len(p)) + p)
        if self.part_t is None:
            self.part_t = time.perf_counter()
        t0 = time.perf_counter()
        res = self.dec.feed(p)
        dt = (time.perf_counter() - t0) * 1000
        if res in ("part", "frame"):
            self.stats.part(len(p), rect_pixels(p))
            self._decode = getattr(self, "_decode", 0.0) + dt
        if res == "frame":
            h = ms.parse_header(p)
            lat = ((host_us() - h["time_us"]) & 0xFFFFFFFF) / 1000.0
            self.stats.done(bool(h["flags"] & ms.F_KEY), self._decode, lat if lat < 10000 else None)
            self.send(mp.FRAME_ACK, ms.frame_ack(self.dec.display, self.dec.frame, int(self._decode * 1000)))
            self._decode = 0.0
            self.part_t = None
            self.dirty = True
        elif res == "sync":
            self.stats.invalid += self.dec.why in (ms.SYNC_BAD, ms.SYNC_SIZE)
            self._decode = 0.0
            self.part_t = None
            self.ask_sync()
        elif res == "ignored":
            self.stats.ignored += 1


class Window:
    def __init__(self, rx, a):
        import tkinter as tk
        self.tk = tk
        self.rx, self.a = rx, a
        self.s = a.scale
        self.root = tk.Tk()
        self.root.title("MFD %s" % rx.uid)
        self.root.configure(bg="#202024")
        sw, sh = int(W * self.s), int(H * self.s)
        kh = max(int(KEY_H * min(self.s, 1)), 24)
        self.sw, self.sh, self.kh = sw, sh, kh
        self.c = tk.Canvas(self.root, width=sw, height=sh + 2 * kh, bg="#202024", highlightthickness=0)
        self.c.pack()
        if a.pos:
            self.root.geometry("+%s+%s" % tuple(a.pos.split(",")))
        self.keys, self.key_text = [], []
        for i in range(8):
            col, top = i % 4, i < 4
            x0 = col * sw // 4 + 6
            y0 = 4 if top else sh + kh + 4
            self.keys.append(self.c.create_rectangle(x0, y0, x0 + sw // 4 - 12, y0 + kh - 8,
                                                     fill="#303036", outline="#606068", width=2))
            self.key_text.append(self.c.create_text(x0 + (sw // 4 - 12) // 2, y0 + (kh - 8) // 2, text="",
                                                    fill="#e8e8f0", font=("Consolas", 9, "bold")))
        self.img = tk.PhotoImage(width=W, height=H)
        self.shown = self.img
        self.pic = self.c.create_image(0, kh, image=self.img, anchor="nw")
        self.status = self.c.create_text(sw // 2, kh + sh // 2, text="NO LINK", fill="#ff5040",
                                         font=("Consolas", 18, "bold"))
        self.info = self.c.create_text(4, kh + sh - 4, text="", anchor="sw", fill="#c0c040",
                                       font=("Consolas", 9), state="hidden")
        self.lut = None
        if a.tint:
            r, g, b = (int(a.tint[i:i + 2], 16) for i in (0, 2, 4))
            self.lut = [bytes(v * c // 255 for v in range(256)) for c in (r, g, b)]
        self.c.bind("<ButtonPress-1>", lambda e: self.click(e, True))
        self.c.bind("<ButtonRelease-1>", lambda e: self.click(e, False))
        for k in range(8):
            self.root.bind("<KeyPress-%d>" % (k + 1), lambda e, k=k: self.key(k, True))
            self.root.bind("<KeyRelease-%d>" % (k + 1), lambda e, k=k: self.key(k, False))
        self.root.bind("<F2>", lambda e: self.c.itemconfigure(
            self.info, state="normal" if self.c.itemcget(self.info, "state") == "hidden" else "hidden"))
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self.held, self.down = None, set()
        self.drawn = 0.0
        self.root.after(5, self.poll)

    def quit(self):
        write_stats(self.rx, self.a)
        self.root.destroy()

    def key_at(self, x, y):
        col = min(int(x) * 4 // self.sw, 3)
        if y < self.kh:
            return col
        if y > self.kh + self.sh:
            return 4 + col
        return None

    def click(self, e, down):
        k = self.key_at(e.x, e.y) if down else self.held
        if k is None:
            return
        self.held = k if down else None
        self.key(k, down)

    def key(self, k, down):
        if down == (k in self.down):
            return
        (self.down.add if down else self.down.discard)(k)
        self.rx.key(k, down)

    def draw(self):
        fb = bytes(self.rx.dec.fb)
        if self.lut:
            px = bytearray(W * H * 3)
            px[0::3] = fb.translate(self.lut[0])
            px[1::3] = fb.translate(self.lut[1])
            px[2::3] = fb.translate(self.lut[2])
            data = b"P6 %d %d 255\n" % (W, H) + bytes(px)
        else:
            data = b"P5 %d %d 255\n" % (W, H) + fb
        self.img.configure(data=data, format="PPM")
        if self.s != 1:
            if self.s >= 1:
                self.shown = self.img.zoom(int(self.s))
            else:
                self.shown = self.img.subsample(int(round(1 / self.s)))
            self.c.itemconfigure(self.pic, image=self.shown)

    def poll(self):
        rx = self.rx
        if rx.poll() and time.monotonic() - self.drawn > 1 / 60:
            self.drawn = time.monotonic()
            self.draw()
        linked = rx.linked()
        self.c.itemconfigure(self.status, text="NO LINK" if not linked else "SYNC",
                             state="hidden" if linked and rx.dec.synced else "normal")
        for k in range(8):
            lvl = rx.levels[k] if linked else 0
            g = 48 + lvl * 207 // 255
            fill = "#%02x%02x%02x" % (g * 3 // 4, g, g // 3) if lvl else "#303036"
            self.c.itemconfigure(self.keys[k], fill=fill, outline="#ffffff" if k in self.down else "#606068")
            self.c.itemconfigure(self.key_text[k], text=rx.labels.get(k, ""))
        if self.c.itemcget(self.info, "state") == "normal":
            s = rx.stats
            lat = s.latency_ms[-20:]
            self.c.itemconfigure(self.info, text="display %s frame %d  %.1f KB/s  %d updates  latency %s ms  resyncs %d  bad %d"
                                 % (rx.dec.display or "-", rx.dec.frame, s.rate() / 1024, s.frames,
                                    "%.1f" % (sum(lat) / len(lat)) if lat else "-", s.syncs, s.invalid))
        stats_tick(rx, self.a)
        if self.a.seconds and time.monotonic() - rx.stats.t0 > self.a.seconds:
            self.quit()
            return
        self.root.after(5, self.poll)

    def run(self):
        self.root.mainloop()


def write_stats(rx, a):
    if not a.stats:
        return
    out = dict(uid=rx.uid, display=rx.dec.display, synced=rx.dec.synced, frame=rx.dec.frame, **rx.stats.summary())
    with open(a.stats, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)


def stats_tick(rx, a):
    t = time.monotonic()
    if a.stats and t - getattr(rx, "_stats_at", 0) > 5:
        rx._stats_at = t
        write_stats(rx, a)


def run_headless(rx, a):
    while not a.seconds or time.monotonic() - rx.stats.t0 < a.seconds:
        rx.poll()
        stats_tick(rx, a)
        time.sleep(0.002)
    write_stats(rx, a)


def main():
    ap = argparse.ArgumentParser(description="a virtual MFD receiver")
    ap.add_argument("--uid", default="vmfd-1")
    ap.add_argument("--bridge", default="127.0.0.1:5181")
    ap.add_argument("--scale", type=float, default=1)
    ap.add_argument("--tint", help="RRGGBB: colour the grey, e.g. 60ff80")
    ap.add_argument("--pos", help="X,Y of the window")
    ap.add_argument("--no-deflate", action="store_true", help="do not offer DEFLATE")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--stats", help="write statistics (JSON) here")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long")
    ap.add_argument("--record", help="append every FRAME to this file")
    ap.add_argument("--link-kbps", type=float, default=0, help="read no faster than this (KB/s), like a USB link")
    ap.add_argument("--all", action="store_true", help="start five, vmfd-1 .. vmfd-5")
    a = ap.parse_args()
    if a.all:
        procs = []
        for i in range(1, 6):
            cmd = [sys.executable, os.path.abspath(__file__), "--uid", "vmfd-%d" % i, "--bridge", a.bridge,
                   "--scale", str(a.scale if a.scale != 1 else 0.5),
                   "--pos", "%d,%d" % ((i - 1) * int(W * (a.scale if a.scale != 1 else 0.5) + 16), 40)]
            for flag, v in (("--tint", a.tint), ("--seconds", a.seconds or None), ("--link-kbps", a.link_kbps or None)):
                if v:
                    cmd += [flag, str(v)]
            if a.no_deflate:
                cmd.append("--no-deflate")
            if a.headless:
                cmd.append("--headless")
            for flag, path, dflt in (("--stats", a.stats, ".json"), ("--record", a.record, ".mfds")):
                if path:
                    root, ext = os.path.splitext(path)
                    cmd += [flag, "%s-%d%s" % (root, i, ext or dflt)]
            procs.append(subprocess.Popen(cmd))
        for p in procs:
            p.wait()
        return 0
    rx = Receiver(a.uid, a.bridge, not a.no_deflate, a.record, a.link_kbps)
    if a.headless:
        run_headless(rx, a)
    else:
        Window(rx, a).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
