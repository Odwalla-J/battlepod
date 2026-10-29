# Building the cockpit: the order of work

The roadmap for connecting physical cockpit hardware to battlepod, aimed first
at the smallest thing that proves the whole chain:

> **One physical MFD - an LCD and eight lit buttons - on USB, talking to a
> running game in both directions.**

What exists is [COCKPIT_INTERFACE.md](COCKPIT_INTERFACE.md); the architecture
being built toward is [HARDWARE_MAPPING.md](HARDWARE_MAPPING.md).

Phase 0 is done (2026-10-29): see its *Status* below. Steps that change
the emulator are marked.

---

## The first milestone, defined

A builder presses the MFD's second key. The emulated pod receives button
`04`, selects its map display, and answers on the Remote I/O link with
`MAP DISPLAY SELECTED` on the message line, lamp `04` at 3 and lamp `00` at 2.
The MFD shows the message, key 2 lights bright and key 1 drops to dim.

Every element of that is already confirmed on the firmware
(COCKPIT_INTERFACE.md, *What a game actually sends*), which is why it is the
first milestone: if it fails, the fault is ours, not an unknown in the game.

**Done when:**

1. the MFD enumerates on USB and the bridge finds it by its unique id, not by
   COM port;
2. all eight keys reach the game and all eight lamps follow the game, for the
   bindings below;
3. the message line updates on the LCD on every function key;
4. key press to lamp change, through the emulator, is under 100 ms, measured;
5. unplugging and replugging the MFD mid-game restores its lamps and text
   within a second, with no restart of anything;
6. the cockpit's own panel windows still work alongside.

The MFD's bindings - MFD C in HARDWARE_MAPPING.md:

| key | id | function | lamp |
|---|---|---|---|
| 1 | `00` | radar display | `00` |
| 2 | `04` | map display | `04` |
| 3 | `08` | damage display | `08` |
| 4 | `40` | target select | `40` |
| 5 | `3C` | radar 2 km | `3C` |
| 6 | `3D` | zoom in | `3D` |
| 7 | `43` | zoom out | `43` |
| 8 | `10` | indirect fire | `10` |

Screen: the 24-character message line (`8C`-`8E`), engine status (`90`), the
two-digit readout (`8F`), and a soft label over each key.

---

## Phase 0 - the emulator's side of the wire  *(changes the emulator)*

Four changes, all in `src/battlepod.c` and `src/rio.h`, all small. Without
the first, no hardware can ever see game output. They are correctness fixes:
the panel windows `cockpit.exe` already has will light up in a game for the
first time.

**0.1 A transmitter that takes time.** Today `duart_isr` and `duart_read`
report TxRDY on channel A whenever the transmitter is enabled, which wedges
the firmware's queue (COCKPIT_INTERFACE.md, *Defects*, 1). Model a byte time:
after each write to TBA, hold TxRDY low for a number of instructions, then
raise it. A 9600-baud byte is about 1.04 ms; at the default `--clock` divisor
(4096 instructions a hundredth) that is about 430 instructions. Make it a knob
like `--rirq LEVEL:DELAY`, say `--rio-baud`, because it is a calibration and
not a measurement. Leave channel B, the console, alone unless it shows the
same race.

*Check:* the write trap on `0x0218230A` alternates 1/0 with traffic instead
of sticking at 1; a game's capture holds the stream COCKPIT_INTERFACE.md
records; `make conformance` stays at 292/292 or every change is explained.

**0.2 Parse what a game sends.** In `src/rio.h`: a raw-mode walker that
splits by opcode length (`D1` 10, `D2` 3, `D3` 3, `D4` 4, `D5`-`DA` 1); `D4`
into the panel state as a 2-character field per id; `D5`-`DA` recognised and
counted, not dropped as bad bytes. The framed walker stays for boot and the
diagnostic menu. A stream can change mode mid-way; a frame starts with `01`,
and no raw opcode is `01`, so a combined walker can tell them apart.

*Check:* `paneltest --selftest` gains framed, raw and mixed cases, written as
our own bytes, not captured ones.

**0.3 Stream the output.** `g_riobuf` is an 8 KB capture that stops filling.
Make it a ring with a write count, and give each consumer - the panel
windows, `--rio-dump`, the port below - its own read cursor. `--rio-dump`
writes everything, not the first 8 KB.

**0.4 A port.** `--rio-port HOST:PORT`: every byte the pod transmits on
channel A goes out as UDP datagrams, and whatever arrives is appended to
channel A's receive side through `rx_append`, exactly as the hub's `R`
datagrams are today. Polled with `net_poll`'s cadence (every 4096
instructions, about a millisecond), not `live_pump`'s 200,000.

| direction | byte 0 | then |
|---|---|---|
| pod to bridge | `r` | Remote I/O bytes as transmitted: framed or raw, unaltered |
| bridge to pod | `R` | Remote I/O bytes for the receiver, as api.md defines `R` today |
| either | `H` / `A` | hello and acknowledge, as `--net` does |

Unaltered bytes, not decoded state: the bridge owns decoding, so the emulator
never needs to know a lamp from a display, and a capture of the port is a
capture of the wire. Document it in `docs/api.md`.

UDP on localhost needs no new dependency (`--net` already links Winsock).

**Status: done.** 0.1 is `--rio-baud` (default 9600, a byte every
`clock divisor x 1000 / baud` instructions, 426 at the default clock); 0.2
`rio_raw_len` and the combined `rio_walk`, with `paneltest --selftest`
raw and mixed cases; 0.3 `rio_since` and `struct rio_reader` over a 64 KB
ring; 0.4 `--rio-port`, `rioport_poll`, documented in api.md, with
`tools/riobridge.py`. Measured on BadLands-16: a 600M-instruction game sends
430 bytes - 1 frame and 104 raw messages, no errors - where it used to wedge
after the first; the port's stream is byte-identical to `--rio-dump`; and
`B1 04`/`B0 04` sent over the port is answered with the first milestone's
exact message and lamps.

---

## Phase 1 - the chain in software

Everything from the port to a module, with no hardware, so that Phase 2 only
has to prove the board.

**1.1 The bridge**, `tools/bridge.py`. Python, like `tools/hub.py`, for speed
of iteration. Its parts are the layers in HARDWARE_MAPPING.md:

- *adapter*: Remote I/O bytes to events (`text`, `bar`, `indicator`) and
  button events back to `B0`/`B1` frames;
- *model*: current state by logical name;
- *profile* and *layout*: plain text files, `profiles/battletech-13.1.8` and
  a sample layout for the milestone's MFD;
- *module manager*: discovery, `HELLO`, state replay on connect, heartbeats;
- *log*: every id the game sends that nothing is bound to - this is how the
  remaining unknowns get found during play.

**1.2 A virtual MFD**, `tools/vmfd.py`: a window with a 320x240 screen and
eight keys that speaks the module protocol over TCP on localhost. The bridge
cannot tell it from hardware.

**1.3 The module protocol, written down**, `docs/MODULE_PROTOCOL.md`, before
any firmware exists. Version 0:

Each message is `type u8, seq u8, payload, crc16`, COBS-encoded and ended
with `0x00`. COBS gives an unambiguous resync point after a lost byte, which a
hot-plugged serial link needs.

| from | message | payload |
|---|---|---|
| host | `HELLO?` | protocol version |
| module | `HELLO` | class, unique id, protocol version, firmware version, capabilities: keys, LED channels and levels, screen width, height and kind |
| host | `LED` | channel, level 0-255 |
| host | `LEDS` | first channel, levels... |
| host | `TEXT` | field, x, y, size, attribute, UTF-8 string |
| host | `BAR` | field, x, y, w, h, value, maximum |
| host | `CLEAR` | - |
| host | `PING` | - |
| module | `KEY` | index, down or up, module time in ms |
| module | `PONG` | module time |
| module | `ERROR` | code, text |
| host | `FRAME` | *reserved*: a rectangle of pixels, for later games |

Keys and LEDs are physical indices. Which logical control an index is lives
only in the bridge's layout, so the firmware never changes when the binding
does.

*Phase 1 is done when* the milestone's acceptance list passes with the
virtual MFD standing in for the board.

**Status: done in software (2026-10-29).** `tools/modproto.py` (the
protocol, with `--selftest`), `tools/bridge.py` (adapter, model, profile and
layout, module manager, the unbound-id log), `profiles/battletech-13.1.8`,
`layouts/milestone-mfd`, `tools/vmfd.py`, and `docs/MODULE_PROTOCOL.md`.
Against a live headless game (BadLands-16) the acceptance list:

1. the virtual MFD is found by its unique id (`vmfd-C`), never a port;
2. all eight keys reach the game and every lamp follows: the display keys
   (`00`, `04`, `08`) and `10`, `40` latch bright and drop the others to
   dim, the one-shots (`3C`, `3D`, `43`) flash;
3. the message line updates on every one - `RADAR DISPLAY SELECTED`, `MAP
   DISPLAY SELECTED`, `DAMAGE DISPLAY SELECTED`, `TARGET SELECT`, `RADAR AT
   2 KM RANGE`, `ZOOM IN RADAR`, `ZOOM OUT RADAR`, `INDIRECT FIRE MODE` -
   and the pod clears it two seconds later;
4. key to lamp, measured at the bridge through the emulator: 1-7 ms;
5. closing the virtual MFD and starting it again restores every lamp and
   field about two seconds after launch, nothing restarted;
6. the cockpit's own windows are untouched.

USB serial (`--serial`, pyserial) is written but has no hardware to meet
until Phase 2.

---

## Phase 2 - one physical MFD

**Parts.** A Raspberry Pi Pico (RP2040); a 2.0-2.8 inch 320x240 SPI LCD
(ST7789 or ILI9341); eight keys with LEDs - tactile switches and LEDs on a
board, or illuminated push buttons (12 V lamps will need a MOSFET per
channel). Anything equivalent will do; the protocol's `HELLO` reports what is
there.

**Firmware**, in a new `firmware/mfd/`: pico-sdk (BSD-3) and TinyUSB (MIT),
both compatible with this project's MIT licence.

- USB CDC with the unique id as the serial string;
- the protocol codec as one C file with no hardware in it, so the same file
  compiles into a host test;
- keys scanned and debounced (PIO or a timer), reported as `KEY` on both
  edges with a timestamp;
- LED levels as PWM, at least four visibly distinct steps;
- a small text and bar renderer with **our own font** - nothing of VWE's on
  the module;
- `NO LINK` and dimmed keys after two seconds of silence; a self-test on
  boot that lights every LED, as the firmware's own diagnostic did.

**Bring-up order**: USB echo; `HELLO`; keys to the bridge's log; LEDs from
the bridge; text; then the game.

---

## Phase 3 - stick, throttle and pedals

Start with an **off-the-shelf HID** joystick and pedals: no firmware, and it
settles the scaling questions before anyone builds a throttle quadrant.

- the bridge reads them through SDL2 (or `pygame`, which wraps it), and sends
  `C0 A0`-`A4` at a fixed rate and on change;
- scaling and calibration in the layout: throttle `0`-`0x340`, stick
  `±0x80`, pedals `0`-`0x340`;
- settle the open axis questions - `A1`/`A2` as one axis or two, what `A3`
  is - by reading `0x0214E064` and by flying;
- triggers `A5`-`A7` and a hat on the stick.

A custom RP2040 HID controller for a replica stick or throttle can follow; it
needs no protocol of its own.

---

## Phase 4 - the rest of the panel

- the other four MFDs and the lit-button panel, from the same board design;
- the weapons banks, paged or faithful (HARDWARE_MAPPING.md, *A layout for
  five MFDs*), chosen by building one and flying it;
- the keypad;
- several modules at once: discovery, naming, and a `--list-modules` in the
  bridge.

## Phase 5 - the unknowns, settled by playing

With output flowing (Phase 0) and the bridge logging unbound ids (Phase 1),
most of COCKPIT_INTERFACE.md's open list closes by playing and watching:

- what weapon bars and slot lamps mean: fire each group, run dry, take damage;
- heat: overheat, watch `8F`, the unknown bars, the message line and the
  keypad's "overheat codes";
- display `91`, bars `8E`/`90`/`91`, lamps `50`/`52`/`60`;
- `D5` and `D6`-`DA`.

Each answer is a line in the profile and a paragraph in DEVICES.md, with a
conformance checkpoint where the release can show it.

## Later, separate tracks

- **The secondary display**: decode the SecCom ring or run `AMIGA3_0` on a
  second Musashi context (ROADMAP.md). Until then the radar, map and damage
  screen does not exist, on any hardware.
- **Text on the head-up display** (RENDERING.md).
- **Other games**: a Red Planet profile; adapters for later engines when
  their interfaces are known.

---

## Testing

| level | what | where |
|---|---|---|
| unit | `rio.h` framed, raw and mixed walkers; `D4`; bad-byte counting | `paneltest --selftest`, our own bytes, runs in CI |
| unit | protocol codec: round trips, CRC, COBS resync after a dropped byte | host build of the firmware's codec file, in CI |
| unit | bridge adapter and model against a recorded port capture | Python, captures in `out/`, never committed |
| conformance | in a game: output frames flow; after `B1 04` the message line reads the map display and lamp `04` is 3; no bad bytes | new scenarios in `tools/conformance.sh`; needs the release; 292 today |
| conformance | the TX fix changes nothing else: every existing checkpoint, and the DUART interrupt counts recorded | `make conformance` |
| system | bridge plus virtual MFD plus `cockpit.exe`: the milestone list | by hand, then scripted with the virtual MFD driven from a test |
| hardware in the loop | latency: the MFD timestamps a key, the bridge logs the matching lamp, a histogram over 1,000 presses | bridge `--latency` |
| hardware in the loop | 100 unplug-replug cycles mid-game; an hour's game for buffer growth | by hand, with the bridge's log |
| builds | every exe still cross-builds and self-tests | netlab ([netlab.md](netlab.md)) |

Captures of a real game contain VWE output - weapon names, status text - and
stay in `out/`, like everything else derived from the release.

---

## Risks

| risk | likelihood | impact | mitigation |
|---|---|---|---|
| **The TX fix moves other behaviour.** Real byte times change when the firmware's interrupt handler runs, and a few checkpoints may shift | medium | medium | it is a knob; run the full harness at several values; record the interrupt counts before and after; explain every changed checkpoint |
| **The meaning of some outputs stays unknown** - weapon bars, heat, a few lamps | high | medium | profile files, an unbound-id log, Phase 5; the milestone uses none of them |
| **Five eight-key MFDs is a later pod's layout**, not 13.1.8's, and the weapons banks do not fit it | certain | medium | the layout is data; build one bank both ways and fly it |
| **No secondary display** - the radar and map a pilot leans on | certain | high for fidelity, none for the milestone | a separate track; the message line still announces the mode |
| **Emulator latency.** Input and panel are pumped every 200,000 instructions in the SDL build | medium | medium | the port polls at `net_poll`'s cadence; measured in Phase 2 |
| **Windows serial ports**: COM numbers change, a CDC device that resets mid-write | medium | low | identify by unique id; COBS resync; reconnect replays state |
| **Analog semantics** - which channel is which axis, and their sign | medium | medium | off-the-shelf HID first; calibration in the layout; read `0x0214E064` |
| **Later games' I/O is unknown**; Firestorm may share nothing with Remote I/O | high | low now | adapters are per game; the model and modules are not |
| **IP**: a module that carries VWE fonts or images, or a committed capture | low | high | our own fonts on modules; captures in `out/`; CONTRIBUTING.md applies to firmware too |
| **Electrical**: lamp current, 12 V illuminated buttons on 3.3 V logic | medium | low | MOSFET drivers; documented with the board, not the protocol |

## Where the code would go

| path | what |
|---|---|
| `src/battlepod.c`, `src/rio.h` | Phase 0 only |
| `tools/bridge.py`, `tools/vmfd.py` | the bridge and the virtual MFD |
| `profiles/`, `layouts/` | profile and layout files; our own text, no VWE data |
| `firmware/mfd/`, `firmware/common/` | RP2040 firmware; the protocol codec in `common` |
| `docs/MODULE_PROTOCOL.md` | the module protocol |
| `docs/api.md` | gains the `r` datagram |

Nothing here touches the emulator's core loop beyond Phase 0, and nothing in
the emulator learns what a module is.
