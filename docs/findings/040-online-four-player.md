# Online four-player race: what the game already does, and what the port must add

Status: partly superseded by 044 (the lockstep split-screen race plan)
Checked: 2026-11-04

Do not build online races as one shared split-screen race with the same inputs on every PC
(lockstep): that plan was dropped on 2026-10-16. Each PC runs a one-player race with puppet
riders (finding 044). Still true, and cited by the code: the multitap and pad mapping (now
implemented: `port/hle/iop.c` `rpc_mtap`, `port/hle/pad.c`, local split screen with 3-4 pads),
the rider world `D_002AF2C0`, the setup block `D_004B25A0`, the seed, and the split-screen
differences; sections 6-7 (lockstep hazards, rollback) and the integration recipe are history.

Research only (2026-10-16). Nothing here was run: another worker was benchmarking on
this PC. Every claim cites the US (SCUS_971.77) code; experiments that need a run are written
out in "Experiments for the game-side worker". Plan: `docs/ONLINE-PLAN.md`.

Goal: four PCs each run the game's own 4-player split-screen race with the same inputs
(lockstep), and each shows only its own player's view, full screen.

## Answers in short

1. **Pads.** The game asks the multitap one question, `sceMtapGetConnection(0)`
   (`func_00245DA0`, reply word 1). With 1 it maps player i to port 0 slot i; with 0, player 1
   is port 1 slot 0. It then opens all 8 port/slot pairs and polls each pad whose
   `scePadPortOpen` returned nonzero. The port answers all multitap RPCs with zeros and opens
   only port 0 slot 0, so the game sees one pad. Two players need no multitap at all (port 1);
   three and four need `GetConnection(0) = 1` and slots 1-3 of port 0 opened and refreshed.
2. **Players and riders.** Player i (0-3) is human rider i, viewport i, camera
   `D_0037EA60[i]`, and pad `D_00660DA0[i]` (= 2i with a multitap). The rider world is
   `D_002AF2C0`: 10 riders of 0x7BD0 bytes (US stride equals PAL), count at +0x501A8. Rider
   +0x7A5C is 1 for a human and 3 for AI, +0x7978 is the player (or AI) index, +0x78D4 is the AI
   controller (0 for humans).
3. **Race start.** Menus write the race setup into `D_004B25A0` (mode +0x10C, track +0x2A0,
   human count +0x288, riders +0x290, bikes +0x11C). The shell loop exits when menu 9 (the load
   screen) finishes, then `func_001B7198` builds the race from that block. The seed is
   `func_00238F10()` at `func_0016B8C8`, already pinnable with `DHD_RACE_SEED`.
4. **Split screen.** `func_0016B8C8` sets the viewport count (`func_0023D610`: 1, 2 or 4) and a
   preset per view (`func_0023C8D8`) from a 12-entry table at `D_0027ECC0`. Four players use
   presets 6-9: 256x112 quarters of the 512x224 field. The game also *simulates* per camera
   (visibility and distance to the nearest view in rider and object code), so the full-screen
   view must be made in the renderer, not by changing viewports on one PC.
5. **Field size.** `func_001B89C8` gives the AI count: with 1 player 9, 7 or 5 AI (10, 8 or 6
   riders) by track class; with 2 players 6, 5 or 4; **with 3 or 4 players none**. A 4-player
   race holds exactly the 4 humans. With 2+ players the game also loads other level files and
   drops trackside actors (section 4).
6. **Determinism.** The default paced virtual frame clock already makes game time a function of
   EE work, not host time. The hazards left are listed with their fixes below; the biggest are
   (a) host input reaching the pads, (b) anything that changes the number of kernel calls per
   frame on one PC only (e.g. drawing fewer viewports), (c) uninitialised host stack (finding 034
   fixed four producers), (d) the save/options state.
7. **Rollback.** The game-writable static area is 6.1 MB (.data+.sdata+.sbss+.bss), of which
   the rider world alone is 320 KB; the rest of EE RAM is heap. Per-field dirty size must be
   measured (experiment E5).

## 1. Multitap and pads

### What the game does

Boot: `func_0021F400` (`src/unsorted/0021F400.c`) loads SIO2MAN, MTAPMAN, MCMAN, MCSERV,
PADMAN, ... then:

- `func_00245AE8` = `sceMtapInit` (SDK asm, `build/asm/text_00243198.s` 0x245AE8): binds RPC
  servers 0x80000901-0x80000905 (clients `D_0075F040`, `068`, `090`, `0B8`, `0E0`), then calls
  `func_00245E10` (server 0x80000905, function 1, 128-byte buffer `D_0075F140`) and checks
  `reply[0] >> 8 == 3` (module version 3.x). A mismatch only prints; the game ignores the
  result.
- `func_00245D28(0..3)` = `sceMtapPortOpen(port)`: server 0x80000901, function 1, send word 0 =
  port, returns reply word 1. Results ignored.
- `func_00244D88` = `scePadInit` (binds 0x80000100/0x80000101, checks padman version 4.x via
  `func_00245A58`), `func_00245E78` = `sceMcInit`.
- `func_0021F620` (`0021F620.c`): `D_0029E2B0 = sceMtapGetConnection(0) != 0`
  (`func_00245DA0`: server 0x80000903, function 1, send word 0 = port, returns reply word 1).
  Then for logical controller i (0-7) `D_00660DA0[i]` = `2i` with a multitap, else `i`.
- The loop in `func_0021F400`: for port 0-1, slot 0-3, physical index `id = port + 2*slot`
  (`D_00660C80[port][slot]`), `D_0029E298[id] = scePadPortOpen(port, slot, D_00660380[slot][port]) != 0`
  (`func_00244F68`, the per-pad 256-byte area).

ps2sdk's open `libmtap.c` uses the same three servers, function 1, port in send word 0 and the
result in reply word 1; Sony's library adds 0x80000904/0x80000905 (the version query above).

Per frame: the main loops call `func_0021F8E0`, which runs `func_0021FAD0(port, slot)` for every
opened pad. That is a small state machine per pad (`D_00660D00[id]`): `scePadGetState`
(`func_00245230`, state byte +0x70 of the newer frame; 6 or 2 count) -> `scePadInfoMode`
(`func_00245498`) -> analog (ID 7: states 70-78, `scePadSetMainMode` locked, actuator align,
pressure mode via `func_00245988`/`func_002459E8`) -> state 99, where `scePadRead`
(`func_002451B0`) copies the 32-byte reply into `D_00660B80[id]` and `func_002202F8` turns
pressures into digital bits. `D_0029E2A8[id]` = pad active.

Accessors (all take the physical index): `func_00220230` buttons (inverted, 1 = pressed),
`func_00220268` raw word, `func_00220280` four stick bytes, `func_002204A8` sticks packed,
`func_00220640`/`func_002205B8` edge detection (`D_00660D80`), `func_002201B0` any input.

Re-detection: `func_0021F820` (called from `func_001B57F8`) polls `func_00246030` (mcserv
function 0x15) per port and reruns `func_0021F620` when the value changes. The port's mcserv
answers zeros, so it never changes.

### What the port does now

- `port/hle/pad.c` keeps `pads[2]`, takes the port as `a[1] & 1`, ignores the slot, and
  `PAD_OPEN` succeeds only for port 0 slot 0. Every pad frame is filled from
  `host_pad_read(0, ...)`.
- `port/hle/iop.c:151` `rpc_mtap` is `rpc_unknown`, which zeroes the reply
  (`port/hle/sif.c:223`): GetConnection = 0. `sif.c:34-38` binds all five multitap ids to the same
  handler, so the handler cannot tell PortOpen from GetConnection.

### What it must return for 4 pads

| Call | Must return |
|---|---|
| 0x80000905 fn 1 (version) | reply word 0 = 0x0300 (silences the version message; optional) |
| 0x80000901 fn 1 (PortOpen port) | reply word 1 = 1 for port 0 |
| 0x80000903 fn 1 (GetConnection port) | reply word 1 = 1 for port 0, 0 for port 1 |
| 0x80000902, 0x80000904 | zeros (never called by the game) |
| padman `PAD_OPEN` port 0 slot 0-3 | `r[3] = 1`, remember the area `a[4]` per (port, slot) |
| every vsync | refresh the older frame of every open (port, slot) area, state 6 |
| `PAD_SET_MMODE` / `SET_BTNINFO` / `SET_ACTALIGN` | per (port, slot): use `a[1]` port and `a[2]` slot |

Libpad's `PAD_INIT` sends area 0 in this build (`func_00244EC8` stores 0 at send word 4), so
`open_slots` stays unused. The game never asks for the slot count (no calls to the
`GetSlotMax` / `GetPortMax` wrappers).

## 2. Players, riders and where pads are read

Rider world `D_002AF2C0` (`func_00119E60`, `0011AD18.c`, `0011C0D0.c`, `00154E10.c`):

| Offset | Contents |
|---|---|
| +0x00000 | `Rider riders[10]`, 0x7BD0 each (`include/rider.h` checks the size) |
| +0x4D620 | 9 AI controllers, 0x450 each (`func_0010A730` initialises) |
| +0x4FCF0 | 4 human controllers, 0x110 each (`func_00199338`) |
| +0x50130 | `Rider *human[4]`, +0x50140 `Rider *ai[9]`, +0x50164 `Rider *order[10]` |
| +0x501A8 | rider count |

`func_001B87F8` (`001B7198.cpp:649`) fills it: human i = rider i (`func_0011AD18` ->
`func_0012C130`: +0x7A5C = 1, +0x7978 = i, +0x7988 = i, +0x78D0 = human controller i, HUD slot
`D_003B2270[i]`, camera `func_001487D8(D_0037EA60[i])`), then AI j = rider humans + j
(`func_0011AD80` -> `func_0012C398`: +0x7A5C = 3, +0x7978 = j, +0x7988 = -1, +0x78D4 = AI
controller j). Bikes: setup slot i -> `func_001FABE8(rider + 0x78D8)` (three part ids at rider
+0x7908/+0x790C/+0x7910). The PAL notes (rider type at +0x7A5C, 1 human / 3 AI, stride 0x7BD0)
hold for the US build at the same offsets.

Pad reads per field go through `func_002205A0(rider->x7978)` -> physical index:
`func_001E5CC8`/`func_001E5D90` (trick button combos, `001E5680.c`), `func_001E6970`,
`func_001E6F58`, `func_001E8298`, `func_00174C58`/`func_00174D48`, `func_00199338` (human
controller: steering, pedal), `func_0017DD70`. Humans are gated by `x7A5C == 1` or
`x78D4 == 0`. So **an AI rider cannot read a pad as it stands**: AI riders carry +0x78D4 and go
through their controller; making one pad-driven means turning it into a human
(`func_0012C130`) with a player index, which also gives it a camera and HUD slot (only 4 exist).

Inputs are sampled once per game frame: `func_0021F8E0` copies padman's newest frame into
`D_00660B80[id]`, and padman's frame is written by `pad_vsync` at each VBLANK.

## 3. Starting a race directly

### The game's own path

- Menu state: `D_0029DED8` (current menu, what `menu_nav.h` watches), `D_0029DEEC` (previous),
  dispatch in `func_001EB4D0` (`001EB1D8.cpp:632`). Shell loop: `while (!D_0029DFB4)` at
  `001EB1D8.cpp:582`.
- Menu 15 (`func_001ED5B8`): choice 0 = one player (`x288 = 1`, menu 32), choice 1 =
  multiplayer (menu 10).
- Menu 10 (`func_001EED78`, `001EB1D8.cpp:1958`): 2/3/4 players -> `x288 = 2/3/4`, next menu
  16. Back sets `x288 = 1`.
- Menus 3, 4, 5, 6 all run `func_001F24B8` (rider select); with 2+ players, Back returns to 10
  (`001EB1D8.cpp:3613`, `3833`). Most likely one menu per player (to confirm in E2).
- Menu 1 (`func_001ED900`) picks the mode (`x10C`): 12, (menu 12 ->) 3, 4, 15, 17, 11.
- Menu 18 (`func_001F1500`) fades out and goes to menu 9 with `D_0029DF40 = 120`; menu 9
  (`func_001F43C0`) counts `D_0029DF44` to `D_0029DF40` and sets `D_0029DFB4 = 1`.
- After the shell, the top-level loop `func_001B6708` (asm only, `text_001B6708.s`) recomputes
  the track `x2A0` for career modes from `D_004225A0` (`func_00194968`, `func_00194C48`),
  then calls `func_001B7198` (race build) and `func_0016B8C8` (race init).

### The setup block `D_004B25A0`

| Field | Meaning | Evidence |
|---|---|---|
| +0x10C | mode (0-20; 10 = attract demo, 20 = no riders) | `func_001B74E8`, `func_001B89C8`, title idle (`001EB1D8.cpp:639`) |
| +0x110 | menu branch flag | `func_001ED900` |
| +0x288 | human players (also read as `D_004B2828`) | menus 10/15; ~70 readers |
| +0x28C, +0x358 | AI count | `func_001B74E8` from `func_001B89C8` |
| +0x290[4] | human rider ids (0x20..0x1E0 step 0x20) | `func_001B74E8`, `func_001E5680` |
| +0x2A0 | track (0-25; class from `func_00182290`) | `func_001B89C8` |
| +0x2A8[16] | rider id -> rider slot, -1 = free (reset to -1 by rider select) | `func_001B74E8`, `001EB1D8.cpp:3619` |
| +0x11C[4] | human bike (3 ints each) | `func_001B87F8` |
| +0x14C[...] | AI bike (3 ints each) | `func_001B87F8` |
| +0x368 | first rider id tried for AI | `func_001B74E8` |
| +0xB174 | no-AI flag | `func_001B87F8`, `func_001B89C8` |

### Seed

`func_0016B8C8` (`0016B8C8.cpp:93`): if `D_0043F9F0.x44` (replay) reuse `D_0043F9F0.x20`, else
`seed = func_00238F10()` (Timer 1 count plus its overflow sum `D_0029E3E0`) and store it at
0x0043FA10. `func_00222E10(seed)` seeds three generators: libc `srand` (`func_002559C8`; `rand` =
`func_002559D8`, used by `func_00222BD8(lo, hi)`), the generator at `D_00663140`
(`func_00242FB8`), and `D_0029E2C0 = 0x0379EA54`. A seed of 0 falls back to the RTC
(`func_00248058`, seconds + minutes * 60): never pass 0. The port already patches the call
(`port/patches.py:325`, `hle_race_seed`, `DHD_RACE_SEED`).

### What already exists

`DHD_NAV` (`menu_nav.h`, `autopad.c`) presses buttons when a menu shows; the 1-player script is
`race_compare.NAV` (`0:triangle|cross,15:cross,3:cross,16:cross,1:cross,11:cross,18@4C7F38=0:start,18@4C7F38=1:cross,9/5:cross`).
It drives port 0 only (`host_pad_read` applies scripts when `port == 0`) and cannot write memory.

### Quickest route

1. Navigate the real menus with `DHD_NAV`, the same chord sent to all four pads, down to menu 9
   (path from E2: 0, 15 down+cross, 10 down down cross, 16, 3-6 cross each, 1, ..., 18, 9).
2. On the first field that shows menu 9, write the lobby's choices into `D_004B25A0` (track,
   riders, bikes; `x288 = 4`) before `D_0029DFB4` ends the shell; pin the seed.

A shorter jump (write the block and set `D_0029DED8 = 9`, `D_0029DEEC = 8`, `D_0029DF40 = 120`,
`D_0029DF44 = 0` at the first menu 15) skips the rider-select shell, which also resets
`x2A8` and builds the shell's own state; do it only after E2 shows the full block at menu 9.

## 4. Split screen

`func_0016B8C8` (`0016B8C8.cpp:124`): 1 player `func_0023D610(1)`, preset 0, and FOV 277 on
view 0; 2 players two views, presets 3 and 4; 3 or more four views, presets 6-9 (a 3-player race
draws four views). `func_0023C8D8(preset, view)` stores the preset in `D_0075A9D0[view]`
(0x14 bytes: camera, preset, ...) and writes the view's SCISSOR into its packet
`D_0075BAC0[view]` (0x360 bytes). `func_0023CB00(view)` builds the view's matrices; camera i is
player i's (`func_0023D020`).

Preset table `D_0027ECC0` (0xC0 bytes each; projection scale, size, origin, centre, XYOFFSET,
aspect; decoded from `build/asm/data/data_0026DD00.s`):

| Preset | Size | Origin | Projection | Aspect | Use |
|---|---|---|---|---|---|
| 0 | 512x224 | 0,0 | 277 | 0.5833 | 1 player |
| 1 / 2 | 512x120 / 512x108 | 0,0 / 0,116 | 277 | 0.5833 | horizontal halves (unused here) |
| 3 / 4 | 262x224 / 250x224 | 0,0 / 262,0 | 222 | 0.5833 | 2 players |
| 5 | 640x224 | 0,0 | 348 | 0.47 | wide |
| 6-9 | 256x112 | 0,0 / 256,0 / 0,112 / 256,112 | 210 | 0.47 | 4 players |
| 10 | 64x64 | | 13 | 0.98 | small target |

### Why not change viewports on one PC

The view count `D_0029E450` (`func_0023D608`) and view cameras feed simulation:
`func_0012D510` (objects hidden or not by the nearest view camera), `func_00130010` (rider
visible to view i, per-view mask), `func_0011C918`/`func_00128DD8` (per view motion sampling
into rider +0x793C), `func_0011C6F0`/`func_00128B80`/`func_0016C4B0` (environment settings
for N views). Drawing fewer views would also cut the kernel calls of the scene walk
(`func_0021B8A8`, about 375 `FlushCache` per view), and the virtual clock charges 1024 cycles per
kernel call (`port/hle/timer.c`), so game time per frame would differ between PCs. Any EE-side
change is safe only if **every PC makes the same change**.

### Full screen, render side only

Every PC runs the identical 4-view frame. The native renderer (`port/gs/gpu_native.inc`) then
shows only the local player's quarter: primitives whose SCISSOR is the local preset's rectangle
are mapped `x' = 2(x - x0)`, `y' = 2(y - y0)` with a full-screen scissor; primitives of the other
three rectangles are dropped from the picture; full-screen primitives (shared HUD, fades) are
kept, or scaled when they lie inside the local quarter. The exact backend keeps drawing all four
views, so the lens-flare Z readbacks stay identical on every PC. Quarters keep the 2:1 field
shape, so the 2x scale is exact; the image keeps the 4-player framing (projection 210 on 256
pixels, a narrower view than 1 player's 277 on 512) and the 4-player level of detail.
Interpolation (`DHD_INTERP`) is display-only and works unchanged.

For a 2-player online race the 2-player presets (262 and 250 wide) can't be scaled to full
screen; the same patch on every PC can point views 0 and 1 at presets 6 and 7 instead.

### What else differs in split screen

About 330 sites read the human count (`D_004B25A0.x288`, also seen as `D_004B2828`). A
helper agent sorted them from the C in `src/unsorted` and the asm. The game-state ones:

| Effect | Sites |
|---|---|
| **No AI** at 3-4 players (section 5); every AI loop then runs zero times | `func_001B89C8`; AI loops `func_0010AF20`, `func_0010C108`, `func_00112360`, `func_001132B0`, `func_0011A050`, `func_0011A938`, `func_0011ADE8` |
| **Other level files** with 2+ players: `ALP2`, `MOAB2`, `GLAC2P`, `CITY2`, `FUJI2`, `PERU2`, ... | `func_001BA3B8` (`001BA3B8.c:28`) |
| Trackside actors: none at 3+ players, every second one at 2 | `func_0011CF70`, `func_00129158`, `func_00159518` (`< 3`), `func_0011CA78` (stride 2) |
| Extra RNG calls in multiplayer (`humans == 1 \|\| RNG(0,1)`) | `func_0011E9F0`, `func_00129C68` |
| Hazard spawner: RNG range and calls per frame scale with humans | `func_001DC328`, `func_001DC548` (asm only), `func_001DC830` |
| Start grid: one RNG swap per human | `func_0011AEC0` |
| Lightning odds depend on the AI count (weather LCG `D_0029D6D0`) | `func_001DFC08` |
| Race end waits for every human to finish | `func_0019F9B0` |
| Ghost recording (mode 18) only in 1 player | `func_00138B40`, `func_00138E08`, `func_00139660` |
| Scenery ids 4000-4499 not registered at 3+ players | `func_00127738`, `func_00192F68` |
| Rider particles only below 3 players (particle LCG `D_0029DAF8`) | `func_00161EB8` |
| Character select forces the unlock mask `0x020001C2` at 3+ players | `func_00193F38`/`func_00193F68` |

Unclear: `func_00127CC8` (with 2+ humans the resource loader takes every third id),
`func_0012C130` (1 player gets a shadow handle from `func_0021D638`, multiplayer sets
`x7930->x4 = 1`), `func_001C2A90` (1 player on some tracks waits for the audio stream flag at
the start). The rest are cameras, HUD (about 60 sites in `func_001751C8`..`func_0017C2A0`),
rumble, audio panning and menus.

Besides the main RNG (`func_00222BD8`/`func_00222B68` -> `rand`) there are three generators:
`D_00663140` (`func_00222B08`, ambient audio), the particle LCG `D_0029DAF8` (`func_00222C38`)
and the weather LCG `D_0029D6D0`. All live in EE RAM, so the lockstep and rollback treatment is
the same as for the main RNG.

Update rate: no branch ties the step to the player count. Each frame `func_00238C00` counts
elapsed vblanks from Timer 1 (one per 9619 ticks, at most 3), and `func_00238D98` sets the
delta `D_0029E3D0` = vblanks since the last frame, clamped to 1-2, then raises it while the rate
`D_0029E3F0 / delta` exceeds the cap `func_00238F68(60)` sets. The 30 fps path in
`func_001A4F70` needs `D_0029D36C`, which nothing sets. A 4-player race steps with delta 2
only if its frames take more than one vblank of virtual time (E3).

So 4-player online is a different game from 1 player: other level files, no AI, no trackside
actors. That is the game's own 4-player mode, and the same on every PC, so it does not hurt
lockstep.

## 5. Field size

`func_001B89C8` (`001B7198.cpp:676`), with track class `func_00182290(x2A0)`:

| Track class (tracks) | 1 player | 2 players | 3-4 players |
|---|---|---|---|
| 2 (0, 1, 2, 3, 5, 11, 12, 19, 20) | 9 AI (8 in mode 10) | 6 | 0 |
| 1 (4, 10, 13, 16, 18, 22, 24) | 7 | 5 | 0 |
| 0 (6, 7, 8, 9, 14, 15, 17, 25) | 5 | 4 | 0 |
| 3 (others; track 23 gives 5 / 4) | 0 | 0 | 0 |

Modes 5, 11, 13, 14, 17, 18 and the no-AI flag `+0xB174` have no AI at all. The world has room
for 10 riders and 9 AI controllers; humans are limited to 4 (controllers, cameras, HUD slots).
Adding AI to a 4-player race means patching `func_001B89C8` on every PC; it was never tested on
the PS2, and pools the game sizes by player count (`D_004B2828 < 3` checks) may not cover it.

## 6. Determinism hazards for lockstep

| # | Host-dependent input | Where | Neutralise |
|---|---|---|---|
| 1 | Controller state | `pad.c` `fill` -> `host_pad_read` (XInput, HID, keyboard, `DHD_AUTOPAD`) | Pads come only from the lockstep buffer for field F; the local controller only feeds the network. All 4 slots always report the same virtual DualShock 2 (connected, ID 0x73/0x79), whatever is plugged in. |
| 2 | Wall clock in game time | `host_seconds` (`host.c:589`) falls back to QPC without the frame clock; `DHD_PACE=wall` | Require the frame clock (default paced mode); refuse `DHD_PACE=wall` online. |
| 3 | Kernel calls per frame | virtual clock +1024 cycles per kernel call (`timer.c`); frame delta from Timer 1 (`func_00238C00` counts vblanks, one per 9619 ticks; `func_00238D98` clamps the delta to 1-2) | Same EE work on every PC: all 4 views drawn EE-side, same build, same options. Check delta and kernel-call count per field in E3. |
| 4 | Race seed | `func_00238F10` (Timer 1), depends on boot path length | Lobby picks the seed; `DHD_RACE_SEED` (nonzero). |
| 5 | RTC | `func_00248058` in `func_00222E10` when seed = 0, and save dates (`func_00193B00`) | Nonzero seed; no RTC in game state otherwise. |
| 6 | Uninitialised host stack and padding | finding 034: `func_001306C0`, `func_001304F8`, `func_00136038`, sound requests `func_00201B68`/`func_00201BD0` (fixed in `port/patches.py`) | Keep the fixes; per-field state hash catches new ones; E4 runs two configurations against each other. |
| 7 | GS readback | lens-flare Z probe, `sceGsExecStoreImage` from `func_00228328`, used next frame | Exact backend on every PC (finding 038), all four views drawn exactly. |
| 8 | Renderer, MTVU, interpolation, CPU | finding 038 gate: exact and native arms give identical EE at 6 moments; `DHD_INTERP` gate MATCH | Allowed per PC; covered by E4 across two machines. |
| 9 | x87 / MXCSR state | finding 038 (`fabsf` without prototype left values on the x87 stack) | Keep the `x87_leaks.py` scan in the release check. |
| 10 | Sound to game | 989snd replies are a function of the vsync count (`sound.c` header comment); WASAPI only drains the ring | Keep; `DHD_SND_OFF` must not change replies (check in E4). |
| 11 | IOP RPC timing | `sif.c:136` replies 0.001 s later in virtual time | Deterministic under the frame clock; keep. |
| 12 | Save data and options | `D_004225A0` (unlocks, options); the port's mcserv answers zeros, so every PC starts from a new profile | Online forces the default profile and options; no memory card in online mode. Hash `D_004225A0` in the lobby if saves are added later. |
| 13 | Boot path | menu timing, attract demo after 3600 frames idle at the title (`001EB1D8.cpp:637`), memory-card dialog | Same scripted boot on every PC, or a barrier at the first menu-9 field after which the setup block is written identically. |
| 14 | Multitap re-detect | `func_0021F820` reruns `func_0021F620` if mcserv function 0x15 changes | mcserv stays constant (zeros) or returns a constant. |
| 15 | Window and focus | message pumping in `kernel_poll` | No game state; pads never read focus. |

Lockstep field numbering: `kernel_vsync_count` counts from boot, which differs per PC if boot
paths differ. Number fields from the barrier (hazard 13), and feed field F's inputs at the
`pad_vsync` of field F.

## 7. Rollback cost (later)

- EE static writable: .data 0x1AB70 + .sdata 0x1BF0 + .sbss 0x848 + .bss 0x601E80 (0x29E580 to
  0x8A0400) = 6.1 MB (`build/dhd.map`). Inside it: the rider world 0x501C8 (320 KB).
- EE heap: 0x8A0400 to 32 MB (25 MB) holds level data, mostly read-only after relocation but with
  live scene nodes.
- Scratchpad 16 KB, VU0 (data memory and registers), the pad and RNG state (inside EE RAM),
  989snd voice state (the game reads its replies), the HLE kernel (threads are fibers with host
  stacks: snapshot only at the vsync wait in `sceGsSyncV`, where every EE thread is blocked).
- Deferred lens-flare readbacks: save the probe results with the snapshot.
- Size per field: measure (E5). Windows write watches (`MEM_WRITE_WATCH`, `GetWriteWatch`) on
  the EE RAM mapping give the dirty pages per field and can also drive incremental snapshots.

## Integration recipe

### Pads (port/hle/pad.c, iop.c, sif.c, host.c)

1. `pad.c`: `static Pad pads[2][4]`; in `rpc_pad` take `port = a[1] & 1`, `slot = a[2] & 3` for
   every command that addresses a pad (`PAD_OPEN`, `PAD_CLOSE`, `PAD_SET_MMODE`,
   `PAD_SET_ACTALIGN`, `PAD_SET_ACTDIR`, `PAD_SET_BTNINFO`). `PAD_OPEN` succeeds for
   `slot < pads_connected(port)`: port 0 slots 0..N-1 with a multitap (N = `DHD_PADS`, 1-4),
   else port 0 slot 0 and, for N = 2, port 1 slot 0.
2. `pad_vsync`: loop over both ports and four slots; `fill(p, older, player)` with
   `player = slot` (multitap) or `port` (no multitap).
3. `fill`: read `pad_input(player, &in)` instead of `host_pad_read(0, &in)`. `pad_input` returns
   the lockstep input for the current field online, else `host_pad_read(player, ...)`.
4. `sif.c`: give 0x80000901/0x80000903/0x80000905 their own handlers (or pass the server index).
   `iop.c`: PortOpen and GetConnection put 1 in `recv` word 1 for port 0 when `DHD_PADS > 2`
   (0 otherwise); version puts 0x0300 in word 0; the rest zero.
5. `host.c`: `host_pad_read(port, ...)` already passes the port to XInput; apply `DHD_NAV`
   scripts to every player (same chord), not only `port == 0`, so the menus accept any pad.
6. Test: `DHD_PADS=4` with four XInput pads should show four quarters after the menus.

### Race-start shortcut

1. Extend `menu_nav.h` steps with a write: `9!4B282C=...` style or a separate
   `DHD_RACE_SETUP=track,rider0..3,bike0..3` applied in `autopad_nav` on the first field that
   shows menu 9. Writes: `D_004B25A0 + 0x288 = 4`, `+0x2A0 = track`, `+0x290 + 4i = rider id`,
   `+0x11C + 12i` = the three bike ids, plus the mode found in E2.
2. Pin the seed with `DHD_RACE_SEED` (the lobby's seed, nonzero).
3. Start the lockstep barrier at that field.

### Full-screen view

1. `DHD_VIEW=p` (0-3): in the native renderer's command recording, classify each draw by its
   SCISSOR against presets 6-9 (`(0,0)-(255,111)`, `(256,0)-(511,111)`, `(0,112)-(255,223)`,
   `(256,112)-(511,223)`), keep and scale view p's draws by 2 about its origin, drop the other
   three, keep full-screen draws.
2. Leave the exact backend (readbacks) and all EE code unchanged.

## Experiments for the game-side worker

Use `build/` for dumps (game data). All runs: `DHD_RENDER=native`, `DHD_RACE_SEED=81257`,
`DHD_SND_OFF=1` unless noted.

- **E1, multitap visible.** After the pad changes: `DHD_PADS=4`, `DHD_LOG=1`, boot to menu 10.
  Expect no "rpc multitap ... not written" lines, `D_0029E2B0` (EE 0x0029E2B0) = 1,
  `D_0029E298[0,2,4,6]` = 1, and `D_00660D00[0,2,4,6]` reaching 99.
- **E2, the 4-player menu path and setup block.** `DHD_PADS=4`, `DHD_NAV_DUMP=build/port/e2`,
  `DHD_NAV='0:triangle|cross,15:down,15:cross,10:down,10:down,10:cross,16:cross,3:cross,4:cross,5:cross,6:cross,1:cross,11:cross,18@4C7F38=0:start,18@4C7F38=1:cross,9/5:cross'`
  (adjust from the screens shown; every pad pressed). From the dumps record `D_004B25A0`
  +0x10C, +0x110, +0x288, +0x290..+0x29C, +0x2A0, +0x2A8..+0x2E4, +0x11C..+0x14B, +0x368,
  +0xB174 at menu 9, and the menu sequence. Confirm one rider-select menu per player.
- **E3, frame delta in 4 players.** In a 4-player race log `func_00238C00`'s frame increment
  (EE `D_0029E3D8` per field) and kernel calls per field; delta 1 means a 60 Hz simulation.
  Compare to PCSX2 with the same script (the PS2 may run 4 players at 30 Hz).
- **E4, cross-configuration determinism.** Same E2 script and `DHD_RACE_PAD` on all four pads,
  `DHD_RACE_CAPTURE=g+600,g+1800,g+3600`, `DHD_RACE_DUMP=build/port/e4-X`; run X = exact,
  native, native + `DHD_INTERP=1`, `DHD_SND_OFF` unset, then on a second PC. Compare EE dumps
  (whole 32 MB and the rider world 0x2AF2C0-0x2FF488); all must match.
- **E5, dirty state per field.** `DHD_RACE_CAPTURE=g+600,g+601,g+602`, dumps as E4; count the
  differing 4 KB pages between consecutive fields, separately for 0x0026DD00-0x008A0400 and
  the heap.
- **E6, full-screen view.** With `DHD_VIEW=0..3` compare EE dumps against E4 (must match) and
  capture pictures of each.

## Sources

- ps2sdk `ee/rpc/multitap/src/libmtap.c` (server ids 0x80000901-0x80000903, function 1, port in
  send word 0, result in reply word 1). ps2sdk `ee/rpc/pad` for the padman protocol (as in
  finding 020).
- Pedroj-64/DownHill-Port-PC `docs/p2s-savestates.md` (PAL rider stride 0x7BD0 and type byte
  +0x7A5C, 1 human / 3 AI), via the project's research notes; confirmed here on US code.
- The game's manual (multitap on port 1 for 3-4 players).

## Issue #12: freeride tracks at 3 players (2026-10-29)

At 3 players the game draws four views (`D_0029E450`), but the level registers its objects for
views 0-2 only (`func_001589A8` -> `func_0023D0B0`), so the fourth view has no `x4` slot and its
rider slot is empty. The PS2 never offered these tracks at 3-4 players (the 5-track mask of issue
#1), so nothing there hit it. The freeride tracks' race build (`func_0011C6F0`) stores through every
view's `x4`, and the per-field callbacks of five track families (`0011C918`, `00128DD8`,
`00181B88`, `00195F90`, `0016C720`) write `x4 + 0xB8` and read the view rider's `x793C`: on the EE
low RAM, in the 64-bit build its null guard. Port patches (`port/patches.py`): `func_0023D1A0` takes
a missing slot from the pool (`func_00208FB8`, as `func_0023D0B0` does), and the five callbacks skip
`func_0021D758` for a view without a rider. Checked with scripted races on Italy freeride (track 0):
3 players crashed before the fix at the race build (write of guest 0) and ran after it (the fourth
view shows the track camera); 2 and 4 players still run.

### Glacier's empty-view motion callbacks (2026-10-29)

A three-player Glacier freeride run with `DHD_RACE_SETUP=5` faulted during the countdown at guest address `0xB8` in `func_00167A00`: the fourth view has no rider or motion target. Glacier's `func_0016C720` samples that target before the later allocation in `func_0023D1A0`; `func_0016B400` has the same ordering. The port skips those callbacks' empty-rider iteration before sampling. Iterations with riders are unchanged. A first attempt to allocate only in `0016B400` still faulted; after guarding both callbacks, the same minimized harness reached GO, captured at g+300 and remained in race past clock 3952. The test used the setup override, so it does not cover track-menu selection.


### BC2's inactive paired actors (2026-10-29)

BC2 (track 19) has another cause besides the empty fourth view: `func_00192F68`
disables trackside actor allocation at **both** three and four players. The paired
controller manager still registers BC2's path controllers (`0016C8A0`), but
`0016CE68` leaves their state at 0 unless both actor nodes bind. The null-owner
initialization (`0016CDE8` -> `001224F0`) leaves each part's reset flag at 1.
`0016D0E0` updated these inactive controllers anyway, so `0016D5C0` requested
`0016D648` -> `00127098` -> `00126B20`, which dereferenced a missing actor's
vtable. A fresh unguarded diagnostic run reproduced RVA `0x32a9c3`: the first
controller (`D_003A6A10 + 0x50`, mapped guest `0x083A6A60`) had state 0,
both actor pointers 0 and both reset flags 1 immediately before the fault.
A two-player diagnostic control reached GO and g+300 without that inactive log.

The port now leaves state-0 controllers dormant in `0016D0E0`, alongside the
existing completed-state-14 early return. This avoids updating absent actors
instead of hiding two individual bad resets; active controllers are unchanged.
An earlier candidate, guards on the two part resets in `0016D648`, was dropped for
this; it also reached GO, and a load stall seen once with it did not come back.
Diagnosis used the local matching
sources, a temporary port-only logging patch and `host64_rva.py`; no external code
was needed. Diagnostic logging is absent from the final build.

Final minimized sweep: all nine class-2 Freeride tracks from `00182290`, each at
three and four local players, using rider indexes `0,1,2` / `0,1,2,3`
(guest IDs `20,40,60` / `20,40,60,80`). Each PASS independently checks GO,
`shot-g+300+0.bmp` written, a live race heartbeat beyond clock 3600 and at least
61 wall seconds after observing GO (actual minimum 62.3 s). Track selection uses
`DHD_RACE_SETUP`; this tests level/race behavior, not selecting each menu entry.
Strict wasm link and offset scan pass; the win-x64 host build passes.

| Track (level) | 3 players | 4 players |
|---|---|---|
| 0 (ALP2) | PASS, clock 3989 | PASS, clock 3719 |
| 1 (MOAB2) | PASS, clock 4971 | PASS, clock 4603 |
| 2 (JUNGLE2) | PASS, clock 4359 | PASS, clock 3761 |
| 3 (PERU2) | PASS, clock 3817 | PASS, clock 4938 |
| 5 (GLAC2P) | PASS, clock 5395 | PASS, clock 4267 |
| 11 (AUBERST2) | PASS, clock 4030 | PASS, clock 4394 |
| 12 (FUJI2) | PASS, clock 5164 | PASS, clock 5088 |
| 19 (BC2) | PASS, clock 3768 | PASS, clock 3724 |
| 20 (CITY2) | PASS, clock 4896 | PASS, clock 5170 |
