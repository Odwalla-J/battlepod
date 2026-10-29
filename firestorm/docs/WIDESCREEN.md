# A 16:9 main view: what is known so far

Work in progress. The aim: the pod's main view rendered at a wide size
(1920x1080, 1600x900 ...) with the 3D view widened, not stretched. Nothing
here is in the shim yet; the experiment's code is kept outside the
repository until it works.

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

So something between the menu and the mission writes 800x600 back -
probably a second pass of the options loader with a different source, or a
reset of the index field (`[esi+0x40]` in the options object). Finding that
writer is the next step.

## After the size

Once the mission runs at 1920x1080 the open questions are the projection
(does the game build it from the view's width/height - Hor+ for free - or
assume 4:3, in which case the shim adjusts the projection matrix in
`IDirect3DDevice7::SetTransform`), the game's own culling at the wider edges,
and the 2D cockpit overlay's placement. The shim's window side is ready:
`view_box()` already draws any shape.
