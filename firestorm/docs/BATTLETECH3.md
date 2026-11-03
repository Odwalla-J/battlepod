# BattleTech 3.0 in the launcher

The launcher sets up and runs both games: **Firestorm** (MW4 5.07 on the
Tesla II pod, this folder) and **BattleTech 3.0** (the 1990s pod, VWE
release 13.1.8, its own 68020 firmware on battlepod's emulator - the
repository round this folder). The switch under the title plate picks one;
each has its own pages, and *Pilots* and *Leaderboards* are shared. A
packaged copy has a second starter, `BattleTech 3.0.exe` (the same program
as `Firestorm.exe`, copied), that opens on 3.0.

## What a battle is

A battle is battlepod's `tools/hub.py` - the centre: the operator console's
start of a game, the network between the pods, and CPU pilots - with this
PC's cockpit as one pod (`cockpit.exe`: the pod's firmware drawing the main
view) and every CPU pilot a whole emulated pod of its own (`battlepod.exe`,
no window), flown through the panel's own input reports. The launcher
(`launcher/bt3.py`) starts the hub with the page's choices; the hub writes
the battle as it goes (`--state`, JSON: every Mech's place, heading and
speed, who is down, who destroyed whom), which the page draws as the
operator's view, with the scores. Closing the cockpit (or Esc) ends the
battle; *Stop* ends it from the launcher.

## The pages

- **Battle.** Your callsign (eight characters - the pod's name plate; pick
  one from the roster) and 'Mech (the release's 35, by chassis, with
  Firestorm's picture of it) and its paint (six schemes, eight trims - the
  pod's own, chosen by MECH_CLASS `+0x4E`; each CPU pilot takes a trim of
  its own), the map (the release's eleven scenarios), day,
  dusk or night, up to seven CPU pilots and their 'Mechs, where you start (on
  open ground in a ring, facing in, or at the map's drop points), whether a
  destroyed Mech drops in again (after 10 or 20 s, where it began - as the
  centres ran their battles - or is out), and a time limit. *Drop in*. The operator's view follows the Mechs; the table has
  each pilot's state, kills, deaths and hits taken; destructions are listed
  as they happen. Below: the release (found by itself - the repository's
  `Play.cmd`, a packaged copy's `battletech\Game Files`, or a search near
  the launcher - or set by hand) and the centre's log.
- **Display.** The main view's window: size (the pod's 480 x 360 at 2x, up
  to 1920 wide, or any), place, or full screen on any monitor; smooth or
  sharp scaling; the panel under the picture (the *strip*: the message line,
  engine status, throttle, the first six weapons with their recycle bars,
  ammunition and trigger groups, and a radar - the cockpit in one window). The
  cockpit panel's three windows (lamps, displays, bar graphs - the Remote
  I/O by wire number) on or off. The **radar screen** (below): on or off,
  its size, place or full screen on any monitor, and the page and range it
  opens on.
- **Controls.** The keyboard and a gamepad (below), and USB controllers:
  each control bound to a controller's axis or button by *Learn*, live
  readings beside them. The defaults are Firestorm's cockpit boards
  (`firmware/rp2040-controls`), so one set of controls flies both games.

Settings are saved as they change, to `battletech.ini`; the next battle
uses them.

## The controls

| keyboard | | gamepad | |
|---|---|---|---|
| W / S, X | throttle up / down in eighths; closed | left stick up / down | throttle lever |
| A / D | turn | either stick left / right | turn |
| Space, F, G | trigger groups 1, 2, 3 | D-pad up / down | throttle full / closed |
| Enter | all three | right trigger | group 1 |
| Q / E, C | twist the torso left / right; centre it | A | target select |
| R / V | look up / down | Y | searchlight |
| Z, - | zoom in, out | | |
| T, L | target select, searchlight | | |
| F1-F3 | the pod's display keys: the radar screen's Sector, Map, Contacts | | |
| Tab, PgUp / PgDn | the radar screen's next page; its range | | |

They are the panel's own input reports (battlepod's
`docs/COCKPIT_INTERFACE.md`): throttle `A0`, stick `A1`/`A2` (in advanced
mode with "stick turns", chosen with the first turn), triggers `A5`-`A7`,
torso `51`/`1C`/`28`, view `24`/`2C`, zoom `3D`/`43`, target `40`, light
`20`, displays `00`/`04`/`08`.

## battletech.ini

The cockpit reads it when `BATTLEPOD_INI` names it (the launcher sets that
for every pod it starts); without it the cockpit is as it always was.
Where: a packaged copy's `battletech\`, or battlepod's `build\`.

| section | key | default | what |
|---|---|---|---|
| `view` | `x`, `y`, `w`, `h` | where Windows puts it, 960 x 720 (the launcher: 960 x 888, with the strip) | the main view's window |
| `view` | `fullscreen` | 0 | 1: borderless over the monitor `x`, `y` is on (or `monitor` = n, from 1, without them) |
| `view` | `smooth` | 0 (1 from the launcher) | scale the picture smoothly |
| `view` | `title` | battlepod - main view | the window's name |
| `view` | `effects` | 1 | weapon effects: a beam, bolt or tracer for every shot and smoke behind missiles - added, as the pod drew only a muzzle flash and the hit (`src/fx.h`) |
| `view` | `gamma` | 0 | a display curve for the 3D view's colours (2.2 lifts the dark roads and arena floors, and pales everything else); 0 draws the pod's colours straight |
| `view` | `strip` | 0 (1 from the launcher) | the panel in a band under the picture: message line (displays `8C`-`8E`) and readout (`8F`), engine (`90`), throttle, weapons `80`-`85` (name, recycle bar, count, trigger-group lamps `4k+1`..`4k+3`), and a radar: every Mech within 500 m round yours, your heading up, from the position broadcasts (0xEC) on the network - the pod's own radar was the secondary display's, not emulated; the window is 84 of 480 taller |
| `radar` | `show` | 0 (1 from the launcher) | the radar screen, a window of its own |
| `radar` | `x`, `y`, `w`, `h`, `fullscreen`, `monitor`, `title` | 960 x 600, monitor 2 | as `[view]`'s |
| `radar` | `layout` | the window's shape | `portrait`: the scope across the top, the readouts and contacts in a band beneath (480 x 640, for a screen stood on end); `landscape`: the readouts beside it (640 x 400) |
| `radar` | `page`, `range` | 0, 1 | the page it opens on (0 Sector, 1 Map, 2 Contacts) and its range (0-4: 250 m, 500 m, 1, 2, 4 km) |
| `panels` | `show` | 1 (0 from the launcher) | the lamp, display and bar-graph windows |
| `lamps`, `displays`, `bargraphs` | `x`, `y`, `w`, `h` | | each of those windows |
| `joy` | `throttle`, `stick_x` | `Firestorm Throttle.X`, `Firestorm Stick.X` | `CONTROLLER.AXIS`: a controller by USB ids (`2E8A:1001`), part of its name, or `#n`; the axis X Y Z R U or V; `-` in front reverses it |
| `joy` | `fire1`-`fire3`, `fire_all`, `target`, `light`, `twist_left`, `twist_right`, `centre`, `view_up`, `view_down`, `zoom_in`, `zoom_out` | the cockpit boards' buttons and hat | `CONTROLLER.Bn` or `CONTROLLER.HatUp` (`HatDown`, `HatLeft`, `HatRight`); `none` for nothing |
| `joy` | `deadzone`, `enabled` | 4, 1 | |
| `game` | `files`, `scenario`, `vehicle`, `pilot`, `paint`, `accent`, `bots`, `opponents`, `env`, `arena`, `minutes`, `remote`, `remote_names`, `remote_vehicles`, `join_host`, `join_seat`, `local` | | the launcher's: the release, the last battle's choices, the seats for other PCs, the battle last joined, and whether a centre's console has a cockpit of its own (`local`, 1) |

## The radar screen

The pod's second screen was an Amiga board below the main monitor, driven by
the 68020 through a shared block and a message protocol not yet decoded. The
radar screen stands in for it, in a window of its own (on a second monitor,
full screen, it sits where the pod had it), drawn from what the cockpit
already receives:

- **Sector** (F1): the radar round your 'Mech, your heading up - range rings,
  the compass turning, a sweep - with the map's buildings and terrain as
  their footprints (each model's own bounding box, placed as the console's
  0xE4s placed it; drop pads and roads filled dimly in their own shapes, the
  wide terrain patches left out) and every other 'Mech as a triangle along its heading, in
  its trim's colour, with its pilot's name.
- **Map** (F2): the whole battlefield, north up, every 'Mech on it and the
  radar's range round yours.
- **Contacts** (F3): every 'Mech by pilot, chassis (the ROM's vehicle
  record), range and bearing; one not heard from for three seconds is LOST.

The side panel has your heading, speed and position and the contacts. A
window taller than it is wide - a portrait monitor, full screen, as a
cockpit's radar screen is - gets the portrait layout: the scope across the
top, and beneath it the page, your readouts, the contacts and the keys.
Positions are the pods' own 0xEC broadcasts; names, chassis and paint their
MECH_CLASS. F1-F3 are also the pod's own display buttons, sent to it as
before; Tab steps through the pages, PgUp/PgDn the range.

## Several PCs

A battle can have people on other PCs - up to three, beside this PC's own
cockpit and the CPU pilots (eight Mechs in all). On the PC that runs it,
*Other PCs* keeps seats 2, 3, 4 for them, each with a name and a 'Mech: the
operator gives them, as the arcade's console did. On each other PC, *Join a
battle on another PC*: the first PC's address and the seat. Its cockpit
starts there and joins the centre over the network (UDP port 5170; Windows
asks the first time whether to let the battle's Python through its
firewall); its Mech drops in once the battle has started, and a seat's
person can join late. The scores and the operator's view are on the first
PC, and so are the careers: every person in the battle gets their line.
Tried on one PC (the hub on the local address, so no firewall), a seat
joined by the launcher's own join: the hub's seat 2 said hello, got the
game, and its Mech was live beside this PC's and a CPU pilot's.

**In a centre** (`firestorm/README.md`, *A centre: one console, its
pods*) nobody types an address: on the console, the pods its Pods page has
in service take the place of *Other PCs* - up to seven, seats from 2, or
from 1 when *This PC* is "the operator's only" (the hub then runs with no
cockpit of its own). Each seat gets the pod's name and the 'Mech the
operator picks. *Start the battle* (or the Pods page's *3.0 battle*) closes
the Firestorm console if it is up - the pods play one game at a time - starts the hub, then tells each pod to
join (`/bt3/join`, its seat); the pod's Ready screen gives way to the
cockpit, full screen as its setup placed it. When the battle ends - its time
limit, or *Stop* - the hub ends their cockpits too (`hub.py --end-remote`),
the console tells them to leave besides, and they show Ready again. Tried
on one PC with a pod agent beside the console: seat 1 joined, its Mech live
against a CPU pilot; the 30-second battle ended by itself and the pod went
back to Ready; then to Firestorm, up in 11 seconds.

## The MFD board

The boards on the *MFD boards* page (firmware/esp32-mfd: an Elecrow
CrowPanel) show the 3.0 cockpit's panel during a battle. The launcher starts
battlepod's `tools/bridge.py` with the battle (`--game battlepod`,
`layouts/milestone-mfd`, the 3.0 profile) and has the cockpit send its
Remote I/O to it (`--rio-port`, through the hub's `--human-args`, so only
your cockpit's panel - not the CPU pilots'). The layout is keys, text and
bars, and the board only shows pictures, so the bridge draws the page
itself (`Panel` in bridge.py, the font in `tools/mfdpaint.py`) and streams
it as Firestorm's MFDs are streamed: the eight key labels - RADAR, MAP,
DAMAGE, TARGET above; 2 KM, ZOOM+, ZOOM-, INDIRECT below - filled when the
game lights them bright, ruled when dim; the message line; the engine
status; the readout; the first weapon and its recycle bar. A board named
for Firestorm (`mfd:4`) takes the 3.0 layout's free MFD by itself, so one
board serves both games. A board unplugged, or plugged in mid-battle, is
found again within three seconds. Seen on `tools/mfdrecv.py` standing in
for the board: RADAR lit, ENGINEOK, AFC 2520 with a full bar.

## Careers

A finished battle of a minute or more goes into the pilots' careers, as
Firestorm's matches do ([CAREERS.md](CAREERS.md)), marked *BattleTech
3.0*: your kills and deaths, and a score of 500 a kill - what Firestorm
counts for a destroyed 'Mech - so one rank covers both games. Place is by
kills. A callsign longer than eight characters is matched to its profile by
its first eight. The Leaderboards page shows either game, or both.

## Packaging

`tools/package.py` adds `battletech\` when battlepod is built (`make && make
cockpit` in the repository): the two programs (statically linked: no DLLs),
the centre's three scripts, and a copy of the release's Game Files (6 MB,
yours, as `game\` is; `--bt3-files` names it, else the launcher's own
finding). Nothing else is needed: the embeddable Python runs the centre.

## Not yet

- **A wide view.** The emulator draws the pod's frame at its own 480 x 360,
  and the pod sends only what its own view (about 54 degrees across) can see,
  so a wider picture would show empty edges; the view is kept at 4:3.
- **The head-up display's text, sound, the secondary display**: as in
  battlepod's README. The renderer is sent only the reticle and the drop
  bay; the text readouts and the pod's radar screen were the Amiga board's,
  which is not emulated. The radar screen stands in for its radar (below);
  its DAMAGE page has no stand-in yet.
- A Mech dropping in again is its first MECH_CLASS and PLAYER_LINK sent
  again (the hub's `--respawn`); seen with two CPU pilots: each went down,
  came back ten seconds later where it began, and fought on, kills and
  deaths counted. Not yet tried with a person's cockpit.
- CPU pilots go for the nearest Mech, a person's counting as half as far;
  they close, stop at about 90 m and hold the trigger while they face it.
