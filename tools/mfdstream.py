#!/usr/bin/env python3
"""MFD display streaming: docs/MFD_STREAMING_PROTOCOL.md.

The reference for the FRAME message's payload - the encoder the shim's C
mirrors (firestorm/src/mfdstream.c) and the decoder a module runs (the
virtual receiver tools/mfdrecv.py today, ESP32-S3 firmware later). A frame
is 8-bit grey, one byte a pixel, row by row; an update is a list of
rectangles of it, each coded by itself:

  RAW   the pixels
  RLA   runs, literals and copies from the row above (the framebuffer's
        row above - which the receiver already holds - so a rectangle needs
        no context of its own)

and a part's rectangles may be DEFLATE-compressed as a whole, for a module
that says it can inflate. Standard library only (zlib for DEFLATE).

  tools/mfdstream.py --selftest
"""
import os
import random
import struct
import sys
import zlib

STREAM_VERSION = 1

# FRAME flags
F_KEY = 0x01        # a whole frame: needs nothing before it
F_END = 0x02        # the update's last part
F_DEFLATE = 0x04    # the rectangles are one raw DEFLATE stream

# rectangle codecs
C_RAW = 0
C_RLA = 1

# SYNC reasons
SYNC_NEW, SYNC_GAP, SYNC_BAD, SYNC_SIZE, SYNC_STREAM = 0, 1, 2, 3, 4
SYNC_REASONS = {0: "new", 1: "gap", 2: "bad data", 3: "wrong size", 4: "new stream"}

# HELLO's capability bytes after the screen kind (docs/MODULE_PROTOCOL.md)
SCREEN_GREY = 3     # an 8-bit grey framebuffer, fed by FRAME
CAP_DEFLATE = 0x01

HEADER = struct.Struct(">BBBBIIHHHI")     # ver display flags 0 stream frame part w h time_us
RECT = struct.Struct(">HHHHBH")           # x y w h codec length
MAX_BODY = 4096     # a part's rectangles, before DEFLATE: what a module must buffer
MAX_RUN = 8192


# ------------------------------------------------------------------ RLA

def rla_encode(fb, stride, x, y, w, h):
    """The rectangle (x, y, w, h) of fb, as RLA. Copies from above read row
    y-1 of fb as it will be when the receiver gets here - the caller sees to
    that by sending rectangles top to bottom."""
    out = bytearray()
    lit = bytearray()

    def put(kind, n):
        n -= 1
        if n < 32:
            out.append(kind << 6 | n)
        else:
            out.append(kind << 6 | 0x20 | n >> 8)
            out.append(n & 0xFF)

    def flush():
        j = 0
        while j < len(lit):
            k = min(len(lit) - j, MAX_RUN)
            put(0, k)
            out.extend(lit[j:j + k])
            j += k
        lit.clear()

    # A rectangle's pixels in order, and each one's neighbour above.
    px = bytearray()
    up = bytearray()
    for r in range(y, y + h):
        px += fb[r * stride + x:r * stride + x + w]
        up += fb[(r - 1) * stride + x:(r - 1) * stride + x + w] if r > 0 else bytes(w)
    n, i, prev = len(px), 0, 0
    can_up = y > 0
    while i < n:
        run = 0
        while i + run < n and px[i + run] == prev and run < MAX_RUN:
            run += 1
        a = 0
        while i + a < n and a < MAX_RUN:
            j = i + a
            if j < w and not can_up:                    # the screen's top row: nothing above
                break
            if px[j] != (up[j] if j < w else px[j - w]):
                break
            a += 1
        if a >= 2 and a >= run:
            flush()
            put(2, a)
            i += a
            prev = px[i - 1]
        elif run >= 2 or (run == 1 and not lit):
            flush()
            put(1, run)
            i += run
        else:
            lit.append(px[i])
            prev = px[i]
            i += 1
    flush()
    return bytes(out)


def rla_decode(data, fb, stride, x, y, w, h):
    """Write an RLA rectangle into fb. ValueError if the data does not fill
    it exactly or reaches outside it."""
    n, i, at, prev = w * h, 0, 0, 0
    while at < n:
        if i >= len(data):
            raise ValueError("RLA: data ends early")
        b = data[i]
        i += 1
        kind, m = b >> 6, b & 0x1F
        if b & 0x20:
            if i >= len(data):
                raise ValueError("RLA: data ends early")
            m = m << 8 | data[i]
            i += 1
        m += 1
        if kind == 3 or at + m > n:
            raise ValueError("RLA: bad token")
        while m:
            r, c = divmod(at, w)
            k = min(m, w - c)                       # to the end of this row
            o = (y + r) * stride + x + c
            if kind == 0:
                if i + k > len(data):
                    raise ValueError("RLA: data ends early")
                fb[o:o + k] = data[i:i + k]
                i += k
            elif kind == 1:
                fb[o:o + k] = bytes([prev]) * k
            else:
                if y + r == 0:
                    raise ValueError("RLA: copy from above the top")
                fb[o:o + k] = fb[o - stride:o - stride + k]
            prev = fb[o + k - 1]
            at += k
            m -= k
    if i != len(data):
        raise ValueError("RLA: data left over")


# ------------------------------------------------------------------ parts

def header(display, flags, stream, frame, part, w, h, time_us):
    return HEADER.pack(STREAM_VERSION, display, flags, 0, stream & 0xFFFFFFFF,
                       frame & 0xFFFFFFFF, part, w, h, time_us & 0xFFFFFFFF)


def parse_header(p):
    if len(p) < HEADER.size:
        raise ValueError("FRAME: short header")
    ver, display, flags, _, stream, frame, part, w, h, t = HEADER.unpack_from(p)
    return dict(version=ver, display=display, flags=flags, stream=stream, frame=frame,
                part=part, width=w, height=h, time_us=t)


def deflate(body):
    c = zlib.compressobj(6, zlib.DEFLATED, -15)
    return c.compress(body) + c.flush()


def inflate(data, limit=MAX_BODY):
    d = zlib.decompressobj(-15)
    out = d.decompress(data, limit)
    if d.unconsumed_tail or not d.eof:
        raise ValueError("FRAME: DEFLATE body too big or cut short")
    return out


def deflate_part(payload):
    """A part, its rectangles DEFLATE-compressed - what the bridge does for a
    module that can inflate. Unchanged if that would not make it smaller."""
    h = parse_header(payload)
    if h["flags"] & F_DEFLATE:
        return payload
    body = deflate(payload[HEADER.size:])
    if len(body) >= len(payload) - HEADER.size:
        return payload
    return bytes(payload[:2]) + bytes([h["flags"] | F_DEFLATE]) + bytes(payload[3:HEADER.size]) + body


class Encoder:
    """One display's stream: the frame last sent, and parts for the next.
    The same algorithm as firestorm/src/mfdstream.c."""

    def __init__(self, display, w=640, h=480, tile=16, stream=None, max_body=MAX_BODY):
        self.display, self.w, self.h, self.tile = display, w, h, tile
        self.stream = stream if stream is not None else random.getrandbits(32)
        self.max_body = max_body
        self.prev = None
        self.frame = 0

    def changed_spans(self, cur):
        """Changed tiles, as runs along each tile row: [(x, y, w, h)]."""
        t, w, h, prev = self.tile, self.w, self.h, self.prev
        spans = []
        for ty in range(0, h, t):
            th = min(t, h - ty)
            row = []
            for tx in range(0, w, t):
                tw = min(t, w - tx)
                same = all(cur[(ty + r) * w + tx:(ty + r) * w + tx + tw] ==
                           prev[(ty + r) * w + tx:(ty + r) * w + tx + tw] for r in range(th))
                row.append(not same)
            tx = 0
            while tx < len(row):
                if row[tx]:
                    e = tx
                    while e < len(row) and row[e]:
                        e += 1
                    x0 = tx * t
                    spans.append((x0, ty, min(e * t, w) - x0, th))
                    tx = e
                else:
                    tx += 1
        return spans

    def rects(self, cur, spans):
        """Each span as a coded rectangle, split until each fits a part."""
        out = []
        limit = self.max_body - RECT.size
        todo = list(spans)
        while todo:
            x, y, w, h = todo.pop(0)
            data = rla_encode(cur, self.w, x, y, w, h)
            codec = C_RLA
            if len(data) >= w * h:                 # noise: RLA would only add to it
                data = b"".join(cur[r * self.w + x:r * self.w + x + w] for r in range(y, y + h))
                codec = C_RAW
            if len(data) > limit:
                if w > self.tile:                   # in two, on a tile edge
                    half = (w // self.tile // 2) * self.tile
                    todo[0:0] = [(x, y, half, h), (x + half, y, w - half, h)]
                else:                               # one tile: by rows
                    half = h // 2
                    todo[0:0] = [(x, y, w, half), (x, y + half, w, h - half)]
                continue
            out.append(RECT.pack(x, y, w, h, codec, len(data)) + data)
        return out

    def parts(self, rects, flags, time_us):
        out, body = [], bytearray()
        for r in rects:
            if body and len(body) + len(r) > self.max_body:
                out.append(body)
                body = bytearray()
            body += r
        out.append(body)
        n = len(out)
        return [header(self.display, flags | (F_END if i == n - 1 else 0), self.stream,
                       self.frame, i, self.w, self.h, time_us) + bytes(b)
                for i, b in enumerate(out)]

    def update(self, cur, time_us=0):
        """Parts for this frame: none if nothing changed (the first frame is
        a key frame)."""
        cur = bytes(cur)
        if self.prev is None:
            self.prev = cur
            self.frame += 1
            return self.key(time_us)
        spans = self.changed_spans(cur)
        self.prev = cur
        if not spans:
            return []
        self.frame += 1
        return self.parts(self.rects(cur, spans), 0, time_us)

    def key(self, time_us=0):
        """The frame last sent, whole - for a receiver that has to start
        again. It keeps the frame's number: it is the same picture."""
        if self.prev is None:
            return []
        t = self.tile
        spans = [(0, y, self.w, min(t, self.h - y)) for y in range(0, self.h, t)]
        return self.parts(self.rects(self.prev, spans), F_KEY, time_us)


# ------------------------------------------------------------------ decoder

class Decoder:
    """A module's side: its framebuffer and where its stream stands. feed()
    takes one FRAME payload and says what came of it:

      "frame"   an update finished: the framebuffer is that frame now
      "part"    a part applied; more to come
      "ignored" a part that is not for here, or nothing new
      "sync"    out of step - ask the host for a key frame (SYNC)
    """

    def __init__(self, display=None, w=640, h=480, deflate_ok=True):
        # With no display given it takes whichever display's key frames come:
        # the host decides which MFD a module shows, not the module.
        self.follow = display is None
        self.display, self.w, self.h = display, w, h
        self.fb = bytearray(w * h)
        self.deflate_ok = deflate_ok
        self.synced = False
        self.stream = None
        self.frame = 0          # the frame the framebuffer holds, when synced
        self.building = None    # (stream, frame, next part, key) while an update is part done
        self.why = SYNC_NEW
        self.errors = 0

    def lost(self, why):
        self.synced = False
        self.building = None
        self.why = why
        return "sync"

    def feed(self, p):
        try:
            h = parse_header(p)
        except ValueError:
            self.errors += 1
            return self.lost(SYNC_BAD)
        if h["version"] != STREAM_VERSION:
            return "ignored"
        if h["display"] != self.display:
            if not (self.follow and h["flags"] & F_KEY and h["part"] == 0):
                return "ignored"
            self.display = h["display"]                 # a key frame for another display: take it
            self.synced, self.building = False, None
        if (h["width"], h["height"]) != (self.w, self.h):
            self.errors += 1
            return self.lost(SYNC_SIZE)
        key = bool(h["flags"] & F_KEY)
        ident = (h["stream"], h["frame"])
        b = self.building
        if b and (b[0], b[1]) == ident and b[2] == h["part"] and b[3] == key:
            pass                                        # the next part
        elif h["part"] == 0 and key:
            pass                                        # a key frame starts: always welcome
        elif h["part"] == 0 and self.synced and h["stream"] == self.stream and \
                h["frame"] == (self.frame + 1) & 0xFFFFFFFF:
            pass                                        # the next update
        elif not key and self.synced and h["stream"] == self.stream and \
                h["frame"] == self.frame and not b:
            return "ignored"                            # one we have
        elif not self.synced and not b:
            return "sync"                               # still waiting for a key frame
        else:
            return self.lost(SYNC_STREAM if h["stream"] != self.stream and not key else SYNC_GAP)
        body = p[HEADER.size:]
        try:
            if h["flags"] & F_DEFLATE:
                if not self.deflate_ok:
                    raise ValueError("DEFLATE not asked for")
                body = inflate(body)
            elif len(body) > MAX_BODY:
                raise ValueError("FRAME: body too big")
            self.apply(body)
        except (ValueError, zlib.error, struct.error):
            self.errors += 1
            return self.lost(SYNC_BAD)
        if h["flags"] & F_END:
            self.building = None
            self.synced, self.stream, self.frame, self.why = True, h["stream"], h["frame"], None
            return "frame"
        self.building = (h["stream"], h["frame"], h["part"] + 1, key)
        if key:
            self.synced = False                         # until the key frame is whole
        return "part"

    def apply(self, body):
        at = 0
        while at < len(body):
            if at + RECT.size > len(body):
                raise ValueError("FRAME: rectangle header cut short")
            x, y, w, h, codec, n = RECT.unpack_from(body, at)
            at += RECT.size
            data = body[at:at + n]
            at += n
            if len(data) != n:
                raise ValueError("FRAME: rectangle data cut short")
            if not w or not h or x + w > self.w or y + h > self.h:
                raise ValueError("FRAME: rectangle outside the screen")
            if codec == C_RAW:
                if n != w * h:
                    raise ValueError("FRAME: raw rectangle of the wrong size")
                for r in range(h):
                    o = (y + r) * self.w + x
                    self.fb[o:o + w] = data[r * w:(r + 1) * w]
            elif codec == C_RLA:
                rla_decode(data, self.fb, self.w, x, y, w, h)
            else:
                raise ValueError("FRAME: unknown codec %d" % codec)


# ------------------------------------------------------------------ messages

def sync(display, why, have):
    return struct.pack(">BBI", display, why, have & 0xFFFFFFFF)


def parse_sync(p):
    return struct.unpack(">BBI", p[:6])


def frame_ack(display, frame, decode_us):
    return struct.pack(">BII", display, frame & 0xFFFFFFFF, min(decode_us, 0xFFFFFFFF))


def parse_frame_ack(p):
    return struct.unpack(">BII", p[:9])


def capabilities(flags, max_body=MAX_BODY):
    """HELLO's trailing bytes for a framebuffer module."""
    return struct.pack(">BH", flags, max_body)


def parse_capabilities(rest):
    if len(rest) >= 3:
        flags, max_body = struct.unpack(">BH", rest[:3])
        return flags, max_body
    return 0, 0


# ------------------------------------------------------------------ selftest

def selftest():
    rnd = random.Random(1)
    w, h = 64, 48
    enc = Encoder(1, w, h, tile=16, stream=7)
    dec = Decoder(1, w, h)
    frame = bytearray(w * h)
    for i in range(30):
        for _ in range(rnd.randint(0, 6)):
            x, y = rnd.randrange(w), rnd.randrange(h)
            n = min(rnd.randint(1, 20), w * h - (y * w + x))
            frame[y * w + x:y * w + x + n] = bytes([rnd.randrange(256)]) * n
        for p in enc.update(frame):
            dec.feed(p)
        assert dec.fb == frame, i
    print("mfdstream selftest: ok")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    print(__doc__)
