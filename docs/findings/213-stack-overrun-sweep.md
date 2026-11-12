# 213: stack locals that the EE frame makes one object (the class of 094 and 207)

Status: in progress

Branch `fix/stack-overrun-sweep` from `main` b8c4cf88, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges. Someone who has only this file and the git log
must be able to pick the lane up.

## Goal and acceptance

The retail code sometimes writes (or reads) past a stack local into the bytes the EE frame puts
after it; in the port clang lays locals out as it likes, so the access lands elsewhere: a crash
when it runs off the fiber's stack slot (094, 207), silent corruption otherwise. Find the rest
before players do and make the class hard to bring back:

- a static inventory of every place a local's address escapes, with the local's size and the EE
  frame around it, narrowed to real overruns, with the counts at each step;
- a test-only build mode that catches such writes at run time, walked through as much of the game
  as the harness reaches;
- each real overrun fixed by a port patch (`src/` keeps matching);
- the static check in `check.py port`, the run-time mode in `docs/TESTING.md`;
- the seven state-trace recipes against the format-2 references, each difference explained.

## Progress

- [x] 2026-11-12: finding reserved (2b89ca3b)
- [x] 2026-11-12: `port/tools/stack_escapes.py`, its test and reviewed list, the step in `check.py port`
- [x] 2026-11-12: seven overruns fixed in `port/patches_stack.py` (54f65d94, 0de4b49c)
- [x] 2026-11-12: `DHD_CORE_REDZONE=1` (`port/tools/redzone.py`, `build_wasm.py`) and `redzone_walk.py` (bc908b48, 20364604)
- [x] 2026-11-12: the seven traces: boot equal, every race differs, explained by two fixes (Result)
- [x] 2026-11-12: the red-zone walk: recipes and OPTIONS, 0 hits (Result)

## The static inventory (`port/tools/stack_escapes.py`)

**The EE frames.** Every `src/` unit is compiled once more with the matching compiler (SN cc1, the
unit's own flags) and `-gstabs`: each local's stab gives its offset from the frame top (the
`.frame` size turns it into `$sp + N`) and its type, whose size the tool reads from the stabs type
table (structs, arrays, typedefs; `mode(TI)` typedefs, which stabs give a 64-bit base type, are
found in the unit's headers and sized 16). The 207 frame comes out as the finding says: `name`@0,
`sp50`@0x50, `sp60`@0x60 (16), `sp70`@0x70 (0x610). Cached in `build/stack_escapes` by the
unit's and its headers' content: about 90 s cold, 8 s warm.

**What each function does through its pointer arguments.** A forward dataflow over each
function's basic blocks (the cc1 output for C functions, the splat asm for the rest, delay slots
in order) tracks registers and `$sp` stack slots that hold `$sp + N`, `argument k + c` or a
constant; at a join the same object at two offsets becomes a walk. Each store and load through
`argument k + c` widens that argument's write or read range; a call passes the callee's range on
(to a fixed point over the call graph, 14 rounds; a recursion that moves the pointer each call is
marked open-ended). `memcpy`, `memmove`, `memset` (`func_00254204`) and `strncpy` take their
constant length; `sprintf` (`func_00254D68`), `vsprintf`, `strcpy`, `strcat` are string writers. A
function that always returns `argument k + c` (`func_00261718`: `a0 + 0x1C`) makes its result a
pointer into the caller's local too. Things that had to be right before the counts meant anything
(each found by a false candidate): `.set noreorder` with a tab, the EE's three-operand `mult`,
`lw $r, D_sym` (no base register) killing `$r`, `swl/sdl` writing below their offset, a VLA's
`$sp` move, and C++ mangled names (`func_00203590__FP6Handleiii`) being the same function.

**The escapes and the narrowing**, on `main` b8c4cf88 with this lane's tool:

| Step | Count |
|---|---|
| C functions the port compiles (asm-only ones keep their EE frame on the emulated stack) | 4539 |
| ... with a local whose address escapes | 901 (1574 locals) |
| escapes (a local's address to a callee, stored to memory, or returned to it + c) | 3429 |
| fits: the callee's whole range lies inside the local | 2749 |
| no named local at that offset (a compiler temporary) | 26 |
| unbounded: string writer 309, callee loops over it 123, callee stores the pointer 84, indexed 67, stored to memory 33 | 616 |
| read overrun candidates | 29 |
| write overrun candidates | 9 |
| `sprintf` into a local: format at least as long as the local / possibly longer | 1 / 6 |

Each write candidate, each format candidate and each read candidate was read in the code; the
verdicts are `port/tools/stack_escapes_reviewed.txt`, one line each. Real: all 9 write candidates
(8 functions), the format overrun (094) and one read (`func_001AE358`); the read-backs of the
`AnimationHandle` locals go with their writes. All fixed (below; 094's and `func_00242998`'s had
been fixed before). Not real: the 6 `sprintf` maybes (values bounded by `% 60`, a clamp, a loop
index), 22 reads (the
w lane of a `float[3]` loaded by `lqc2` and never used; NUL-terminated strings read by
`strcmp`/`strncmp`; the tool's widening of `sorted[j - 1]`).

The unbounded 616 are what the static tool cannot size: the run-time red zones are for them.

## The real overruns and their fixes

All in `port/patches_stack.py`, each with its EE frame in the comment; `src/` is unchanged.

| Function | Local (bytes) | What writes or reads past it | EE frame after it | In the port | Found |
|---|---|---|---|---|---|
| `func_0019E550` (OPTIONS > POSTERS) | `sp60` (16) | `func_00184940`: header + 0x4FC-byte menu | `sp70`, the menu | crash (207) | static; fixed by 207 |
| `func_0017A678` (career finish text) | `buf` (32) | `sprintf` "%d MORE CREDITS REQUIRED TO ADVANCE" | the frame above | crash (094) | static (format); fixed by 094 |
| `func_00155BA8` (from `func_001554C0`'s step) | `float angle` (4) | `func_00221888`'s `sqc2`, 16 bytes | padding to 0x60 | `angle` gets another lane, 12 bytes around it overwritten | static |
| `func_0017E440` (the attack side pick) | `float value` (4) | `func_0017E670` -> `func_00221888`'s `sqc2` | padding to 0x10 | the same: which side the rider picks reads a wrong lane | static |
| `func_001F8CA8`, `func_0013C6A8` | `int tmp[3]` (12) | `func_00203590` fills an `AnimationHandle` (16) | padding | 4 bytes past the array (frame padding in practice) | static |
| `func_00150C58` | `Handle12 handle` (12) | the same | padding to 0x20 | the same | static |
| `func_00242998` | `float distance[2]` (8) | `func_00242D08`'s `sqc2` | padding to 0x10 | fixed before (port/patches.py) | static |
| `func_001AE358` (rider camera angles) | `Vec c` (16) | `func_00221AB8` reads four rows from `&c` | `d`, `e`, `unused`: the matrix | the camera's three angle targets from a garbage matrix, every field of every race | static (read) |
| `func_00124B40` | `Vec first` (16) | `setRadius(&first)`: `func_00261718` returns `&first + 0x1C` | `second.w` | 0.25 written 12 bytes past `first`, `func_002362E8` reads its `pos[7]` from there | by reading a red-zone run's fault, then static |

The fixes declare the EE's object: the quadword (`float angle[4]` aligned 16), the 16-byte handle,
the matrix rows (`Vec rows[4]; Vec &c = rows[0], ...`), the pair (`Vec pair[2]`). The older
patches of the same class in `port/patches.py` (`00131630` ends, `00187438` the load task,
`00214BA0` `16(&low)`, `00167A00`, `00230DF8`, `00242998`) were found one by one by crashes and
trace differences; the tool lists the two of them that are escapes (`00242998`, and 094's
format) as fixed.

## Catching them at run time (`DHD_CORE_REDZONE=1`)

Chosen over the alternatives:
- AddressSanitizer: clang has it only for Emscripten among wasm targets; its shadow memory would
  need a quarter of the core's 4 GB address space committed beside guest memory, and the
  recompiled code (libc's `sprintf` included: 094's writer) is not instrumented, so 094 would not
  be seen.
- The PS2's own frame layout forced: makes the behaviour the EE's, but shows nothing by itself
  (only a differential run against the normal build, whose differences are then unexplained).
- Red zones in the source (chosen): works with wasm2c unchanged, catches a write by anyone
  (recompiled code and the HLE too), names the function and the local.

`build_wasm.py` with `DHD_CORE_REDZONE=1` (part of the core cache key; never set by any
players' build or tool besides `redzone_walk.py`) rewrites each game unit's final text with
`port/tools/redzone.py`: clang's AST (`-ast-dump=json`) gives every local whose address is taken
(an `&`, or an array that decays) and every reference to it (macro arguments included); the
function gets one struct with those locals in declaration order, each 16-aligned as the port's
locals are (`align_aggregate_allocas`), with 16 bytes of 0xCD before, between and after them, and
every reference names its member. A `cleanup` variable checks the red zones on every return path;
the frame's description is copied into a table at entry, so a write over it is reported too. A
hit is one dhd.log line per function and local:

    redzone: func_0019E550: bytes 0..15 of the 16 after sp60 (16 bytes) written

Left alone (and counted in `build/wasm/redzone.tsv`): locals with an initializer, static ones,
VLAs, unnamed types, and any local with a reference the AST does not give (a nested macro's
argument spelled in a `#define`, an unevaluated `__typeof__` in the port's asm macros); a unit
whose rewrite does not compile is built as in players' builds. Coverage of the last build:
1108 units, 1927 functions, 3930 locals; 2 units unchanged.

`port/tools/redzone_walk.py --build` makes that game into `build/redzone/` (and the players'
build again); `redzone_walk.py` runs the state-trace recipes and the OPTIONS walk on it and prints
each hit once.

## Decisions

- The static tool works on the retail source and the matching compiler's frames, not on the
  port's: that is where the EE layout is defined, and a newly matched function brings its frame
  with it. Fixed candidates therefore stay listed, as `fixed` with the unit whose patch fixes
  them; `--check` makes sure that unit still has port patches.
- The check fails on unreviewed write overruns and `sprintf` format candidates only; read
  candidates are listed (`--report`) but do not fail it: a read past a local is wrong but cannot
  corrupt, and most are `lqc2` of a `float[3]`.
- `check.py port` runs it (and its test) rather than `quick`: it needs the matching toolchain.
- The fixes keep the EE behaviour where it was defined (the first lane of the quadword, the rows of
  the matrix) and leave the unused bytes as they were (`unused`, the padding).

## Surprises

- The first red-zone game faulted in its own check (`dhd_rz_leave` reading a clobbered frame
  description in `func_00124B40`, race2, field 4479): that led to the `first`/`second` pair. The
  description now lives in a table.
- The registry first had 8192 frames of 64 locals: the core's data then no longer fit below
  `INITIAL_MEMORY` (wasm-ld "initial memory too small"); 1024 frames of 32 locals fit.
- Older patches already fixed four of this class by hand; `func_00242998`'s was found again by
  the tool with the same fix.

## Result

(Lane result, 2026-11-12; the lead fills in the merge.)

### Found and fixed

Nine overruns in nine functions (two fixed before this lane, 094 and `func_00242998`; 207's in
the base): the table above. Seven new, all found statically; one of them (`func_00124B40`) first
showed as a red-zone game's fault and was then found by the tool once it followed returned
pointers. None was found by the red-zone walk: the fixes were in the walked game.

What they did to players:
- `func_001AE358`: every race, every rider (the camera of each view): the race camera's three
  angle offsets (`CameraAngles.xA0..xA8`, clamped to +-0.262, +-2.269 and -1.134..1.483 rad)
  eased toward angles from a garbage matrix instead of the rider's velocity. Silent, visible.
- `func_00155BA8`: on a sideways slide past the limit (the velocity's angle in the contact frame)
  `func_00155E78` starts a rider reaction (state kind 2, or 7, its sound, a timed hold); `angle`
  read whichever lane of the 16-byte result the 4-aligned float sat in, so the reaction fired at
  other moments than on the PS2, and 12 bytes around `angle` were overwritten. Silent, changes the
  ride.
- `func_0017E440`: the side a rider picks (`x1C0` / `x1C4`) from the nearest target's direction:
  the same wrong lane and 12 overwritten bytes. Not on the recipes' routes (they never attack).
- `func_00124B40`: 0.25 written 12 bytes past `first`; `func_002362E8` read its factor from there.
  Not visible on the recipes (their traces are equal with and without the fix).
- The three `AnimationHandle` locals: 4 bytes past the array, in clang's frame padding in
  practice; harmless so far, wrong by reading.

### The state traces

Players' build of this branch at 20364604 (sha256 dae50dff...), each recipe against
`main-f0971070-ref-v2` (made with the same `state_trace_codeptrs.txt` and `state_trace_mask.txt`),
then the first field each column differs (the `regptrs` column aside: it holds raw values that
`state_diff.py` compares by identity):

| Recipe | state_diff.py | First field per column |
|---|---|---|
| boot | PASS, 600 of 600 | |
| race | 2047 of 5000 differ | ram 2954, picture 2968, VU0 3065; cpu, scratchpad, sound 3434 |
| race2 | 11054 of 14000 | ram 2947, picture 2953; nothing else |
| race3 | 10350 of 14000 | ram 3651, picture 3673; cpu and the rest 12783 |
| race4 | 10062 of 14000 | ram 3939, picture 3953, VU0 4069; cpu and the rest 5697 |
| race5 | 10248 of 14000 | ram 3753, picture 3759; nothing else |
| race6 | 9537 of 14000 | ram 4464, picture 4477, VU0 4727 |

Each race differs from its first racing field, boot is equal. Explained by two fixes, shown by
isolation builds (the same branch with fixes left out, `race` again):

- without the `func_001AE358` fix: `race` equal to the reference up to field 3433, different from
  3434 (1567 of 5000 rows);
- without it and without the `func_00155BA8` fix: `race` PASS, 5000 of 5000.

So the five other fixes change nothing on these routes, the camera fix changes RAM and the picture
from the race's first field, and the `func_00155BA8` fix changes the ride itself (cpu, rider 0's
struct) from the first moment the slide check decides differently (race field 3434, race3 12783,
race4 5697). The first word that differs (race, field 2954, `DHD_STATE_DUMP` of both builds) is
the same word in each of the ten riders: `D_002AF2C0 + n * 0x7BD0 + 0x4890`, `CameraAngles.xA0`:
0.05 in the reference (a full easing step toward a garbage target), about 0 with the fix. The
traces of the build with six fixes (all but `func_00124B40`) equal these, recipe by recipe.

New references are needed for later lanes: the game state of every race changes with this lane.

### The run-time walk

`redzone_walk.py` on the red-zone game of this branch (1108 units, 1927 functions, 3930 locals
instrumented; 2 units without): `boot`, `race`, `race2` to `race6`, `split`, `memcard` (all ran
to their last field) and `options_walk.py`'s four scenarios (Unlock All on and off, a fresh card
and one that owns POSTERS and PICKUP EDITOR; every OPTIONS row opened and closed): 14 logs,
**0 hits**, no fault. The red zones work: `test_stack_escapes.py` builds the rewrite and its runtime
as a native program and gets exactly one hit for a 20-byte write into an `int[4]`; and the first
red-zone game of the unfixed branch faulted in its own check in `func_00124B40` on race2 (the
description in the frame, since moved to a table).

Not walked (no harness route yet): the Bike Shop pages, rider and bike select beyond the recipes'
defaults, career, mosh bowl, super jump and time trial starts and finishes, replays, the pickup
editor's pages, the cheats page, credits and movies, the online pages. The red-zone game and
`redzone_walk.py --recipes ... --env DHD_NAV=...` are ready for them.

### The check and its cost

`check.py port` runs `test_stack_escapes.py` (a second) and `stack_escapes.py --check`: about
90 s on a fresh checkout (the matching compiler on every unit, 6 jobs), 8 s after (cached). It
fails on a write overrun or a `sprintf` format candidate not in the reviewed list. The red-zone
build is not in any check (about 15 minutes for the core, then the host); its walk about 40.

### Not reviewed yet, in order

1. The 616 unbounded escapes, the string writers first (309; 094's kind): `sprintf` with a
   non-literal format or `%s`, `strcpy`/`strcat` into locals of 16 to 64 bytes (the list:
   `stack_escapes.py --json`, verdict `unbounded`, flag `string`). Then the 123 locals a callee
   fills in a loop (`walk-w`).
2. A function's own accesses past its local (an index, an asm offset such as `00214BA0`'s
   `16(&low)`): not escapes, so not in the static tool; the red zones see them.
3. The 26 escapes of compiler temporaries (no named local at that offset).
4. The screens and modes above, on the red-zone game.
