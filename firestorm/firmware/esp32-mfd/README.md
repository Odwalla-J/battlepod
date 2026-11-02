# esp32-mfd

One of Firestorm's five MFDs on an ESP32-S3 display board: the game's MFD
picture, streamed by the shim through `tools/bridge.py`
(`docs/MFD_STREAMING_PROTOCOL.md`), on a real screen.

Written for the **Elecrow CrowPanel 7.0" (DIS08070H), revision v3.0**:
ESP32-S3-WROOM-1 N4R8, an 800 x 480 RGB panel, backlight PWM on GPIO 2, a
CH340 USB-serial chip. The 640 x 480 MFD picture is drawn 1:1 in the middle,
with 80 px of black each side. Touch is not used.

Each update is decoded into a second framebuffer and put on the screen whole
at a frame boundary - no tearing - then copied into the first. It is
acknowledged as soon as it is decoded, before that wait: acknowledged after
it, the `FRAME_ACK` met the host's next update on the CH340, which then lost
bytes coming back (5-13 bad packets in 10 s instead of 0-2).

Measured on MFD 4, the heaviest display: 20 updates a second at ~33 ms from
the game to the screen, nothing skipped (`docs/MFD_HARDWARE_REQUIREMENTS.md`,
*First hardware*).

## Each board

Every board gets the same firmware. Which MFD it is - `mfd:1` to `mfd:5`, the
names in `layouts/firestorm-mfds` - is set from the launcher's **MFD boards**
page (or `launcher/mfdtest.py name COM8 4`) and kept in its flash; a board
never named is `mfd-` and the end of its MAC address, which the bridge leaves
alone. The bridge sends each board the display its name's layout line names.
The page also loops a test picture on every board: its MFD's page, drawn, or
a few seconds recorded from the game. The MFD's colour is `TINT_R/G/B`
(white, as the shim's windows).

## Building and flashing

`arduino-cli` and the Espressif ESP32 core (3.x) are in
`C:\Projects\BattleTechPreservation\toolchain`. From there:

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=4M,CDCOnBoot=default ^
    --output-dir build-mfd ..\battlepod\firestorm\firmware\esp32-mfd
arduino-cli upload --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=4M,CDCOnBoot=default ^
    -p COM8 --input-dir build-mfd ..\battlepod\firestorm\firmware\esp32-mfd
```

The CH340 resets the board into its bootloader by itself. Until the first
picture the board is black.

## Running it

```
tools\bridge.py --game firestorm --serial COM8@2000000
```

with `[stream] enable = 1` in the game's `shim.ini` (`displays = 4` to stream
only MFD 4). `--stream-stats` logs every 10 s what each board is getting, how
quickly it shows it, and anything lost; the board itself reports link errors
as `ERROR` lines when there are any.

The link runs at 2 Mbaud (`LINK_BAUD`), the CH340's most. The bridge keeps
DTR and RTS released: on this board they reset the ESP32.

## The factory firmware

Before the first flash the board's whole 4 MB was saved to
`toolchain\backups\DIS08070H-v3.0-factory-4MB.bin`. To put Elecrow's demo
back:

```
esptool --chip esp32s3 --port COM8 write-flash 0 DIS08070H-v3.0-factory-4MB.bin
```

(`esptool.exe` is in the Arduino folder:
`%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\<version>\`.)

## Pins and timing

From the ESPHome device page for this board's v3.0, tested on two boards:
data B0-4 = 15, 7, 6, 5, 4; G0-5 = 9, 46, 3, 8, 16, 1; R0-4 = 14, 21, 47, 48,
45; DE 41, HSYNC 39, VSYNC 40, PCLK 0 at 15 MHz, latched on the falling edge;
HSYNC 40/48/13 and VSYNC 1/31/13 (front porch, pulse, back porch). It warns
not to write to the PCA9557 at I2C 0x18 (the touch controller's reset), which
this firmware never does.
