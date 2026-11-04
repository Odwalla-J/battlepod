# esp32-mfd

One of Firestorm's five MFDs on an ESP32-S3 display board: the game's MFD
picture, streamed by the shim through `tools/bridge.py`
(`docs/MFD_STREAMING_PROTOCOL.md`), on a real screen.

Written for the **Elecrow CrowPanel 7.0" (DIS08070H), revision v3.0**:
ESP32-S3-WROOM-1 N4R8, an 800 x 480 RGB panel, backlight PWM on GPIO 2, a
CH340 USB-serial chip. The 640 x 480 MFD picture is drawn 1:1 in the middle,
with 80 px of black each side. Touch is not used.

The picture is kept as the stream sends it, grey, a byte a pixel, in two
framebuffers in PSRAM (firmware 0.6 on). The panel has no framebuffer of its
own: the firmware fills the driver's two 30-line bounce buffers itself as the
panel scans (`on_bounce_empty`, the driver's `no_fb` mode), turning grey into
the MFD's colour on the way - so a colour change shows from the next frame.
Each update is decoded into the second framebuffer and taken by the panel
from the top of the next frame - no tearing - then copied into the first.

Before 0.6 both framebuffers were the panel's, RGB565, filled by the driver:
twice the PSRAM traffic, and a whole frame decoded and copied (a page
change) took enough of it that a refill came too late and the picture
slipped, part of its bottom at the top - about 1.6 times for every whole
frame, measured. Grey, with 30-line bands, a stream of whole frames as fast
as the board takes them slips none, its worst refill using half of the 1.8 ms
it has. The board counts slips and says so, as `ERROR` lines (`panel: N
slips ...`) the bridge logs. An update is
acknowledged as soon as it is decoded, before the wait for the panel: acknowledged after
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
a few seconds recorded from the game.

The game streams grey; the board shows it through its colour, which it keeps
in its flash too (firmware 0.5 on): the launcher's *MFD colour* (Displays),
the same as the shim's MFD windows, sent to the boards when it is saved,
when the boards are found, and before the bridge starts - so a pod whose
setting the console changed has it on its boards by its next game
(`launcher/mfdtest.py tint COM8 40FF60` by hand). White until told.

## Flashing a board

The launcher's **MFD boards** page does it: *Find boards*, then *Flash* (or
*Flash all new boards*) - a board still on Elecrow's demo shows there as *New
board*. About 25 s a board, several at once, and the board is found again as
an MFD board, ready to be given its MFD. Without the launcher, from
`battlepod\firestorm`:

```
python launcher\mfdflash.py COM8 COM9
```

`launcher/mfdflash.py` talks to the ESP32-S3's ROM bootloader itself (the
protocol esptool uses, without its stub), with pyserial alone: it writes the
images in `bin/` where arduino-cli's upload puts them (bootloader 0x0,
partitions 0x8000, boot_app0 0xE000, the firmware 0x10000), checks each by
MD5 and resets the board. It offers only ports with a WCH (CH340) USB chip
and writes only to an ESP32-S3. The board's NVS is left alone, so a board
keeps its MFD through flashing again.

`bin/` holds the images of the version the launcher flashes (`FIRMWARE`, in
the page's Firmware column). After changing the firmware, build it (below)
and copy them in:

```
copy build-mfd\esp32-mfd.ino.bootloader.bin ..\battlepod\firestorm\firmware\esp32-mfd\bin\bootloader.bin
copy build-mfd\esp32-mfd.ino.partitions.bin ..\battlepod\firestorm\firmware\esp32-mfd\bin\partitions.bin
copy build-mfd\esp32-mfd.ino.bin           ..\battlepod\firestorm\firmware\esp32-mfd\bin\esp32-mfd.bin
```

(`boot_app0.bin` is the ESP32 core's `tools\partitions\boot_app0.bin`.) A
board on an older version shows *Update* on the page.

## Building and flashing with arduino-cli

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
