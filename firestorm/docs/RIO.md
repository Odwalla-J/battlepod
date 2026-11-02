# The RIO board, as Firestorm sees it

A Tesla II pod's controls reach `MW4.exe` from VWE's Remote I/O board on a
serial port (COM1). The game's driver is at
`0x566EB0`; everything here is read from it and confirmed by running the
game against the shim's virtual board.

## Messages

A message is an opcode byte with the top bit set, its payload in 7-bit
bytes, then a checksum: the sum of every byte before it, top bits masked, the
result masked to 7 bits.

Payload lengths for opcodes `0x80`-`0x8C` (table `0x76B630`, `-trio 0`, the
default): `0 0 0 1 2 2 2 10 1 1 2 2 1`. `-trio 1` selects a second table,
`0x80`-`0x8E`, for another board.

### Board to game

| message | meaning |
|---|---|
| `87` + 10 | five analog values, two bytes each, low 7 bits first, 14-bit two's complement: throttle, left pedal, right pedal, stick Y, stick X. Also the heartbeat: with none for 2.8 s the game resets the board |
| `88 id` | button `id` pressed |
| `89 id` | button `id` released |
| `FC` | acknowledge the game's last message. `FD` refuses it, and a refused message is sent again. The game sends one message at a time |

The game calibrates itself: the throttle counts down from the highest value
it has seen, the pedals up from the lowest. The shim sends throttle `0`
(closed) to `-800` (open), pedals `0`-`500`, stick `-75`-`75` (X negated).

The stick (`0x569060`) has a dead zone of 5 either side and is full at 75;
it is a rate, not a position - held over, the torso keeps moving. A stick
value of exactly `0`, though, means "as before": Y keeps its last value (the
torso goes on to its stop) and X takes its last value negated, so it flips
every message (the view shakes). A real stick's noise never rests on 0; the
shim sends `1` for centre.

### Game to board

| message | meaning |
|---|---|
| `82` | poll, whenever 50 ms pass with nothing heard |
| `83 00` | once, after the game resets the board by DTR |
| `84 id level` | a button's lamp: `00` off, `14` dim (available), `32`, `3C` bright (on) |

## Buttons

`0x40`, `0x46` and `0x47` are the stick's buttons, read directly (`0x566B70`)
as joystick buttons 0, 3 and 2; every other id is posted to the game as
window message `0x464` and handled by whatever display owns it. `0x3F` is the
throttle's button, `0x3E` the panic button.

| id | joystick button | fires |
|---|---|---|
| `0x40` | 0 (the trigger) | weapon group 1 |
| `0x47` | 2 | weapon group 2 |
| `0x46` | 3 | weapon group 3 |

All three held fire everything. The game also reads joystick button 1, which
no board message sets; set by hand, it does nothing.

The rest of the pod's own controls, from the Tesla II and System 4
technical manuals (`Firestorm/MW4-5.07_variants`) and pressed in a mission
through the shim:

| id | where | in Firestorm |
|---|---|---|
| `0x41`-`0x44` | the stick's four-way hat (the joystick board's J7-J10) | looks round while held: `0x42` (forward) down, `0x41` (back) behind, `0x44` left, `0x43` right |
| `0x45` | the stick's fourth button | nothing |
| `0x3F` | throttle board J3: the throttle button, unlit | **reverse, while held** (the speed reads negative; a tap does nothing) |
| `0x3E` | throttle board: the panic button (System 4: also the door switch) | nothing seen in the game |
| `0x38`-`0x3D` | throttle board, "not connected" / "future use" | nothing |

The manuals call the stick "five buttons and four hat positions", but its
board has eight inputs, `0x40`-`0x47`. The board numbers each button board's
eight inputs from its ribbon header: aux monitors 5 and 4 `0x00`, `0x08`; the
radar ("secondary screen") `0x10`, its two boards `0x10`-`0x17` and
`0x18`-`0x1F`; aux 1-3 `0x20`-`0x37`; the throttle board `0x38`; the
joystick board `0x40`. A hex keypad on the upper middle panel (and on System
4 a second) is for the RIO's diagnostics (code A9E), not the game. The game
lights lamps `0x1E`, `0x3B` and `0x3D`, where these pods wire no button.

The throttle (`0x568F50`) takes the highest value it has seen as closed and
counts down from it (800 is full); the record (`0x76B620`) starts over at
`0x80000000` whenever the RIO's setup runs for a game (`0x567D2D`,
`0x567EF9`). A difference of exactly 0 means "as before".

### MFDs

Each aux monitor has four buttons above and four below. With `base` the
monitor's board address, the top row is `base+7` to `base+4` left to right,
the bottom row `base+3` to `base+0`.

| MFD (left to right) | base | top row (as labelled in a mission) | bottom row |
|---|---|---|---|
| 1 | `0x28` | pilot, scores | - |
| 2 | `0x20` | -, NEXT WAYPOINT, PREV WAYPOINT, OBJECTIVES | ALL, ... |
| 3 | `0x30` | MUTE, NEAREST ENEMY, CROSSHAIR TARGET, ARMOR MODE | ZOOM MODE, JUMP MODE, FLUSH MODE, LIGHT AMP MODE |
| 4 | `0x08` | NEAREST FRIENDLY, NEXT FRIENDLY, PREV FRIENDLY, TARGET MODE | NEAREST ENEMY, NEXT ENEMY, PREV ENEMY, CROSSHAIR TARGET |
| 5 | `0x00` | GROUP FIRE, CHAINFIRE, NEXTWEAPON, PREVWEAPON | MODE, ASSIGNGROUP1, 2, 3 |

The labels change with each display's mode; the buttons do not.

### Radar

Six buttons above the screen and six below, as the game draws it -
landscape, its labels in rotated text: in the pod the monitor stood on its
side, and the rows were the left and right columns (`[radar] rotate = 90`
shows it so).

| row | ids, left to right | labels |
|---|---|---|
| top | `0x18`-`0x1D` | RANGE, RADAR MODE, LIGHT AMP, SEARCHLIGHT, AUTO CENTER, PILOT MODE (BAS / MID / ADV) |
| bottom | `0x10`-`0x15` | ZOOM, SHUTDOWN, AUXIL MODE (MAP / DMG / TGT), FLUSH, CROUCH, PROG MODE |

### Lamps lit at the start of a mission

Dim: `12 15 18 1B 1C` and most MFD buttons with a function; bright: `1E`,
`3B`. Pressing a button the game treats as a toggle brightens its lamp.

## The plasma sign

On top of each pod, facing the room, an orange dot-matrix plasma display
showed the pilot's name, and after a mission their rank and score in two
boxes under it. The game drives it itself, on a second serial port:
`\\.\com2`, 9600 baud, 8N1 (`0x5680B0`), unless started with `-noplasma`.
It only writes; nothing is read back.

Text is sent as plain bytes. Commands start with `ESC` (`1B`):

| bytes | meaning |
|---|---|
| `1B 40` | clear |
| `1B 52 x` | the next text's left edge |
| `1B 51 y` | the next text's top |
| `1B 4B f` | font: 4 and 5 are 10x14 dots, the others 5x7 |
| `1B 48 n`, `1B 47 n` | an attribute and a mode; the game sends 0 |
| `1B 58 x0 y0 x1 y1` | draw a box |
| `1B 78 0 x0 y0 x1 y1` | clear a rectangle |

The game (`0x56A060`) has three states: blank; the pilot's name, centred on
(56, 15) - large if it is nine characters or fewer, else small - so the sign
is 112 dots across and 32 high; and rank and score, in boxes at
(27-42, 19-30) and (42-93, 19-30), 5x7.

The shim answers the port and draws the sign in a window; the bytes also go
to the control port as `plasma <hex>`, for a real display. Over a pilot's
name it adds their rank on the top row (rows 0-6, unused by the game), in
the same commands, so a real display shows it too ([CAREERS.md](CAREERS.md)).
