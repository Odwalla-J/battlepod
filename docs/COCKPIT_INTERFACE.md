# The cockpit's interfaces, as they stand

What the emulated cockpit already exposes to the outside world - controls in,
lamps, displays and game state out - where each lives in the source, and how
sure we are of it. This is the audit behind [HARDWARE_MAPPING.md](HARDWARE_MAPPING.md)
and [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). The hardware findings it
builds on are in [DEVICES.md](DEVICES.md); nothing here repeats them at length.

Release audited: **BattleTech 13.1.8**, `Full_Load_3_0`. Code audited: branch
`feature/cockpit-interface` at `8430e9d`. No source was changed to produce this.

Every claim carries one of these marks:

| mark | means |
|---|---|
| **code** | read in this repository's source; file and function cited |
| **firmware** | read in `ROM3_0`; address cited |
| **measured** | observed on the running firmware during this audit; the recipe is in *Reproducing the measurements* |
| **documented** | established earlier and written up in DEVICES.md or RENDERING.md |
| **hypothesis** | consistent with the evidence, not proven |
| **unknown** | not known; what would settle it is said |

---

## Summary

- The original cockpit talked to **all of its controls, lamps, bar graphs and
  alphanumeric displays over one 9600-baud serial link**, Remote I/O, on DUART
  channel A. That link is modelled, and it is the natural seam for real
  hardware. **code**, **documented**
- **Input works end to end.** Analog, button and keypad reports injected on
  that link move the Mech, fire its weapons and change its modes. **measured**
- **Output does not reach the wire during a game, and this audit found why.**
  The firmware *does* drive the panel in play - weapon names, weapon lamps,
  a 24-character message line, engine status - but the emulator's DUART
  transmitter is instantaneous, which wedges the firmware's transmit queue
  after the first frame. Only the boot frame ever gets out. See
  *Defects that block hardware output*. **measured**, **firmware**
- **In a game the panel stream is unframed.** The firmware switches Remote I/O
  to raw mode, so in-game output is bare `D1`/`D2`/`D3`/`D4` payloads with no
  `01` header and no checksum. `src/rio.h` only parses the framed form.
  **firmware**, **measured**
- The **main view** (480x360) is drawn by the emulator from the pod's own
  display lists. The **secondary display** (an Amiga 500 drawing radar, map
  and damage) is not emulated at all. **code**, **documented**
- **Position, heading and speed** are available on a firmware-defined wire
  format (`0xEC`); **hits and kills** too (`0xEA`). Heat, ammunition, armour
  and target are in memory at partly-known offsets or not located yet.

---

## 1. The emulator's architecture

### Entry points

| program | entry | built by | what it is |
|---|---|---|---|
| `build/battlepod.exe` | `main` in `src/battlepod.c` | `make` | the emulator and every report; no SDL |
| `build/cockpit.exe` | the same `main`, compiled `-DBATTLEPOD_SDL` | `make cockpit` | the emulator plus four SDL windows; `--live-pod` makes the keyboard the panel |
| `build/panel.exe` | `main` in `src/panel.c` | `make panel` | replays a Remote I/O capture into the panel windows, no emulator |
| `build/view.exe` | `main` in `src/view.c` | `make view` | plays raw 480x360 RGB frames |
| `build/paneltest.exe`, `viewtest.exe` | the same files, `-DPANEL_NO_SDL` / `-DVIEW_NO_SDL` | `make test` | self-checks without SDL |
| `tools/hub.py` | `main` | Python | the centre: launches pods, starts games, relays packets, flies CPU pilots |
| `tools/play.sh` | sh | - | builds the start-of-game packets, then runs `cockpit.exe --live-pod` |

`main` (`src/battlepod.c`) parses options, declares RAM, loads the images
named by the load script (`load_script`, `load_images`), installs the stubs,
and runs the **instruction loop** at the bottom of `main`: one
`m68k_execute(1)` per step, with these hooks around it (all **code**):

| every | hook | does |
|---|---|---|
| step | `duart_irq`, `rirq_asserted` -> `m68k_set_irq` | raise the DUART (level 4 by default) or renderer (`--rirq`, level 5) interrupt |
| `--clock` divisor (4096) | `clock_tick` | advance the timebase at `0x02000808` (hundredths of a second) |
| 4096 steps | `net_poll` | UDP to and from the hub (`--net`) |
| 4096 steps | `realtime_pace` | hold the timebase to wall-clock hundredths (`--realtime`, `--live-pod`) |
| 4096 steps | `rec_poll` | feed frames to `ffmpeg` (`--record`) |
| PC in the monitor stub | `mon_recv_polled`, `mon_send_called` | deliver queued packets, capture transmitted ones |
| PC at a tap | `tap_check` | `--tap` register dumps, `--set-at` writes |
| 200,000 steps | `live_pump` (SDL only) | walk new Remote I/O output, draw windows, read the keyboard |

### CPU

[Musashi](https://github.com/kstenerud/Musashi), fetched by `make deps` into
`third_party/musashi`, configured by `src/battlepod_m68kconf.h`. The board is a
68020 with a 68881; Musashi enables its FPU only on the 68040 profile, so that
is the default (`--cpu`). Sixteen missing 68881 transcendentals are added to
Musashi's `m68kfpu.c` by `tools/musashi_fpu.py`. Interrupt vectors come from
`bp_int_ack`: the renderer's `0x42`, or whatever the firmware wrote to the
DUART's IVR (`0x47`). No cycle accuracy; the timebase is paced to instructions
or to the wall. **code**

### Memory and devices

Sparse 64 KB pages (`ram_add`, `g_page`); anything unmapped is logged with the
PC that first touched it - which is how the device map in DEVICES.md was built.
The bus hooks are `m68k_read_memory_*` / `m68k_write_memory_*`. **code**

| board | address | in the emulator | function |
|---|---|---|---|
| CPU DRAM | `0x02000000`, 16 MB | RAM | |
| DUART MC68681 | `0x00011000` | modelled (`--duart`) | `duart_read`, `duart_write`, `duart_isr` |
| watchdog / control ports | `0x00010000`-`0x00010015` | logged, unmodelled | DEVICES.md |
| TMS34020 renderer | `0x20000000`-`0x3FFFFFFF` | stubbed (`--rstub`, `--rirq`) | `rstub_*`, `rstub_dlist`, `rstub_post` |
| Amiga 500 secondary | `0x40000000` | RAM window, handshake word set by hand | not run |
| ADSP audio | `0x50001000` | stubbed (`--astub`) | FIFO drained instantly |
| boot monitor | `0x02000400` | stub service table (`--monitor`) | `mon_install`; packets in at `+0x18`, out at `+0x24` |
| ARCNET (COM90C66) | - | not modelled; replaced at the monitor's packet slots | `--packet`, `--net` |

### Graphics

The 68020 builds display lists for the TMS34020; the emulator **reads them on
the 68020 side and draws them itself** rather than emulating the TI (a
recorded decision, ARCHITECTURE.md). `rstub_dlist` walks each list into a
`struct pod_frame`; `frame_draw` draws it through `src/raster.h` (span
rasteriser), `src/mesh.h` (the model bytecode), `src/rig.h` (Mech assembly),
`src/scene.h`, plus `sky_draw`, `hud_sprite` and `fx_draw`. Output is
480x360 RGB24 in `struct raster`. **code**

### SDL2

Only `cockpit.exe`, `panel.exe` and `view.exe` link SDL2 (Makefile: `cockpit`,
`panel`, `view`; statically, via `pkg-config --static --libs sdl2`). In
`battlepod.c` all of it sits inside `#ifdef BATTLEPOD_SDL`: `live_open`,
`live_key`, `live_pump`, `live_hold`, `live_close`. Panel drawing is
`src/paneldraw.h`. Video only (`SDL_INIT_VIDEO`); no SDL audio, no SDL
joystick or game controller. **code**

### How the parts talk

```
                     ROM3_0 (game, 68020)
       ┌────────────┬────────┴─────┬───────────────────┬──────────────┐
  display lists  DUART ch A     DUART ch B      monitor +0x18/+0x24   0x4007E000
       │         Remote I/O      console          packets           SecCom ring
       v           │   ^            │               │   ^               │
  rstub_dlist      │   │            v               v   │               v
  frame_draw   g_riobuf  g_in[0]  g_conbuf     mon_send_called      (nobody)
       │      (8 KB)     ^                     net_send_packet
       │           │     │ rx_append                │   ^
       v           v     │                          v   │ pkt_add_raw
  main view   rio_walk -+-- live_key (keyboard)   UDP 'P'  'P','R'
  window /    struct     +-- net_poll 'R' <-------- tools/hub.py
  --record    rio_panel  +-- --rio-in                 (CPU pilots, relay,
              panel windows                            operator's view)
```

Three seams carry everything, and each is a wire format the original hardware
used: display lists, the Remote I/O byte stream, and network packets.

### Networking

ARCNET is replaced at the boot monitor's packet slots. `--net HOST:PORT`
opens a non-blocking UDP socket to `tools/hub.py` (`net_open`, `net_poll`,
`net_send_packet`); datagram kinds are `H`, `P`, `A`, `R`, `Q`, documented in
[api.md](api.md). The hub can already **inject panel input** into any pod
(`R`, appended to DUART channel A by `rx_append`); there is **no datagram that
carries panel output back**. **code**

---

## 2. Cockpit input

### The path

All cockpit controls arrive on **DUART channel A**, the Remote I/O link, as
the original panel board sent them. **documented**, **code**

```
source -> rx_append (battlepod.c) -> g_in[0] -> duart_read RBA
       -> firmware RX handler 0x0215B6CC -> receive ring 0x0217D4B6
       -> Get_Event 0x02122B94 -> event kinds B0/B1/C0/D0 -> the Mech
```

Three sources feed `rx_append` today:

| source | function | form |
|---|---|---|
| `--rio-in "01 00 ..."` | `main` | hex on the command line, framed reports |
| `cockpit.exe --live-pod` keyboard | `live_key` -> `live_analog` / `live_button` -> `live_report` | framed reports built per key |
| hub `R` datagram | `net_poll` | framed reports, `tools/hub.py` `rio()` |

`--rio-late N` (`rx_pending`) holds input back until the packet queue is
delivered and the wire has been polled `N` more times, because a Mech still
dropping in ignores its controls (`+0xBB` bit 0). **code**

The firmware has **two receive modes** - framed (`0x0215B76A`) and raw
(`0x0215B72C`) - and switches to raw in a game. The report parser skips bytes
it does not recognise, so framed reports are accepted in either mode.
**firmware**, **documented**

### Report formats

From the firmware's own input viewer (`0x02125248`) and `Get_Event`. **documented**

| report | meaning |
|---|---|
| `B1 id` | button `id` down |
| `B0 id` | button `id` up |
| `C0 id hi lo` | analog `id` = signed 16-bit value |
| `D0 key` | keypad key |

Framed on the wire as `01 00 len len <report> sum(report)`.

### Analog channels

| id | control | range | evidence |
|---|---|---|---|
| `A0` | **throttle** | `0x0000`-`0x0340`; `Get_Event` clamps to 0 below -10 | **documented**: full scale is exactly the chassis' top speed |
| `A1`, `A2` | **stick**, left / right | about `±0x0080`; `0`-`0x80` used by the keyboard and hub | **documented**: steers in advanced mode with "stick turns" (buttons `33`, `31`); turn opposite ways |
| `A3` | probably the stick's other axis or a pedal | | **unknown**: full scale moved nothing watched |
| `A4` | **turn** (pedals in basic mode) | `0x0340` full | **documented**: turns the Mech |

`A0`-`A4` are filed into the pilot's Mech at `+0x84`, `+0x86`, `+0x88`,
`+0x8A`, `+0x8C` by `0x021317C6`. The control code at `0x0213078C`-`0x02130BFE`
reads them under a mode at `+0x90` (1 basic, 2 stick turns, 3 stick moves
crosshairs). **documented**

The manual's ranges, from optical encoders on the Remote I/O board: throttle
and pedals `$0000`-`$0340`, joystick `$0000` centred and about `±$80`. Whether
`A1`/`A2` are two halves of one axis or X and Y is **unknown** (UNRESOLVED.md);
settled by reading `0x0214E064`.

### Buttons

The Mech's button switch is at `0x02132536`, reached from `0x0213187A`. Most
arms print a status line through `0x02106120`, so the table names itself;
**each arm also lights the lamp with its own id** (for example the torso-right
arm calls the lamp routine `0x02143820` with `(0x1C, 3)`). **documented**,
**firmware**

| id | what it does |
|---|---|
| `00` `04` `08` | radar / map / damage display selected |
| `0C` | an arm that does nothing in this game (`0x02131F76`, a branch to the end) |
| `18` | prints nothing; calls `0x0214950E (0x1E, 0x20, 1)` and `0x021361DC (Mech, 0)`. **unknown** |
| `10` | indirect fire mode |
| `1C` / `51` | twist torso right / left |
| `20` | searchlight on / off |
| `24` / `2C` | view up / down |
| `28` | centre the torso |
| `30` / `31` | stick moves crosshairs / stick turns or tips torso |
| `32` / `33` | basic / advanced mode |
| `34`-`3B` | set and clear `+0x8F` bits 2-5 (by the lamp pairs, four two-way switches; which is which is **unknown**) |
| `3C` `3D` `43` | radar to 2 km, zoom in, zoom out |
| `3E` `3F` | FO select |
| `40` | target select |
| `41` `42` | damage display on, map grid on |
| `53` | forward observer on / off |
| `A5` `A6` `A7` | **triggers**, three groups (`+0x93` bits 1, 2, 0; fire routine `0x0215A9B4`) |
| `A8` | toggles `+0x7C` bit 0; **unknown** |
| `4k+1`..`4k+3`, k = 0-11 | **weapon-slot toggles**: pressing one flips the lamp of the same id between 3 and 1. **measured**. That they assign the slot's weapon to trigger groups 1-3 is a **hypothesis** |

### Keypad

`D0 key` is decoded by `Get_Event`. The manual says the keypad is for
maintenance and "overheat codes in BT". In this audit `D0 05` changed nothing
on the panel. **documented**; its game use is **unknown**.

### The keyboard, today

`live_key` in `src/battlepod.c`: W/S throttle in eighths (`A0`), X closes
it, A/D stick (`A1`/`A2`, and the first press also sends `33` and `31`),
space `A5`, F `A6`, G `A7`, Enter all three, Q/E torso `51`/`1C`, C `28`,
R/V `24`/`2C`, Z and + `3D`, - `43`, T `40`, L `20`, F1-F3 `00`/`04`/`08`.
A gamepad (`live_pad`) and USB controllers through winmm (`live_joy`,
`battletech.ini [joy]`, firestorm/docs/BATTLETECH3.md) send the same
reports. **code**

### Toggle switches

The firmware's inputs are all **momentary** (`B1` down, `B0` up). Where the
original had a two-way function it shows as a pair of button ids with a lamp
each (`30`/`31`, `32`/`33`, `34`-`3B`), and `20` and `53` toggle in firmware.
A latching physical switch therefore has to be presented to the game as a
press of the button for its new position. **firmware**, **measured** (lamp
pairs below)

---

## 3. Cockpit displays

A 13.1.8 cockpit has four kinds of display surface on three different paths,
and they share nothing. **documented** (RENDERING.md)

| surface | original hardware | count | resolution / size | format on the wire | refresh | in the emulator |
|---|---|---|---|---|---|---|
| **Main view** | TMS34020 + TMS34082, CRT viewed through optics | 1 | **480x360**, 16 bpp (`PSIZE` 16) | display lists the 68020 writes into TI memory | one per render-complete interrupt; paced to at least 3 hundredths (`--realtime`) | drawn by `frame_draw`; RGB24 `struct raster`; window, `--frame-out`, `--record` |
| **Secondary display** (radar / map / damage) | an Amiga 500 board, `AMIGA3_0` | 1 | **unknown** | 1,652-byte SecCom block at `0x4007E000`: a 32-slot ring of 42-byte messages | unknown | **not emulated**; the Amiga program does not start on its own |
| **Alphanumeric displays** | on the Remote I/O panel | 18, ids `0x80`-`0x91` | 8 characters each (`D1`); some also take a 2-character field (`D4`) | Remote I/O, see section 4 | on change | `rio_apply` (D1 only), `draw_text` window |
| **Bar graphs** | on the Remote I/O panel | 15, ids `0x80`-`0x91` less `0x8C` `0x8D` `0x8F` | a bar count; 8 is the largest seen | Remote I/O `D2` | on change | `rio_apply`, `draw_bars` window |
| Head-up display | drawn into the main view | - | 480x360 overlay | display-list items `$100`, `$240` | with the frame | reticle and drop bay drawn; text not yet |

Notes:

- **The displays cannot share one rendering architecture.** The main view is a
  3D frame; the secondary display is a separate computer fed messages; the
  panel displays are short strings and numbers on a serial link.
- **Selection and routing.** The secondary display's mode is chosen by buttons
  `00`/`04`/`08` and the firmware announces it on the message line. There is
  no routing layer in the emulator: each surface has its own window
  (`live_open`, `PANELS[]` in `src/paneldraw.h`).
- **Independence.** The main view and the panel are independent today and
  can be drawn by separate processes - `panel.exe` already draws the panel
  from a file. Each panel device is individually addressed, so a physical
  module can own any subset of ids.
- **Soft labels.** RENDERING.md quotes the cockpit patent: small CRTs carrying
  soft labels, ringed by lit momentary buttons. What this release puts on the
  wire is 8-character text, so the physical form of those displays in a 3.0
  pod is **unknown**; for building purposes it does not matter.
- The five green "MFD" bezels in the photographs ARCHITECTURE.md describes
  belong to a **later** pod generation. The 13.1.8 firmware has no notion of
  an MFD; it has displays, bar graphs and lamps.

---

## 4. Lamps, displays and other outputs: the Remote I/O output stream

### Framing, and the two modes

`rio_send` (`0x0215B924`) appends a payload to a 10,000-byte transmit ring at
`0x0217FBE8`-`0x021822F8` and, if the transmitter is idle, enables it; the
DUART's transmit interrupt (`0x0215B8D0`) drains it a byte at a time.
**firmware**

| | framed | raw |
|---|---|---|
| selected by | `0x0215B654`; at boot | `0x0215B686`; in a game (5 call sites) |
| flag | `[0x0218230B]` = 0 | `[0x0218230B]` = 1 |
| on the wire | `01 <seq & 3F> len len <payload> sum(payload)` | `<payload>`, back to back |
| parsed by `src/rio.h` | yes (`rio_walk`) | **no** |

The byte after `01`, which `rio.h` calls the node, is `[0x021822F8]`, and
`rio_send` increments it on every send (`0x0215B976`): it is a **sequence
counter**, not an address. In raw mode it still counts but is not sent.
**firmware**

A raw stream has no delimiters, so it can only be split by knowing each
opcode's length - which the builders give.

### Every output opcode

The builders are a contiguous family at `0x021435F6`-`0x02143902`, each a
thin wrapper round `rio_send`. **firmware**

| opcode | bytes | builder | callers | payload | meaning |
|---|---|---|---|---|---|
| `D1` | 10 | `0x0214372C` | 56 | id, 8 chars | text on display `id`. **documented** |
| `D1` x3 | 30 | `0x021435F6` | 46 | 24 chars | the **message line**: the same string split across displays `0x8C`, `0x8D`, `0x8E`. Called from the status-line printer around `0x02106120`. **firmware**, **measured** |
| `D2` | 3 | `0x0214387E` | 13 | id, count | bar graph. **documented** |
| `D3` | 3 | `0x02143820` | 552 | id, brightness | lamp. **documented** |
| `D3` loop | - | `0x02143852` | 1 | | every lamp `0x00`-`0x52` to the same brightness (the diagnostic's "all lights") |
| `D4` | 4 | `0x021437A6` | 14 | id, 2 chars | a **two-character field**. Used for display `0x8F` with `%02d` of a Mech field `+0xA8` / 100 (`0x02130EF0`), and for each weapon display `0x80`+i when a 32-byte weapon record's `+0xAA` is positive (`0x0214F8F0`). **firmware**; **not in `rio.h`** |
| `D5` | 1 | `0x021438B0` | 12 | - | sent once at boot; 12 sites. Meaning **unknown** (a panel reset is the obvious **hypothesis**) |
| `D6`-`DA` | 1 | `0x021438D6` | 5 | - | `0x36 + n` for analog `n` = `A0`-`A4`. `D6` is sent by `Get_Event` (`0x02122C0A`) on an `A0` report; `D7`-`DA` together at `0x02154E20`, followed by stick limits of `±0x50`. Re-zeroing the encoders is a **hypothesis** |

A physical panel therefore has to accept `D1`-`DA`; `D6`-`DA` can be
acknowledged and ignored by a panel whose sensors are absolute.

### What a game actually sends

Measured in this audit from a running 13.1.8 game (BadLands-16, MadCat Prime,
`tools/play.sh`'s packets) by reading the transmit ring. 183 messages, every
byte accounted for by the table above:

```
D1 80 "PPC     "  D3 01 01  D3 02 03  D3 03 01  D2 80 08     slot 0
D1 81 "E LAS MD"  D3 05 01  D3 06 03  D3 07 01  D2 81 08     slot 1
D1 82 "LASER MD"  ...                                         slot 2
D1 83 "LRM 1524"  ...                                         slot 3
D1 84 "        "  D3 11 00  D3 12 00  D3 13 00  D2 84 00     slot 4, empty
...                                                           slots 5-11
D1 8C/8D/8E blank                                             message line
D3 33 02  D3 32 03  D3 31 02  D3 30 03  ...                   mode lamps
D3 00 03  D3 04 02  D3 08 02  D3 10 02  D3 24 02 ...          function lamps
D3 60 00                                                      lamp 60
D1 90 "ENGINEOK"                                              engine status
```

and, as buttons are pressed:

```
B1 04  ->  D1 8C "MAP DISP" 8D "LAY SELE" 8E "CTED    "  D3 00 02  D3 04 03
B1 20  ->  D3 20 03  D1 8C "SEARCH L" 8D "IGHT ON " 8E "        "
B1 33  ->  D1 8C "ADVANCED" 8D " MODE EN" 8E "ABLED   "  D3 33 03  D3 32 02
B1 01  ->  D3 01 03
```

The weapon names and their order match the Mech's weapon records at entity
`+0x2C0`, which DEVICES.md lists as `PPC`, `E LAS MD`, `LASER MD`, `LRM 1524`
for each side - slots 0-3 and 6-9.

### The panel, as far as it is now known

| ids | what | evidence |
|---|---|---|
| displays `0x80`-`0x85`, `0x86`-`0x8B` | **weapon slots 0-5 and 6-11**, two banks of six: name, plus a 2-character field | **measured**, **firmware**. The banks being the manual's Weapons A and B is a **hypothesis** |
| bar graphs `0x80`-`0x8B` | one per weapon slot; 8 for a fitted weapon, 0 for an empty slot | **measured**. What the bars measure (charge, readiness, ammunition) is **unknown**: firing did not change them in this audit |
| lamps `4k+1`, `4k+2`, `4k+3` | three toggle lamps per weapon slot, 3 on / 1 off / 0 empty slot | **measured** |
| lamps `4k` (`00` ... `2C`) | the function buttons, each lamp on the button of the same id; `0C` and `14` were never sent | **measured**, **firmware** |
| lamps `30`-`3B` | mode switches in pairs: `30`/`31`, `32`/`33`, `36`/`37` lit one at 3 and its partner at 2; `34`, `35`, `38`-`3B` at 0 for this Mech | **measured** |
| lamps `3C`-`43`, `51`, `53` | function buttons; `3C`/`3D` flash 3 then return to 2 | **measured** |
| lamp `60` | sent 0 at game start; **unknown** | **measured** |
| lamps `50`, `52` | never sent in this audit; **unknown** | |
| displays `0x8C`-`0x8E` | the **24-character message line** | **measured** |
| display `0x8F` | a 2-character numeric readout, `+0xA8` / 100 | **firmware**; what `+0xA8` is is **unknown** (heat is the obvious **hypothesis**) |
| display `0x90` | **engine status**, `ENGINEOK` | **measured** |
| display `0x91`, bar graphs `0x8E` `0x90` `0x91` | **unknown**; never sent in this audit. The heat scale the hardware notes mention is presumably among them | |

**Brightness**: the values seen are 0, 1, 2 and 3. The firmware's pattern is
consistent with 0 off or not available, 1 available but off (weapon toggles),
2 available (function buttons), 3 active. The diagnostic accepts any byte, so
an output device should take at least four levels and should not clip higher
values. **measured**; the mapping of levels to meanings is a **hypothesis**.

### Can the outputs be extracted without touching the game?

**Yes.** Every lamp, bar graph and alphanumeric display is driven by the
firmware over the Remote I/O link with no help from us; extracting them needs
only the emulator to model its own DUART correctly. No game file needs
changing. What is missing is on our side - see the next section.

The **secondary display** is the exception: its content crosses the SecCom
ring to the Amiga, which is not run, and what the ring's messages contain is
**unknown** (UNRESOLVED.md).

Sound, the intercom and any cabinet outputs (fans, seat motion) are not in
this release's Remote I/O vocabulary; **unknown** whether the 3.0 cockpit had
any beyond the audio board.

---

## 5. Defects that block hardware output

Found in this audit; [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)
schedules them. **1-4 are fixed by Phase 0** (`--rio-baud`, the raw-mode
walker in `rio.h`, the 64 KB ring, `--rio-port`): a game now streams its
panel, and pressing `04` over the port brings back `MAP DISPLAY SELECTED`
with lamp `04` at 3 and `00` at 2. The descriptions below are of the code
as audited.

**1. The transmit queue wedges after the first frame.** `duart_isr` and
`duart_read` report TxRDY whenever the channel's transmitter is enabled - an
infinitely fast transmitter. `rio_send` enables the transmitter at
`0x0215B984` and only marks it busy at `0x0215B98C`, the next instruction. In
between, the emulator takes the transmit interrupt back to back, drains the
whole ring, finds it empty, disables the transmitter and clears the busy flag
(`0x0215B912`-`0x0215B91A`). Then `0x0215B98C` sets the flag. From then on
the flag says busy, the transmitter is off, and **nothing is ever sent
again**. Write-trapping `0x0218230A` shows exactly that order:

```
wtrap 0218230A = 00  from pc 0215B636     init
wtrap 0218230A = 00  from pc 0215B91A     ring drained, transmitter off
wtrap 0218230A = 01  from pc 0215B98C     rio_send marks it busy - too late
```

A real 68681 at 9600 baud takes about a millisecond per byte, so the flag is
set long before the ring empties. This is why DEVICES.md and ARCHITECTURE.md
record that "a boot emits one frame and no more", and why every diagnostic
capture had to come from a separate run: it was never the firmware.

Clearing the flag once from the command line (`--set-at 0215B698
2182308=00000001`) releases exactly one more message, `D1 80 "PPC     "`, and
the race wedges it again.

**2. In-game output is unframed, and `rio_walk` cannot parse it.** It skips
any byte that is not `01` and counts it as bad.

**3. `rio_apply` ignores `D4`**, and has no notion of `D6`-`DA`.

**4. The output capture is 8 KB and never wraps.** `duart_write` stores
channel A bytes into `g_riobuf[RIOCAP]`, `RIOCAP` 8192, and drops the rest;
`live_pump` and `rio_dump` only see the first 8 KB. Once defect 1 is fixed a
game fills that in minutes.

**5. Panel refresh and input are pumped every 200,000 instructions**
(`LIVE_EVERY`), about 50 ms at the emulator's speed. Fine for windows; worth
measuring against real buttons.

---

## 6. Telemetry

What game state can be read today, and through what.

**Stable** means a firmware-defined format the pod itself produces - a
network packet or a panel message - which the firmware will keep producing
whatever the emulator does. **Memory** means an offset in the entity record,
found by reverse engineering, readable only by peeking emulator memory.

| value | how it can be had | stability | evidence |
|---|---|---|---|
| position X, Y, Z | `0xEC` `+0x0C`.. floats; entity `+0x26`/`+0x2A`/`+0x2E` | **stable** (network; needs `--net`) | **documented**, named by the firmware's printf |
| heading | `0xEC`; entity `+0xF8` Course | **stable** | **documented**; h walks along (sin h, -cos h) |
| speed | `0xEC`; entity `+0x114` Speed | **stable** | **documented**; units of metres per hundredth |
| throttle position | entity `+0x84` (filed from `A0`); also known to whoever sends `A0` | memory | **documented** |
| stick, pedals | entity `+0x86`-`+0x8C` | memory | **documented** |
| control and pilot mode | lamps `30`-`33`; entity `+0x90`, `+0x8F` | **stable** (panel) | **measured** |
| torso, view, searchlight state | lamps `1C` `51` `24` `2C` `28` `20`; entity `+0xBB` bits | **stable** (panel) | **measured**, **documented** |
| weapon names, fitted slots | displays `0x80`-`0x8B` | **stable** (panel) | **measured** |
| weapon group assignment | lamps `4k+1`..`4k+3` | **stable** (panel) | **measured**; meaning a **hypothesis** |
| weapon readiness | bar graphs `0x80`-`0x8B` | **stable** (panel) | **measured**; meaning **unknown** |
| ammunition | `D4` on weapon displays when a weapon record's `+0xAA` > 0 | **stable** (panel) | **firmware**; that it is ammunition is a **hypothesis** |
| heat | possibly display `0x8F` (`+0xA8` / 100), bar graphs `0x8E`/`0x90`/`0x91` | - | **unknown** |
| engine status | display `0x90` | **stable** (panel) | **measured** |
| status and warning messages | the message line, `0x8C`-`0x8E`; also the HUD through `0x02106120` | **stable** (panel) | **measured** |
| damage taken | `0xEA` from the owning pod: thing, shooter, location, `-1` destroyed; per-location tables at entity `+0x490` | network **stable**; tables memory | **documented** |
| armour by location | records at entity `+0x2C0`, damage tables `+0x490` | memory | **documented**; layout partly known |
| target | button `40` announces `TARGET SELECT`; the selected target's identity is not located | - | **unknown** |
| destroyed | owner's `0xEA` with location `-1`; a dying pod sends `0xED` then `0xE2` and stops broadcasting | **stable** | **documented** |
| game started | `0xED` PLAYER_LINK accepted; Mech dropping while `+0xBB` bit 0 is set | network / memory | **documented** |
| game length, clock | `0xE5` `+0x3C`, hundredths | network | **documented** |
| game over | `GAME_OVER` exists in the console's vocabulary; its opcode is not mapped | - | **unknown** |

Tools that read these today: `--peek ADDR:LEN`, `--watch`, `--wtrap`,
`--mech-dump FILE`, `--send-log FILE`, and `tools/hub.py`, which decodes
`0xEC`, `0xEA` and `0xED` as they pass (`Hub.from_pod`).

For a physical cockpit, **the panel stream is the telemetry**: it is what the
firmware chose to show the pilot, it is stable, and it carries most of what
a cockpit needs. Peeking memory should be the last resort.

---

## 7. Unknowns that need more reverse engineering

| unknown | settled by |
|---|---|
| what weapon bar graphs measure; what the three slot lamps assign | fire each trigger group with the transmitter fixed and watch `D2` and `D3` |
| display `0x91`, bar graphs `0x8E`/`0x90`/`0x91`, lamps `50`/`52`/`60` | play longer games - take damage, overheat - with output flowing |
| what `+0xA8` is (display `0x8F`) | watch it against heat build-up |
| `D5` and `D6`-`DA` semantics | read their callers' context; a real Remote I/O board would settle it |
| `A3`, and whether `A1`/`A2` are one axis | read `0x0214E064` |
| what `A8` and `18` do | follow `+0x7C` bit 0, and `0x021361DC` from the `18` arm |
| keypad use in a game | overheat with the transmitter fixed and watch the message line |
| the secondary display's content | decode SecCom ring messages, or run `AMIGA3_0` on a second Musashi context |
| which physical board carries which id | not recoverable from firmware; for building, the logical grouping above is enough |
| the physical form of the 3.0 panel displays | photographs or a surviving 3.0 pod |

---

## Reproducing the measurements

All output goes to `out/` (gitignored). `$GF` is the release's
`BattleTech 13.1.8/Console Files/Game Files` directory.

```
# the start-of-game packets play.sh would send
VWE_GAME_FILES="$GF" PLAY_PKT=out/play.pkt BIN=none tools/play.sh

# a game, buttons pressed after the drop, and the transmit ring read at the end
./build/battlepod.exe "$GF/Full_Load_3_0" --duart 11000 --rstub 3FF00000 --rirq \
    --astub --monitor --clock 2000808 --set 40000100=1234567 \
    --packet-file out/play.pkt --rio-late 10000 \
    --rio-in "01 00 02 02 B1 04 B5 01 00 02 02 B0 04 B4" \
    --peek 021822F8:0x20 --peek 0217FBE8:0x2710 --steps 1200000000 --top 0

# the race, by write-trapping the busy flag
./build/battlepod.exe ... --packet-file out/play.pkt --wtrap 0218230A:2 --steps 400000000
```

The first `--peek` is the transmit state: read pointer at `+0x02`, write
pointer at `+0x06`, busy flag at `+0x12`, raw-mode flag at `+0x13`. The second
is the ring itself; the in-game stream runs from offset 6 (after the boot
`D5` frame) to the write pointer, and splits by the opcode lengths in the
table above.

`--rio-late` has to be smaller than the number of polls a run makes, or the
input is never delivered; the end-of-run report prints the poll count.
