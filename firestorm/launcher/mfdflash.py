#!/usr/bin/env python3
"""Put the MFD firmware (firmware/esp32-mfd) on a board, for the launcher.

A new board - an Elecrow CrowPanel 7" behind its CH340 - runs Elecrow's demo
and is no MFD board until this is on it. This talks to the ESP32-S3's own ROM
bootloader (Espressif's serial protocol, as esptool does, without its stub):
the CH340's DTR and RTS put the chip in its bootloader, each image is sent
DEFLATEd and checked by MD5 against the flash, then the board is reset into
the firmware. It needs only pyserial, which the launcher already carries.

The images are the ones arduino-cli builds (firmware/esp32-mfd/README.md),
kept in firmware/esp32-mfd/bin (launcher\\esp32-mfd in a packaged copy).
Flashing leaves the board's NVS alone, so a board that was MFD n before
still is after.

usage: mfdflash.py [PORT ...]     (no port: list the ports and what is on them)
"""
import hashlib
import os
import struct
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

# Where each image goes, as arduino-cli's upload puts them.
IMAGES = [(0x0000, "bootloader.bin"), (0x8000, "partitions.bin"), (0xE000, "boot_app0.bin"),
          (0x10000, "esp32-mfd.bin")]
FAST = 460800                   # the ROM's speed while flashing; it starts at 115200

# ROM loader commands
SYNC, READ_REG, SPI_SET_PARAMS, SPI_ATTACH, CHANGE_BAUD = 0x08, 0x0A, 0x0B, 0x0D, 0x0F
DEFL_BEGIN, DEFL_DATA, DEFL_END, SPI_MD5 = 0x10, 0x11, 0x12, 0x13
BLOCK = 0x400                   # the ROM's write block
CHIP_MAGIC_REG = 0x40001000     # the ROM's chip-detect word: 9 on an ESP32-S3


def candidates(skip=()):
    """The ports a new board would be on: a WCH USB-serial chip (the CH340
    the Elecrow board carries) - [{"port", "desc"}], less those in skip."""
    if serial is None:
        return []
    return [{"port": p.device, "desc": p.description} for p in sorted(serial.tools.list_ports.comports(),
            key=lambda p: (len(p.device), p.device)) if p.vid == 0x1A86 and p.device not in skip]


def firmware_dir():
    for d in (os.path.join(HERE, "esp32-mfd"),
              os.path.normpath(os.path.join(HERE, "..", "firmware", "esp32-mfd", "bin"))):
        if all(os.path.isfile(os.path.join(d, f)) for _, f in IMAGES):
            return d
    return None


def firmware_version():
    """The FIRMWARE string built into the images ("esp32-mfd 0.4"), or ""."""
    d = firmware_dir()
    if not d:
        return ""
    data = open(os.path.join(d, "esp32-mfd.bin"), "rb").read()
    at = data.find(b"esp32-mfd ")
    if at < 0:
        return ""
    end = data.find(b"\0", at)
    return data[at:end].decode("ascii", "replace")


class FlashError(Exception):
    pass


class Rom:
    """The ESP32-S3's ROM bootloader on a serial port."""

    def __init__(self, port):
        if serial is None:
            raise FlashError("the launcher needs pyserial to flash boards")
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.05
        s.dtr = s.rts = False
        for tries in range(10):         # a hub can drop a port for a moment while others reset
            try:
                s.open()
                break
            except serial.SerialException:
                if tries == 9:
                    raise
                time.sleep(0.5)
        self.ser, self.buf = s, bytearray()

    def close(self):
        try:
            self.ser.close()
        except OSError:
            pass

    # DTR is IO0 (low: the bootloader), RTS is EN (low: reset), through the
    # board's two transistors. Windows sends RTS only when DTR is set too.
    def _lines(self, dtr, rts):
        self.ser.dtr = dtr
        self.ser.rts = rts
        self.ser.dtr = dtr

    def enter_bootloader(self):
        self._lines(False, True)
        time.sleep(0.1)
        self._lines(True, False)
        time.sleep(0.05)
        self._lines(False, False)

    def hard_reset(self):
        self._lines(False, True)
        time.sleep(0.1)
        self._lines(False, False)

    # SLIP framing
    def _write(self, data):
        out = data.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
        self.ser.write(b"\xc0" + out + b"\xc0")

    def _read_packet(self, timeout):
        end = time.time() + timeout
        while True:
            start = self.buf.find(b"\xc0")
            if start >= 0:
                stop = self.buf.find(b"\xc0", start + 1)
                if stop > start:
                    raw = bytes(self.buf[start + 1:stop])
                    del self.buf[:stop]             # the end mark may start the next one
                    if raw:
                        return raw.replace(b"\xdb\xdc", b"\xc0").replace(b"\xdb\xdd", b"\xdb")
                    continue
            if time.time() > end:
                return None
            self.buf += self.ser.read(4096)

    def command(self, op, data=b"", chk=0, timeout=3.0):
        self._write(struct.pack("<BBHI", 0, op, len(data), chk) + data)
        end = time.time() + timeout
        while time.time() < end:
            p = self._read_packet(max(0.01, end - time.time()))
            if p is None:
                break
            if len(p) < 8 or p[0] != 1 or p[1] != op:
                continue
            value, body = struct.unpack("<I", p[4:8])[0], p[8:]
            if len(body) >= 4 and body[-4]:         # the ROM's status: failed, and why
                raise FlashError("the board refused command 0x%02X (error 0x%02X)" % (op, body[-3]))
            return value, body[:-4]
        raise FlashError("the board did not answer command 0x%02X" % op)

    def sync(self):
        for _ in range(7):
            self.ser.reset_input_buffer()
            self.buf.clear()
            try:
                self.command(SYNC, b"\x07\x07\x12\x20" + b"\x55" * 32, timeout=0.2)
                time.sleep(0.05)
                self.ser.reset_input_buffer()       # the other answers to SYNC
                self.buf.clear()
                return True
            except FlashError:
                pass
        return False

    def connect(self):
        for _ in range(4):
            self.enter_bootloader()
            time.sleep(0.05)
            if self.sync():
                return
        raise FlashError("no ESP32 bootloader answers (is it an Elecrow board, and is no other program on the port?)")

    def read_reg(self, addr):
        return self.command(READ_REG, struct.pack("<I", addr))[0]

    def set_baud(self, baud):
        self.command(CHANGE_BAUD, struct.pack("<II", baud, 0))
        self.ser.baudrate = baud
        time.sleep(0.05)
        self.ser.reset_input_buffer()
        self.buf.clear()

    def attach_flash(self, size):
        self.command(SPI_ATTACH, struct.pack("<II", 0, 0))
        self.command(SPI_SET_PARAMS, struct.pack("<IIIIII", 0, size, 0x10000, 0x1000, 0x100, 0xFFFF))

    def write(self, addr, data, progress=None):
        z = zlib.compress(data, 9)
        blocks = (len(z) + BLOCK - 1) // BLOCK
        erase = (len(data) + BLOCK - 1) // BLOCK * BLOCK
        # erasing comes first, inside BEGIN: ~30 s a megabyte at worst
        self.command(DEFL_BEGIN, struct.pack("<IIIII", erase, blocks, BLOCK, addr, 0),
                     timeout=max(5.0, 30.0 * erase / 1e6))
        for i in range(blocks):
            part = z[i * BLOCK:(i + 1) * BLOCK]
            chk = 0xEF
            for b in part:
                chk ^= b
            self.command(DEFL_DATA, struct.pack("<IIII", len(part), i, 0, 0) + part, chk,
                         timeout=max(3.0, 30.0 * BLOCK * 4 / 1e6))
            if progress:
                progress(i + 1, blocks)
        _, body = self.command(SPI_MD5, struct.pack("<IIII", addr, len(data), 0, 0),
                               timeout=max(3.0, 8.0 * len(data) / 1e6))
        got = body[:32].decode("ascii", "replace").lower() if len(body) >= 32 else body[:16].hex()
        if got != hashlib.md5(data).hexdigest():
            raise FlashError("what was written at 0x%X does not read back the same" % addr)


def flash(port, progress=None):
    """Put the bundled firmware on the board on port. progress(text, fraction)."""
    say = progress or (lambda text, f: None)
    d = firmware_dir()
    if not d:
        raise FlashError("the MFD firmware images are not with this launcher")
    images = [(a, open(os.path.join(d, f), "rb").read()) for a, f in IMAGES]
    total = sum(len(x) for _, x in images)
    rom = Rom(port)
    try:
        say("Connecting to the bootloader", 0.0)
        rom.connect()
        magic = rom.read_reg(CHIP_MAGIC_REG)
        if magic != 0x9:
            raise FlashError("this is not an ESP32-S3 (chip word 0x%08X): not an MFD board, left alone" % magic)
        rom.set_baud(FAST)
        rom.attach_flash(4 * 1024 * 1024)
        done = 0
        for addr, data in images:
            rom.write(addr, data, lambda i, n, base=done, size=len(data):
                      say("Writing", (base + size * i / n) / total))
            done += len(data)
        say("Restarting the board", 1.0)
        rom.ser.baudrate = 115200
        rom.hard_reset()
    finally:
        rom.close()


class Flasher:
    """Flashing boards in the background, one thread a port; state() for the page."""

    def __init__(self):
        self.jobs = {}                  # port -> {"text", "fraction", "done", "error"}
        self.lock = threading.Lock()

    @property
    def running(self):
        return any(not j["done"] for j in self.jobs.values())

    def start(self, ports):
        for k, port in enumerate(ports):
            with self.lock:
                if port in self.jobs and not self.jobs[port]["done"]:
                    continue
                self.jobs[port] = {"text": "Waiting", "fraction": 0.0, "done": False, "error": ""}
            threading.Thread(target=self._run, args=(port, k * 1.5), daemon=True).start()

    def _run(self, port, wait):
        job = self.jobs[port]
        time.sleep(wait)                # not every board resetting at once

        def say(text, f):
            job["text"], job["fraction"] = text, round(f, 3)
        try:
            flash(port, say)
            job["text"] = "Flashed"
        except Exception as e:  # noqa: BLE001 - shown on the page, not lost
            job["error"] = str(e)
            job["text"] = "Failed"
        finally:
            job["done"] = True

    def state(self):
        return {p: dict(j) for p, j in sorted(self.jobs.items())}

    def clear(self):
        with self.lock:
            self.jobs = {p: j for p, j in self.jobs.items() if not j["done"]}


def main(argv):
    if serial is None:
        print("needs pyserial")
        return 1
    print("firmware:", firmware_version() or "(images not found)")
    if not argv:
        for p in serial.tools.list_ports.comports():
            print(p.device, p.description)
        return 0
    f = Flasher()
    f.start(argv)
    last = {}
    while f.running:
        for p, j in f.state().items():
            line = "%s: %s %d%%" % (p, j["text"], j["fraction"] * 100)
            if last.get(p) != line:
                print(line, flush=True)
                last[p] = line
        time.sleep(0.5)
    for p, j in f.state().items():
        print(p, j["text"], j["error"])
    return 0 if all(not j["error"] for j in f.state().values()) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
