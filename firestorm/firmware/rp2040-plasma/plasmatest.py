"""Write on the plasma sign board (rp2040-plasma) from the command line.

    python plasmatest.py                 a name, large, as the game puts it
    python plasmatest.py "NAME"          another name
    python plasmatest.py --match         a name with the match's place and score boxes
    python plasmatest.py --all           every dot lit (wiring and brightness)
    python plasmatest.py --clear
    python plasmatest.py --boot          the boot sequence again

Finds the board by its USB ids (1209:0004); --port COMn to pick one.
"""
import argparse
import sys

import serial
from serial.tools import list_ports

W = 128
ESC = b"\x1b"


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x1209 and p.pid == 0x0004:
            return p.device
    sys.exit("no Firestorm Plasma board found (1209:0004); give --port")


def at(x, y):
    return ESC + b"R" + bytes([x]) + ESC + b"Q" + bytes([y])


def name(n, small=False):
    n = n[:20 if small else 12].encode("ascii", "replace")
    w = len(n) * (6 if small else 10)
    font = 2 if small else 4
    return ESC + b"K" + bytes([font]) + at(max(0, (W - w) // 2), 10 if small else 8) + n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", nargs="?", default="MECHWARRIOR")
    ap.add_argument("--port")
    ap.add_argument("--match", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--clear", action="store_true")
    ap.add_argument("--boot", action="store_true")
    a = ap.parse_args()

    out = ESC + b"@"
    if a.boot:
        out = ESC + b"b"
    elif a.all:
        out += ESC + b"x\x01" + bytes([0, 0, W - 1, 31])
    elif a.match:
        dx = (W - 112) // 2      # the game's boxes, moved to the middle as the shim moves them
        out += name(a.name, small=True)
        out += ESC + b"X" + bytes([27 + dx, 19, 42 + dx, 30]) + ESC + b"X" + bytes([42 + dx, 19, 93 + dx, 30])
        out += ESC + b"K\x00" + at(32 + dx, 21) + b"1" + at(56 + dx, 21) + b"12345"
    elif not a.clear:
        out += name(a.name)
    with serial.Serial(a.port or find_port(), 9600, timeout=1) as s:
        s.write(out)
    print("sent %d bytes" % len(out))


if __name__ == "__main__":
    main()
