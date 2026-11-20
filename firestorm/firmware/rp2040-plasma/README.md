# rp2040-plasma

The pod's plasma sign on a 128x32 HUB75 RGB LED matrix, driven by an RP2040
board (a Waveshare RP2040-Zero, or any RP2040). Windows sees it as a USB
serial port, "Firestorm Plasma", 1209:0004. It takes the game's own sign
protocol (`../../docs/RIO.md`, *The plasma sign*), as the shim passes it on
with every x already moved to the middle of a 128-dot sign (`[plasma] width`
128), and draws it in orange. At power-up, before any bytes arrive, it shows
FIRESTORM.

## Wiring

| HUB75 | R1 | G1 | B1 | R2 | G2 | B2 | A | B | C | D | CLK | LAT | OE | GND |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| RP2040 | GP2 | GP3 | GP4 | GP5 | GP6 | GP7 | GP8 | GP9 | GP10 | GP11 | GP12 | GP13 | GP14 | GND |

The panel needs its own 5 V supply. Its grounds and the board's must be
joined. The panel is 1/16 scan (A-D).

## Building

Arduino IDE 2 or `toolchain/arduino-cli`, with Earle Philhower's RP2040 core,
board *Waveshare RP2040 Zero*, **USB Stack: Adafruit TinyUSB**:

    arduino-cli compile --fqbn rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb --output-dir bin .

To flash a board for the first time, hold BOOT while you plug it in (or hold
BOOT and tap RESET). Then copy `bin/rp2040-plasma.ino.uf2` to the RPI-RP2
drive. After that, an upload resets the board on its own.

Tuning, at the top of the sketch: `RED_ON` sets the brightness, `GREEN_ON`
sets how orange the orange is, and `CLK_NOPS` slows the clock for long wires.

## With the game

The shim finds the board on its own (`[plasma] device`, `auto` by default:
the first 1209:0004 port that opens). It sends the board everything the
game writes to its sign. When the board is found, or plugged in again
mid-session (the shim looks every 3 s), it is sent the sign as it stands.
To use a particular port instead, set `device = COMn` in shim.ini; `0`
turns it off. The pod's log says `plasma: the sign on COMn` when it finds
the board.

## Trying it

    python plasmatest.py "OSPREY"
    python plasmatest.py --match
    python plasmatest.py --all
