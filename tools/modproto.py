#!/usr/bin/env python3
"""The cockpit module protocol, version 0: docs/MODULE_PROTOCOL.md.

A message is type (u8), seq (u8), payload, CRC-16/CCITT-FALSE (big-endian)
over everything before it; COBS-encoded and ended with a zero byte. The same
bytes on a USB serial link and on the TCP socket a virtual module uses.

Imported by tools/bridge.py and tools/vmfd.py; `modproto.py --selftest`
checks the encoding against fixed vectors. Standard library only.
"""
import binascii
import struct
import sys

VERSION = 0

# host -> module
HELLO_Q = 0x01
LED = 0x10
LEDS = 0x11
TEXT = 0x12
BAR = 0x13
CLEAR = 0x14
PING = 0x15
FRAME = 0x16        # part of a picture: docs/MFD_STREAMING_PROTOCOL.md (tools/mfdstream.py)
# module -> host
HELLO = 0x02
KEY = 0x20
PONG = 0x21
ERROR = 0x22
SYNC = 0x23         # send me a whole frame
FRAME_ACK = 0x24    # I have shown this frame

NAMES = {HELLO_Q: "HELLO?", HELLO: "HELLO", LED: "LED", LEDS: "LEDS", TEXT: "TEXT",
         BAR: "BAR", CLEAR: "CLEAR", PING: "PING", FRAME: "FRAME", KEY: "KEY",
         PONG: "PONG", ERROR: "ERROR", SYNC: "SYNC", FRAME_ACK: "FRAME_ACK"}

SCREEN_NONE, SCREEN_MONO, SCREEN_COLOUR, SCREEN_GREY = 0, 1, 2, 3


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE: polynomial 0x1021, initial 0xFFFF, no reflection
    (binascii's crc_hqx is that CRC, in C - frames are kilobytes)."""
    return binascii.crc_hqx(data, crc)


def cobs_encode(data):
    """Each zero-free run, in blocks of at most 254, behind its length + 1."""
    out = bytearray()
    runs = bytes(data).split(b"\x00")
    for run in runs:
        while len(run) >= 254:
            out.append(255)
            out += run[:254]
            run = run[254:]
        out.append(len(run) + 1)
        out += run
    return bytes(out)


def cobs_decode(data):
    out, i, n = bytearray(), 0, len(data)
    while i < n:
        code = data[i]
        if code == 0:
            raise ValueError("zero inside a COBS packet")
        i += 1
        if i + code - 1 > n:
            raise ValueError("COBS block runs off the end")
        out += data[i:i + code - 1]
        i += code - 1
        if code < 255 and i < n:
            out.append(0)
    return bytes(out)


def pack(mtype, seq, payload=b""):
    """One message, ready for the wire: COBS-encoded, zero-terminated."""
    body = bytes([mtype, seq & 0xFF]) + bytes(payload)
    return cobs_encode(body + struct.pack(">H", crc16(body))) + b"\x00"


def unpack(packet):
    """A packet without its zero: (type, seq, payload). ValueError if bad."""
    raw = cobs_decode(packet)
    if len(raw) < 4:
        raise ValueError("short message")
    body, crc = raw[:-2], struct.unpack(">H", raw[-2:])[0]
    if crc16(body) != crc:
        raise ValueError("bad CRC")
    return body[0], body[1], body[2:]


class Reader:
    """Split a byte stream into messages. A bad packet is dropped and
    counted; the next zero byte is where reading starts again."""

    def __init__(self):
        self.buf = bytearray()
        self.bad = 0

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            z = self.buf.find(0)
            if z < 0:
                break
            pkt = bytes(self.buf[:z])
            del self.buf[:z + 1]
            if not pkt:
                continue
            try:
                out.append(unpack(pkt))
            except ValueError:
                self.bad += 1
        return out


# ---- payloads

def _str(s):
    b = s.encode("utf-8")[:255]
    return bytes([len(b)]) + b


def _take_str(p, at):
    n = p[at]
    return p[at + 1:at + 1 + n].decode("utf-8", "replace"), at + 1 + n


def hello_q():
    return bytes([VERSION])


def hello(cls, uid, firmware, keys, leds, levels, width, height, kind, caps=None, max_body=0):
    return (bytes([VERSION]) + _str(cls) + _str(uid) + _str(firmware) +
            struct.pack(">BBBHHB", keys, leds, levels, width, height, kind) +
            (struct.pack(">BH", caps, max_body) if caps is not None else b""))


def parse_hello(p):
    ver = p[0]
    cls, at = _take_str(p, 1)
    uid, at = _take_str(p, at)
    fw, at = _take_str(p, at)
    keys, leds, levels, w, h, kind = struct.unpack(">BBBHHB", p[at:at + 8])
    # A framebuffer module adds its capabilities and the largest FRAME body
    # it takes (docs/MFD_STREAMING_PROTOCOL.md); older modules end here.
    caps, max_body = struct.unpack(">BH", p[at + 8:at + 11]) if len(p) >= at + 11 else (0, 0)
    return dict(version=ver, cls=cls, uid=uid, firmware=fw, keys=keys, leds=leds,
                levels=levels, width=w, height=h, kind=kind, caps=caps, max_body=max_body)


def led(channel, level):
    return bytes([channel, level])


def text(field, x, y, size, attr, s):
    return struct.pack(">BHHBB", field, x, y, size, attr) + s.encode("utf-8")


def parse_text(p):
    field, x, y, size, attr = struct.unpack(">BHHBB", p[:7])
    return field, x, y, size, attr, p[7:].decode("utf-8", "replace")


def bar(field, x, y, w, h, value, maximum):
    return struct.pack(">BHHHHHH", field, x, y, w, h, value, maximum)


def parse_bar(p):
    return struct.unpack(">BHHHHHH", p[:13])


def key(index, down, ms):
    return struct.pack(">BBI", index, 1 if down else 0, ms & 0xFFFFFFFF)


def parse_key(p):
    index, down, ms = struct.unpack(">BBI", p[:6])
    return index, bool(down), ms


def selftest():
    # CRC-16/CCITT-FALSE's check value.
    assert crc16(b"123456789") == 0x29B1
    # COBS: the published examples.
    for raw, enc in ((b"\x00", b"\x01\x01"), (b"\x00\x00", b"\x01\x01\x01"),
                     (b"\x11\x22\x00\x33", b"\x03\x11\x22\x02\x33"),
                     (b"\x11\x22\x33\x44", b"\x05\x11\x22\x33\x44"),
                     (b"\x11\x00\x00\x00", b"\x02\x11\x01\x01\x01")):
        assert cobs_encode(raw) == enc, (raw, cobs_encode(raw))
        assert cobs_decode(enc) == raw
    long = bytes(range(1, 256)) * 2
    assert cobs_decode(cobs_encode(long)) == long
    # A message round trip, through a stream cut anywhere, with noise between.
    msgs = [pack(LED, 1, led(3, 255)), pack(TEXT, 2, text(1, 8, 100, 2, 0, "MAP DISPLAY")),
            pack(KEY, 3, key(2, True, 123456))]
    stream = b"\x00garbage\x00" + b"".join(msgs)
    r, got = Reader(), []
    for i in range(0, len(stream), 3):
        got += r.feed(stream[i:i + 3])
    assert [g[0] for g in got] == [LED, TEXT, KEY], got
    assert r.bad == 1                           # the garbage
    assert parse_text(got[1][2]) == (1, 8, 100, 2, 0, "MAP DISPLAY")
    assert parse_key(got[2][2]) == (2, True, 123456)
    h = parse_hello(hello("mfd", "vmfd-C", "vmfd 0", 8, 8, 4, 320, 240, SCREEN_COLOUR))
    assert h["uid"] == "vmfd-C" and h["keys"] == 8 and h["width"] == 320 and h["caps"] == 0
    h = parse_hello(hello("mfd", "mfd-3", "x", 8, 8, 8, 640, 480, SCREEN_GREY, 1, 4096))
    assert h["kind"] == SCREEN_GREY and h["caps"] == 1 and h["max_body"] == 4096
    # The fast CRC and COBS against the plain loops they replaced.
    import random
    rnd = random.Random(5)

    def crc_ref(data, crc=0xFFFF):
        for b in data:
            crc ^= b << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
                crc &= 0xFFFF
        return crc

    def cobs_ref(data):
        out, block = bytearray(), bytearray()
        for b in data:
            if b == 0:
                out.append(len(block) + 1)
                out += block
                block.clear()
            else:
                block.append(b)
                if len(block) == 254:
                    out.append(255)
                    out += block
                    block.clear()
        out.append(len(block) + 1)
        out += block
        return bytes(out)

    for n in list(range(0, 600)) + [5000]:
        for zeros in (0, 0.01, 0.5):
            d = bytes(0 if rnd.random() < zeros else rnd.randrange(1, 256) for _ in range(n))
            assert cobs_encode(d) == cobs_ref(d), (n, zeros)
            assert cobs_decode(cobs_encode(d)) == d
            assert crc16(d) == crc_ref(d)
    # A flipped bit is caught.
    bad = bytearray(msgs[0])
    bad[2] ^= 0x01
    assert Reader().feed(bytes(bad)) == []
    print("modproto selftest: ok")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    print(__doc__)
