# MFD display streaming: implementation

Phase 1 of the MFD hardware: Firestorm's five MFD pictures streamed,
pixel for pixel, through the cockpit bridge to virtual modules, with no
hardware. The protocol is [MFD_STREAMING_PROTOCOL.md](MFD_STREAMING_PROTOCOL.md);
what it asks of hardware is [MFD_HARDWARE_REQUIREMENTS.md](MFD_HARDWARE_REQUIREMENTS.md);
the starting point was [FIRESTORM_DISPLAY_AUDIT.md](FIRESTORM_DISPLAY_AUDIT.md).

Results in one paragraph: with `[stream] enable = 1`, five virtual receivers
show the five MFDs **byte-identical to the game's own surface** (15 of 15
snapshots in each of two live runs), **2-3 ms** after the game presents them
(95th percentile under 4 ms), at **0-25 KB/s per display** in the game
(35 KB/s peak). The game's thread pays **0.09 ms** a frame; with streaming
off it pays nothing measurable and the game runs as before. The bridge
keeps a slow module from building a delay, and lamps and keys keep working
alongside. 33 automated tests pass.

## What was built

| file | what |
|---|---|
| `firestorm/src/mfdstream.c`, `.h` | **new**: the encoder - tile diff, RLA, parts - and the module protocol's framing, in plain C |
| `firestorm/src/ddraw.c` | **added** a section "streaming the MFDs" (`stream_offer`, `stream_main` and helpers) and one call after `mfd_present()` in `present()`; nothing else in the shim changed |
| `firestorm/tests/streamtest.c` | **new**: the C encoder alone, for the tests |
| `firestorm/Makefile` | `mfdstream.c` in the DLL; `make test` |
| `tools/mfdstream.py` | **new**: the reference encoder and decoder |
| `tools/mfdrecv.py` | **new**: the virtual MFD receiver (a module, in a window or headless) |
| `tools/mfdsource.py` | **new**: a stand-in for the shim's stream port - synthetic scenes, recordings, load loops |
| `tools/test_mfdstream.py` | **new**: the tests |
| `tools/modproto.py` | `SYNC`, `FRAME_ACK`, screen kind 3, `HELLO`'s capability bytes; CRC and COBS now in C speed (`binascii.crc_hqx`, `bytes.split`), checked byte for byte against the old loops |
| `tools/bridge.py` | the stream: a client for the shim's port, forwarding to modules by `display N`, DEFLATE per module, key-frame requests, flow control; per-module queues with control messages first; non-blocking module sockets |
| `layouts/firestorm-mfds` | `mfd:N display N` for the five modules |
| `docs/MODULE_PROTOCOL.md`, `IMPLEMENTATION_PLAN.md`, `firestorm/README.md` | the new messages and settings |

### Integration points

- **Where the pixels come from.** `present()` already reads the MFD
  surface back into `d->rgb` (0x00RRGGBB, each channel widened from 5/6 bits)
  for the windows. The exporter takes that buffer as it is, after
  `mfd_present()` has drawn the windows - so the windows, their timing and
  `[mfd] tiles` mapping are untouched, and what is streamed is exactly what
  the windows show. Display *n* is the *n*th tile, the window "MFD *n*".
- **The game's thread** (`stream_offer`): if streaming is on and a client is
  connected, one `memcpy` of the 1.2 MB surface into a staging buffer, a
  timestamp, and an event. With streaming off: one integer test. No client:
  one more.
- **The streaming thread** (`stream_main`): swaps the staging buffer for its
  own (newest frame wins; frames are skipped, never queued), extracts each
  enabled display's channel, runs `ms_update`, and sends parts to the
  client with a blocking `send` - if the bridge stalls, only this thread
  waits. It also answers `HELLO?` and `SYNC`.
- **The bridge** connects to `127.0.0.1:9110` whenever it runs with `--game
  firestorm` (every 2 s until something answers), so a shim without
  streaming costs it nothing either.

## Choices, measured

### Codec

On real MFD frames (the full-page column is a snapshot; the update columns
are recorded live-mission sequences, re-encoded):

| | MFD 1 page | MFD 3 page | MFD 5 page | MFD 2 update, walking | MFD 2 update, mission | MFD 5 update, mission |
|---|---|---|---|---|---|---|
| raw | 307,200 | 307,200 | 307,200 | 1,474 (changed tiles) | 13,646 | 2,444 |
| byte RLE (PackBits) | 8,520 | 50,054 | 36,591 | | | |
| zlib -6, whole frame | 1,811 | 12,076 | 7,347 | 5,562 | 5,724 | 6,506 |
| zlib -6 of XOR with the last frame | | | | 563 | 1,904 | 489 |
| **RLA**, changed 16x16 tiles | **295** | 33,247 | 18,148 | **439** | 2,688 | **253** |
| **RLA + DEFLATE** (per part) | **174** | **11,610** | **5,855** | **330** | **1,304** | **147** |

(bytes; **[M]**.) RLA's "copy from above" is what MFD pages are made of:
rules, boxes and black. Alone it beats byte RLE 1.5-30x and whole-frame zlib
on everything but dense pages; with DEFLATE behind it, it beats every
alternative on every sample - and DEFLATE stays optional, so a module that
cannot inflate still gets a usable stream. An LZ codec (LZ4/heatshrink)
would sit between RLA and DEFLATE and adds a decoder to maintain for little
over RLA + the S3's ROM inflate; not adopted.

Decode complexity: RLA is `memset`/`memcpy` with bounds checks, writing
straight into the framebuffer - no window, no scratch. DEFLATE needs
`tinfl` (in the S3's ROM), ~11 KB state and one 4 KB output buffer.

### Tile size

Bytes per update, RLA, same sequences **[M]**:

| tile | MFD 2 walking | MFD 2 mission | MFD 5 mission |
|---|---|---|---|
| 8 x 8 | 335 | 2,517 | 293 |
| **16 x 16** | 439 | 2,688 | **253** |
| 32 x 32 | 660 | 3,136 | 262 |

8 x 8 wins a little on some displays and loses on others, at four times
the comparisons; 32 x 32 sends too much around small changes. **16 x 16**
it is (`[stream] tile` changes it - the protocol carries rectangles, not
tiles). The C encoder compares and codes a display in **0.19-0.31 ms** a
frame, worst 0.8 ms **[M]**.

## Configuration

`shim.ini` (beside the game; see `firestorm/README.md`):

```
[stream]
enable=1          ; default 0
port=9110         ; tcp, 127.0.0.1
displays=1,2,3,4,5
tile=16
max_fps=0         ; 0: every MFD frame the game draws (~20/s)
stats=1           ; log rates, sizes and coding time every 10 s
```

The bridge: `python tools/bridge.py --game firestorm [--stream-stats]
[--stream-port 9110]`. The layout gives each module its display
(`mfd:3 display 3`); a module gets pictures only if its `HELLO` says screen
kind 3.

## Running the virtual receivers

```
(firestorm-run\MW4\shim.ini: [stream] enable=1, then start the centre as usual)
python tools/bridge.py --game firestorm
python tools/mfdrecv.py --all                  five windows, half size, in a row
python tools/mfdrecv.py --uid vmfd-4 --tint 60ff80     one, full size, green
```

`--scale 1` is 1:1 (the default for one window); F2 shows the live
statistics; 1-8 or a click presses a key. Without the game,
`python tools/mfdsource.py --activity combat` stands in for the shim.
`--stats f.json` writes the statistics, `--record f.mfds` every FRAME
(playable with `mfdsource.py --play`), `--headless` runs without a window,
`--link-kbps 1000` reads no faster than a USB Full-Speed link would deliver.

## Build and test

```
cd firestorm && make && make test     (MSYS2 i686 gcc; make test needs Python)
python tools/test_mfdstream.py        (skips the C tests if streamtest.exe is not built)
python tools/modproto.py --selftest; python tools/bridge.py --selftest; python tools/mfdstream.py --selftest
```

`make install GAME=...` as before; stop the game first.

### The tests (33, all passing)

| group | what is checked |
|---|---|
| RLA | shapes (1x1, columns, rows, top row, corners), runs past 8192, noise, every malformed token |
| frame accuracy | every update reconstructs exactly; 60 frames of text, readouts, a live picture and page changes |
| static images | 20 unchanged frames send nothing; the frame number does not move |
| small changes | one pixel sends one 16x16 tile (and only it); two far apart send two |
| full-screen changes | page, noise, black, white, partial noise; parts stay within 4096 bytes and are numbered |
| DEFLATE | round trips; a module that did not offer it refuses it |
| missing messages | a lost update, a lost middle part, a lost last part: each detected, `SYNC`, recovered by a key frame |
| reconnection | a fresh receiver mid-stream; a new stream after the host restarts; a key frame while in step; duplicates ignored |
| five at once | five interleaved streams, every receiver hearing all of them, each showing only its own |
| protocol integrity | short header, wrong size, truncated body, rectangle off the screen or empty, wrong raw length, unknown codec, short and overlong RLA, cut rectangle header, bad DEFLATE, a DEFLATE bomb, an oversized body - each refused, then recovered; 200 random bit-flip runs on the wire never show a wrong picture |
| stress | 40 alternating pages and noise; 640 x 480 pictures and pages |
| the C encoder | byte-identical to the reference across sequences, key requests, odd sizes and tiles 8/16/32, full size |
| through the bridge | the real `bridge.py` with five receivers over sockets: exact pictures; unchanged frames send no bytes; a receiver unplugged and back; the shim restarting; a stalled receiver skipped and recovered while the others carry on; **lamps lit and keys pressed while pictures flow**; whole-page stress |

## Performance

### On the game **[M]**

Two live runs on this PC (RTX 2080, pod at 1600x1200 16-bit, 144 Hz
monitor), each: a mission launched by the console, 25 s standing, 25 s
walking and turning, 40 s firing with MFD keys pressed; five headless
receivers through the bridge. MFD frames come at ~20 per second (every
seventh main frame).

| | MFD 1 | MFD 2 | MFD 3 | MFD 4 | MFD 5 |
|---|---|---|---|---|---|
| updates/s, walking | 0 | 14.8-16.1 | 0 | 0 | 0 |
| updates/s, mission | 0 | 19.8-19.9 | 0.1 | 0 | 12.8-12.9 |
| bytes per update (mean / max) | - | 1,168-1,315 / 5,041 | 6,550 / 12,147 (a page) | - | 164 / 6,392 |
| average / peak, mission | 0 | 22.7-25.5 / 31.8-34.8 KB/s | 0.3 / 0.9 KB/s | 0 | 2.1 / 4.9-8.0 KB/s |
| key frame | 306 B | 4.9-5.2 KB | 12.1 KB | 2.5 KB | 6.3 KB |
| compression (pixels rewritten per byte) | | 10.5-11.1 | 25 | 125 | 19 |
| end-to-end latency, p50 / p95 / max | | 2.6 / 3.9 / 5.6-6.6 ms | | | 2.5-2.6 / 3.7-3.8 / 6.7-7.1 ms |
| decode, Python receiver, mean | | 0.61-0.65 ms | | | 0.11 ms |
| dropped or invalid / resyncs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

Latency is from the moment the game's thread hands over the frame to the
moment the receiver has the whole update decoded - copy, coding, the TCP
hop to the bridge, the bridge, the hop to the receiver, decoding - on one
clock (the performance counter, which Python's `perf_counter` reads). Key
frames, decoded whole in Python, take 7-17 ms.

The shim's cost, from its own statistics: the game's copy **0.085-0.088 ms**
a frame (worst 0.62 ms); coding on the streaming thread 0.19-0.31 ms per
display. Frame rate with streaming on **141.5-142.3 fps**, game and copy
1.69-1.80 ms; with it off (same DLL, a baseline run) **141.2-142.8 fps**,
1.74-1.78 ms - no difference within the noise.

**Fidelity:** at the end of each phase the shim's `snap` wrote the 1280x480
surface; every display's channel of each snapshot is byte-identical to a
frame the receiver rebuilt - **15 of 15 in each run** - typically the last
one it received.

What this mission did *not* exercise: there was no enemy, so MFD 4's live
target view (4 % of its pixels a frame in the audit) never moved, and the
MFD keys pressed changed one page. The synthetic loads below cover both.

### Synthetic loads **[M]**

`mfdsource.py --loop 40` serves pre-coded 640x480 scenes at 21 fps through
the real bridge to five receivers, 25 s each.

| scene | per display | updates/s | bytes/s each | latency p50 / p95 | skipped |
|---|---|---|---|---|---|
| combat: readouts on all five, a 280x200 live picture on MFD 4 every frame | MFD 4 | 21.0 | 195 KB/s (9.5 KB/update) | 3.2 / 4.3 ms | 0 |
| | MFDs 1-3, 5 | 20.8-21.0 | 4.6-5.7 KB/s | 1.6-2.5 / 2.6-3.6 ms | 0 |
| stress: a new page or 30 % noise every frame, all five | each | 18.8 | 1.65 MB/s (90 KB/update) | 760 / 875 ms | 0 |
| stress, no DEFLATE | each | 17.7 | 2.0 MB/s (115 KB/update) | 840 / 950 ms | 0 |
| stress, links limited to 1000 KB/s (USB FS) | each | 4.5-5.4 | 340-420 KB/s | 160-210 / 337 ms | 11-13 / 10 s |
| stress, links limited to 300 KB/s | each | 3.1-3.5 | 228-239 KB/s | 550-590 / 735-741 ms | 13-14 / 10 s |

Readings:

- **Combat-like load is comfortable**: even a 280 x 200 picture changing
  every frame is 195 KB/s after DEFLATE - a fifth of a Full-Speed link. Its
  pre-DEFLATE size is 1.17 MB/s; the synthetic picture is a regular pattern
  that DEFLATE squeezes far better than a rendered 3D view would, so for
  the real target view expect between the two, nearer the audit's 4 %
  (~12 k pixels, ~12 KB a frame) - **to be measured with an enemy**.
- **Stress is ~50x the game's load**, and finds the Python bridge's limit:
  about 8-11 MB/s through it in total, beyond which updates wait in the
  shim-to-bridge connection (the 760 ms). The real shim skips frames when
  the bridge falls behind and keeps its socket buffer small (256 KB), so
  the wait is bounded there.
- **On a slow link the pacing works**: the bridge skips a busy module to
  the next key frame instead of queueing, so the delay stays near two
  updates' transmission time (0.34 s at 1000 KB/s with 90 KB updates,
  0.74 s at 300 KB/s) instead of growing. A first version let it grow to
  4-6 s - the acknowledgement timeout was shorter than a slow link's honest
  acknowledgements, socket buffers hid the backlog, and a skip cut off a key
  frame in transit; all three were fixed and re-measured.

### Against the targets

| target | result |
|---|---|
| up to 21 updates/s per MFD | **met**: 21.0 updates/s on every display in the synthetic combat load; the game itself draws ~20 |
| ~1 MB/s transport per module | **met with room**: game ≤ 36 KB/s peak per display; synthetic combat 195 KB/s on the busiest; only the 50x stress needs more, and then degrades gracefully |
| < 30 ms added latency | **met on this PC**: p95 < 4 ms in the game, < 5 ms synthetic combat. Hardware adds USB transfer (an update of 1-5 KB: 1-5 ms at 1 MB/s) and panel scan-out (≤ 17 ms at 60 Hz) **[E]** |
| pixel-perfect | **met**: 15/15 live snapshots exact twice; every automated reconstruction exact |

## Known limitations

1. **MFD 4's target view was not measured in real combat** (no enemy in the
   test mission). Synthetic stand-ins bracket it; measure with a bot.
2. **The Python bridge tops out near 8-11 MB/s** of pictures. That is ~50x
   the game's traffic; a C bridge or moving the forwarding into the shim
   would lift it if ever needed.
3. **A receiver shows a mixed picture after a fault** until the key frame
   arrives (tens of ms on this PC), because rectangles go straight into the
   framebuffer. Firmware with spare PSRAM can double-buffer (hardware
   document).
4. **The shim answers a key-frame request within 50 ms** (its thread wakes
   for frames or every 50 ms).
5. **One stream client** at a time, on 127.0.0.1: the bridge runs on the
   pod's PC.
6. **Latency was measured with one clock** on one PC. Hardware cannot
   timestamp against the host; there, `PING`/`PONG` round trips and
   `FRAME_ACK` timing are the tools.
7. The **Python receiver** decodes a dense full frame in ~20-25 ms - fine
   for a window, not a measure of the S3.
8. **`mfdsource.py` codes live in Python**, too slowly for heavy scenes at
   full size; `--loop` pre-codes them.
9. The launcher has no switch for streaming yet; it is a `shim.ini` line.
