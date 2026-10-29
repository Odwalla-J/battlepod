#!/usr/bin/env python3
"""The smallest cockpit bridge for battlepod's --rio-port.

Listens where the pod sends its Remote I/O link, prints what the pod lights
and writes as it happens, and sends the pod panel reports typed on stdin:

    press ID / release ID / tap ID     button ID (hex): A5 trigger, 40 target
    analog ID VALUE                    analog ID (hex) to VALUE: A0 throttle
                                       (0 to 0x340), A1/A2 stick left/right
    quit

usage: tools/riobridge.py [--port 5180] [--quiet]
then:  cockpit.exe ... --rio-port 127.0.0.1:5180

The datagrams are docs/api.md, "The cockpit's port"; the stream inside them
is docs/COCKPIT_INTERFACE.md, "Every output opcode". Standard library only.
"""
import argparse
import socket
import sys
import threading

RAW_LEN = {0xD1: 10, 0xD2: 3, 0xD3: 3, 0xD4: 4,
           0xD5: 1, 0xD6: 1, 0xD7: 1, 0xD8: 1, 0xD9: 1, 0xDA: 1}


def frame(data):
    """A report, framed as the panel board frames it (tools/hub.py's rio)."""
    d = bytes(data)
    return bytes([1, 0, len(d), len(d)]) + d + bytes([sum(d) & 0xFF])


def describe(p):
    op = p[0]
    if op == 0xD1:
        return "display %02X %r" % (p[1], bytes(p[2:10]).decode("latin-1"))
    if op == 0xD2:
        return "bargraph %02X %d" % (p[1], p[2])
    if op == 0xD3:
        return "lamp %02X %d" % (p[1], p[2])
    if op == 0xD4:
        return "field %02X %r" % (p[1], bytes(p[2:4]).decode("latin-1"))
    return "%02X" % op


class Walker:
    """src/rio.h's rio_walk, over a stream that arrives in pieces."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out, at, b = [], 0, self.buf
        while at < len(b):
            n = RAW_LEN.get(b[at])
            if n:
                if at + n > len(b):
                    break
                out.append(describe(b[at:at + n]))
                at += n
                continue
            if b[at] != 0x01:
                at += 1
                continue
            if at + 4 > len(b) or at + 5 + b[at + 2] > len(b):
                break
            ln = b[at + 2]
            out.append(describe(b[at + 4:at + 4 + ln]) + " (framed)")
            at += 5 + ln
        del self.buf[:at]
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=5180)
    ap.add_argument("--quiet", action="store_true", help="do not print the stream")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", a.port))
    pod = [None]
    walker = Walker()
    print("riobridge: listening on udp://127.0.0.1:%d" % a.port, flush=True)

    def reader():
        while True:
            data, addr = s.recvfrom(4096)
            if not data:
                continue
            if pod[0] != addr:
                pod[0] = addr
                s.sendto(b"H", addr)        # the stream again, from the start
                print("riobridge: pod at %s:%d" % addr, flush=True)
            if data[:1] == b"H":
                s.sendto(b"A", addr)
            elif data[:1] == b"r" and not a.quiet:
                for line in walker.feed(data[1:]):
                    print(line, flush=True)

    threading.Thread(target=reader, daemon=True).start()
    for line in sys.stdin:
        f = line.split()
        if not f:
            continue
        if f[0] == "quit":
            break
        if pod[0] is None:
            print("riobridge: no pod yet", flush=True)
            continue
        try:
            if f[0] in ("press", "release", "tap"):
                b = int(f[1], 16)
                rep = {"press": frame([0xB1, b]), "release": frame([0xB0, b]),
                       "tap": frame([0xB1, b]) + frame([0xB0, b])}[f[0]]
            elif f[0] == "analog":
                v = int(f[2], 0)
                rep = frame([0xC0, int(f[1], 16), (v >> 8) & 0xFF, v & 0xFF])
            else:
                print("riobridge: press/release/tap ID, analog ID VALUE, quit", flush=True)
                continue
        except (IndexError, ValueError):
            print("riobridge: bad command", flush=True)
            continue
        s.sendto(b"R" + rep, pod[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
