# From the original cockpit to modern hardware

How the controls and displays the 13.1.8 firmware knows about map onto a
modular, buildable cockpit: microcontroller modules on USB, a host bridge, and
the emulator. What each original interface *is* - with source locations and
how sure we are of it - is in [COCKPIT_INTERFACE.md](COCKPIT_INTERFACE.md);
the order to build it in is [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).

This is a **proposal**. Where it relies on something not yet proven, it says so.

---

## The mismatch to design around

The cockpit we want to build - five MFDs, each an LCD with eight lit buttons,
plus stick, throttle, pedals and extra lit buttons - is modelled on
photographs of a **later** pod generation (ARCHITECTURE.md, *What it looks
like*). The 13.1.8 firmware has no notion of an MFD. What it drives is a flat
set of logical devices on one serial link:

| the firmware's devices | count | ids |
|---|---|---|
| lamps, almost all on push buttons of the same id | 65 | `00`-`3B`, `50`-`53`, `60` |
| 8-character alphanumeric displays | 18 | `80`-`91` |
| 2-character fields on some displays | - | `D4` to `80`-`8B`, `8F` |
| bar graphs | 15 | `80`-`91` less `8C` `8D` `8F` |
| analog channels | 5 | `A0`-`A4` |
| trigger buttons | 3 | `A5`-`A7` (and `A8`) |
| keypad | 1 | `D0` reports |
| main view | 1 | display lists |
| secondary display | 1 | SecCom, to an Amiga |

And it groups them, by the evidence in COCKPIT_INTERFACE.md, into: twelve
weapon slots in two banks of six, each a name, a number, a bar and three lit
buttons; about thirty function buttons with a lamp each; a 24-character
message line; an engine status display; a two-digit readout.

So the hardware cannot be designed as "MFD *n* is the game's display *n*".
It has to be designed as **modules that render whatever logical controls they
are bound to**, with the binding in a file. That one decision is what makes
the same modules serve 13.1.8 today, Red Planet with a different file, and a
later game when its interface is known.

---

## The four layers

```
 ┌─────────────────────────── 1. game-specific emulation ───────────────────────────┐
 │  battlepod / cockpit.exe: ROM3_0 on Musashi                                         │
 │  emits: the Remote I/O byte stream (D1-DA)   accepts: B0/B1/C0/D0 reports           │
 │  emits: main-view frames                     (unchanged: windows, --record)         │
 └───────────────────────────┬──────────────────────────────▲─────────────────────────┘
          raw Remote I/O bytes, both directions, over a local socket (--rio-port)
 ┌───────────────────────────▼──────────────────────────────┴─────────────────────────┐
 │  2. cockpit state and command interface                (the bridge, a process)     │
 │  game adapter:  Remote I/O bytes  <->  logical events and state                    │
 │  cockpit model: indicators, text fields, bars, buttons, axes, keys - by name       │
 │  profile:       game id  ->  logical name        layout: logical name -> module    │
 └───────────────────────────┬──────────────────────────────▲─────────────────────────┘
          module protocol: HELLO, STATE, LED, TEXT, BAR, BUTTON, AXIS, PING ...
 ┌───────────────────────────▼──────────────────────────────┴─────────────────────────┐
 │  3. hardware transport                                                              │
 │  USB CDC serial (MFDs, lamp and button panels)     USB HID (stick, throttle, pedals)│
 │  discovery, identity, reconnection, flow control                                    │
 └───────────────────────────┬──────────────────────────────▲─────────────────────────┘
 ┌───────────────────────────▼──────────────────────────────┴─────────────────────────┐
 │  4. physical modules                                                                │
 │  MFD: RP2040, SPI LCD, 8 lit keys     lamp/button panel     HOTAS     keypad        │
 └─────────────────────────────────────────────────────────────────────────────────────┘
```

### 1. Game-specific emulation

Stays as it is, with one addition: the Remote I/O byte stream it already
produces and consumes gets a **port** - a local socket - so another process
can read the output and write input. The emulator learns nothing about USB,
LCDs or modules. This is the seam ARCHITECTURE.md already chose ("Why the
seam is the RIO wire and not an API"), and it keeps every hardware concern out
of `battlepod.c`.

It needs three fixes first (COCKPIT_INTERFACE.md, *Defects*): a transmitter
that takes time, a raw-mode parser, and an output buffer that streams. Those
are emulator correctness fixes, not hardware features.

### 2. The cockpit state and command interface

A separate process, **the bridge**. Three parts:

**The game adapter** turns one game's wire format into the cockpit model and
back. For every Remote I/O game (BattleTech, Red Planet, Martian Football -
GAMES.md - all on the same panel hardware) it is one adapter: parse framed and
raw `D1`-`DA`, emit `B0`/`B1`/`C0`/`D0`. A future engine with another I/O path
gets another adapter, and nothing below it changes.

**The cockpit model** is game-neutral state, by name:

| kind | state | events |
|---|---|---|
| `indicator` | level, 0-255 (13.1.8 uses 0-3) | - |
| `text` | width, string | - |
| `bar` | value, maximum | - |
| `button` | pressed | down, up |
| `axis` | value, normalised -1..1 or 0..1 | changed |
| `key` | - | pressed |

**The profile** names the game's devices; **the layout** puts names on
modules. Both are data files the builder edits. A sketch:

```
# profiles/battletech-13.1.8   (game id -> logical name)
lamp 00      func.radar            button 00     func.radar
lamp 04      func.map              button 04     func.map
text 80      weapon.0.name         field 80      weapon.0.count
bar  80      weapon.0.bar          lamp 01..03   weapon.0.group.1..3
text 8C-8E   message (24)          text 90       engine
axis A0      throttle  0..0340     axis A4       pedals.turn  0..0340
button A5    trigger.1             ...

# layouts/my-cockpit            (logical name -> module, place)
mfd:C  key 1  func.radar           mfd:C  text  message
mfd:C  key 2  func.map             mfd:C  text  engine
hid:stick  axis X  stick.x         hid:throttle  axis Z  throttle
```

The model keeps the **authoritative state** of every name. Modules are
stateless mirrors of it, which is what makes reconnection trivial.

### 3. Hardware transport

| | USB CDC serial | USB HID |
|---|---|---|
| use for | MFDs, lamp and button panels, keypad | stick, throttle, pedals |
| why | bidirectional, any message size, text and bitmaps fit; built-in driver on Windows 10+, Linux, macOS; debuggable with a terminal | the OS sees a game controller: no driver, works in any game, read by SDL2 which the project already links |
| framing | COBS packets with a CRC-16; see IMPLEMENTATION_PLAN.md | HID reports |
| identity | USB serial string = the MCU's unique id; plus `HELLO` | VID/PID + serial string |

A module may be **composite** (CDC + HID): an MFD whose keys also appear as
HID buttons works as a plain button box when the bridge is not running.
Optional; not needed for the first milestone.

**Discovery.** The bridge enumerates serial ports by VID/PID, opens each, and
sends `HELLO`. The answer names the module class (`mfd`, `lamps`, `keypad`),
its unique id, protocol version, and capabilities (keys, LED channels and PWM
levels, display geometry and kind). The layout file refers to modules by
**unique id or a user-set name**, never by COM port number, which Windows
reassigns.

**Reconnection.** A module that disappears is marked offline; its bindings
keep their state in the model. When it comes back the bridge replays the full
state for its bindings. A module that hears nothing for two seconds shows
`NO LINK` and dims its keys. Nothing in the game notices.

**Timing.** The original link ran at 9600 baud, about a millisecond a byte,
and the start-of-game panel fill is 424 bytes (measured, 104 messages) - so a
real pod took nearly half a second to light its panel and nobody minded. USB full speed is far
faster. Targets: button to game under 10 ms at the bridge, lamp to LED under
20 ms, text to LCD under 50 ms. The largest term today is the emulator's
200,000-instruction pump (about 50 ms), which the port should not inherit.

### 4. Physical modules

| module | MCU | parts | talks |
|---|---|---|---|
| **MFD** | RP2040 | SPI TFT (320x240, ST7789 or ILI9341), 8 keys with LEDs, PWM per LED | CDC |
| lamp / button panel | RP2040 | up to 32 keys and LEDs via shift registers or an LED driver (e.g. TLC5940-class) | CDC |
| keypad | RP2040 (or on an MFD) | 3x4 matrix | CDC |
| stick, throttle, pedals | RP2040, or any off-the-shelf HID controller | potentiometers or Hall sensors, trigger switches | HID |

**RP2040 over ESP32**, for these modules:

- **native USB** with TinyUSB's CDC and HID classes, and a ROM bootloader that
  appears as a drive - firmware updates need no tools. The original ESP32 has
  no native USB (a USB-UART bridge only); the ESP32-S2/S3 do, and remain an
  option for anyone who wants wireless, which this design does not need;
- **PIO** for LED PWM and key scanning without CPU cost;
- 264 KB RAM holds a full 320x240x16 frame (150 KB) if ever needed;
- inexpensive and widely stocked, which matters for a builder's project.

The module protocol is the same for either chip, so the choice is not
binding.

**Rendering on the module, not the host.** The bridge sends *content* -
`TEXT weapon.0.name "PPC"`, `BAR weapon.0.bar 8/8`, `LED 3 level 3` - and the
module draws it with its own font and layout. That keeps USB traffic tiny,
makes a module work unchanged with an 8-character VFD or a 320x240 LCD, and
puts no VWE material on the module. A framebuffer message is reserved for
later games whose displays are pictures.

---

## Mapping the controls

| original | ids | modern | how it reaches the game |
|---|---|---|---|
| **joystick** | `A1`, `A2` (and probably `A3`) | HID axes X (and Y) | bridge reads SDL2 joystick, scales to `±0x80`, sends `C0 A1/A2 hi lo`. Whether `A1`/`A2` are halves of one axis decides the scaling - **unknown**, see COCKPIT_INTERFACE.md |
| **triggers** | `A5`, `A6`, `A7` | HID buttons on the stick | `B1`/`B0` |
| stick hat or extra buttons | e.g. `40` target, `1C`/`51` torso, `24`/`2C` view | HID buttons | `B1`/`B0`, bound in the layout |
| **throttle** | `A0` | HID axis | scale to `0`-`0x340`; never negative (the firmware zeroes below -10) |
| **foot pedals** | `A4` (turn), maybe `A3` | HID axis or axes | scale to `0`-`0x340`; whether pedals are one axis or two is **unknown** |
| **function buttons** | `00`-`2C` step 4, `3C`-`43`, `51`, `53` | MFD keys or panel keys | `B1`/`B0`; their lamp is the same id |
| **mode switches** | pairs `30`/`31`, `32`/`33`, `34`-`3B` | lit keys, or a latching switch | a latching switch sends the press for its new position; the lamps say which is active |
| **weapon toggles** | `4k+1`..`4k+3`, 36 keys | lit keys (see layout below) | `B1`/`B0` |
| **keypad** | `D0 key` | 3x4 keypad | `D0` |

## Mapping the outputs

| original | ids | modern |
|---|---|---|
| lamps | `D3 id level` | the LED in the key bound to `id`; PWM with at least four distinct levels for 0-3 |
| weapon names | `D1 80`-`8B` | text on a weapons MFD, or 8-character modules |
| weapon numbers | `D4 80`-`8B` | the same, a 2-character field beside the name |
| weapon bars | `D2 80`-`8B` | a bar drawn on the MFD, or a 10-segment LED bar |
| message line | `D1 8C`-`8E` | one 24-character line, the obvious home is the centre MFD |
| engine status | `D1 90` | a text field |
| two-digit readout | `D4 8F` | a numeric field; possibly heat |
| unknown displays and bars | `91`, bars `8E` `90` `91` | bind when known; the bridge logs any id it has no binding for |
| main view | display lists | a monitor - the emulator's SDL window, full screen on the chosen display. No change needed |
| secondary display | SecCom | a second monitor or a large LCD, once it is emulated. **Not available today** |

---

## A layout for five MFDs

One way to put 13.1.8 on five eight-key MFDs and one extra lit-button panel.
Every binding here is a line in a layout file, so this is a starting point,
not a decision. Buttons whose lamps pair up are kept together.

| module | screen shows | keys 1-4 (top) | keys 5-8 (bottom) |
|---|---|---|---|
| **MFD C** (centre top) | message line, engine status, `8F` readout | `00` radar, `04` map, `08` damage, `40` target | `3C` 2 km, `3D` zoom in, `43` zoom out, `10` indirect |
| **MFD L** (left top) | Weapons A: slots 0-5, name, number, bar, groups | slot 0-3 select | slot 4-5 select, group cycle, `A8` |
| **MFD R** (right top) | Weapons B: slots 6-11 | slot 6-9 select | slot 10-11 select, group cycle, `18` |
| **MFD LL** (lower left) | pilot interface: active modes | `32` basic, `33` advanced, `30` crosshairs, `31` stick turns | `34`-`37` (two pairs, **unknown** meaning) |
| **MFD LR** (lower right) | advanced functions | `20` searchlight, `28` centre, `1C` torso right, `51` torso left | `24` view up, `2C` view down, `53` FO, `3E` FO select |
| **lit-button panel** | - | `38`-`3B`, `3F`, `41`, `42`, `50`, `52`, `60` | |

The weapon banks are where five-by-eight is tight: each bank has 18 toggles.
Two ways, and the layout file can express either:

- **paged**: a slot key selects a slot, and three keys toggle its groups -
  fewer keys, one more press;
- **faithful**: a dedicated 6x3 grid of lit keys per bank on a lamp panel
  beside the MFD, the MFD showing names, numbers and bars only. Closer to what
  the lamp numbering suggests the 3.0 pod had.

The first milestone needs neither: it uses MFD C, whose eight keys and eight
lamps are all confirmed.

---

## Other games, and what is not known

- **Red Planet and Martian Football** run on the same firmware and panel
  (GAMES.md). Same adapter, a different profile. Their panel use has not been
  measured.
- **Later generations, Firestorm included.** Nothing in this repository
  describes their I/O, and nothing here should assume it resembles Remote
  I/O. The architecture is built so that a later game is a new adapter and a
  new profile; the modules, transport and model stay. Whether their MFDs
  carry pictures rather than text is **unknown**, which is why the module
  protocol reserves a framebuffer message.
- **The real 3.0 panel's physical arrangement** - which board each id sat on,
  how the weapon keys were laid out - is not recoverable from the firmware.
  A builder chasing exact fidelity needs photographs of a 3.0 pod, not a
  later one.
