# 197: the phone's vertex loops and palette path, Native VU off

Status: in progress

Branch `perf/phone-fable-2` from `main` 652a734b (release 0.7.15), started 2026-11-10. This file is
the lane's plan while it runs and its finding when it merges. Someone who has only this file and the
git log must be able to pick the lane up.

## Goal and acceptance

The owner's minimum target on Android is 60 fps at 2x on a Redmi 9 class phone (Helio G80,
Mali-G52) with the default settings, Native VU off. Finding 195 left the CPU-bound stand-in (the
Mi 11i throttled to the t612 profile at 2x) at 61.9 fields/s at the race start with Native VU off:
no margin. Its ranked cost table names what is left there: the VU and GS threads' loops over the
scene's vertices (`native_geometry_loop_impl`, `packed_strip`, `native_draw_strip` and
`native_set`: 18.5 of the 28 ms the two threads work at the race start) and the GS thread's
palette and CLUT path (1.6 to 2.1 ms).

The work: cut those, the vertex loops first, then the palette path; each gain proven by timed runs
on the phone (uncapped, the screen on, other apps closed, base / change / base interleaved in one
batch), on finding 195's stand-ins:

- "t612 2x": `port/tools/phone_throttle.py --apply t612 --big-max 1209600 --little-max 1497600`,
  `DHD_SCALE=2`: the CPU limits. The primary cell is Native VU off.
- "4x": `--apply t612` (the profile's own clocks), `DHD_SCALE=4`: the GPU limits.

Acceptance:

- Each change keeps the game's state: boot (600 fields) and race (5000) state traces equal to
  `main`'s in every row.
- The emulated VU stays exact, and the pictures identical to `main`'s (finding 195's picture route:
  `native_vu_pictures.py`, eleven fields, 2x and 4x, Native VU off and on).
- Each kept change has a measured gain on a named stand-in.
- `python tools/check.py port` passes; the release APK builds.

Out of scope: gameplay, Native VU as the default, the render thread and the GPU (finding 195's
"What is left" 2 to 5), anything findings 146-149, 172 and 195 tried and dropped, unless a new idea
differs from what they tried (said where it does).

## Progress

- [x] 2026-11-10: finding reserved (`08abe158`)
- [x] 2026-11-10: the base: `main` 652a734b built for Windows and as a release-signed APK (finding
  195's profile, line tables; core id daa92ea5cda6b962), its boot (600 fields) and race (5000)
  state traces, and finding 156's route at 2x with the exact probe: its eleven pictures and a hash
  of every recorded native frame (9,780 fields; a temporary patch, "Method")
- [x] 2026-11-10: what the route's PACKED strips are made of, counted on the PC ("The strips")
- [x] change 1, `3a71c56e`, `0f421b3e` and `31691c59`: the game's strips decoded and recorded in
  one pass, from the packet. Checked on the PC ("Change 1"); `check.py quick` passes. Not yet timed
  on the phone.
- [x] 2026-11-10, `3168d56c`: the VU1 kernels' difference test builds for the phone, counts CPU
  cycles per kernel and takes special values ("Method"). Today's kernels on the Mi 11i: no
  difference from the interpreter in 6,000 runs of the race start, with and without special values.
- [x] 2026-11-10: the VU1 kernels' cycles on the phone's Cortex-A78, by kernel, and what the operand
  clamps cost there ("The VU1 kernels")
- [x] change 2, `5b6a4ab5` and `13756b83`: nine VU1 kernels with their operands clean where they
  are written, not clamped where they are read ("Change 2"). Against the interpreter on the PC and
  on the phone: no difference. On the phone's Cortex-A78: the VU1 work of the race start's capture
  -10.3 %, mid-race -8.8 %.
- [ ] the phone's game runs: mirrored when the lane started and still at 2026-11-11 00:11
  ("Surprises", 1); no timed game run made yet. The APKs are built (`base`, `v5`).
- [ ] the base timed (t612 2x, Native VU off and on, the race start and mid-race)
- [ ] changes 1 and 2 timed in the game, base / change / base
- [ ] the kernels not yet converted: 1430, 2DC8 and the two particle loops (0E10, 2E48)
- [ ] the palette path

## Method

- **Frame hash** (temporary, not committed; finding 146's check): where a recorded field is handed
  to the render thread, a hash of its vertices, its indices and its commands (every field but the
  pointers; a texture by its key) goes to a file, one line a field. With the exact probe
  (`DHD_EXACT_PROBE=1`, the picture route's) every run records the same fields, so two builds are
  compared line by line: equal lines are byte-identical vertex and index arrays and the same draws.
- **Pictures**: `native_vu_pictures.py capture` / `compare --same` (finding 194), the route's eleven
  fields.
- **The GS unit test** (`port/gs/test_gs.c`, `strips()`): random strip tags through the fast paths
  against the qword-at-a-time path, the draws and the whole GS state compared.
- **The VU1 kernels against the interpreter, on the PC and on the phone** (`3168d56c`):
  `vu1_seq_diff.py` replays captured VU1 runs on the interpreter (the referee), the translated
  blocks and the blocks with the native kernels, and compares every register, the data memory and
  every kicked packet. New in this lane: `--target android-arm64` builds it for the phone (pushed
  to the phone's temp folder with a capture, run from `adb shell`: no app, no install, no
  throttle); with a repeat it counts the CPU cycles of each microprogram key (`perf_event_open`),
  which do not move with the core's clock, so builds are compared on a phone that is otherwise as
  it is (pinned to one A78 with `taskset`; two rounds agree within 1 %); `--specials SEED` puts
  exponent-255 and denormal bit patterns into lanes of each run's registers, ACC, Q and data
  memory first; `--clean-check` compares every value a kernel writes as clean with its clamp.
  Captures: `DHD_VU_SEQ` on finding 156's route at the race start (6,000 runs, fields 6405-6407),
  mid-race (6,000, 9285-9288), the title (4,000) and the shop (4,000). The route's clock runs 240
  fields a second (`DHD_FPS=240`), so `DHD_VU_SEQ_AT` is the field divided by 240.

## The strips

Every PACKED strip tag of finding 156's route (title, shop, the first race to field 9780), counted
by a temporary counter in `packed_strip` (13.6 million calls):

- **One register list: ST, RGBAQ, XYZF2** (three registers, REGS 0x412; the nibbles past NREG
  vary and mean nothing). Never FST, never flat-shaded; always into an empty strip window (the tag
  sets PRIM).
- **37 vertices a call** on average (80 % of the calls have 32 to 63).
- **Four in ten kicks draw nothing**: of 509 million vertices, 283 million made a drawn triangle
  and 199 million a triangle skipped by ADC (41 % of the kicks). The drawn triangles come in short
  runs, 5.0 on average (56 million runs; one or two triangles: 31 %), each begun by two ADC
  vertices.
- A recorded field holds 57,500 vertices and 40,900 triangles at the race start, 50,300 and 35,200
  mid-race, 69,500 and 49,200 in the title.

## Change 1: the game's strips decoded and recorded in one pass (`3a71c56e`, `0f421b3e`)

Before: `gs_feed`'s `packed_strip` decoded each vertex of a strip tag into a 40-byte `GsVertex` in
a list (with the kick's counters updated in memory for every vertex), and `native_draw_strip` read
the list back, converted every vertex and recorded the drawn triangles: two loops over every
vertex, and a conversion for vertices only skipped triangles have.

Now a tag in the game's layout that starts a strip goes to the front end as it lies in the packet
(`gs_strip_hook`, `port/gs/gs_strip.h`):

- `gs.c` (`packed_strip_game`) works out what the tag's loops leave without a list: the counters
  from the ADC bits (one load a vertex), XYZF2 and XYZF3 from the last vertex written without and
  with ADC, RGBAQ, ST, Q, the vertex register and the strip window from the last vertices.
- `native_strip` (the hook) records the triangles from the packet. A triangle right after a
  recorded one converts its third vertex where the frame keeps it and takes its other two indices
  from that triangle; one after a skipped triangle converts all three and goes through
  `native_triangle_indexed`'s steps. A vertex of skipped triangles only is never converted. The
  first drawn triangle still goes through `native_draw` (it may change the state), and what the
  game's strips never are (a field that is not indexed, flat shading, FST, a per-primitive
  diagnostic) goes through `native_draw` triangle by triangle.
- Any other register list, and a tag that continues a strip, keeps the vertex list
  (`packed_strip`, `native_draw_strip`) as before.

On arm64 a continuing vertex is about 55 instructions (decode, convert, box, three indices) where
the two loops had about 150. `0f421b3e` takes the loop twice, with the exact probe as a constant:
its `native_exact` call had put the loop's offsets, Z limit, fog table and index pointer on the
stack.

Checked on the PC against the base:

- The frame hash over finding 156's route at 2x: all 9,780 fields equal.
- The picture route at 2x, Native VU off: the eleven fields identical.
- The two loop instances against each other on every strip call of the route (a temporary
  comparison: the usual one run first into the frame, hashed and taken back): 12.9 million calls,
  272 million triangles, no difference.
- `test_gs`: 192 random tags, 60 of them read in place, the draws and the GS state equal to the
  qword-at-a-time path's; three deliberate errors in `packed_strip_game` (the draw count, the
  window's first vertex, Q) each fail it.
- The core is unchanged (core id daa92ea5cda6b962: host code only), so the headless state traces,
  which have no renderer and never take the hook, are the base's.

## The VU1 kernels

Finding 195's race-start profile again, by kernel (its recording, `t6-c2-off-perf-start`):
`native_geometry_loop_impl` is 18 % of the VU thread, `native_clip_impl` 7 %,
`native_geometry_260` 7 %, `native_skin_468` 5 %, `native_geometry_20c` 5 %,
`native_geometry_1430` 4 %. The same from the harness on the phone, in cycles on a Cortex-A78: the
race start's 6,000 runs (two to three fields) take 37.7 million cycles with the kernels, 95 million
through the blocks alone: about 15 million a field, 12 ms at the profile's 1.21 GHz, which is what
the game's timeline has for the VU thread less its VIF unpack.

- **The kernels clamp every FMAC operand** (`operand4`: exponent 255 becomes the largest finite
  magnitude; three NEON instructions). The mesh loop 0170 has 29 a vertex, 87 of its 372
  instructions; the skinning loop 77.
- **What they cost, measured**: a build with every kernel clamp switched off (not exact: 3 of
  6,000 runs differ) takes 18.1 % fewer cycles on the race start's capture and 17.1 % mid-race:
  the mesh loop -20 %, 20C -24 %, 260 -19 %, 1430 -28 %, the skinning loop -55 %, the two particle
  loops -34 % and -36 %. That is the ceiling of any change that only removes clamps.
- **The loop's cost on the A78 follows its instruction count, not its NEON count**: 372
  instructions with the clamps, 284 without, -20 % of the cycles; a version with 80 fewer NEON
  instructions and 70 more stack loads and stores (below) gained nothing.

## Change 2: the kernels' operands clean where they are written (`5b6a4ab5`, `13756b83`)

Most operands are results of earlier FMAC or ITOF operations, which are finite already (under the
VU's rounding an overflow gives the largest finite value, and flush-to-zero leaves no denormal:
the note at `native_result`, which every kernel already relied on for its results). A kernel now
keeps beside each register `v[r]` its clamped value `c[r]`, with the invariant `c[r] ==
operand4(v[r])`, kept where the register is written:

- an FMAC or ITOF result: `c[r]` is the result itself (`NATIVE_FULL`, `NATIVE_SET`);
- a raw value (the file at the kernel's entry and after the clipping routine, a load from data
  memory, MIN/MAX, FTOI): one `operand4` (`NATIVE_RAW`, `NATIVE_SETRAW`, `NATIVE_LOAD_FILE`);
- a move or a merge: the same move or merge of `c` (`NATIVE_MOVE`, `NATIVE_SETMOVE`,
  `NATIVE_SETLANE`).

Every FMAC operand reads `c[]` (`native_add_c` and its siblings keep `operand4`'s optimizer
barrier, without its clamp). The operations and their operand values are the ones the clamps at
each use gave: a clamp for each raw value instead of one for each use, and none for a copy nobody
reads (the compiler drops it). Q as an operand is the file's clamped once and a divide's result as
it is. In the mesh loop also: the divide's denominator is vf26's W as the last MADD wrote it (an
FMAC result; only the entry's needs `arithmetic4`); the result's XYZ and its tag word go out in
one 16-byte store (a word store right before the three-lane merge's 16-byte load of the same qword
missed store forwarding: 6 % of the loop's samples); I and the integer registers live in locals.

Converted: the mesh loop 0170 by hand (`5b6a4ab5`), then the skinning loop 0468 and the loops
1638, 17E0, 20C, 260, 08B0, 0798 and 3140 by one rule set (a scratch tool that classifies each
write by its right-hand side and stops at a form it does not know; `13756b83`).

**Two things the compiler needed** ("Surprises", 3): the file loaded and stored register by
register (in a loop with the clamp in it nothing was unrolled, and both arrays stayed in memory),
and only the registers a loop writes stored back (a raw copy kept only to be written back
unchanged cost a register or a stack slot).

Cycles of the native runs on the Mi 11i's Cortex-A78 (`vu1_seq_diff` on the phone, two rounds
interleaved with the base, each run of the capture repeated 5 or 4 times; the rounds within 1 %):

| capture | key (program / entry / VI13) | share of the base | base | change 2 | |
|---|---|---:|---:|---:|---:|
| race start | the whole capture (6,000 runs) | | 37.67 M | 33.78 M | **-10.3 %** |
| race start | mesh loop, 0078 / 1040 / 0011 | 23.1 % | 7,999 | 6,864 | -14.2 % |
| race start | mesh loop, 0078 / 1CB0 / 0011 | 7.2 % | 11,953 | 10,802 | -9.6 % |
| race start | skinning, 0020 / 0020 | 6.1 % | 28,677 | 18,818 | -34.4 % |
| race start | 1638 / 1040 / 02C9 | 6.3 % | 14,258 | 13,471 | -5.5 % |
| race start | 20C, 0040 / 1040 / 020C | 4.1 % | 5,788 | 5,193 | -10.3 % |
| race start | 260, 0020 / 1040 / 0260 | 4.0 % | 4,418 | 3,982 | -9.9 % |
| race start | 17E0 / 1040 / 02FE | 2.3 % | 5,458 | 4,896 | -10.3 % |
| mid-race | the whole capture (6,000 runs) | | 40.33 M | 36.80 M | **-8.8 %** |
| mid-race | mesh loop, 0078 / 1040 / 0011 | 35.6 % | 8,931 | 7,876 | -11.8 % |
| mid-race | mesh loop, 0078 / 1CB0 / 0011 | 10.4 % | 13,248 | 12,114 | -8.6 % |
| mid-race | 1638 / 1040 / 02C9 | 6.4 % | 11,022 | 10,296 | -6.6 % |
| mid-race | 08B0, 07F0 / 1040 / 0100 | 3.8 % | 3,450 | 3,114 | -9.7 % |
| mid-race | skinning, 0020 / 0020 | 2.6 % | 26,582 | 17,464 | -34.3 % |

The mesh loop on arm64: 372 -> 308 instructions a vertex (284 with no clamp at all).

Checked against the interpreter (20,000 runs: the four captures):

- no difference on x64 and on the phone (its own FPCR and NEON paths), plain, with the sticky
  flags cycled (PC), and with special values (the nine kernels: seeds 1, 2 and 6 on the PC, 1 and
  2 on the phone; the mesh loop alone before them: 1 to 5; a perturbed run the VU gives up on, at
  most five of 6,000, is counted and not compared);
- in the plain-IEEE build the browser uses (`PS2_VU_IEEE`, built for the PC): no difference, plain
  and with special values (seed 4);
- `--clean-check`: no value written as clean differs from its clamp, plain and with special
  values;
- negative controls on the mesh loop: a loaded matrix row written as clean, or the file loaded
  without its clamps, fails the special-values runs (26 to 197 runs differ); vf06 back from its
  slot or a MIN result written as clean changes no output (the lanes are overwritten before they
  are read) and fails `--clean-check`;
- the unit tests (`test_vu1`, `test_vif`): 0 failures.

Flush-to-zero was checked directly on both machines (a scratch program under the VU's mode: 1.5
FLT_MIN - FLT_MIN, FLT_MIN * 0.5, 1 / 2^127): an exact tiny result is flushed to zero on the x64 PC
(a Ryzen) and on the phone alike, so a divide by a normal number never leaves a denormal.

## Next

- **The game's timed runs**, when the mirror is gone ("Surprises", 1): the base, change 1 and
  change 2, interleaved, t612 2x, Native VU off and on.
- **The kernels not converted**: 1430 (4 % of the race start's cycles, -28 % with no clamp), 2DC8
  (3.7 % mid-race, -14 %), the particle loops 2E48 and 0E10 (3.5 % and 1.9 % mid-race, -36 % and
  -34 %). Their sources are laid out differently; the rule set applies once their prologues are
  matched.
- **The mesh loop's rest**: 308 instructions a vertex, 67 of them NEON register moves (the
  program's own pipeline moves, doubled where `v` and `c` differ) and 29 stack loads and stores.
- **What is not a kernel**: program 0040's set-up (7 % of the race start's cycles, through the
  translated blocks), the clipping routine (7 % of the VU thread in finding 195's profile), the
  per-run entry of `run` (5 %) and the VIF unpack (12 %).
- **The palette path** (finding 195: 1.6 to 2.1 ms of the GS thread): not started.

## Decisions

- **The fused path is for the game's one register list only.** The route's 13.6 million strip
  calls have no other; the general list path stays for any other list and is not slowed.
- **The hook is a global beside `gs_mesh_hook`, not a member of `struct Gs`**: the core's state
  trace holds that struct's size, and a new member would change the core.
- **`gs.c` counts the ADC bits itself** (a pass of one load a vertex) instead of taking the counts
  from the backend's loop: the GS state then depends on `gs.c` alone, and the unit test compares it
  with the qword path whatever the hook does.
- **No timed game run while the phone is mirrored** ("Surprises", 1): the mirror is not this
  lane's to stop, and finding 195 measured 8-13 % of disturbance from one. What did not need the
  game was measured by the kernels' harness instead, in cycles.
- **Clean copies kept at each write, not a proof for each operand.** A check of the entry
  registers with an unclamped loop behind it was the first design; it needs each kernel's list of
  operands read before they are written and a second loop for the rare unclean entry. Keeping
  `c[r] == operand4(v[r])` at every write needs only each write's kind (a result, a raw value, a
  copy), holds from the first vertex and after the clipping routine, and has no second path.
- **Raw is the default.** A write the rule set does not know as an FMAC or ITOF result is clamped;
  MAX of two clean values is taken as clean in one place (the skinning loop's 04F0, said there).
- **`native_arithmetic` stays on the divide's result** although flush-to-zero makes it the
  identity on both machines tried: the plain-IEEE build needs it, and it is integer work off the
  NEON pipes.
- **The kernels' cycles from a command-line harness on the phone, not from the game**: it needs
  neither the app, an install nor the throttle, is exact to 1 %, and tests the arm64 code against
  the interpreter at the same time.

## Surprises

1. **The phone was in use when the lane started** (2026-11-10 21:40). T3 Code's own Device panel
   was mirroring it: a scrcpy server on the phone, started from the PC at 19:58 by the app's device
   hub, right after the 0.7.15 release APK was installed (19:58:45). The game was running in the
   foreground, in a race at paced 60 since about 20:01 with no input since (the phone's last user
   activity was 100 minutes before; 202 `perf: race` lines of 30 s in its dhd.log), the battery at
   38-39 C. That is finding 195's mirroring trap (its "Surprises", 7), and here the encoder is busy
   all the time (the game draws): the game 238 % of a core, the hardware codec 25 %, the server
   15 %, the compositor 31 %. Neither is this lane's to stop, so no timed run was made; the app's
   dhd.ini was read (md5 41477ea7..., as the lead gave it) and kept as the lane's first backup.
2. **A function added after the profile was recorded lands outside the hot code.** The APK with
   change 1 has `native_strip` and `gs_feed` (changed, so its profile is refused) at 0x17e..., far
   from the profiled functions at 0x50...: both are compiled without counts. Change builds made
   with finding 195's profile are therefore handicapped against the base, and the lane's last
   build needs a profile recorded on its own code (`docs/BUILDING.md`).

3. **Fewer NEON instructions bought nothing until the compiler kept the registers.** The first
   form of the mesh loop had 80 fewer NEON instructions a vertex and the same 370 instructions in
   all: 94 of them were stack loads and stores (25 before). Two causes. The file was loaded in a
   32-step loop with the clamp in it, which the compiler did not unroll, so `v[]` and `c[]` stayed
   in memory for the whole kernel (written out register by register: 308 instructions, -14 % of
   the cycles). And every register was stored back at the end, so the raw copy of each register
   the loop only reads had to live beside its clean one. On the A78 the loop's cycles follow its
   instruction count (372, 284 with no clamp: -20 %), not its NEON count.
4. **An idle phone parks its big cores.** With the game closed, `taskset 10` failed ("Invalid
   argument"): `core_ctl` had cpu4 and cpu7 isolated. The harness runs pick an A78 (cpu4 to cpu6)
   that takes a task, again for each run.
5. **The mirror outlived its panel.** 2026-11-11 00:04, on the lead's word that the Device panel
   was closed: the game was no longer running, but the scrcpy server (and its helper) was still on
   the phone with both sockets connected, and on the PC the adb process carrying it and the app's
   device hub were still running, the forward still listed. Idle while the screen is static (1.6 %
   of a core); it would encode again once the game draws. Reported to the lead; the game's timed
   runs wait for it to go.

## Result
