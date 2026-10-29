# firestorm-shim

Runs VWE's Firestorm (`MW4.exe` 5.07, a MechWarrior 4 build for the Tesla II
pod) on an ordinary Windows PC: the main view, the five MFDs and the radar
each in a window of their own, and a whole centre - operator console,
launcher and pod - on one machine if wanted.

Two programs, and no change to any game file:

- **`ddraw.dll`**, which sits beside `MW4.exe`. It stands in for the pod's
  three graphics cards, runs every full-screen display windowed, and smooths
  over the places the game assumes it is alone on a dedicated pod PC.
- **`fsrun.exe`**, which starts `MW4.exe` (or `Launcher.exe`) with the shim
  already loaded, so it is in place before the game's first instruction.

No game files are in this repository. Test against a **copy** of the game,
and tell the tools where it is: `make install GAME=C:/path/to/MW4`,
`launcher/fslauncher.py --game C:\path\to\MW4`, `tools/start-centre.ps1
-Game C:\path\to\MW4`. (Their default, `../firestorm-run/MW4`, is beside a
standalone checkout of this folder.)

## Build

MSYS2 with the 32-bit toolchain (`pacman -S mingw-w64-i686-gcc`), then:

```
make            # build/ddraw.dll, build/fsrun.exe
make install    # copy both into $(GAME), default ../firestorm-run/MW4
make uninstall
```

Stop every `MW4.exe` and `Launcher.exe` before `make install`: a running copy
holds `ddraw.dll` open and the copy fails.

## The launcher

`launcher\Firestorm Launcher.cmd` (or `python launcher/fslauncher.py
--game C:\path\to\MW4`) opens a window for setting a PC up and starting it:

- **This PC** - a pod, the console, or both on one PC; the game folder, the
  network addresses, monitors and gamepads it sees.
- **Displays** - resolution and colour depth, the main view full screen on a
  monitor or in a window, the MFDs as five windows or one wall, their tint,
  the radar upright, the plasma sign, and every cockpit window's place, on a
  map of the monitors drawn to scale. *Arrange on this monitor* lays the
  cockpit out on one monitor: the MFDs across the top, the radar and sign below.
- **Controls** - the gamepad, the aim keys' ramp, the mouse, and a binding
  for every action: click **+** and press a key, or pick a pad button.
- **Performance** - frame pacing, the frame limit, presenting by the card.
- **Console** - the pod list (`ctcl.ini [teslas]`, or 16 in a row from a
  first address) and the battle-report printer.
- **Launch** - start this PC's role, stop everything, or a test match on this
  PC; what is running, and the logs.

- **Pilots** - the roster of profiles (a callsign of up to 15 characters, a
  'Mech and variant, notes), kept on the console; there the operator seats
  pilots in pods, and on a pod a player picks themselves and *Sit here*.

It is dressed as the BattleTech Console - hazard stripes, black panels ruled
in white, steel pills - in the game's own fonts (Bank Gothic, DIN), and shows
each pilot's 'Mech with the battle report's portraits; all of these are read
from the game folder (`hsh\fonts`, `hsh\Mechs`) as the window draws, never
copied into this repository.

### 'Mech loadouts

The variant files (`resource\Variants\*.mw4`) are packed in the engine's own
bit-level format, so the game is used to read them: `tools/mechcat.py --game
C:\path\to\MW4` seats each of the 357 'Mechs and variants in the pod in turn
(through the profiles mechanism below), LOADs it and reads the statistics off
the radar's 'Mech view, LAUNCHes and reads the weapon table off MFD 5, with
Windows' own text recognition. About 40 s a 'Mech, four hours in all; it
saves after each and resumes where it stopped, and it drives the console's
window, so the PC is the tool's while it runs. The catalogue,
`profiles\mechs.json`, and the crops it was read from (`profiles\mechcat\`)
stay in the game folder; `--report` writes `profiles\MECHS.md`, a table of
every variant. The launcher's Pilots page shows a 'Mech's tonnage, class,
ratings and weapons from it under the portrait.

### Profiles

The roster lives on the console PC, in `profiles\profiles.json` beside the
game. The console's launcher offers it to the pods on TCP 8790 (`[launcher]
pod_port` in `launcher.ini`; Windows asks once to let Python through its
firewall); a pod's launcher reaches it at the address on its This PC page,
and seats a pilot in itself - the console takes the pod to be whoever asked.

Seats are `profiles\assign.ini`, a section per pod address. When the console
loads a match it sends each pod its setup on 1001; the console's shim
finds that pod's slot in the message - the pilot's name, then two bytes of
'Mech index, then the 'Mech's name (`Loki`, or `Loki B` for a variant) and
the pod's address - and writes the seated pilot in, with the message's
length. The pod goes by the index: a 'Mech's place, from 1, in the console's
list of every chassis alphabetically, each followed by its
`resource\Variants` files (Loki 208, Loki B 211, Zeus 357); the launcher
works it out from the 65 chassis and the variant files there, the same list
the console builds. So the operator still puts each pod in a slot, but what
they type there no longer matters: the pod flies the seated pilot, and their
name is on its plasma sign and in the battle report. `[shim] log_net = 1`
logs the CTCL traffic, for studying it.

It edits `shim.ini`, `options.ini` and `ctcl.ini` in place, keeping their
comments and the keys it does not know, with the old file as `.bak`. It is
a page served on `127.0.0.1` in an Edge app window, standard library only;
it stops when the window closes.

## Running a centre on one PC

In the game directory:

1. `ctcl.ini` - the site file a pod's `postinstall.bat` put at `c:\ctcl.ini`;
   the shim reads it from beside the game instead. Start from the site's
   `ctcl-game.ini`, with:
   - `[teslas]` listing this PC's address (`ipconfig`), one line per pod:
     `192.168.1.132=Pilot1, 192.168.1.132`
   - `[Games]` `1=` pointing at fsrun with the pod's arguments:
     `1=C:\...\MW4\fsrun.exe -mechview 1 -tmfds 1 -3dt -ctcltype 2 /gosnoblade`
2. Start the pod side: `fsrun -exe Launcher.exe`. The launcher listens on TCP
   1000 and starts the pod's MW4 from `[Games]`, which listens on 1001.
3. Start the console: `fsrun -ctcltype 1 /gosnoblade`. It connects to every
   `[teslas]` address on 1000 and 1001; the pod's slot turns green, "Idle".

Or let `tools/start-centre.ps1` do steps 2 and 3, and with `-Mission` also
click through the console - pilot in slot 1, a bot in slot 2, LOAD, LAUNCH -
the way an operator would (`-Pilot NAME`, `-NoBot`, `-Stop`; needs Python).

`/gosnoblade` makes GameOS load DirectDraw directly instead of through
`blade.dll`, MW4's software renderer, which otherwise wraps every display.

Each copy writes `shim-<role>.log` beside the game (`shim-ctcl1.log` for the
console, `shim-ctcl2.log` for a pod, `shim-launcher.log`). Each also gets its
own `c:\mw4files`, the folder a pod keeps pilot and mission files in:
`mw4files\<role>` beside the game.

## Flying it

A pod's controls - stick, throttle, pedals and every button on the MFDs and
radar - reach the game from VWE's Remote I/O board on COM1. In a cockpit
(`-tmfds`) the shim is that board, and these drive it:

**The MFD and radar windows.** Click a soft-key label to press its button.
The mouse is left free for this: a pod has no mouse, and the game would
otherwise confine and recentre the cursor all mission. Scroll Lock gives it
back to the game.

**A gamepad** (any XInput pad):

| | |
|---|---|
| left stick X | pedals (turn) |
| left stick Y | throttle lever: push to open, pull to close |
| right stick | the stick (torso and aim) |
| right trigger | fire group 1 (the trigger, button 0x40) |
| left trigger, RB | fire groups 2 and 3 (the stick's buttons 0x47, 0x46) |
| A | fire everything: all three at once |
| LB | the throttle button (0x3F) |
| D-pad up / down | throttle wide open / closed |
| D-pad left / right | select the MFD the F-keys work |

**The keyboard**, while one of the game's windows has focus:

| | |
|---|---|
| A / D | left / right pedal |
| J / L, I / K | stick X, stick Y: a tap nudges the aim, holding pushes harder (full after `[keys] ramp_ms`) |
| R / F | throttle open / close |
| Space or 1, 2, 3 | fire weapon group 1, 2, 3 |
| 4 or `\` | fire everything (groups 1-3 at once) |
| F1-F4, F5-F8 | the selected MFD's top and bottom soft keys, left to right |
| F9 / F10 | select the previous / next MFD (its title says `[F1-F8]`) |
| Ctrl+F1-F6, Shift+F1-F6 | the radar's top and bottom buttons |
| F11, F12 | throttle button (0x3F), panic button (0x3E) |
| Scroll Lock | free the mouse / give it to the game |

These, and the pad's buttons above, are defaults: every action can be bound
in `shim.ini` - `[keys] <action> = K1 K2` (up to four keys by name: letters,
digits, F1-F24, Space, Enter, Backslash, Shift, Ctrl, Up, Num0-Num9, ... or
`0xNN`) and `[pad] <action> = RT` (A B X Y LB RB LT RT Back Start LS RS DUp
DDown DLeft DRight) - or in the launcher. The actions: `throttle_up`,
`throttle_down`, `throttle_full`, `throttle_zero`, `pedal_left`,
`pedal_right`, `twist_left`, `twist_right`, `aim_up`, `aim_down`, `fire1`,
`fire2`, `fire3`, `fire_all`, `throttle_button`, `panic`, `mfd_prev`,
`mfd_next`, `mouse`. The MFD and radar soft keys (F1-F8, Ctrl/Shift+F1-F6)
are the panel and stay where they are.

**Anything else** - a test script today, a real cockpit's controls tomorrow -
over the control port, UDP `127.0.0.1:9100` (9101 for the console), one text
command per datagram; `tools/rio.py` speaks it:

```
rio.py tap 34                 press and release button 0x34
rio.py press 40 / release 40  hold the trigger, let it go
rio.py axes -800 0 0 0 0      hold throttle, left and right pedal, stick Y, X
rio.py axes off               back to the keyboard and pad
rio.py peek 82bfec            read a dword of the game's memory (for finding things out)
rio.py snap                   snapshot every display
rio.py listen 30              print what the game tells the board: the lamps
```

The board's protocol, its button numbers, and the lamps are in
[docs/RIO.md](docs/RIO.md).

**A cockpit's own keys.** battlepod's cockpit bridge speaks this control
port: `python tools/bridge.py --game firestorm` (in the battlepod repository)
puts the five MFDs' keys and lamps on cockpit modules - real ones on USB, or
`tools/vmfd.py --uid vmfd-1` to `-5` in windows - the same modules that fly
battlepod's 3.0 pod. On `hello` the shim sends every lamp as it stands, so a
bridge started mid-mission lights its keys at once.

## Resolution

The main view's size is `options.ini`'s `screenwidth`/`screenheight` (and
`bitdepth`, 16 or 32). The game knows six sizes, all 4:3: 512x384, 640x480,
800x600, 1024x768, 1280x1024 and 1600x1200 (`0x426FC2`); any other is
ignored and the menus' 800x600 kept. 1600x1200 runs well on a modern card -
at `bitdepth=16`: at 32 the MFD device can no longer make its surfaces
(out of memory, `8007000E`) and the MFDs stay black. On a wide screen,
`[main] fullscreen = 1` fills it with the view centred between black bars. The console's menus are 800x600 whatever is set.

## shim.ini

Optional, beside the game. A section can be specialised by role -
`[main.ctcl2]` applies to pods only, over `[main]`.

| section | key | default | what |
|---|---|---|---|
| `main` | `x`, `y`, `w`, `h` | 0, 0, mode | the main view's window |
| `main` | `fullscreen`, `monitor` | 0, 1 | borderless over a whole monitor (1 the primary, 2 the next...) |
| `main` | `keep_aspect` | 1 | draw the view at its own 4:3 shape, with bars, whatever the window's shape |
| `main` | `gpu_present` | 1 | the card blits the view to the window (no read-back); 0 reads each frame back and draws it with GDI, as the MFDs and radar are |
| `mfd1`..`mfd5`, `radar`, `mfd_surface` | `x`, `y`, `w`, `h`, `borderless` | on the second monitor | each cockpit window |
| `mfd` | `tiles` | `L:R,L:G,L:B,R:R,R:G` | which part of the 1280x480 MFD surface each MFD shows: head `L`/`R`, then channel `R`/`G`/`B`, or `C` for the head in colour |
| `mfd` | `tint` | `FFFFFF` | colour of the monochrome MFDs, e.g. `40FF60` for green phosphor |
| `mfd` | `show_surface` | 1 | a window with the whole 1280x480 surface, for checking `tiles` |
| `mfd` | `layout` | `windows` | `wall`: all five MFDs in one window, `[mfd_wall]` `cols` across (default 5) - for a video-wall controller or one wide screen |
| `mfd_wall` | `x`, `y`, `w`, `h`, `borderless`, `cols` | | the wall's window |
| `radar` | `rotate` | 0 | 90 or 270: the radar upright, for a monitor mounted portrait as the pod's was - the game draws it on its side |
| `shim` | `vsync` | 1 | one main-view frame per refresh of the monitor (DwmFlush), evenly spaced; 2 for every other refresh; 0 to pace by the clock at `max_fps` |
| `shim` | `max_fps` | 300 with vsync, else 120 | ceiling on the main view's frame rate; 0 for none |
| `shim` | `frame_stats` | 0 | log the frame rate, frame times and the shim's copying cost every 10 s |
| `keys` | `ramp_ms` | 600 | how long I/K/J/L take to push the stick all the way; 0 for all the way at once |
| `keys` | `mask` | 1 | hide the cockpit's keys from the game |
| `shim` | `always_active` | 1 | the game believes its window has focus, so a pod in the background still starts its mission |
| `shim` | `capture_mouse` | 0 in a cockpit, else 1 | let the game confine and recentre the cursor |
| `rio` | `enabled` | 1 with `-tmfds` | the virtual RIO board on COM1 |
| `rio` | `control_port` | 9100 cockpit, 9101 else | the control port; 0 for none |
| `rio` | `mfd1`..`mfd5` | 40, 32, 48, 8, 0 | each MFD's button base (decimal) |
| `rio` | `radar_top`, `radar_bottom` | 24, 16 | the radar's first top and bottom button (decimal) |
| `pad` | `enabled`, `index`, `deadzone`, `invert_y` | 1, 0, 7000, 0 | the gamepad |
| `plasma` | `enabled`, `port` | 1 with `-tmfds`, 2 | the virtual plasma sign on `\\.\com<port>` |
| `plasma` | `x`, `y`, `w`, `h`, `borderless` | on the second monitor, 672x192 | the sign's window |
| `print` | `to_file` | for a printer named \*PDF\* or \*XPS\* | 1: give each battle report a file of its own instead of the printer asking where; 0: never |
| `print` | `folder` | `reports` beside the game | where those files go |
| `shim` | `pod_files` | `ctcl2` | the `mw4files` folder a `\\pod\mw4files` path means |
| `shim` | `cockpit_windows` | 1 with `-tmfds` | open the MFD and radar windows |
| `shim` | `allow_multiple` | 1 | let a console and a pod run on one PC |
| `shim` | `private_ctcl` | console only | keep this copy's CTCL state to itself |
| `shim` | `real_adapters` | 0 | also offer the real monitors as adapters |
| `shim` | `snapshots` | 1 | write `snap-*.bmp`: at frames 120 and 1200, and whenever a file named `snap` appears in the game directory |

## Status

**Working**

- The game is offered the primary display plus two virtual adapters, MFD and
  radar, and picks them the way it picked the pod's cards.
- Main view (800x600), MFD surface (1280x480) and radar (640x480) each run
  windowed, 16-bit, presented from the game's own back buffers; the MFD
  surface is split into five windows (the default `tiles` is confirmed
  against a running mission), or one wall.
- `c:\ctcl.ini` and `c:\mw4files` are found beside the game.
- Launcher, pod and console run together on one PC; the console loads and
  launches a mission on the pod, against bots.
- The pod is flown through a virtual RIO board: keyboard, gamepad, clicks on
  the MFD and radar labels, or the control port. Every MFD and radar button
  is mapped.
- Weapon groups 1-3 and all at once fire from the stick's buttons, keys and
  pad (confirmed on the weapons MFD).
- Frames are paced to the monitor's refresh, so moving views do not judder
  (at 120 fps on a 144 Hz monitor they did: the view seemed to shake while
  the Mech walked), and the card puts the main view on screen itself: at
  1600x1200 a pod holds 144 fps with under a millisecond of the shim's own
  work a frame (`[shim] frame_stats = 1` shows it).
- `tools/start-centre.ps1 -Mission` has a mission running in about 35 s.
- Surfaces the game makes without naming a pixel format get the mode's
  16 bits, as on the pod, not the desktop's 32: the target's picture on
  MFD 4 was striped and off to one side before.
- The plasma sign on top of the pod - the pilot's name in orange dots, and
  after a mission their rank and score - is drawn in its own window from
  what the game sends to COM2 (docs/RIO.md, *The plasma sign*).
- Battle reports print (below).

**Not yet**

- Lamps: the game's lamp commands are logged and passed to the control port,
  not shown on screen - the game already highlights the labels of what is on.

## Battle reports

With *Print Debriefing* ticked on the console, each pilot got a printed page
after a match: their score, a table of every pilot's damage to every other,
and *Mission Highlights*, the match told blow by blow ("Adam's life flashes as
Wraith reduces the Loki to a burning wreck"). Each pod writes its record,
`c:\mw4files\<guid>.pr`, as the match goes. At the end the console finds the
window of VWE's `mw4print.exe`, which has to be running already, and sends it
each record's path as `WM_COPYDATA` (tag `0xF4F72A54`, 272 bytes: 16 of
header, then the path). The path is the pod's by network, `\\<pod
address>\mw4files\...`; `mw4print` reads it, takes its margins from
`options.ini` `[battle tech print]`, and prints to the default printer
without asking.

`start-centre.ps1` starts `mw4print` (minimised; `-NoPrint` leaves it out)
through `fsrun`, so the shim is in it. There `c:\mw4files` is the console's
folder and `\\<pod>\mw4files` the pod's (`[shim] pod_files`). With a paper
printer as the default, reports come out on paper. With Microsoft Print to
PDF (or the XPS writer) each goes to `reports\<date time> battle report.pdf`
instead of a dialog per page, which `mw4print` would otherwise keep
reopening every second.
- More than one pod on one PC: each would need its own COM port and control
  port, and the launcher starts only one.

## What the game does with its displays

Read from `MW4.exe` 5.07 and confirmed by running it.

| display | device class | asks for | adapter chosen by |
|---|---|---|---|
| main view | GameOS | `options.ini` `screenwidth`/`screenheight` | first 3D-capable adapter that is not the MFD one |
| five MFDs | `CMFD_Device` | **1280x480**, 16 bpp | first adapter that offers 1280x480 |
| radar | `CRadar_Device` | 640x480 | a second 1280x480 adapter, else another 3D adapter |
| mission review | `CMR_Device` | 640x480 | the radar's adapter, else the MFD one |

- Each display device (`0x420050`) makes its own `IDirectDraw7` on its adapter
  by index into the game's device table (`0x7970B8`), sets exclusive full
  screen (`0x293`) and the mode, then (`0x4200F0`) a flipping primary with one
  back buffer and a Direct3D 7 HAL device on it.
- Adapter selection is at `0x42A599`-`0x42A68C`. Entries whose device
  identifier matches the primary's are dropped (`0x41BE10`), so the virtual
  adapters report identifiers of their own.
- With `-tmfds 1` the MFD surface is two 640x480 halves, each with a 40-pixel
  soft-key label strip top and bottom split into four - the four buttons above
  and four below each aux monitor (line table `0x756E10`).

## What the game assumes about its PC

Each of these is a pod assumption the shim has to answer for a windowed or
shared PC, found by running the game under it:

| assumption | where | the shim |
|---|---|---|
| DirectDraw goes through `blade.dll` | `0x78C36A`, `/gosnoblade` | use `/gosnoblade` |
| `c:\ctcl.ini` exists | imports of the file and profile functions | read it from beside the game |
| one copy per PC: a shared word | `.SHARED_` `0x888004`, `WinMain` `0x40DFE0` | cleared at load |
| one copy per PC: CTCL application type | `CTCL_GetApplType`, `0x40E003` | answered 0 at start-up |
| one copy per PC: window and file mapping | `0x4253F0`, `0x425439` | answered "none running"; mapping renamed per process |
| the screen is the window | `GetCursorPos`, `SetCursorPos`, `ClipCursor` | translated to the main view's window |
| Windows' compatibility engine re-patches DirectDraw | `apphelp.dll`, every `DirectDrawCreateEx` | hooks taken back after each creation |

## The roles

| role | how it starts | listens | connects to |
|---|---|---|---|
| launcher | at boot on every pod | TCP 1000 | the pod's MW4 on 1001 |
| pod (`-ctcltype 2`) | by its launcher, from `[Games]` | TCP 1001 | - |
| console (`-ctcltype 1`) | by the operator | - | every `[teslas]` address on 1000 and 1001 |

A pod's launcher and MW4 share state through `ctcls.dll`'s shared memory:
application type and state, game state and time, server flag.
