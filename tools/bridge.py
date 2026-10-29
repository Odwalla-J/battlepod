#!/usr/bin/env python3
"""The cockpit bridge: battlepod's Remote I/O link on one side, cockpit
modules on the other.

    pod (--rio-port) --UDP--> adapter -> model -> layout --> modules
    pod              <--UDP-- B1/B0   <- profile <- KEY  <-- modules

The layers are docs/HARDWARE_MAPPING.md's: the adapter turns the game's
Remote I/O bytes into events and key presses back into reports; the model
holds every name's current state; the profile names the game's ids
(profiles/), the layout puts names on modules (layouts/). Modules speak
docs/MODULE_PROTOCOL.md - over TCP for a virtual one (tools/vmfd.py), over
USB serial for a real one (with --serial, which needs pyserial).

The same modules can fly Firestorm (MW4 5.07 on the Tesla II pod) under
firestorm-shim: --game firestorm talks to the shim's virtual RIO board on
its control port (udp 9100) instead of to battlepod, and
profiles/firestorm-5.07 with layouts/firestorm-mfds puts its five MFDs' keys
and lamps on five modules. One cockpit, either game.

usage:
  tools/bridge.py [--profile profiles/battletech-13.1.8]
                  [--layout layouts/milestone-mfd]
                  [--pod-port 5180] [--module-port 5181] [--serial COM5 ...]
  cockpit.exe ... --rio-port 127.0.0.1:5180
  tools/vmfd.py --uid vmfd-C
  tools/bridge.py --game firestorm      (then vmfd.py --uid vmfd-1 ... -5)

Every id the game sends that the profile does not name is logged once: the
way the remaining unknowns get found during play. A module that goes away
keeps its state in the model and gets all of it back when it returns.
"""
import argparse
import os
import select
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modproto as mp  # noqa: E402

RAW_LEN = {0xD1: 10, 0xD2: 3, 0xD3: 3, 0xD4: 4,
           0xD5: 1, 0xD6: 1, 0xD7: 1, 0xD8: 1, 0xD9: 1, 0xDA: 1}

# 13.1.8 lights a lamp at 0-3; a module's LED takes 0-255.
LAMP_PWM = [0, 40, 110, 255]


def now_ms():
    return int(time.monotonic() * 1000)


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ------------------------------------------------------------------ adapter

class Adapter:
    """src/rio.h's rio_walk, as events: framed and raw, in pieces."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        b, at, out = self.buf, 0, []
        while at < len(b):
            n = RAW_LEN.get(b[at])
            if n:
                if at + n > len(b):
                    break
                out.append(self.event(bytes(b[at:at + n])))
                at += n
                continue
            if b[at] != 0x01:
                at += 1
                continue
            if at + 4 > len(b) or at + 5 + b[at + 2] > len(b):
                break
            ln = b[at + 2]
            out.append(self.event(bytes(b[at + 4:at + 4 + ln])))
            at += 5 + ln
        del self.buf[:at]
        return [e for e in out if e]

    @staticmethod
    def event(p):
        op = p[0]
        if op == 0xD1 and len(p) >= 2:
            return ("text", p[1], p[2:10].decode("latin-1"))
        if op == 0xD2 and len(p) >= 3:
            return ("bar", p[1], p[2])
        if op == 0xD3 and len(p) >= 3:
            return ("lamp", p[1], p[2])
        if op == 0xD4 and len(p) >= 4:
            return ("field", p[1], p[2:4].decode("latin-1"))
        return None


class FirestormAdapter:
    """firestorm-shim's control port: "game 84 ID LEVEL SUM" is a lamp, at
    0x00 off, 0x14 available, 0x3C lit (docs/RIO.md there); presses go back
    as "press ID" / "release ID". Levels become 13.1.8's 0-3."""

    @staticmethod
    def feed(data):
        f = data.decode("latin-1", "replace").split()
        if len(f) >= 4 and f[0] == "game" and f[1] == "84":
            v = int(f[3], 16)
            return [("lamp", int(f[2], 16), 3 if v >= 0x3C else 2 if v >= 0x28 else 1 if v else 0)]
        return []


def report(data):
    """A panel report, framed as the board frames it (tools/hub.py's rio)."""
    d = bytes(data)
    return bytes([1, 0, len(d), len(d)]) + d + bytes([sum(d) & 0xFF])


# ------------------------------------------------------------------ files

def lines(path):
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if line:
                yield n, line.split()


class Profile:
    """Game id -> logical name, and back."""

    def __init__(self, path):
        self.lamp, self.button, self.field, self.bar, self.axis = {}, {}, {}, {}, {}
        self.text = {}          # id -> (name, position in the joined text)
        self.text_ids = {}      # name -> [ids]
        for n, f in lines(path):
            kind = f[0]
            try:
                if kind == "text":
                    ids = [int(x, 16) for x in f[1].split(",")]
                    self.text_ids[f[2]] = ids
                    for i, x in enumerate(ids):
                        self.text[x] = (f[2], i)
                elif kind in ("lamp", "button", "field", "bar"):
                    getattr(self, kind)[int(f[1], 16)] = f[2]
                elif kind == "axis":
                    self.axis[f[2]] = (int(f[1], 16), int(f[3], 0), int(f[4], 0))
                else:
                    raise ValueError(kind)
            except (IndexError, ValueError):
                sys.exit("%s:%d: cannot read %r" % (path, n, " ".join(f)))
        self.button_id = {v: k for k, v in self.button.items()}


class Layout:
    """Logical name -> module and place."""

    def __init__(self, path):
        self.modules = {}       # name -> {"cls", "uid"}
        self.keys = {}          # name -> {index: (logical, label)}
        self.texts = {}         # name -> [(field, x, y, size, logical)]
        self.bars = {}          # name -> [(field, x, y, w, h, max, logical)]
        for n, f in lines(path):
            try:
                if f[0] == "module":
                    self.modules[f[1]] = {"cls": f[2], "uid": f[3]}
                    self.keys[f[1]], self.texts[f[1]], self.bars[f[1]] = {}, [], []
                elif f[1] == "key":
                    self.keys[f[0]][int(f[2])] = (f[3], " ".join(f[4:]))
                elif f[1] == "text":
                    self.texts[f[0]].append((int(f[2]), int(f[3]), int(f[4]), int(f[5]), f[6]))
                elif f[1] == "bar":
                    self.bars[f[0]].append(tuple(int(x) for x in f[2:8]) + (f[8],))
                else:
                    raise ValueError(f[1])
            except (IndexError, ValueError, KeyError):
                sys.exit("%s:%d: cannot read %r" % (path, n, " ".join(f)))

    def module_for(self, uid):
        for name, m in self.modules.items():
            if m["uid"] == uid or name == uid:
                return name
        return None


# ------------------------------------------------------------------ modules

class Module:
    def __init__(self, link, write, what):
        self.link, self.write, self.what = link, write, what
        self.reader = mp.Reader()
        self.seq = 0
        self.name = None        # the layout's name, once it has said HELLO
        self.info = None
        self.heard = time.monotonic()
        self.pinged = 0.0

    def send(self, mtype, payload=b""):
        self.seq = (self.seq + 1) & 0xFF
        try:
            self.write(mp.pack(mtype, self.seq, payload))
        except OSError:
            pass


class Bridge:
    def __init__(self, a):
        self.profile = Profile(a.profile)
        self.layout = Layout(a.layout)
        self.adapter = Adapter()
        self.state = {}         # logical name -> lamp level, text, bar count
        self.texts = {}         # id -> 8 characters, for the joined texts
        self.unbound = set()
        self.pressed = {}       # button id -> when its key went down, for latency
        self.modules = []
        self.pod = None
        self.pod_heard = False
        self.verbose = a.verbose

        self.game = a.game
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if self.game == "firestorm":
            # The shim answers whoever spoke last; say hello now and then.
            self.adapter = FirestormAdapter()
            self.udp.bind(("127.0.0.1", 0))
            self.pod = ("127.0.0.1", a.firestorm_port)
            self.hello_at = 0.0
        else:
            self.udp.bind(("127.0.0.1", a.pod_port))
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(("127.0.0.1", a.module_port))
        self.tcp.listen(8)
        self.serials = []
        for port in a.serial or []:
            self.open_serial(port)
        log("bridge: %s on udp://127.0.0.1:%d, modules on tcp://127.0.0.1:%d"
            % ("firestorm-shim" if self.game == "firestorm" else "pod",
               a.firestorm_port if self.game == "firestorm" else a.pod_port, a.module_port))

    # ---- the pod side

    def from_pod(self, data, addr):
        if self.game == "firestorm":
            if not self.pod_heard:
                log("bridge: firestorm-shim answered")
            self.pod_heard = True
            for ev in self.adapter.feed(data):
                self.game_event(*ev)
            return
        if self.pod != addr:
            # A pod new to us: ask it for the stream from the start, so the
            # panel it lit before we were here is lit here too.
            self.pod = addr
            self.adapter = Adapter()
            self.udp.sendto(b"H", addr)
            log("bridge: pod at %s:%d" % addr)
        if data[:1] == b"H":
            self.udp.sendto(b"A", addr)
        elif data[:1] == b"r":
            for ev in self.adapter.feed(data[1:]):
                self.game_event(*ev)

    def game_event(self, kind, gid, value):
        p = self.profile
        if kind == "lamp":
            name = p.lamp.get(gid)
            if name and gid in self.pressed:
                log("bridge: key %02X to lamp %02X: %d ms"
                    % (gid, gid, now_ms() - self.pressed.pop(gid)))
        elif kind == "text":
            self.texts[gid] = value
            name = p.text.get(gid, (None,))[0]
            if name:
                value = "".join(self.texts.get(x, " " * 8) for x in p.text_ids[name])
        else:
            name = getattr(p, kind).get(gid)
        if not name:
            if (kind, gid) not in self.unbound:
                self.unbound.add((kind, gid))
                log("bridge: unbound %s %02X = %r" % (kind, gid, value))
            return
        if self.state.get(name) != value:
            self.state[name] = value
            if self.verbose or (name == "message" and value.strip()):  # the pod's narration
                log("bridge: %s %s = %r" % (kind, name, value.strip() if kind == "text" else value))
            for m in self.modules:
                if m.name:
                    self.push(m, name)

    def to_pod(self, data):
        if self.pod:
            self.udp.sendto(b"R" + data, self.pod)

    def press(self, bid, down):
        """A key, to whichever game this is."""
        if self.game == "firestorm":
            self.udp.sendto(("%s %02X" % ("press" if down else "release", bid)).encode(), self.pod)
        else:
            self.to_pod(report([0xB1 if down else 0xB0, bid]))

    # ---- the module side

    def push(self, m, changed=None):
        """Send a module its bindings' state: everything, or one name's."""
        lay = self.layout
        for index, (logical, label) in lay.keys[m.name].items():
            if changed in (None, logical):
                level = self.state.get(logical, 0)
                if isinstance(level, int):
                    m.send(mp.LED, mp.led(index - 1, LAMP_PWM[min(level, 3)]))
        for field, x, y, size, logical in lay.texts[m.name]:
            if changed in (None, logical) and logical in self.state:
                m.send(mp.TEXT, mp.text(field, x, y, size, 0, str(self.state[logical]).rstrip()))
        for field, x, y, w, h, mx, logical in lay.bars[m.name]:
            if changed in (None, logical) and logical in self.state:
                m.send(mp.BAR, mp.bar(field, x, y, w, h, min(int(self.state[logical]), mx), mx))

    def labels(self, m):
        """The soft label over each key, as text fields 100 + index."""
        w = m.info["width"] if m.info else 320
        h = m.info["height"] if m.info else 240
        for index, (logical, label) in self.layout.keys[m.name].items():
            if not label:
                continue
            col = (index - 1) % 4
            y = 4 if index <= 4 else h - 14
            m.send(mp.TEXT, mp.text(100 + index, col * w // 4 + 4, y, 1, 1, label))

    def from_module(self, m, data):
        m.heard = time.monotonic()
        for mtype, seq, p in m.reader.feed(data):
            if mtype == mp.HELLO:
                info = mp.parse_hello(p)
                name = self.layout.module_for(info["uid"])
                m.info = info
                log("bridge: %s says HELLO: %s %s, %d keys, %dx%d, firmware %r -> %s"
                    % (m.what, info["cls"], info["uid"], info["keys"], info["width"],
                       info["height"], info["firmware"], name or "not in the layout"))
                if name:
                    for other in self.modules:
                        if other is not m and other.name == name:
                            other.name = None          # replaced: reconnected
                    m.name = name
                    m.send(mp.CLEAR)
                    self.labels(m)
                    self.push(m)
            elif mtype == mp.KEY and m.name:
                index, down, _ = mp.parse_key(p)
                logical = self.layout.keys[m.name].get(index, (None,))[0]
                bid = self.profile.button_id.get(logical)
                if bid is None:
                    log("bridge: %s key %d is not bound" % (m.name, index))
                    continue
                if down:
                    self.pressed[bid] = now_ms()
                self.press(bid, down)
                if self.verbose:
                    log("bridge: %s key %d %s -> %s %02X" % (m.name, index, "down" if down else "up",
                                                             logical, bid))
            elif mtype == mp.ERROR:
                log("bridge: %s error %d %s" % (m.what, p[0], p[1:].decode("utf-8", "replace")))

    def open_serial(self, port):
        try:
            import serial  # pyserial
        except ImportError:
            sys.exit("--serial needs pyserial: pip install pyserial")
        s = serial.Serial(port, 115200, timeout=0)
        m = Module(s, s.write, port)
        self.serials.append(m)
        self.modules.append(m)
        m.send(mp.HELLO_Q, mp.hello_q())
        log("bridge: opened %s" % port)

    def drop(self, m, why):
        log("bridge: %s (%s) gone: %s" % (m.what, m.name or "unnamed", why))
        self.modules.remove(m)
        if m in self.serials:
            self.serials.remove(m)
        try:
            m.link.close()
        except OSError:
            pass

    # ---- the loop

    def run(self):
        while True:
            socks = [self.udp, self.tcp] + [m.link for m in self.modules if m not in self.serials]
            r, _, _ = select.select(socks, [], [], 0.02)
            for s in r:
                if s is self.udp:
                    try:
                        data, addr = self.udp.recvfrom(4096)
                    except OSError:         # nobody on the port yet (Windows says so)
                        continue
                    if data:
                        self.from_pod(data, addr)
                elif s is self.tcp:
                    c, addr = self.tcp.accept()
                    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    m = Module(c, c.sendall, "tcp %s:%d" % addr)
                    self.modules.append(m)
                    m.send(mp.HELLO_Q, mp.hello_q())
                else:
                    m = next(x for x in self.modules if x.link is s)
                    try:
                        data = s.recv(4096)
                    except OSError as e:
                        data, why = b"", str(e)
                    else:
                        why = "closed"
                    if data:
                        self.from_module(m, data)
                    else:
                        self.drop(m, why)
            for m in list(self.serials):
                try:
                    data = m.link.read(4096)
                except OSError as e:
                    self.drop(m, str(e))
                    continue
                if data:
                    self.from_module(m, data)
            t = time.monotonic()
            # Until the shim answers, every 2 s; then every 10, in case
            # another client (tools/rio.py) has since become its peer.
            if self.game == "firestorm" and t - self.hello_at > (10 if self.pod_heard else 2):
                self.udp.sendto(b"hello", self.pod)
                self.hello_at = t
            for m in list(self.modules):
                if t - m.pinged >= 0.5:
                    m.send(mp.PING)
                    m.pinged = t
                if t - m.heard > 5 and m not in self.serials:
                    self.drop(m, "silent for 5 s")


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="the cockpit bridge")
    ap.add_argument("--game", choices=("battlepod", "firestorm"), default="battlepod")
    ap.add_argument("--profile")
    ap.add_argument("--layout")
    ap.add_argument("--firestorm-port", type=int, default=9100)
    ap.add_argument("--pod-port", type=int, default=5180)
    ap.add_argument("--module-port", type=int, default=5181)
    ap.add_argument("--serial", action="append", help="a module on this serial port")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    fs = a.game == "firestorm"
    a.profile = a.profile or os.path.join(here, "profiles", "firestorm-5.07" if fs else "battletech-13.1.8")
    a.layout = a.layout or os.path.join(here, "layouts", "firestorm-mfds" if fs else "milestone-mfd")
    if a.selftest:
        return selftest(a)
    Bridge(a).run()
    return 0


def selftest(a):
    pr = Profile(a.profile)
    lay = Layout(a.layout)
    assert pr.lamp[0x04] == "func.map" and pr.button_id["func.map"] == 0x04
    assert pr.text_ids["message"] == [0x8C, 0x8D, 0x8E]
    assert lay.module_for("vmfd-C") == "mfd:C"
    assert lay.keys["mfd:C"][2] == ("func.map", "MAP")
    ad = Adapter()
    ev = ad.feed(bytes([0xD1, 0x8C]) + b"MAP DISP" + bytes([0xD3, 0x04]))
    ev += ad.feed(bytes([0x03, 0xD4, 0x8F]) + b"42" + bytes([1, 0, 3, 3, 0xD3, 5, 1, 0xD9]))
    assert ev == [("text", 0x8C, "MAP DISP"), ("lamp", 4, 3), ("field", 0x8F, "42"),
                  ("lamp", 5, 1)], ev
    assert report([0xB1, 0x04]) == bytes([1, 0, 2, 2, 0xB1, 0x04, 0xB5])
    assert FirestormAdapter.feed(b"game 84 34 3C 74") == [("lamp", 0x34, 3)]
    assert FirestormAdapter.feed(b"game 84 12 14 2A") == [("lamp", 0x12, 1)]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fp = Profile(os.path.join(here, "profiles", "firestorm-5.07"))
    fl = Layout(os.path.join(here, "layouts", "firestorm-mfds"))
    # MFD 3's top right key is ARMOR MODE, 0x34: base 0x30, top row base+7..+4.
    assert fp.button_id[fl.keys["mfd:3"][4][0]] == 0x34
    print("bridge selftest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
