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
make test       # the MFD stream's encoder against tools/mfdstream.py (needs Python)
```

Stop every `MW4.exe` and `Launcher.exe` before `make install`: a running copy
holds `ddraw.dll` open and the copy fails.

## A copy that starts from one program

`make package GAME=C:/path/to/MW4 DEST=C:/Games/Firestorm` (or `make`, then
`python tools/package.py --game ... --dest ...` where MINGW32 has no
Python) lays out a folder that needs nothing installed:

```
Firestorm.exe   double-click: opens the launcher
python\         an embeddable Python from python.org
launcher\       the launcher
tools\          start-centre.ps1 and desk.py, for the test match
game\           a copy of GAME with the shim in place
BattleTech 3.0.exe   the same starter, opening on BattleTech 3.0
battletech\     BattleTech 3.0: battlepod's emulator, its centre and a copy
                of the release's Game Files (when battlepod is built)
```

It carries your game files, so it is for you, not for handing out. Every
path is relative to `Firestorm.exe`: move the folder, or copy it to another
PC, and the launcher puts `ctcl.ini` right on its next start (the pod's
`fsrun.exe` path, and on a PC that is both console and pod, this PC's
address). Run it again to pick up a new shim or launcher; it leaves
`game\` - settings, pilots and all - alone unless given `--fresh-game`.

## The launcher

`launcher\Firestorm Launcher.cmd` (or `python launcher/fslauncher.py
--game C:\path\to\MW4`) opens a window for setting a PC up and starting it.
It runs both games: the switch under the title plate picks **Firestorm** or
**BattleTech 3.0** - the 1990s pod on battlepod's emulator, with its own
*Battle*, *Display* and *Controls* pages and a live operator's view
([docs/BATTLETECH3.md](docs/BATTLETECH3.md)). For Firestorm:

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

- **MFD boards** - the cockpit's MFD screens (`firmware/esp32-mfd`): find
  the boards on the serial ports, tell each which MFD it is (it keeps it),
  and loop a test picture on all of them - each MFD's page drawn by the
  launcher (`launcher/mfdtest.py`), or four seconds recorded from the game's
  own MFDs while a mission streams them (kept in `profiles\mfd-test`).
  Its *Bridge* card runs battlepod's `tools/bridge.py` for the boards given
  an MFD: by default it starts with the pod (and with a test match), takes
  the game's MFD stream when *The game streams its MFD pictures* is ticked,
  and stops with *Stop*. It is a program of its own, logging to
  `bridge.log` beside the game, so it runs on when the launcher's window
  closes. Finding boards, test pictures and recording stop it, as they need
  the same serial ports (recording starts it again after).
- **Pilots** - the roster of profiles (a callsign of up to 15 characters, a
  'Mech and variant, notes), kept on the console; there the operator seats
  pilots in pods, and on a pod a player picks themselves and *Sit here*.
  Each pilot's rank and career - matches, wins, kills, deaths, score.
- **Leaderboards** - every pilot's standing, for all time or a recent
  period, the best of it, and the latest matches. Each pod's launcher sends
  its finished matches (the records the battle report is printed from) to
  the console's, which keeps the careers; ranks come from experience, and
  the pod's plasma sign shows a pilot's rank above their name
  ([docs/CAREERS.md](docs/CAREERS.md)).

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

### Portraits the game does not have

`hsh\Mechs` has a portrait of every chassis but Behemoth, Behemoth II,
Longbow and Victor. `tools/mechbust.py --game C:\path\to\MW4` makes
them from the 'Mech the radar turns while a pilot is LOADed: it watches the
radar window from before LOAD, sees the 'Mech appear - always facing away,
square-on - and the turn come round (about 2.3 s, 25 poses), takes the pose
half a turn on, and cuts it to the game's own portrait shape: 800 x 250, on
white, head to waist. Checked against the Atlas, whose game portrait it
matches. They go in the game folder, `profiles\portraits\<chassis>.png`,
where the launcher looks after `hsh\Mechs`; `--only` names others, and
`--keep-frames` keeps every pose in `profiles\mechcat\bust\`. About 40 s a
'Mech, driving the console's window as `mechcat.py` does. The radar draws
the 'Mech in the seated pilot's camo, so these come in that, not the
portraits' own.

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
the console builds. The pod flies the seated pilot, and their name is on its
plasma sign and in the battle report, whatever the console's slot says.

**The console fills in its slots** too. Once its menu is up (3 s after its
first frame), the console's shim does what the operator would: for each pod
in `ctcl.ini`'s `[teslas]` with a pilot seated, in that order, it opens the
next slot's player list, picks the pod, types the pilot's name and presses
Enter, then sets the slot's 'Mech (the chassis - the console's list has no
variants; the variant goes in at LOAD as above), camo and unit - with a
pointer and keys of its own, fed to the game through `GetCursorPos`,
`GetKeyState` and its window's messages, so the real mouse is not touched.
Whoever runs the console sees what each pod will fly. Up to five pods (the
player lists of lower slots open where it has not been measured);
`[profiles] fill_console = 0` turns it off, `fill_delay_ms` changes the wait.
On the launcher's Pilots page the console seats a pilot with the button on
their row ("Seat in Pilot1"), or a pod's list under Seats.

**Camo and unit.** A pilot has a camo (1-16) and a unit insignia (1-17) -
the console's two numbered lists - picked on the Pilots page. The pictures
there are read off the console once by `python tools/livery.py --game ...`
(about a minute; it drives the console's slot 1 with the shim's pointer)
into `profiles\livery` beside the game; without them the page offers the
numbers. The camo and unit travel to the pod in the console's setup, so the
shim finds a pod's slot there by its address alone. `[shim] log_net =
1` logs the CTCL traffic, for studying it.

It edits `shim.ini`, `options.ini` and `ctcl.ini` in place, keeping their
comments and the keys it does not know, with the old file as `.bak`. It is
a page served on `127.0.0.1` in an Edge app window, standard library only;
it stops when the window closes.

## A centre: one console, its pods

Every PC gets the same packaged copy (`make package`). The first time
`Firestorm.exe --auto` runs - or **Start with Windows**, which the setup
adds - it opens the **first-time setup** (`launcher/web/setup.html`):

- **Pod or console.**
- A pod's **number and name** (the console lists pods by them; the name
  shows under Ready).
- A pod's **screens**: click the monitor the pilot looks through (the main
  view) and the one with the cockpit screens. Firestorm's view goes full
  screen on the first and its MFDs, radar and plasma sign are laid out on the
  second (the Displays page's *Arrange*); 3.0's view and radar likewise.
- Its **controls**, live, to check they are seen.
- **Network**: one button lets the centre's programs through Windows
  Firewall (Windows asks for an administrator once) - UDP 5197, 5198 and
  5170, TCP 8790 and 8795, and MW4, Launcher.exe, mw4print, cockpit and
  battlepod.
- **Start with Windows**: a shortcut in the user's Startup folder to
  `Firestorm.exe --auto`.

From then on a **pod** boots into pod mode with no window: every monitor
black, **Ready** in large white letters on the main one, no cursor
(`launcher/podagent.py`), with no game running. The pods play whatever the
console plays: when the console starts Firestorm they start Firestorm's pod
side - the console waits for its pods before it opens - and leave it, back
to Ready with no game, when it closes; when the console starts a 3.0 battle
they join it, and come back to Ready when it ends. While Firestorm's
MW4 waits for a match the shim paints the same Ready on its main view
(`shim.ini [ready] text`, set by the pod), so the pilot sees no change; a
mission's view is the game's own, and Ready comes back after it. A game that
falls over while it should be running is started again.

The **console** boots into the launcher, on its **Pods** page. It finds the
pods by itself - each broadcasts a beacon on UDP 5198, the console asks
"who" on 5197, which also tells each pod where the console is for the pilot
roster and careers - and shows what each one is doing. The **Game** card
picks what the centre plays:

| Button | What happens |
|---|---|
| Firestorm | Ends a 3.0 battle if one is on, then starts the Firestorm console and its pods' Firestorm |
| 3.0 battle | Closes Firestorm if it is up (a match on ends), then starts a 3.0 battle with the 3.0 page's Battle settings; the pods join it |
| Back to Ready | Closes whichever is on; the pods show Ready with no game |

Starting one game closes the other first, so changing over is one click,
about fifteen seconds, with no keyboard and no reboot. Per pod, or for all
at once:

| Button | What the pod does |
|---|---|
| In service | Plays the console's game, whichever it is (the default) |
| Out of service | Neither game, whatever the console plays; Ready |
| Identify | Its Ready turns amber for six seconds |
| Settings | Opens the launcher's window over its Ready screen, for whoever is at the pod |
| More... | Restart its game, restart the PC, shut it down |

**Firestorm**: starting it adds every pod in service to the console's list
(`ctcl.ini [teslas]`; a pod out of service is turned off in it), tells them
to start Firestorm and waits until they are
up (about 15 s), then starts the console. Closing it - *Back to Ready* on
the Pods page, the console window's close button, Alt+F4 or Ctrl+Q (the last
three ask first) - closes it as its own Quit does, closes the report printer,
and brings the launcher back in front. However it closes, the pods leave
Firestorm within a couple of seconds and show Ready with no game - which
also suits the game: a pod's MW4 takes one console a run, so the next
console finds every pod freshly started. Closing mid-match ends the match
on the pods the same way. The console's "who" carries whether it is up, so
a pod that missed being told catches up within seconds.

**3.0**: the pods in service (with 3.0 installed) are the battle's seats.
The 3.0 page's Battle tab seats them - seat
by seat, each with the 'Mech the operator picks - and *This PC* chooses
whether the console has a cockpit of its own or is the operator's only.
*Start the battle* there, or *3.0 battle* on the Pods page, tells each pod to
join; when the battle ends (its time limit, *Stop*, *Back to Ready*, or
*Firestorm*) their cockpits close and they show Ready again.

The pod's commands are HTTP on TCP 8795 (`GET /status`; `POST /mode` (`auto` or `ready`), `/firestorm`,
`/bt3/join`, `/bt3/leave`, `/identify`, `/settings`, `/restart`, `/power`),
answered only from the centre's own (private) network.

For an unattended pod, Windows itself should sign in on its own (`netplwiz`)
and not sleep; the pod keeps its displays awake while it runs.

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

**USB controllers** - the cockpit's own throttle, stick and pedals, or a
flight stick - any game controller Windows knows, several at once
(`shim.ini` `[joy]`, or the launcher's Controls page, which shows them live
and learns an axis or button as you move it). The cockpit's own boards,
`firmware/rp2040-controls`, are mapped with no setup: *Firestorm Throttle*
(the lever, the throttle and panic buttons), *Firestorm Stick* (twist, aim,
four buttons, the hat) and *Firestorm Pedals*. Otherwise each axis -
`throttle`, `pedal_left`, `pedal_right`, `stick_x`, `stick_y` - is
`CONTROLLER.AXIS`, the controller named by its USB ids (`2E8A:1001`), part of
its name, or `#n`, the axis X Y Z R U or V, a `-` in front to reverse it; any
action is `CONTROLLER.Bn` or `CONTROLLER.HatUp` (`HatDown`, `HatLeft`,
`HatRight`); `deadzone` (percent, default 4), `enabled = 0`. The lever takes
over from the throttle keys whenever it moves. The game calibrates the
throttle and pedals from what it sees each mission, so the shim shows it them
at rest for half a second whenever it starts over: a lever left open, or a
foot on a pedal, does not become the new "closed" or "rest".

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
| F11, F12 | throttle button (0x3F: reverse while held), panic button (0x3E) |
| arrow keys | the stick's hat, looking round while held: up down (0x42), down behind (0x41), left (0x44), right (0x43) |
| Scroll Lock | free the mouse / give it to the game |

These, and the pad's buttons above, are defaults: every action can be bound
in `shim.ini` - `[keys] <action> = K1 K2` (up to four keys by name: letters,
digits, F1-F24, Space, Enter, Backslash, Shift, Ctrl, Up, Num0-Num9, ... or
`0xNN`) and `[pad] <action> = RT` (A B X Y LB RB LT RT Back Start LS RS DUp
DDown DLeft DRight) - or in the launcher. The actions: `throttle_up`,
`throttle_down`, `throttle_full`, `throttle_zero`, `pedal_left`,
`pedal_right`, `twist_left`, `twist_right`, `aim_up`, `aim_down`, `fire1`,
`fire2`, `fire3`, `fire_all`, `throttle_button`, `panic`, `mfd_prev`,
`mfd_next`, `mouse`, `hat_up`, `hat_down`, `hat_left`, `hat_right`,
`button4` (the stick's fourth button, 0x45, which this game ignores). The
MFD and radar soft keys (F1-F8, Ctrl/Shift+F1-F6)
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

The console's menus, on its port (9101), with the shim's own pointer:
`click X Y` (the menu's 800x600 coordinates), `chars TEXT` (typed into the
box that has focus), `enter`, `vkey XX` (a key by virtual-key code: 24 Home,
28 Down, 0D Enter alone), `pointer off` (the real mouse again).

The board's protocol, its button numbers, and the lamps are in
[docs/RIO.md](docs/RIO.md).

**A cockpit's own keys.** battlepod's cockpit bridge speaks this control
port: `python tools/bridge.py --game firestorm` (in the battlepod repository)
puts the five MFDs' keys and lamps on cockpit modules - real ones on USB, or
`tools/vmfd.py --uid vmfd-1` to `-5` in windows - the same modules that fly
battlepod's 3.0 pod. On `hello` the shim sends every lamp as it stands, so a
bridge started mid-mission lights its keys at once.

**The MFD pictures, to modules.** With `[stream] enable = 1` the shim also
serves the five MFD pictures, exactly as the game drew them, on a local TCP
port (9110); the same bridge takes them to modules with a screen - the
ESP32-S3 MFD boards (`firmware/esp32-mfd`; the launcher's MFD boards page
starts the bridge for them), or `tools/mfdrecv.py --all` in five windows:

```
shim.ini:   [stream]
            enable=1
python tools/bridge.py --game firestorm
python tools/mfdrecv.py --all
```

Off by default, and then it costs nothing; the MFD windows are drawn as
before either way. The protocol is
[docs/MFD_STREAMING_PROTOCOL.md](../docs/MFD_STREAMING_PROTOCOL.md), the
measurements [docs/MFD_STREAMING_IMPLEMENTATION.md](../docs/MFD_STREAMING_IMPLEMENTATION.md).

## Resolution

The main view's size is `options.ini`'s `screenwidth`/`screenheight` (and
`bitdepth`, 16 or 32); the pod switches to it at LAUNCH, its menus run at
800x600. The game knows five mission sizes, all 4:3: 640x480, 800x600,
1024x768, 1280x1024 and 1600x1200 - the options loader turns the width into
one of five (`0x53A6A8`) and the size is made from that (`0x556965`); any
other width would be ignored and 800x600 kept - so for any other size
(1920x1080, 2560x1080 ...) the shim rewrites the game's five mission sizes
to it, in memory, and offers it as a display mode: the mission runs at that
size.

At a wide size the cockpit's view is **widened, not stretched** (Hor+): the
shim keeps the 4:3 view's height and makes it as wide as the window, so 16:9
sees 75 degrees across instead of 60 and 21:9 about 92. The cockpit frame is
3D and widens with it; the 2D HUD (reticle, weapon groups, messages) is drawn
at its own 4:3 shape in the middle, and the target's box stays on the target
(`[main] hud_4x3`). Direct3D draws nothing wider than 2048 here - wider, the game stops
with "No software rasterizer available" - so a larger size is drawn at its
shape within 2048 (2560x1080 as 2048x864) and scaled up to the window.
[docs/WIDESCREEN.md](docs/WIDESCREEN.md) has how.

1600x1200 runs well on a modern card - at `bitdepth=16`: at 32 the MFD
device can no longer make its surfaces (out of memory, `8007000E`) and the
MFDs stay black. `[main] fullscreen = 1` fills a monitor; with `keep_aspect`
a view of another shape sits between black bars. The console's menus are
800x600 whatever is set.

**Each copy keeps its own `options.ini`.** MW4 rewrites `options.ini` as it
starts, with only the sections that copy uses: a pod drops `[graphics
options]` - its screen size - and on one PC the console and pod share the
file. The arcade never noticed: each pod's `postinstall.bat` copied
`options-game.ini` over it. So the shim does the same: each copy (pod,
console) reads and rewrites `mw4files\<role>\options.ini`, copied afresh
from `options.ini` as it starts - and mw4print, whose report layout
(`[battle tech print]`) the console's rewrite drops, has
`mw4files\ctcl1\options-mw4print.ini`. Edit `options.ini` (or use
the launcher); the copies are the game's.

## shim.ini

Optional, beside the game. A section can be specialised by role -
`[main.ctcl2]` applies to pods only, over `[main]`.

| section | key | default | what |
|---|---|---|---|
| `main` | `x`, `y`, `w`, `h` | 0, 0, mode | the main view's window |
| `main` | `fullscreen`, `monitor` | 0, 1 | borderless over a whole monitor (1 the primary, 2 the next...) |
| `main` | `keep_aspect` | 1 | draw the view at its own shape, with bars, whatever the window's shape |
| `main` | `widen` | 1 | at a size that is not 4:3, widen the cockpit's view to fit (0: stretch it) |
| `main` | `widen_mask` | 2 | which cameras `widen` changes, by who sets them up: 2 the cockpit's, 1 the MechLab's |
| `main` | `hud_4x3` | 1 | at a size that is not 4:3, draw the HUD at its own 4:3 shape in the middle (the target's box stays on the target); 0: stretched |
| `main` | `max_size` | 2048 | the largest side Direct3D draws; a larger view is drawn at its shape within it and scaled up |
| `main` | `gpu_present` | 1 | the card blits the view to the window (no read-back); 0 reads each frame back and draws it with GDI, as the MFDs and radar are |
| `ready` | `text`, `sub` | (none) | a pod's main view while it waits for the console: `text` large and white on black, `sub` small and grey under it; none, the game's own idle screen. The pod agent sets them |
| `mfd1`..`mfd5`, `radar`, `mfd_surface` | `x`, `y`, `w`, `h`, `borderless` | on the second monitor | each cockpit window |
| `mfd` | `tiles` | `L:R,L:G,L:B,R:R,R:G` | which part of the 1280x480 MFD surface each MFD shows: head `L`/`R`, then channel `R`/`G`/`B`, or `C` for the head in colour |
| `mfd` | `tint` | `FFFFFF` | colour of the monochrome MFDs, e.g. `40FF60` for green phosphor |
| `mfd` | `show_surface` | 1 | a window with the whole 1280x480 surface, for checking `tiles` |
| `mfd` | `hide_numbers` | 1 | MFD 1's top keys: leave out the slot number ("1".."4") where the game writes a pilot's name over it |
| `mfd` | `target_mech` | 1 | MFD 4's target page: the target's mech and variant ("Stock" for a stock mech) in the empty box at the bottom left |
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
| `plasma` | `ranks` | 1 | the pilot's rank on the sign's top row, from `profiles\ranks.ini` ([docs/CAREERS.md](docs/CAREERS.md)) |
| `print` | `to_file` | for a printer named \*PDF\* or \*XPS\* | 1: give each battle report a file of its own instead of the printer asking where; 0: never |
| `print` | `folder` | `reports` beside the game | where those files go |
| `shim` | `pod_files` | `ctcl2` | the `mw4files` folder a `\\pod\mw4files` path means |
| `shim` | `cockpit_windows` | 1 with `-tmfds` | open the MFD and radar windows |
| `shim` | `allow_multiple` | 1 | let a console and a pod run on one PC |
| `shim` | `private_ctcl` | console only | keep this copy's CTCL state to itself |
| `shim` | `real_adapters` | 0 | also offer the real monitors as adapters |
| `shim` | `watch` | | a debugging aid: hex address of a game variable; once the game runs, every write to it is logged with the code that made it |
| `shim` | `snapshots` | 0 | also write `snap-*.bmp` of every display at frames 120 and 1200 of each run; asked for - the control port's `snap`, or a file named `snap` appearing in the game directory - they are always written |
| `stream` | `enable` | 0 | serve the MFD pictures to `tools/bridge.py` ([MFD streaming](../docs/MFD_STREAMING_PROTOCOL.md)) |
| `stream` | `port` | 9110 | its TCP port, on 127.0.0.1 |
| `stream` | `displays` | `1,2,3,4,5` | which MFDs (numbered as `[mfd] tiles` and the windows) |
| `stream` | `tile` | 16 | the size of the squares compared for changes |
| `stream` | `max_fps` | 0 | at most this many updates a second per display; 0: every frame the game draws |
| `stream` | `stats` | 0 | log each display's rates, sizes and coding time every 10 s |

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
- Wide main views (16:9, 21:9 ...): the mission runs at the size and the
  cockpit's view is widened to fit, not stretched (*Resolution*).
- The console's own clicks - filling in its slots from the seated pilots,
  the control port's `click` - work with its window behind the pod's: the
  shim hands them to GameOS's DirectInput mouse too, which a window behind
  another is not given.

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
