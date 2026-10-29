# Firestorm display hardware feasibility audit

How BattleTech: Firestorm (`MW4.exe` 5.07, the Tesla II game) draws its
cockpit displays under `firestorm/` (the `ddraw.dll` shim), what each display
really needs, and what hardware could show them. Investigative only: nothing
in the shim, the launcher or the game was changed for it.

Each finding is labelled:

- **[V]** verified - read in the code, the game's own files or the shim's log;
- **[M]** runtime measurement - taken on a running mission, method given;
- **[E]** engineering estimate - reasoned, not measured;
- **[?]** unknown.

Code references are `firestorm/src/ddraw.c` unless another file is named;
`MW4.exe` addresses are the game's, as documented in `firestorm/README.md`.

Measurements were made on 2026-10-29 on one Windows 10 PC (RTX 2080, 144 Hz
monitor), with the game at 1600x1200, 16-bit, against one bot, using only the
shim's existing snapshot command (`snap` on the control port, which writes each
device's back buffer, stamped with its frame number - `snapshot()`, line 617).

---

## 1. The seven displays

The game drives three DirectDraw devices **[V]** (`README.md`, *What the game
does with its displays*; adapter choice at `MW4.exe 0x42A599-0x42A68C`): the
main view, one 1280x480 "MFD" device, and one 640x480 radar device. The five
MFDs are not five devices: they are **five colour channels of one surface**.
The pod's MFD card had two 640x480 VGA heads; a cable split each head's red,
green and blue into separate monochrome monitors - five used, the sixth
(right head, blue) empty **[V]** (`mfd_setup()` line 427, default tiles
`L:R,L:G,L:B,R:R,R:G`, confirmed against a running mission; the empty channel
measured all-zero).

| | Main view | MFD 1 | MFD 2 | MFD 3 | MFD 4 | MFD 5 | Radar |
|---|---|---|---|---|---|---|---|
| Internal name | `main1` (GameOS device) | `mfd`: left head, red | left head, green | left head, blue | right head, red | right head, green | `radar` (`CRadar_Device`) |
| Purpose (mission defaults) | out-the-window 3D view, HUD | pilot: kills, deaths, messages | navigation: compass, waypoint, score/kills/deaths | own 'Mech: damage silhouette, heat, jump | targeting: range, target name, **live 3D view of the target** | weapons table, groups, ammo | radar scope, minimap, heat/speed bars; before a match the pilot's 'Mech ("mechview") |
| Rendering resolution | `options.ini`: 800x600 on the site's pod profile **[V]** (`options-game.ini`); 1600x1200 in our tests | 640x480 **[V]** | 640x480 | 640x480 | 640x480 | 640x480 | 640x480 **[V]** |
| Original resolution | 800x600 **[V/E]** - the site file says so; that pods ran it is inferred | 640x480 **[V]** (the card's mode, `SetDisplayMode 1280x480 16 bpp 60 Hz`, shim log) | same | same | same | same | 640x480 **[V]** (`SetDisplayMode 640x480 16 bpp 60 Hz`) |
| Aspect | 4:3 | 4:3 | 4:3 | 4:3 | 4:3 | 4:3 | 4:3, mounted portrait **[V/E]** (labels drawn rotated; `radar_rotation()` line 709) |
| Pixel format | 16-bit RGB565 (pod profile) or 32-bit **[V]** | **one channel** of RGB565: red, 5 bits (32 levels) **[V]** | green, **6 bits** (64 levels) | blue, 5 bits | red, 5 bits | green, 6 bits | RGB565 colour **[V]** |
| Levels in use (one frame) | - | 15 **[M]** | 25 | 21 | 32 | 35 | 1691 colours **[M]** |
| Expected refresh | 60 Hz requested **[V]** | 60 Hz VGA scan **[V]**; content far slower (below) | | | | | 60 Hz requested **[V]** |
| Observed update rate | 143.9 fps (paced to the monitor) **[M]** | **20.6 flips/s** at 144 fps main, **9.9/s** at 70 fps: one flip per 7 main frames **[M]** | same flip | same flip | same flip | same flip | 20.6 / 9.9 flips/s, on the same frame as the MFDs **[M]** |
| Content | 3D (Direct3D 7 HAL), HUD text | text, lines, boxes | text, compass, numbers | text, bitmap silhouette, bars | text, **3D render-to-texture** | text table, small bars | colour bitmaps, vectors, text, sweep |
| Image accessible on its own | GPU surface; CPU copy only on demand | yes, as a channel of the shared CPU copy | yes | yes | yes | yes | yes, CPU copy |

Not counted in the seven, but part of the cockpit: the **plasma sign** on top
of the pod - 112x32 dots, one bit each, driven over its own serial port
(`\\.\com2`, 9600 baud) with drawing commands, never pixels **[V]**
(`plasma_feed()` line 2971, `docs/RIO.md` there). And the **mission review**
device (`CMR_Device`, 640x480) uses the radar's adapter after a match **[V]**.

## 2. The rendering pipeline

All images are drawn by **the original game code** with **DirectDraw 7 and
Direct3D 7** (HAL devices) **[V]** (`MW4.exe 0x420050` makes one
`IDirectDraw7` per display, `0x4200F0` a flipping primary with one back
buffer and a D3D7 device on it). No SDL, OpenGL or Vulkan is involved. The
shim never draws game content; it only decides **where surfaces live** and
**where finished frames go**.

```
  MW4.exe (original)                          firestorm/src/ddraw.c (new)
  ------------------                          ---------------------------
  per display: IDirectDraw7 -> SetCooperativeLevel(FULLSCREEN)
                              -> h_SetCooperativeLevel()  1116  windowed instead
               SetDisplayMode(w,h,16)
                              -> h_SetDisplayMode()       1216  emulated: remembers w,h,bpp
               CreateSurface(PRIMARY|FLIP, 1 back)
                              -> h_CreateSurface()        1044
                              -> fake_primary()            999  two OFFSCREEN surfaces in
                                                                video memory, mode's format
  every frame: D3D7 draws into the back buffer   (GPU)
               Flip()        -> h_Flip()                   918  -> present()  842
                                                                     |
        +--------------------------+-----------------------------+---+-----------------------+
        | main view                | MFD surface 1280x480        | radar 640x480            |
        | gpu_present()  788       | Lock(READONLY), conv16()    | Lock, conv16() -> d->rgb |
        | Blt back buffer to the   | 754 -> d->rgb (0x00RRGGBB)  | radar_present()  719     |
        | screen through a clipper |                             | (turned upright if set)  |
        | (no CPU copy)            | mfd_present()  539          | -> draw_rgb() -> window  |
        | frame_cap()  648         |   for each of 5 tiles:      |                          |
        | (DwmFlush pacing)        |   pick channel -> 8-bit grey|                          |
        |                          |   -> StretchDIBits -> window|                          |
        +--------------------------+-----------------------------+--------------------------+
                                                     snapshot()  617  (any device, on request)
```

- **Rendering begins** in the game's frame loop; the shim first sees a frame
  at `Flip` (`h_Flip()`, line 918).
- **Framebuffer memory** for each display is two offscreen surfaces the shim
  creates in video memory in the game's chosen format (`fake_primary()`, line
  999); surfaces the game makes without a format get the mode's 16 bits
  (`h_CreateSurface()`, line 1044).
- **Updates**: the game renders into the back buffer each time it flips that
  display; it does not do partial updates of the surface as far as the shim
  can see - every flip delivers a complete frame **[V]**.
- **Presentation**: main view by GPU blit (`gpu_present()`); MFDs and radar
  by CPU read-back then GDI `StretchDIBits` (`present()`, `mfd_present()`,
  `radar_present()`, `draw_rgb_in()` line 474).
- **Shared resources**: the five MFDs share **one surface, one device, one
  flip**; each is a channel of it **[V]**. The MFD and radar devices flip on
  the same frame **[M]**. The MFD device also owns a 1024x512 3D render
  target (the MFD 4 target view), and the radar one a 512x1024 target and two
  480x640 surfaces (shim log, `h_CreateSurface()`) **[V]**.
- **Composition**: the original hardware composed the MFDs into one 1280x480
  framebuffer; the shim undoes it into five images (`mfd_present()`), or one
  wall window (`[mfd] layout = wall`, `tile_rect()` line 416).

## 3. Framebuffer access

| | Main view | MFDs 1-5 | Radar |
|---|---|---|---|
| CPU-accessible copy | only when asked (snapshot frame) **[V]** | **yes, every flip**: `d->rgb` **[V]** | **yes, every flip** **[V]** |
| GPU texture | the back buffer, an offscreen video-memory surface **[V]** | the back buffer, 1280x480 **[V]** | back buffer, 640x480 **[V]** |
| Source format | RGB565 or XRGB8888 | RGB565; MFD n = one channel | RGB565 |
| CPU copy format | - | 32-bit `0x00RRGGBB` (`conv16()` expands 5/6 bits to 8 by replication) | same |
| Dimensions / stride | mode size; driver pitch | 1280x480; copy stride 1280x4 bytes; MFD n at x offset 0 or 640, byte 2/1/0 | 640x480; stride 2560 bytes |
| Source row pitch | driver-defined (`sd.lPitch`), not logged **[?]** | same **[?]** | same **[?]** |
| Persistent | surfaces live for the mission (new devices per mission) **[V]** | `d->rgb` allocated once per device (`present()` line 859) **[V]** | same |
| Copy needed | yes, a read-back (measured 1.2-1.3 ms at 1600x1200) **[M]** | already made each flip (MFD+radar read 0.2-0.3 ms, draw 0.27 ms per main frame, `[shim] frame_stats`) **[M]** | already made |
| Interceptable without changing game logic | yes, at `present()` | **yes, at `present()`/`mfd_present()`** | yes, at `present()` |

**One interface for all of them is practical** **[E]**: every display already
passes through `present(struct dev *d)` with its dimensions, format and a CPU
copy (except the main view, which would need its read-back re-enabled). A
"display stream" exporter there - per MFD, the channel as 8-bit grey; for the
radar, RGB565 - would expose seven independent streams without touching game
logic. The existing snapshot path (`snap_due()`/`snapshot()`) already does the
same thing to files.

## 4. Bandwidth

Rendering-loop frequency is not image-change frequency. Both were measured.

**Flip rate [M].** Two snapshots 10 s apart, frame numbers compared:

| main view | MFD device | radar device |
|---|---|---|
| 143.9/s | 20.6/s | 20.6/s |
| 69.8/s (main paced to every other refresh) | 9.9/s | 9.9/s |

The small displays flip once every **7** main-view frames, all together - a
divider in the game's frame loop, not a timer. On a pod holding 60 fps that
is **8.6 updates/s**; at 144 fps, **20.6/s**. Design for **≤ 21 updates/s**.

**Change between consecutive flips [M].** Snapshot series 50 ms apart (1-2
flips) standing still and walking/turning, and 1 s apart (about 20 flips):

| | idle, per flip | walking, per flip | over 1 s | pixels changed when changing | 16x16 tiles touched |
|---|---|---|---|---|---|
| MFD 1 | 0% | 0% | 0% | - | 0% |
| MFD 2 | 0% | 0.08% (max 0.64%) | 0.25% | compass and range numbers | ≤ 3% |
| MFD 3 | 0% | 0% | 1.6% mean, 8.5% max (damage events), unchanged in 82% of seconds | | ≤ 12.6% |
| MFD 4 | **3.7%** | **3.4%** (max 4.3%) | 4.2% | the live 3D target view, every flip | **6-8%** |
| MFD 5 | 0% | 0% | 0% (no firing in the sample) | | 0% |
| Radar | 0.01% | 0.18% | 0.5% | the sweep and contacts | 2-6% (but 30-40% of rows) |

**Size [M]** (one frame; zlib level 6 as a stand-in for any LZ codec):

| | raw (8-bit grey) | compressed | XOR with previous frame, compressed |
|---|---|---|---|
| MFD 1 | 307,200 B | 3.3 KB | 0.3 KB |
| MFD 2 | 307,200 B | 5.3 KB | 0.5 KB |
| MFD 3 | 307,200 B | 12.3 KB | 0.3 KB |
| MFD 4 | 307,200 B | 11.2 KB | **8.5 KB** |
| MFD 5 | 307,200 B | 7.8 KB | 0.3 KB |
| Radar (RGB) | 921,600 B | 88.5 KB | - |

**What to design for [E]**, per MFD module, at 21 updates/s:

| case | per update | per second |
|---|---|---|
| raw full frames (no compression) | 300 KB | **6.3 MB/s** - too much for USB Full Speed |
| typical, changed tiles compressed | 0.3-1 KB (MFDs 1, 2, 3, 5) | < 25 KB/s |
| MFD 4 (target view) | ~8.5 KB | **~180 KB/s** |
| page change (whole screen redrawn) | 3-12 KB | once |
| all five together, typical | | **~0.25 MB/s** |

So partial-frame transmission is not just practical, it is what makes USB
work: only MFD 4 changes every flip, and only in a ~6% area. These figures
come from short samples on one map with one opponent; heavy combat (several
targets, weapons recycling on MFD 5, damage on MFD 3) will be busier
**[?]** - the same instrumentation over a full match would settle it.

## 5. Raster versus drawing commands

**Answer: D, a combination - but only raster is available to us cheaply.**

- The MFDs and radar are drawn by the game's own code through **Direct3D 7**:
  text from the game's fonts (anti-aliased: 15-35 grey levels per MFD
  **[M]**), lines, filled rectangles, bitmaps (`hsh\MFD\*.bmp` silhouettes,
  `mfd_texture.bmp`), and on MFD 4 a **3D render of the target 'Mech into a
  1024x512 texture** **[V]** (shim log, surface caps `0x7000`).
- The MFD *contents* are driven by game state through the game's script and
  HUD code; the button labels are drawn into the same raster, in 40-pixel
  strips top and bottom (`MW4.exe 0x756E10`) **[V]**.
- Only the **plasma sign** is natively command-based **[V]**.

**Redirecting drawing commands** (Architecture C) would mean hooking every
Direct3D 7 call on the MFD device - `DrawPrimitive`, textures, render states,
the font engine - and re-implementing that renderer on each microcontroller,
including the 3D target view. **[E] High complexity, high risk**: the 3D view
cannot be reproduced on a microcontroller at all; anti-aliased game fonts
would have to be re-rasterised identically; every MFD page (there are modes
per MFD) would need testing; and a game patch would break it. Exact
reproduction would be unlikely. Capturing the finished raster costs one
memory read that the shim already does.

## 6. External display architectures

Integration points refer to section 3: every architecture except A needs a
small exporter at `present()`/`mfd_present()`; A needs nothing new.

| | **A. Conventional video** | **B. USB framebuffer streaming** | **C. Local rendering** | **D. Display bridge** |
|---|---|---|---|---|
| How it integrates | The shim already opens one window per MFD (`mfd_setup()`); place each borderless on its own monitor (`[mfd1]..[mfd5] x,y,w,h,borderless`, the launcher's Displays page). Monitors come from GPU outputs, USB display adapters, or one output + a video-wall splitter (`[mfd] layout = wall`). | Exporter at `mfd_present()`: per MFD, changed 16x16 tiles of its channel, compressed, sent through the existing bridge (`tools/bridge.py`) as the module protocol's reserved `FRAME` message (`docs/MODULE_PROTOCOL.md`, type `0x16`) over USB CDC; the module keeps a framebuffer and drives its panel. | Hook D3D7 on the MFD device (or read game state) and send commands/state; module renders. | Exporter as in B, to one device (SBC or FPGA) driving several panels. Variant: one 1280x480 video output into an FPGA that splits channels to five panels, as the original cable did. |
| Bandwidth | video link; 0 on USB except DisplayLink-style adapters (compressed, ~tens of MB/s) | **~0.25 MB/s typical** for all five, ~0.2 MB/s peak per module **[M/E]** | lowest (state) | as B, over one link (Ethernet/USB HS); FPGA variant: a video signal |
| Added latency | 1-3 refreshes (USB display adapters add buffering) **[E]** | ~10-30 ms: copy <1 ms, encode ~1 ms, USB 1-10 ms, decode/blit a few ms, panel scan ≤16 ms **[E]** | lowest | B + SBC compositor (1-2 frames) **[E]**; FPGA: < 1 frame |
| Frame rate | the game's 21/s, exactly | the game's 21/s | whatever is reimplemented | 21/s |
| Fidelity | exact if the panel is driven 1:1; most small HDMI panels rescale | **exact, 1:1** on a panel ≥ 640x480 | approximate; target view impossible | exact |
| Hardware cost (5 MFDs) | ~$30-60 per USB adapter + ~$30-80 per small HDMI panel **[E]** | ~$25-45 per module (MCU board with panel) + buttons **[E]** | as B | SBC ~$60-80 per two panels (e.g. a Pi's two HDMI outputs); FPGA board + 5 RGB panels, more design work **[E]** |
| Software complexity | **none** (works today) | moderate: exporter + firmware (codec, panel, USB) | **very high** | moderate (SBC) to high (FPGA) |
| Reliability | good; USB display drivers are the weak point | good: stateless modules, the host replays state (the bridge already does, for lamps) | fragile across game modes | good; one more box |
| Scales to five | yes, but GPU outputs run out; adapters or splitter needed | yes - identical modules, one USB each | yes | yes |
| Buttons | separate (an RP2040 keypad per MFD, Phase 2) | **in the same module** | in the same module | separate |
| Exact original graphics | yes, if 1:1 | **yes** | no | yes |

## 7. Physical displays

| | Physical size | Aspect | Orientation | Visible area | Scaling risk | Fine text |
|---|---|---|---|---|---|---|
| Main view | **[?]** unknown | 4:3 image | landscape | **[?]** | any 4:3 scale fine (3D) | HUD text, small |
| MFDs 1-5 | **[?]** unknown; the Kalamazoo photo shows small monochrome screens with lit buttons above and below **[V]** (photo) | 4:3 | landscape **[E]** | **[?]** | **yes**: 8-12 px labels; non-integer scaling blurs them | **yes**, pixel-perfect wanted |
| Radar | **[?]** | 4:3 image | **portrait** **[V/E]** - the game draws its labels rotated for a monitor on its side | **[?]** | moderate | yes, small labels |
| Plasma sign | **[?]** | 112x32 dots (3.5:1) **[V]** | landscape | - | none - it is dots | 5x7 and 10x14 fonts |

The belief that the original used **seven displays at 640x480** holds for the
**six aux displays** (five MFDs from two 640x480 heads, and the radar)
**[V]**; the main view was 800x600 on the site's pod profile **[V/E]**. The
MFD monitors were **monochrome**, each fed one colour line of a VGA head
**[V]** (the channel split) **[E]** (that the monitors were monochrome, as
the split implies). No physical dimensions were found in the game files, the
docs or the photo; none are given here.

**Panel choice that keeps pixels exact [E]:** an **800x480** panel shows a
640x480 MFD **1:1** with 80-pixel bars at the sides (same height, no
scaling). 640x480 panels exist but are less common; 1024x768 needs a 1.6x
scale (soft text). Monochrome reproduction: a 6-bit channel (MFDs 2 and 5)
needs a panel that shows 64 grey levels - an RGB565 panel shows 32 on white,
64 if tinted green (the green channel has 6 bits).

## 8. Recommended prototype

**One MFD module: an ESP32-S3 with an 800x480 parallel-RGB panel, eight lit
keys on an I2C port expander, USB to the host, Architecture B.**

- **Why a panel with its own RGB bus**: the ESP32-S3's LCD peripheral scans
  out a framebuffer in its PSRAM to a parallel RGB panel continuously; the
  640x480 MFD sits 1:1 in an 800x480 panel - **pixel-exact** - and the
  game's 21 updates/s are written into that framebuffer as they arrive. Ready
  boards combining both exist (ESP32-S3 + 4.3-5" 800x480 RGB panel, often
  ~$25-45) **[E]** - check the panel's resolution and PSRAM size (≥ 2 MB)
  before buying.
- **Why USB Full Speed is enough**: the measured traffic for one MFD is ~0.2
  MB/s at worst (MFD 4), against ~1 MB/s usable on Full Speed **[M/E]**; a
  whole-page redraw is 3-12 KB compressed. The ESP32-S3 has native USB (CDC
  via TinyUSB) and an inflate routine in ROM.
- **Buttons**: eight keys and eight LED channels on a TCA9555/PCA9555-class
  expander (or spare GPIO), since the RGB bus uses most pins; LED brightness
  in four steps as `docs/IMPLEMENTATION_PLAN.md` Phase 2 asks. Keys and lamps
  use the module protocol as they are; only `FRAME` is new.
- **Host side**: an exporter at `mfd_present()` (sends changed tiles of each
  channel), the bridge forwarding `FRAME` to the module bound to that MFD in
  `layouts/firestorm-mfds`. On reconnect the host sends a full frame - the
  same replay the bridge does for lamps.

**Alternatives considered [E]**

- **RP2040** (the Phase 2 plan's part): 264 KB RAM cannot hold a 640x480
  framebuffer at 8 bits (300 KB); fine for the 3.0 game's text MFD
  (320x240), not for Firestorm's pixels.
- **RP2350 (Pico 2)**: 520 KB RAM holds 640x480 at 8 bits, and its HSTX
  outputs DVI at 640x480 natively - a good choice **if** a small panel
  accepting 640x480 over HDMI without rescaling is found; most small HDMI
  panels rescale, which softens text.
- **High-speed USB parts** (ESP32-P4, Teensy 4.x, STM32H7 with HS PHY): more
  headroom than needed for these measured rates; worth it only if raw frames
  or the radar (88 KB compressed, colour) must also stream.
- **Architecture A first**: five USB display adapters and small HDMI panels
  work **today with no code**, and are a good bench reference for the
  prototype - but rescaling panels lose pixel-exactness, adapters are
  driver-dependent, and buttons stay separate.

The main view and the radar should stay on the host's GPU outputs
(Architecture A): the main view is 3D at the monitor's rate, and the radar is
colour, portrait, and 30-40% of its rows change every flip.

## Remaining unknowns

1. **Busier play**: the change rates come from short samples, one map, one
   bot. A full match with several targets and sustained firing (MFD 5 recycle
   bars, MFD 3 damage) - the same `snap` series over ten minutes - would bound
   the peak.
2. **Original frame rate** on a pod (and so its aux update rate, main / 7) -
   not recorded anywhere found.
3. **Physical sizes, visible areas and the monitor models** of all displays.
4. **Source row pitch** of the video-memory surfaces (`sd.lPitch`) - not
   logged; only matters for a GPU-side exporter.
5. **Whether every MFD page** (modes switched by the soft keys) behaves like
   the default pages measured here.
6. **Panel grey response**: whether a chosen RGB565 panel reproduces 32/64
   levels evenly enough for anti-aliased text.
7. **USB hub behaviour** with five Full-Speed modules streaming at once
   (transaction-translator limits) - expected fine at these rates, untested.

## Final answers

1. **Can the seven displays be rendered without seven GPU video outputs?**
   **Yes.** They already are: the shim renders all seven from one GPU into
   ordinary windows. Physically, the main view and radar need two outputs;
   the five MFDs can come from USB modules (B), USB display adapters, or one
   output through a splitter (A/D).
2. **Can the five smaller displays be transmitted over USB?** **Yes** - at
   about 0.25 MB/s for all five typically and ~0.2 MB/s peak per module
   **[M/E]**, within USB Full Speed, provided only changed regions are sent,
   compressed. Raw full frames (6.3 MB/s per MFD) would not fit Full Speed.
3. **Pixel-for-pixel?** **Yes**: the shim has each MFD's exact 640x480 image
   every flip; shown 1:1 on a panel at least 640x480 (e.g. 800x480 with side
   bars), with 5-6 bits of grey, it is exact. Rescaled panels or redrawn
   ("local rendering") graphics would not be.
4. **Minimum practical controller?** A microcontroller with **≥ 300 KB for a
   640x480 8-bit framebuffer** (PSRAM), a panel interface that can scan it
   out, and USB Full Speed: an **ESP32-S3 with PSRAM** and an 800x480 RGB
   panel; an RP2350 is the minimum if driving DVI. The RP2040 is too small
   for Firestorm's MFDs.
5. **Design targets:** **21 updates/s** per MFD (the game's ceiling at 144 fps
   main; 8.6/s at 60), **≤ 0.25 MB/s per module** (budget 1 MB/s for whole-page
   redraws), and **≤ 30 ms added latency** from flip to light - on top of the
   game's own 50 ms between MFD updates.
6. **First prototype:** **Architecture B** - one ESP32-S3 + 800x480 RGB panel
   module with eight lit keys on USB, fed changed tiles from an exporter at
   the shim's `mfd_present()` through the existing bridge and module protocol
   (`FRAME`); main view and radar on GPU outputs. Use Architecture A (USB
   display adapters) as the no-code bench reference.
