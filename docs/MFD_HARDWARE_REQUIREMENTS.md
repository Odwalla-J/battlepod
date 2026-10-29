# MFD module hardware requirements

What a physical MFD module has to do, established by the working software:
the shim's exporter, the bridge and the virtual receiver
([MFD_STREAMING_PROTOCOL.md](MFD_STREAMING_PROTOCOL.md),
[MFD_STREAMING_IMPLEMENTATION.md](MFD_STREAMING_IMPLEMENTATION.md)). Five
identical modules, one per MFD, each an ESP32-S3 with PSRAM, a 640 x 480
LCD, eight lit keys and one USB link to the host. No LCD model is chosen
here; this is what any candidate has to meet.

Figures marked **[M]** were measured on this PC with the game or the
synthetic load; **[E]** are estimates, to be measured in Phase 2 on real
hardware.

## What the module does

1. Enumerates as a USB CDC device; answers `HELLO?` with `HELLO` - class
   `mfd`, its unique id (the flash id, also the USB serial string), 8 keys,
   8 LED channels, 640 x 480, screen kind 3, capabilities (bit 0: DEFLATE)
   and a 4096-byte maximum body.
2. Applies `FRAME` parts to its framebuffer as they come (inflating first
   if flagged), checks every part, and on the update's `END` puts the frame
   on the panel and sends `FRAME_ACK`.
3. Sends `SYNC` when out of step, and keeps showing its last good frame.
4. Lights eight keys at 0-255 (`LED`/`LEDS`), reports presses (`KEY`) with
   its millisecond clock, answers `PING`, and shows NO LINK after two silent
   seconds - exactly as the existing module protocol asks.

The decoder is `tools/mfdstream.py`'s `Decoder` (about 150 lines with
`rla_decode`), with no dependencies; the C port is mechanical.

## Memory

| what | size | where |
|---|---|---|
| framebuffer the panel scans, 640 x 480 RGB565 | 614,400 B | PSRAM |
| receive buffer: one COBS packet, a FRAME part | 4.2 KB | SRAM |
| inflate output: one part's body | 4,096 B | SRAM |
| `tinfl` decompressor state (ROM code) | ~11 KB | SRAM |
| DEFLATE window | none: each part is its own stream and fits the output buffer | - |
| grey-to-panel table | 512 B (256 x RGB565) | SRAM |
| RGB-interface bounce buffers, if the panel is RGB-parallel | 2 x 640 x 10 x 2 = 25.6 KB | SRAM |

**The decoder can write the panel's format directly**: every RLA operation
is "this value", "the previous pixel again" or "the pixel above", so with
literals mapped through the 256-entry table on the way in, runs and copies
work on RGB565 pixels exactly as on grey ones. No separate 8-bit framebuffer
is needed (300 KB saved), unless the firmware wants one for tear-free
double buffering.

**Minimum: 2 MB PSRAM; recommended 8 MB** (an N16R8-class module), which
also leaves room for a second framebuffer (another 614 KB) so a frame is
assembled off-screen and swapped on `END` - no half-applied update ever
shows, even when a part is lost.

## LCD interface

The ESP32-S3 offers two ways to drive a 640 x 480 panel:

| | RGB parallel (`esp_lcd_rgb`) | i80/8080 parallel to a controller with its own RAM |
|---|---|---|
| panels | most 640 x 480 TFTs without a controller | SSD1963-class controller boards |
| framebuffer | in PSRAM, scanned out continuously | in the controller; the ESP32 writes only changed rectangles |
| PSRAM load | 640 x 480 x 2 B x 60 Hz = **36.9 MB/s** of scan-out, all the time, competing with decoding | none for scan-out |
| pins | 16 data + PCLK, HSYNC, VSYNC, DE = **20** | 16 data + WR, RD, DC, CS, RST = **21** (8-bit bus: 13, at half the write speed) |
| tearing | free-running; bounce buffers needed at this size | write a rectangle while the controller scans: possible tearing unless TE is used |
| risk | **the main risk**: the S3's documented limit on PCLK from PSRAM; 640 x 480 at 60 Hz needs ~25 MHz. Lower refresh (40-50 Hz) or bounce buffers are the known remedies | controller board availability at 640 x 480; write speed is ample (a whole frame at 16-bit i80 ~20 MHz takes ~15 ms) |

The MFD pictures are 8-bit grey; a monochrome or RGB666/565 panel is
equally fine - no colour depth beyond 6 bits a channel is used.
**Recommendation: prototype both in Phase 2**; the protocol is the same,
and only the "put the rectangle on the panel" step differs.

## USB bandwidth

Native USB-OTG, Full Speed (12 Mbit/s), TinyUSB CDC. Usable CDC throughput
is roughly **0.8-1 MB/s [E]**.

Measured traffic per module **[M]** (bytes as they reach the module, DEFLATE on):

| | typical | worst seen |
|---|---|---|
| idle, walking (game) | 0-5 KB/s | 9.5 KB/s peak (MFD 2) |
| a mission with readouts moving (game) | 0-25 KB/s | 35 KB/s peak (MFD 2) |
| whole frame (key) | 1.4-12 KB | 12.1 KB (MFD 3) |
| synthetic combat: a 280 x 200 live picture every frame on MFD 4 | 195 KB/s (9.5 KB an update, 21/s) | 232 KB/s peak |
| synthetic stress: a whole new page every frame, all five (~50x the game) | 1.65 MB/s wanted; on a 1000 KB/s link the bridge paces it to ~5 updates/s, 0.34 s p95 | |

So the real game uses **under 4 %** of a Full-Speed link per module; even
the synthetic worst case fits (implementation document, "Load"). Five
modules on one hub share its 12 Mbit/s upstream only if the hub is
Full-Speed: **use a High-Speed (USB 2.0, multi-TT) hub** so each module gets
its own Full-Speed bandwidth.

## CPU

Per update the module: unstuffs COBS and checks the CRC (every byte, a
table CRC: ~2 cycles/byte), optionally inflates (`tinfl`, ROM, a few
cycles/byte of output **[E]**), then decodes RLA: `memset` and `memcpy`
into the framebuffer, a table lookup per literal.

| | bytes in | pixels written | estimate at 240 MHz **[E]** |
|---|---|---|---|
| typical game update | 0.2-3 KB | 1-20 k | < 1 ms |
| whole frame (key) | 2-12 KB | 307 k | 3-8 ms (bound by PSRAM writes, ~50-80 MB/s) |

Measured on the PC, the Python decoder takes 0.1-0.7 ms for a typical
update and 1.6-14 ms for a whole frame **[M]**; C on the S3 will be in the same
range. One core decodes; the other runs USB and keys. **Even a key frame
every update (21/s) is ~10-20 % of one core [E].**

## Keys and lamps (GPIO)

| function | pins | note |
|---|---|---|
| USB | GPIO 19/20 (fixed) | native USB |
| LCD | 20-21 | see above |
| 8 keys | 8 GPIO, or 2 (I2C) via an expander | debounced in firmware, reported with the module's ms clock |
| 8 key lamps, 0-255 | 8 LEDC PWM channels (the S3 has exactly 8), or 2 (I2C) via a PWM LED driver | the lamps need PWM: the game uses off / available / lit |
| backlight PWM | 1 | an LEDC channel - a 9th, so either the lamps go on a driver or the backlight is on/off |
| octal PSRAM | GPIO 33-37 taken | on N16R8 modules |

With an RGB panel the S3 module has just enough pins for direct keys or
direct lamps, not both plus backlight PWM. **Recommendation: keys and lamps
on one I2C bus** (a GPIO expander and a 16-channel PWM LED driver), which
also keeps the button board separate from the controller board.

## Firmware recommendations

1. Port `tools/mfdstream.py`'s `Decoder` and `rla_decode` to C as written:
   decode into the framebuffer in place, validate every token against the
   rectangle and every rectangle against the screen. Run
   `tools/test_mfdstream.py`'s malformed-input cases against it.
2. Offer DEFLATE: `tinfl_decompress` from ROM (`esp_rom/miniz`), one part at
   a time into a 4 KB buffer, refusing output beyond it.
3. Double-buffer the panel framebuffer if PSRAM allows (8 MB): swap on
   `END`. Otherwise accept a mixed picture for the ~100 ms until a key frame
   after a fault.
4. Send `FRAME_ACK` only when the frame is on the panel - it is the bridge's
   flow control.
5. Keys and lamps in their own task at higher priority than decoding, so a
   page change never delays a key press.
6. Record USB throughput, decode time per update and PSRAM contention in
   Phase 2, and replace the [E] figures here.
