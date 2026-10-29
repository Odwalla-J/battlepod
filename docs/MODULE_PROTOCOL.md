# The cockpit module protocol, version 0

How the bridge (`tools/bridge.py`) talks to a cockpit module - an MFD, a
lamp and button panel, a keypad - whether the module is an RP2040 on USB
serial or `tools/vmfd.py` on a TCP socket. The bytes are the same either way,
so the bridge cannot tell a virtual module from a real one, and firmware can
be written against `tools/modproto.py`, which is the reference
implementation and has a self-check (`tools/modproto.py --selftest`).

Where this sits: layer 3 of [HARDWARE_MAPPING.md](HARDWARE_MAPPING.md), *The
four layers*. Why it looks like this: [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md),
Phase 1.

## Framing

A message is

| bytes | field |
|---|---|
| 1 | type |
| 1 | seq: the sender's count of its messages, wrapping; for logs and for spotting a gap |
| n | payload, by type |
| 2 | CRC-16/CCITT-FALSE (polynomial `0x1021`, initial `0xFFFF`, not reflected, no final XOR) of type, seq and payload; big-endian |

COBS-encoded, then a single `0x00`. COBS leaves no zero inside a packet, so
after a lost or corrupted byte the next `0x00` is an unambiguous place to
start again - which a hot-plugged serial link needs. A packet that fails to
decode or whose CRC is wrong is dropped, and counted.

Integers are big-endian. A string is a length byte and that many bytes of
UTF-8.

The CRC's check value is `CRC("123456789") = 0x29B1`.

## Messages

| type | name | from | payload |
|---|---|---|---|
| `0x01` | `HELLO?` | host | protocol version (u8) |
| `0x02` | `HELLO` | module | version (u8), class (str), unique id (str), firmware (str), keys (u8), LED channels (u8), PWM bits per LED (u8), screen width (u16), height (u16), screen kind (u8: 0 none, 1 monochrome, 2 colour, 3 8-bit grey framebuffer); a framebuffer module adds capabilities (u8) and its largest FRAME body (u16) - [MFD_STREAMING_PROTOCOL.md](MFD_STREAMING_PROTOCOL.md) |
| `0x10` | `LED` | host | channel (u8), level (u8, 0-255) |
| `0x11` | `LEDS` | host | first channel (u8), levels (u8 each) |
| `0x12` | `TEXT` | host | field (u8), x (u16), y (u16), size (u8), attribute (u8), the text (UTF-8, the rest of the payload) |
| `0x13` | `BAR` | host | field (u8), x, y, w, h (u16 each), value (u16), maximum (u16) |
| `0x14` | `CLEAR` | host | - |
| `0x15` | `PING` | host | - |
| `0x16` | `FRAME` | host | part of a picture: changed rectangles of a display, for games whose displays are pictures - [MFD_STREAMING_PROTOCOL.md](MFD_STREAMING_PROTOCOL.md) |
| `0x20` | `KEY` | module | index (u8, from 1), down (u8: 1 down, 0 up), module time in ms (u32) |
| `0x21` | `PONG` | module | module time in ms (u32) |
| `0x22` | `ERROR` | module | code (u8), text (the rest) |
| `0x23` | `SYNC` | module | display (u8), reason (u8), frame (u32): send me a whole frame |
| `0x24` | `FRAME_ACK` | module | display (u8), frame (u32), decode µs (u32): this frame is on the screen |

**Keys and LEDs are physical indices.** Key 1 is the module's first key and
LED channel 0 its first LED; on an MFD, keys 1-4 sit above the screen left
to right and 5-8 below, and LED channel *n* is in key *n*+1. Which logical
control a key is lives only in the bridge's layout file, so firmware never
changes when a binding does.

**Fields.** `TEXT` and `BAR` name a field; a later message for the same
field replaces it (the module erases what it drew there). Coordinates are the
screen's pixels from its top left. `size` is a scale - 1 small, 2 normal, 3
large - for the module's own font; the module draws the text, the host never
sends glyphs, so no VWE material is ever on a module. Fields 101-108 are the
soft labels over keys 1-8: a module draws them on or beside the key, wherever
its key is.

## A session

```
host                                  module
HELLO?  version 0          ->
                           <-         HELLO  mfd vmfd-C ... 8 keys 320x240
CLEAR                      ->
TEXT 101..108  key labels  ->
LED  0..7      every lamp  ->
TEXT / BAR     every field ->          (the whole state, from the model)
PING           every 0.5 s ->
                           <-         PONG
LED, TEXT ...  as the game changes them
                           <-         KEY 2 down / KEY 2 up
```

The host keeps the authoritative state of everything; a module is a
stateless mirror of it. So when a module comes back - replugged, rebooted,
its window reopened - the host replays everything after its `HELLO`, and
nothing in the game notices it was gone.

A module with a framebuffer that the layout gives a display (`display N`)
then gets that display's pictures: a whole frame first, then the changed
parts ([MFD_STREAMING_PROTOCOL.md](MFD_STREAMING_PROTOCOL.md)). The bridge
sends lamps and text ahead of pictures, so a lamp never waits behind one.

A module that hears nothing from the host for **two seconds** shows
`NO LINK` and dims its keys. A host that hears nothing from a module for five
drops it.

## Identity

The layout refers to a module by its **unique id** - for an RP2040 its flash
unique id, also its USB serial string - or a name the builder gives it,
never by COM port, which Windows reassigns. `vmfd.py --uid` sets a virtual
one's.

## Transport

| | USB CDC serial | TCP |
|---|---|---|
| who | a real module | `tools/vmfd.py`, and anything else in software |
| where | any port; `bridge.py --serial COM5` (pyserial) | the module connects to the bridge, `127.0.0.1:5181` by default |
| speed | line settings are ignored by CDC; 115200 is opened | - |

## Versions

Version 0 is this document. A module answering `HELLO?` with a version it
does not share says so with `ERROR` and stops; additions that older modules
can ignore - new message types - do not need a new version.
