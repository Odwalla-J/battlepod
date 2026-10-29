#!/usr/bin/env python3
"""A virtual MFD: a 320x240 screen and eight lit keys in a window, speaking
docs/MODULE_PROTOCOL.md to tools/bridge.py over TCP. The bridge cannot tell
it from the RP2040 module it stands in for.

Click a key, or press 1-8 (1-4 the keys above the screen, 5-8 below). With
no word from the bridge for two seconds it shows NO LINK and dims its keys,
as the hardware will, and it keeps trying to reconnect.

usage: tools/vmfd.py [--uid vmfd-C] [--bridge 127.0.0.1:5181] [--scale 2]
Standard library only (tkinter).
"""
import argparse
import os
import socket
import sys
import time
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modproto as mp  # noqa: E402

W, H = 320, 240
KEY_H = 34


class VMFD:
    def __init__(self, a):
        self.a = a
        self.s = a.scale
        host, port = a.bridge.rsplit(":", 1)
        self.addr = (host, int(port))
        self.sock = None
        self.reader = mp.Reader()
        self.seq = 0
        self.heard = 0.0
        self.t0 = time.monotonic()
        self.levels = [0] * 8
        self.items = {}         # field -> canvas items

        self.root = tk.Tk()
        self.root.title("MFD %s" % a.uid)
        self.root.configure(bg="#202024")
        s = self.s
        self.c = tk.Canvas(self.root, width=W * s, height=(H + 2 * KEY_H) * s,
                           bg="#202024", highlightthickness=0)
        self.c.pack()
        self.keys = []
        for i in range(8):
            col, top = i % 4, i < 4
            x0 = (col * W // 4 + 6) * s
            y0 = (4 if top else H + KEY_H + 4) * s
            r = self.c.create_rectangle(x0, y0, x0 + (W // 4 - 12) * s, y0 + (KEY_H - 8) * s,
                                        fill="#303036", outline="#606068", width=2)
            self.keys.append(r)
        self.screen = self.c.create_rectangle(0, KEY_H * s, W * s, (KEY_H + H) * s,
                                              fill="#000000", outline="#404048")
        self.status = self.c.create_text(W * s // 2, (KEY_H + H // 2) * s, text="NO LINK",
                                         fill="#ff5040", font=("Consolas", 12 * s, "bold"))
        self.c.bind("<ButtonPress-1>", lambda e: self.click(e, True))
        self.c.bind("<ButtonRelease-1>", lambda e: self.click(e, False))
        for k in range(8):
            self.root.bind("<KeyPress-%d>" % (k + 1), lambda e, k=k: self.key(k, True))
            self.root.bind("<KeyRelease-%d>" % (k + 1), lambda e, k=k: self.key(k, False))
        self.held = None
        self.down = set()
        self.root.after(20, self.poll)

    # ---- keys

    def key_at(self, x, y):
        s = self.s
        col = min(int(x / s) * 4 // W, 3)
        if y < KEY_H * s:
            return col
        if y > (KEY_H + H) * s:
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
            return                      # the keyboard's auto-repeat
        (self.down.add if down else self.down.discard)(k)
        self.send(mp.KEY, mp.key(k + 1, down, int((time.monotonic() - self.t0) * 1000)))
        self.draw_key(k)

    def draw_key(self, k):
        lvl = self.levels[k] if self.linked() else 0
        g = 48 + lvl * 207 // 255
        fill = "#%02x%02x%02x" % (g * 3 // 4, g, g // 3) if lvl else "#303036"
        self.c.itemconfigure(self.keys[k], fill=fill,
                             outline="#ffffff" if k in self.down else "#606068")

    # ---- the link

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
        self.sock = s
        self.heard = time.monotonic()

    def poll(self):
        if not self.sock and time.monotonic() - getattr(self, "tried", 0) > 1.0:
            self.tried = time.monotonic()
            self.connect()
        if self.sock:
            try:
                while True:
                    data = self.sock.recv(4096)
                    if not data:
                        self.close()
                        break
                    self.heard = time.monotonic()
                    for m in self.reader.feed(data):
                        self.handle(*m)
            except BlockingIOError:
                pass
            except OSError:
                self.close()
        self.c.itemconfigure(self.status, state="hidden" if self.linked() else "normal")
        for k in range(8):
            self.draw_key(k)
        self.root.after(20, self.poll)

    def handle(self, mtype, seq, p):
        s = self.s
        if mtype == mp.HELLO_Q:
            self.send(mp.HELLO, mp.hello("mfd", self.a.uid, "vmfd 0", 8, 8, 8, W, H,
                                         mp.SCREEN_COLOUR))
        elif mtype == mp.PING:
            self.send(mp.PONG, int((time.monotonic() - self.t0) * 1000).to_bytes(4, "big"))
        elif mtype == mp.LED:
            ch, level = p[0], p[1]
            if ch < 8:
                self.levels[ch] = level
        elif mtype == mp.LEDS:
            for i, level in enumerate(p[1:]):
                if p[0] + i < 8:
                    self.levels[p[0] + i] = level
        elif mtype == mp.CLEAR:
            for items in self.items.values():
                for it in items:
                    self.c.delete(it)
            self.items.clear()
        elif mtype == mp.TEXT:
            field, x, y, size, attr, text = mp.parse_text(p)
            for it in self.items.pop(field, []):
                self.c.delete(it)
            if field > 100:                 # a key's label, drawn on the key
                k = field - 101
                if 0 <= k < 8:
                    x0, y0, x1, y1 = self.c.coords(self.keys[k])
                    self.items[field] = [self.c.create_text(
                        (x0 + x1) / 2, (y0 + y1) / 2, text=text, fill="#e8e8f0",
                        font=("Consolas", 7 * s, "bold"))]
                return
            colour = "#ffc040" if size >= 3 else "#60ff80"
            self.items[field] = [self.c.create_text(
                x * s, (KEY_H + y) * s, text=text, anchor="nw", fill=colour,
                font=("Consolas", (5 + 4 * size) * s))]
        elif mtype == mp.BAR:
            field, x, y, w, h, value, mx = mp.parse_bar(p)
            for it in self.items.pop(field, []):
                self.c.delete(it)
            y0 = (KEY_H + y) * s
            frame = self.c.create_rectangle(x * s, y0, (x + w) * s, y0 + h * s, outline="#406048")
            fill = self.c.create_rectangle(x * s, y0, (x + w * value // max(mx, 1)) * s,
                                           y0 + h * s, fill="#40c0ff", width=0)
            self.items[field] = [frame, fill]
        self.c.tag_raise(self.status)

    def run(self):
        self.root.mainloop()


def main():
    ap = argparse.ArgumentParser(description="a virtual MFD")
    ap.add_argument("--uid", default="vmfd-C")
    ap.add_argument("--bridge", default="127.0.0.1:5181")
    ap.add_argument("--scale", type=int, default=2)
    VMFD(ap.parse_args()).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
