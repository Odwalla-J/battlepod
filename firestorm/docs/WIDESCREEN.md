# A wide main view

The pod's main view at a wide size - 1920x1080, 2560x1080, 1600x900 ... -
with the 3D view widened, not stretched.

**Where it stands: done, the 3D view and the HUD.** Put the size in `options.ini`
(`[graphics options] screenwidth=1920`, `screenheight=1080`, or pick it in
the launcher) and the shim makes the mission that size (`main_mode`, below)
and widens the cockpit's camera to fit (`widen_hooks`, "Proportions"):
16:9 sees 75 degrees across where 4:3 saw 60, 21:9 about 92, the view as
tall as before. The cockpit frame is 3D and widens with the view. The 2D
HUD - reticle, weapon groups, messages - is drawn at its own 4:3 shape in
the middle ("The HUD"), the target's box where the target is. Sizes wider
than 2048 are drawn at 2048 and scaled up ("The 2048 limit").

## How the game picks its size

| step | where | what |
|---|---|---|
| read | `options.ini` `[graphics options]` `screenwidth`/`screenheight` | the pod's file (not `options-game.ini`, which nothing here reads for the size) |
| validate | `0x53A6A8` | the width becomes an index: 640→0, 800→1, 1024→2, 1280→3, 1600→4; any other width leaves the index alone (800x600). Widths over 1024 go straight to the 1280/1600 tests at `0x53A6F2` |
| make the size | `0x556965`, jump table `0x5569F4` | index → `[0x7673EC]` x `[0x7673F0]`: 640x480, 800x600, 1024x768, 1280x1024 (`0x55697B`), 1600x1200 |
| set the mode | `0x426C40` (called at `0x550DC1` and `0x4FAFEA` with `[0x7673EC]`/`[0x7673F0]`) | GameOS's set-mode: **any** size; an unlisted width makes `[0x75FBCC]` 0, and `0x426F90` then uses the custom size at `0x796AA4`/`0x796AA0` |
| the pod's devices | shim log | the main view is set to 800x600 when the pod starts, and switched to the options size at LAUNCH (1600x1200 does this; seen) |

## Tried (1920x1080)

1. **The size alone in `options.ini`**: at the pod's menu `[0x7673EC]` reads
   1920 x 1080 (peeked) - but at LAUNCH there is no mode switch, and in the
   mission all of `0x7673EC`, `0x78B5A8`, `0x796AA4` hold 800 x 600.
2. **The 1280 entry rewritten in memory** (the loader's `cmp eax, 1280` at
   `0x53A6F3` and the case's two immediates at `0x556981`/`0x55698B` to
   1920/1080; bytes checked first): applied, still 800x600 in the mission.
3. **1920x1080 offered in the main adapter's mode list** (in case GameOS
   wants a listed mode at 16 bpp): no change.
4. **A hardware watchpoint on `0x7673EC`** (to log who writes 800 at
   LAUNCH), armed from the shim's `DllMain` through a vectored handler:
   the pod died at once. Arm it later, from a thread of the shim's once the
   game runs (suspend the main thread, `SetThreadContext`), or use a debugger.

5. **The watchpoint again, armed from a shim thread** once the game runs
   (`[shim] watch = 7673EC`: every thread suspended, its debug registers
   set, resumed): it works. 1920 is written at the menu by the options
   read (`0x4831A4`, from `0x4FBB3E`); at LAUNCH **`0x5569B1` writes 800** -
   the size table's 800x600 case, from `0x534D06`. Whatever the loader's
   index source is, it lands on 800x600 for any width but the five.
6. **All five size-table cases rewritten in memory** to the custom size
   (`0x556965`, `0x55697B`, `0x556991`, `0x5569A7`, `0x5569BD`; their
   opcode bytes checked first), plus the size offered in the main
   adapter's mode list: **the mission runs at 1920x1080.** This is
   `main_mode()` in the shim.

## Proportions

- The game does **not** set a Direct3D projection: a hook on
  `IDirect3DDevice7::SetTransform` saw no projection matrix at all - it
  transforms vertices itself (MLR), so the fix cannot be made in Direct3D.
- MW4's camera keeps its frustum's edges in itself (`+0x20C` .. `+0x218`,
  near and far at `+0x21C`/`+0x220`), set by `0x4F29F0` (near, far, left,
  right, top, bottom - six floats, `ret 0x18`). Two calls reach it: the
  camera read from the game's data (`0x4F1FDA`) and **`SetPerspective`
  `0x4F2E70`** (near, far, a pointer to the field of view across the view,
  height/width): `half width = near * tan(fov/2)` (`0x4F2F60` is `tan`),
  `half height = half width * height/width`. `0x4F2EF0` reads them back
  (fov `= 2 atan`, height/width `= [+0x214] / [+0x20C]`).
- `SetPerspective` is reached by five direct calls. Logged in a mission:

  | call | camera | fov | h/w | what |
  |---|---|---|---|---|
  | `0x60F0E2` | the one in `[0x8304D4]`, far 1100 | 60 | 0.75 (an immediate) | the MechLab's (its setup registers `ML_CallbackHandler`, `m_mechLabCount`) |
  | `0x51470B` (in `0x5146F0`) | the cockpit's | 60 | 0.75 | the main view |
  | `0x51470B` | another | 60 | 1.0057 | |
  | `0x51470B`, then `0x5746A1` | the target view's | 3 | 0.42, 0.74 | MFD 4's zoomed target |
  | `0x57AE81` | the cockpit's again | | as read back | zoom: `0x4F2EF0` read back, the fov swapped for `[0x76B8D0]`/`[0x76B8D4]` |

- Which is the main view was found by widening each in turn and measuring
  one scene (the same drop point, ScarabStronghold, slot 1) at 800x600 and
  1920x1080: with the cockpit's camera widened, the span from the radio
  tower to the container group is 45% of the width at 4:3 and 32% at 16:9 -
  Hor+ predicts 45 x 0.75 = 34% - and things stand as tall on screen as at
  4:3. Stretched, the span stays 45%. The cockpit's struts move the same way:
  they are 3D, drawn by that camera.
- **The fix** (`h_SetPerspective`): the calls at `0x51470B` and `0x57AE81`
  (and `0x60F0E2`, for `[main] widen_mask = 1`) are pointed at the shim. A
  camera set up at h/w 0.75 by the cockpit's call is remembered; for it, the
  game's field of view is taken as across a 4:3 view and the height kept:
  `fov' = 2 atan(tan(fov/2) * 0.75 / (H/W))`, h/w `= H/W`. Zoom's later
  call passes the widened h/w and a 4:3 fov, and is widened the same way.
  The red box round a target sits on it: the HUD's projection is the
  camera's. The pod's zoom button (MFD 3's ZOOM MODE) does not zoom the
  main view - it is MFD 4's picture.

## The 2048 limit

At 2560x1080 the pod stopped at LAUNCH with GameOS's *Fatal Error*: `Nested
exception! - Cause: 'EnterWindowMode' Error: "No software rasterizer
available"`. `0x427DC0` makes the main view's surfaces and device; at
2560 wide the hardware attempt fails, GameOS falls back to software, and
`/gosnoblade` has left it none (`0x428F54`). 2048x864 runs. So the shim
caps the side at `[main] max_size` (2048), keeping the shape: 2560x1080 is
drawn at 2048x864 and the window scales it up. (The fault the shim logs
then, in `0x437400`, is GameOS's error reporter probing memory with
`IsBadReadPtr`, not the cause.)

## The HUD

The HUD is laid out on an 800x600 page (the reticle at 400,300) and drawn
across the whole view, so at 16:9 it was 1.33x too wide, 1.78x at 21:9.
There is no one scale to change - the page-to-pixels step is repeated in
many places - but everything it draws reaches the device through two
GameOS calls, by a few known routes:

| route | what | ends in |
|---|---|---|
| `0x581460` (thiscall: position, size, colour, ?, now; `ret 0x14`) | every HUD picture, drawn at once or queued; `0x5815E0` draws each | `gos_DrawQuads` from `0x581B44` |
| `0x6A8xxx`, a rectangle helper | panels - the message bar | `gos_DrawQuads` from `0x6A83EF` |
| GameOS's text | the range, the messages | `gos_DrawQuads` from `0x44E4FE` |
| `0x580710`, `0x580A80` | the HUD's scales | `gos_DrawLines` from `0x580A62`, `0x580DA8`, `0x580F02` |
| `0x6A8400` | the panels' outlines, gathered and drawn together | `gos_DrawLines` from `0x6A840F` |

`gos_DrawQuads` is `0x45C500`, `gos_DrawLines` `0x45C490` (stdcall: an array
of `gos_VERTEX`, 8 floats, x first; a count; they call the device in
`[0x796B0C]` at once). The shim (`hud_hooks`) catches both and, for those
routes only and only when the device's render target is the main view's
back buffer, squeezes x toward the middle by `(H * 4/3) / W` - the page at
its own shape, centred. The same routes draw the MFDs and the radar (on
their own devices), and the screen-size globals read the main view's size
there too, so the render target is what tells them apart.

What follows the 3D view is the exception: `HUDTargetArrow` (its code
`0x64EE00`..`0x64FB90`) draws the red box round the target and the arrow at
the edge that points to it, at the target's place as the (widened) camera
sees it. A picture that code asks `0x581460` for is remembered (by its
object), and when `0x5815E0` draws it, each quad keeps its middle and is
only made narrower - the right shape, on the target. Seen at 1920x1080: the
reticle round again, the weapon groups' boxes square, the message bar and
its outline 4:3 in the middle, the box on the Mad Cat and the arrow at the
true screen edge. `[main] hud_4x3 = 0` leaves the HUD stretched.

Other HUD components (`HUDNav`, `HUDHeat`, `HUDDamage` ...) are drawn the
same ways, so they are squeezed too, though the pod's HUD shows few of
them (its MFDs carry the rest). The RTTI names: `HUDReticle`, `HUDWeapon`,
`HUDTargetArrow`, `HUDNav`, `HUDText` ...; `HUDComponent`'s vtable is
`0x707BE0`.

## Not yet

- `HUDNumberText::GetIndex` (`0x582720`) picks its number pictures by
  screen height (480, 600, 768, 1024, 1200) and falls back to the smallest
  for any other ("unknown screen size") - but the pod's HUD never calls it
  (a wrapper on its two calls saw nothing in a mission at 1080 lines): the
  range under the reticle is the same size at 1080 as before.
- The game's culling at the wider edges has shown nothing missing so far.
