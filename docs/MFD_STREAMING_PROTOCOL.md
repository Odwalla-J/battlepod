# MFD display streaming protocol, stream version 1

How Firestorm's five MFD pictures travel from the game to a display module:
the shim's exporter, through the bridge, to a module that keeps a
framebuffer and drives an LCD. It sits inside the cockpit module protocol
([MODULE_PROTOCOL.md](MODULE_PROTOCOL.md)): same framing, same session, three
more message types. A module that does not want pictures never sees them.

The reference implementation is `tools/mfdstream.py` (encoder and decoder,
`--selftest`); the shim's encoder is `firestorm/src/mfdstream.c`, held to the
reference byte for byte by `tools/test_mfdstream.py`. The virtual receiver
`tools/mfdrecv.py` is the module, in a window. Why it looks like this, with
the numbers: [MFD_STREAMING_IMPLEMENTATION.md](MFD_STREAMING_IMPLEMENTATION.md).

```
MW4.exe -> shim (ddraw.dll)          bridge (tools/bridge.py)          module
           exporter thread  --tcp-->  per module: queue, DEFLATE  --USB CDC / tcp-->  decoder -> LCD
           127.0.0.1:9110             key-frame requests                              FRAME_ACK, SYNC
```

## Pixels

A frame is **640 x 480 pixels, 8-bit grey, one byte a pixel, row by row from
the top left**. Each MFD is one colour channel of the game's 1280 x 480
RGB565 surface: MFDs 1, 3 and 4 are 5-bit red or blue channels, 2 and 5
6-bit green ([FIRESTORM_DISPLAY_AUDIT.md](FIRESTORM_DISPLAY_AUDIT.md) §1).
The byte is the channel widened to 8 bits by bit replication - exactly what
the shim's windows show - so the source level comes back exactly:
`level = v >> 3` (5-bit) or `v >> 2` (6-bit), and a 5-bit channel only ever
holds 32 of the 256 values. Nothing is scaled, filtered or dithered. The
module turns grey into its panel's format (RGB565, a tint, a gamma table).

Width and height travel in every FRAME, so a different screen size needs no
new version; a module refuses a size it does not have.

## Messages

| type | name | from | payload |
|---|---|---|---|
| `0x16` | `FRAME` | host | one part of one update, below |
| `0x23` | `SYNC` | module | display (u8, 0 if it does not know yet), reason (u8), the frame it holds (u32) |
| `0x24` | `FRAME_ACK` | module | display (u8), frame (u32), decode time in µs (u32) |

`SYNC` reasons: 0 new (just started), 1 gap (a part or update missing), 2
bad data, 3 wrong size, 4 new stream. They are for logs; the answer is the
same.

The bridge speaks the same three to the shim's stream port: the shim sends
`FRAME`, the bridge sends `SYNC` and `HELLO?` (the shim answers `HELLO`,
class `mfd-source`).

### HELLO's capability bytes

A module with a framebuffer says so in `HELLO`: screen kind **3 (8-bit grey
framebuffer)**, its width and height, and three more bytes after the screen
kind:

| bytes | field |
|---|---|
| 1 | capabilities: bit 0, it can inflate DEFLATE |
| 2 | the largest FRAME body it takes, after inflating (at least 4096) |

Older modules end at the screen kind; the bridge reads a short `HELLO` as
"no capabilities". This is an addition, not a new protocol version.

## FRAME

| bytes | field |
|---|---|
| 1 | stream version, 1 |
| 1 | display, 1-5 |
| 1 | flags: bit 0 `KEY` (a whole frame), bit 1 `END` (the update's last part), bit 2 `DEFLATE` |
| 1 | reserved, 0 |
| 4 | stream id |
| 4 | frame number |
| 2 | part number, from 0 |
| 2 | width |
| 2 | height |
| 4 | capture time, µs, the host's clock (diagnostics only) |
| n | the body: rectangles, or with `DEFLATE` one raw DEFLATE stream of them |

22 bytes of header. Big-endian, as everywhere in the module protocol.

An **update** is one or more parts with the same stream id and frame
number, numbered from 0, the last flagged `END`. Its body never exceeds
**4096 bytes** (before DEFLATE), so a module needs one 4 KB buffer. A
**delta** update (no `KEY`) turns frame *n*-1 into frame *n*. A **key**
update (`KEY`) is a whole frame and needs nothing before it; it keeps the
number of the frame it shows, so a key frame of frame *n* may follow the
delta that made frame *n*. Frame numbers are per display and per stream;
the **stream id** changes whenever the host starts again (the game
restarted, the bridge reconnected to it), so a module can tell an old frame
14 from a new one.

### Rectangles

| bytes | field |
|---|---|
| 2 | x |
| 2 | y |
| 2 | width |
| 2 | height |
| 1 | codec: 0 `RAW`, 1 `RLA` |
| 2 | length of the data |
| n | the data |

A rectangle replaces those pixels of the framebuffer. Rectangles of one
update never overlap, and come top to bottom (by their top edge), left to
right - which `RLA` depends on.

**RAW**: width x height bytes, row by row.

**RLA** (runs, literals, above): tokens, pixels left to right and top to
bottom through the rectangle, crossing row ends:

| token | meaning |
|---|---|
| `00nnnnnn`... | `LIT`: *n* literal bytes follow |
| `01nnnnnn`... | `REP`: the previous pixel, *n* more times |
| `10nnnnnn`... | `UP`: the framebuffer's pixel directly above, *n* times |
| `11xxxxxx` | invalid |

The count: if bit 5 is clear, *n* = (low 5 bits) + 1, 1-32; if bit 5 is
set, *n* = ((low 5 bits) << 8 | next byte) + 1, 1-8192. The "previous
pixel" starts at 0 for each rectangle and is the last pixel written. `UP`
reads the framebuffer row above as it stands - for the rectangle's first
row that is the row above the rectangle, which the receiver already has,
and which is already the new frame's (it is either unchanged, or earlier in
this update: hence top to bottom). `UP` on row 0 of the screen is invalid.
A token that would write past the rectangle, data that ends early, or data
left over is an error.

Why this and not plain RLE or LZ: MFD pages are text and lines on black,
and their rows repeat the rows above. On real frames `RLA` is 1.5-30x smaller
than byte RLE and on most pages smaller than zlib, and it decodes with nothing but
`memset` and `memcpy` straight into the framebuffer - no window, no table,
no scratch buffer. See the implementation document for the measurements.

**DEFLATE** (flag bit 2): the body is a raw DEFLATE stream (RFC 1951, no
zlib header) that inflates to the rectangles, at most the module's declared
maximum. The bridge applies it, part by part, for modules that say they can
inflate - the ESP32-S3 has `tinfl` in ROM - and only when it makes the part
smaller. It halves page changes and key frames or better; small updates gain
less.

### What the module does

```
on FRAME:
  version != 1                                    -> ignore
  display not mine (I have one)                   -> ignore, unless it is a KEY part 0 and the host is moving me
  width/height not my screen                      -> SYNC (wrong size)
  KEY, part 0                                     -> start it, whatever state I am in
  the next part of the update I am building       -> go on
  part 0 of frame (mine + 1), same stream, synced -> start it
  the frame I already have, not KEY               -> ignore (a duplicate)
  anything else                                   -> out of step: SYNC, drop until a KEY comes
  apply the rectangles; any error                 -> SYNC (bad data)
  END                                             -> the frame is whole: show it, FRAME_ACK
```

While out of step a module sends `SYNC` at most four times a second. It
shows the frame it has (with a marker - `mfdrecv.py` says SYNC) until the
key frame is whole. Because rectangles are written straight into the
framebuffer, a frame that fails part way leaves a mixed picture until the
key frame lands - usually within 100 ms. Firmware that must never show that
keeps a second buffer; the protocol does not need it to.

### What the host does

The shim keeps, per display, the frame it last sent. Each time the game
presents the MFDs it compares the new frame with that one in 16 x 16 tiles,
and sends the changed tiles - joined along each tile row into rectangles,
split until each fits a part - as one update. An unchanged frame sends
nothing at all. A new client gets a new stream id, and its first update for
each display is a key frame.

The bridge never decodes pixels. For each module it tracks:

- **want key** - it has just said `HELLO`, sent `SYNC`, or been skipped.
  It gets nothing until the next key frame, which the bridge asks the shim
  for (`SYNC`, at most five times a second per display);
- **in flight** - updates sent but not yet acknowledged. At most **2**: if
  a new update finds a module with 2 in flight, or more than 64 KB of
  pictures queued for it, the bridge skips it to the next key frame instead
  of queueing. A slow module therefore falls back to fewer frames rather than
  to a growing delay. An acknowledgement missing for a second is written off.

Control messages (`LED`, `TEXT`, `PING` ...) go ahead of `FRAME`s in each
module's queue, at message boundaries: a lamp waits for at most the one part
on the wire (4 KB: 4 ms at 1 MB/s). Keys come the other way and never wait
for pictures.

## Errors and recovery

| what goes wrong | how it is caught | what happens |
|---|---|---|
| a byte corrupted on the link | CRC-16 | the message is dropped; the next update is out of step: `SYNC`, key frame |
| a part or a whole update lost | part / frame numbers | `SYNC`, key frame |
| garbage, wrong codec, a rectangle off the screen, a truncated or oversized body, a DEFLATE bomb | the decoder's checks | `SYNC`, key frame |
| the module rebooted or was replugged | its `HELLO` | the bridge replays lamps and text, then a key frame |
| the game restarted | a new stream id | the next update is a key frame for everyone |
| the bridge restarted | the module's `HELLO` | as a replug |
| a module too slow for the stream | the window, the queue | skipped to the next key frame |
| a lost `FRAME_ACK` | a one-second timeout | written off |

A received frame is therefore always either exactly the game's frame or
known not to be. `tools/test_mfdstream.py` checks every row of this table.

## Transport

- **USB CDC serial** (a real module) or **TCP** (`mfdrecv.py`, port 5181):
  the module protocol's transports, unchanged.
- **Shim to bridge**: TCP on `127.0.0.1`, `[stream] port` (default 9110),
  one client at a time.
- Bandwidth per module, measured (the implementation document has the
  tables): 0-25 KB/s per display in the game, 35 KB/s peak; a synthetic
  live picture every frame on MFD 4, 195 KB/s. USB Full Speed CDC gives
  roughly 0.8-1 MB/s, so even the synthetic case uses a fifth of a link.
  A link too slow for its display is paced by the bridge (above), not
  backed up.

## Versions

Stream version 1 is this document. A module that sees another version in a
`FRAME` ignores it. New codecs or flags make a new stream version; the
module protocol's version stays 0, since everything here is new message
types and trailing `HELLO` bytes older modules do not read.
