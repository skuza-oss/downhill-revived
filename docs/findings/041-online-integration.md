# 041 — Online races in the port: four pads, full-screen view, launcher (first design: input lockstep, dropped for state sync)

Status: partly superseded by 044 (the input-lockstep race path, the friends' launcher)
Checked: 2026-11-04

2026-10-16. Step 3 of `docs/ONLINE-PLAN.md`: the client module (`port/net/`, finding 042) wired
into the game, following finding 040's recipe, with the relay of finding 039.

**Dropped the same day.** The owner replaced input lockstep with state sync (finding 044): every
PC runs its own one-player race and the other riders are puppets. Do not bring back the lockstep
race path (shared fields, state hashes, input delay, the 4-pad split screen online). Still true,
and cited by the code: host values in EE memory and `DHD_WATCH`, the split-screen data image bug,
the multitap, the settings variables, the disc hashes. The race-start, results and friends'
package sections of this finding were cut (full text: `git show 860ab6b:docs/findings/041-online-integration.md`);
the friends' launcher and zip are gone (the player launcher is `port/launcher`, finding 093).

## What is there

| Where | What |
|---|---|
| `port/hle/pad.c`, `iop.c`, `sif.c` | A multitap on port 0: slots 0-3, `PortOpen`/`GetConnection`/version handlers (0x80000901/903/905). Player i reads pad 2i (port 0, slot i), each slot from its own source (`pads_read`). |
| `port/hle/online.c` | Pad sources, race setup, the view choice (then also the barrier, lockstep and state hashes; now the state-sync race, finding 044). |
| `port/hle/host.c` | `host_pad_script` / `host_pad_device` split, fiber stack ranges, `DHD_WATCH` (a diagnostic hardware data watchpoint). |
| `port/hle/kernel.c` | `kernel_pace_slip`: the field clock slips by the time spent waiting for other PCs. |
| `port/gs/native_core.inc` (then `gpu_native.inc`), `present.c` | `DHD_VIEW`: one quarter of the split screen, scaled to the window (present only). |
| `port/patches.py` | `001A4F70.c`: `offset` zero-initialised (below). |
| `port/tools/online_race.py` | Two game instances and two `nettool --bot` players through the relay. |

## Settings (environment; tools and the launcher set them)

| Variable | Meaning |
|---|---|
| `DHD_PADS=1..4` | Offline: how many pads are connected (multitap when more than 2). Online: always 4. |
| `DHD_RACE_SETUP=track,r0,r1,r2,r3` | Offline: track and rider indexes written into the setup block at the load screen. |
| `DHD_NET_SERVER` | The relay (from `online-settings.txt`; never in a tracked file). |
| `DHD_NET_CREATE=1` / `DHD_NET_JOIN=CODE` | Host a room / join one. |
| `DHD_NET_NAME`, `DHD_NET_RIDER`, `DHD_NET_TRACK`, `DHD_NET_PLAYERS`, `DHD_NET_PUBLIC`, `DHD_NET_ROOM_NAME` | Lobby choices. `DHD_NET_PLAYERS=n`: the host starts when n are in and ready; otherwise on START/Enter. |
| `DHD_NET_ISO_SHA256` | The disc hash the server compares, set by tools; the game and the launcher compute it themselves (since 2026-10-28 the game files hash, finding 098). |
| `DHD_NET_SCRIPTED=1` | Tests: this PC's slot comes from `DHD_RACE_PAD<slot>` instead of the controller. |
| `DHD_VIEW=0..3` / `all` | Full-screen view of that player in 3-4 player races. Online the default is this PC's player. |
| `DHD_HASH_LOG=FILE`, `DHD_HASH_DUMP=field` | Hash every 60 fields; dump the hashed memory at one field. |
| `DHD_WATCH=addr[:rw]` | Diagnostic: hardware data watchpoint on one EE word; logs writer, field and value. |

## Disconnects

Then: a slot whose player left was held neutral (the rider stopped), because a 3-4 player
lockstep race sets up no AI controller for human slots. Now: a player who leaves becomes an AI
rider on the host (finding 047).

## Host values in EE memory (DHD_WATCH)

Two runs of the same build with the same pads gave EE memory that differed in words holding
host addresses or host stack bytes. Finding them: `DHD_WATCH=addr` sets a hardware data
watchpoint (DR0/DR7 through `SetThreadContext` from a helper thread, a vectored handler for
`EXCEPTION_SINGLE_STEP` that logs the writing function, the field and the value). Results:

- **Rider 0 +0x535C (EE 0x2B461C), `obj->xDB0.w`.** Written once (field 637, loading) by
  `func_00221888`, a VU0 macro routine that loads and stores all four lanes (`lqc2`/`sqc2`),
  called from `func_001A6D00` (`001A4F70.c`): `Sub__c10(&offset, ...)` sets x, y, z of a stack
  `Vec__c10 offset`, never w, so host stack bytes went into the rider. No read of the word in
  80 s of race with a read/write watchpoint, but it sits in the rider block the race hash covers
  and a later version could read it. Fixed: `port/patches.py` zero-initialises `offset`
  (as finding 034 did for other stack structs). After the fix the race hash matched where it
  had differed before (E4 below).
- **Menu stack (`D_003E3BD0`, e.g. EE 0x3E3C34).** `func_00187048` (from `func_001881C0` /
  `func_001882C8`) pushes the address of a stack local (`pair`) and uses it during the call:
  on the PC that's a host stack address that depends on where Windows put the fiber stacks.
  The game reads it only as a pointer, during that call. Harmless; the hashes leave host stack
  ranges out (`host_stack_ranges`, recorded per fiber).
- Other differing words in the static area (0x5F0000-0x630000, 0x862000+) were saved fiber
  stack addresses of the same kind; masked, the whole 32 MB matched at field 660.

The online hash (`game_hash`) covers the race-deciding state of finding 040: the rider world
(0x2AF2C0, 0x501C8 bytes), the twister state `D_00663140`, libc `rand`'s state, the three LCGs
and the pads. `DHD_HASH_LOG` also prints a hash of the static area and of all 32 MB, for
information only.

## The 3-4 player split screen (a data image bug)

In 3-4 player races the top row of the split screen (players 1 and 2) showed a flat brown
fill or a blurred close-up from the start, in both renderers; 2 players (side by side) were
fine. Not the pads (the same with neutral pads, already at GO), not the riders or the track,
not the cameras (the four camera blocks `D_0037EA60[i]` were sane and alike).

- A GS dump of one field (`DHD_GS_CAPTURE`, 8 fields) replayed in PCSX2 (finding 025's
  referee) left the top row black: the GS input itself was wrong, upstream of both renderers.
- In the dump, each view's draw environment had the right FRAME and SCISSOR, but the top row's
  XYOFFSET was y 912 against the bottom row's 1936, so its geometry landed ~1000 lines below
  the scissor (PCSX2 clips it all; our GS rasterised the guard-band triangles as garbage).
- The views' descriptors (`D_0027ECC0`, 0xC0 each, read by `func_0023D650` and friends) held
  height 2160 where the ELF has 112, and corner y 2160 where it has 112. No game store touched
  them (`DHD_WATCH` on the words: no hit), so the port's data image was wrong from the start.
- Cause: splat wrote the halfword pairs `(256, 112)` = 0x00700100 as `.word D_00700100`, an
  address inside the image, and `port/tools/data.py` relocated it (+0x08000000 → height
  0x0870). Fixed by listing those words as numbers (`data.py` `NUMBERS`). Now all four views
  draw, with their place numbers.

The same splat pattern (packed small numbers read as addresses) could exist in other tables;
a word whose target is not a real label, in the middle of a numeric record, is the sign.

## Exact versus native renderer

E4 between the exact and the native renderer (4 players, same pads) once parted at field 1560
of the race (a run with screenshots every 600 race ticks), and another run matched through
field 3540. Not chased: every online PC uses the same renderer, which the game enforces
(`DHD_RENDER=native` or online play stops, `port/hle/online.c`); interpolation is free per PC (E4
native+interp 120 Hz against plain native matched through field 2760).

## The USA disc

USA disc (SCUS-97177, redump disc 30393, 2,530,869,248 bytes): MD5
`3654a6f4438c899be4a1f5236cfdcd74`, SHA-1 `a2739ce3b632c1513d2a96cf3982b5effcca5a3c`, SHA-256
`0fe2b77713d3440cc3030741ea0b18f49e3feaa09671a30a421e0d4888a7eb86`.

Since 2026-10-28 online compares its game files hash instead, so a good dump that differs only
outside the files is taken too: `c8c17bcae383daf9f2feadc7f6d7476c43d659ab8d66127a299b01311f2a3476`
(finding 098, "The game files check").

## Sources

- Finding 040 (recipe, E1-E6), 042 (client), 039 (server), 034 (uninitialised stack structs).
- Intel SDM vol. 3, ch. 18 (debug registers DR0-DR7, DR6 status) for the watchpoint.
- redump.org disc 30393 and HFS DB for the US disc hashes.
