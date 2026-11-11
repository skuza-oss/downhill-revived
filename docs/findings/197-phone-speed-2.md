# 197: the phone's vertex loops and palette path, Native VU off

Status: current

Branch `perf/phone-fable-2` from `main` 652a734b (release 0.7.15), started 2026-11-10. Merged into `main` 2026-11-11 (fast-forward to `cd064781`). This file is
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
  one pass, from the packet. Checked on the PC ("Change 1"); `check.py quick` passes.
- [x] 2026-11-10, `3168d56c`: the VU1 kernels' difference test builds for the phone, counts CPU
  cycles per kernel and takes special values ("Method"). Today's kernels on the Mi 11i: no
  difference from the interpreter in 6,000 runs of the race start, with and without special values.
- [x] 2026-11-10: the VU1 kernels' cycles on the phone's Cortex-A78, by kernel, and what the operand
  clamps cost there ("The VU1 kernels")
- [x] change 2, `5b6a4ab5`, `13756b83` and `9dc03d6a`: eleven VU1 kernels with their operands clean
  where they are written, not clamped where they are read ("Change 2"). Against the interpreter on
  the PC and on the phone: no difference. On the phone's Cortex-A78: the VU1 work of the race
  start's capture -10.8 %, mid-race -9.4 %. The two particle loops are left ("Decisions").
- [x] 2026-11-11: the head's checks at `d0e29c8e`: boot and race state traces equal to `main`'s
  (600 and 5,000 rows), the picture route identical at 2x and 4x, Native VU off and on,
  `check.py port` passes
- [x] change 3, `49ee5211`: a palette's digest found again by its CLUT words, CLUT loads a row at a
  time ("The palette path", "Change 3")
- [x] 2026-11-11 00:43 to 01:25: two timed sessions under the mirror (base, change 1, changes 1 and
  2); 01:25 the mirror's phone processes ended on the lead's word ("Surprises", 5)
- [x] 2026-11-11: a profile recorded on the lane's last code (`dhd-197.profdata`) and the release
  APK built with it
- [x] 2026-11-11: timed without the mirror: each change in turn, then the last build against the
  base at t612 2x (Native VU off and on) and at 4x ("Timed on the phone", "Result")
- [x] 2026-11-11: the new cost table ("The ranked cost table"); the phone put back as it was found

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

## Change 2: the kernels' operands clean where they are written (`5b6a4ab5`, `13756b83`, `9dc03d6a`)

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
write by its right-hand side and stops at a form it does not know; `13756b83`), then 1430 and 2DC8
by the same rules (`9dc03d6a`): eleven kernels. Not converted: the two particle loops (2E48, 0E10;
"Next").

**Two things the compiler needed** ("Surprises", 3): the file loaded and stored register by
register (in a loop with the clamp in it nothing was unrolled, and both arrays stayed in memory),
and only the registers a loop writes stored back (a raw copy kept only to be written back
unchanged cost a register or a stack slot).

Cycles of the native runs on the Mi 11i's Cortex-A78 (`vu1_seq_diff` on the phone, two rounds
interleaved with the base, each run of the capture repeated 5 or 4 times; the rounds within 1 %):

| capture | key (program / entry / VI13) | share of the base | base | change 2 | |
|---|---|---:|---:|---:|---:|
| race start | the whole capture (6,000 runs) | | 37.59 M | 33.52 M | **-10.8 %** |
| race start | mesh loop, 0078 / 1040 / 0011 | 23.1 % | 7,980 | 6,878 | -13.8 % |
| race start | mesh loop, 0078 / 1CB0 / 0011 | 7.2 % | 11,908 | 10,816 | -9.2 % |
| race start | program 0040's set-up, 0040 / 0040 (translated blocks, no kernel) | 7.0 % | 9,238 | 8,985 | -2.7 % |
| race start | 1638 / 1040 / 02C9 | 6.3 % | 14,258 | 13,432 | -5.8 % |
| race start | skinning, 0020 / 0020 | 6.1 % | 28,640 | 18,840 | -34.2 % |
| race start | 20C, 0040 / 1040 / 020C | 4.1 % | 5,780 | 5,205 | -9.9 % |
| race start | 260, 0020 / 1040 / 0260 | 4.0 % | 4,412 | 4,034 | -8.6 % |
| race start | 1430 / 1040 / 0288 | 4.0 % | 7,524 | 6,210 | -17.5 % |
| race start | 17E0 / 1040 / 02FE | 2.3 % | 5,412 | 4,942 | -8.7 % |
| mid-race | the whole capture (6,000 runs) | | 40.18 M | 36.39 M | **-9.4 %** |
| mid-race | mesh loop, 0078 / 1040 / 0011 | 35.6 % | 8,886 | 7,835 | -11.8 % |
| mid-race | mesh loop, 0078 / 1CB0 / 0011 | 10.4 % | 13,190 | 12,056 | -8.6 % |
| mid-race | 1638 / 1040 / 02C9 | 6.4 % | 10,982 | 10,208 | -7.0 % |
| mid-race | 08B0, 07F0 / 1040 / 0100 | 3.8 % | 3,432 | 3,092 | -9.9 % |
| mid-race | 2DC8 / 1040 / 05BD | 3.7 % | 12,741 | 11,286 | -11.4 % |
| mid-race | particles, 2E48 / 2E48 (not converted) | 3.5 % | 46,446 | 46,376 | -0.1 % |
| mid-race | skinning, 0020 / 0020 | 2.6 % | 26,520 | 17,450 | -34.2 % |
| mid-race | particles, 0E10 / 0E10 (not converted) | 1.9 % | 26,016 | 25,990 | -0.1 % |

The mesh loop on arm64: 372 -> 308 instructions a vertex (284 with no clamp at all).

Checked against the interpreter (20,000 runs: the four captures; again for the eleven kernels at
`9dc03d6a`):

- no difference on x64 and on the phone (its own FPCR and NEON paths), plain, with the sticky
  flags cycled (PC), and with special values (seeds 1, 2 and 6 on the PC, 1 and 2 on the phone;
  the mesh loop alone before them: 1 to 5; a perturbed run the VU gives up on, at most five of
  6,000, is counted and not compared);
- in the plain-IEEE build the browser uses (`PS2_VU_IEEE`, built for the PC): no difference, plain
  and with special values (seed 4);
- `--clean-check`: no value written as clean differs from its clamp, plain and with special
  values;
- negative controls on the mesh loop: a loaded matrix row written as clean, or the file loaded
  without its clamps, fails the special-values runs (26 to 197 runs differ); vf06 back from its
  slot or a MIN result written as clean changes no output (the lanes are overwritten before they
  are read) and fails `--clean-check`;
- the driver's own negative control (`vu1_seq_diff.py --negative`, which breaks one MADD and must
  fail): after the conversion it broke a helper no kernel calls any more and found 0 runs
  differing, so it no longer showed that the comparison can fail. It now flips a bit of
  `native_madd_c`'s result, the MADD every converted kernel uses: 4,455 of the race start's 6,000
  runs differ and 4,564 of mid-race's (`6c83e696`; the unused helper is gone);
- the unit tests (`test_vu1`, `test_vif`): 0 failures.

Flush-to-zero was checked directly on both machines (a scratch program under the VU's mode: 1.5
FLT_MIN - FLT_MIN, FLT_MIN * 0.5, 1 / 2^127): an exact tiny result is flushed to zero on the x64 PC
(a Ryzen) and on the phone alike, so a divide by a normal number never leaves a denormal.

## The palette path

Finding 195's profile has it at 1.6 to 2.1 ms a field on the GS thread: `load_clut` 34 %,
`gs_palette` 29 %, `native_mix` 15 %. Counted on the PC over finding 156's route (temporary
counters, a line a field):

| a field | title | shop | race start | mid-race |
|---|---:|---:|---:|---:|
| `load_clut` calls | 343 | 185 | 587 | 548 |
| ... the same words as the last load (its memo) | 282 | 128 | 204 | 178 |
| ... loads of 16 entries | 12 | 15 | 145 | 155 |
| ... loads of 256 entries | 50 | 39 | 196 | 190 |
| ... 16-bit halves of entries changed | 23,700 | 16,400 | 89,800 | 84,600 |
| `native_palette` calls (a textured draw state with an indexed texture) | 66 | 62 | 392 | 380 |
| ... the first 16 entries converted again | 59 | 50 | 335 | 327 |
| ... the other 240 converted | 49 | 36 | 195 | 178 |
| ... of those lookups, found in the last 8 / 64 / 256 palettes | 35 / 59 / 59 | 21 / 50 / 50 | 216 / 252 / 335 | 239 / 262 / 327 |

- **Every CLUT is CT32, CSM1.** A load of 256 entries changes nearly all of them (the game
  alternates textures, each with its own CLUT), so the loads themselves are needed.
- **The one-palette memo of finding 148 never holds**: 335 of 392 calls a field convert, 195 of
  them all 256 entries (`gs_palette` and a `native_mix` for each: 52,000 a field).
- **No palette is new**: a race field goes round some 200 palettes and comes back to every one the
  next field. A store of the last 256 finds all 335; of the last 64, 252.

## Change 3: a palette's digest found again by its CLUT words (`49ee5211`)

- `native_texture` needs only a digest of the converted entries (for the texture's key); the
  entries themselves are read when a texture is decoded, which is rare. `native_palette_digest`
  keeps the digest beside the CLUT words the palette reads: 512 slots, found by a tag of the words,
  CPSM and TEXA (eight probes), then compared word for word. The converted entries follow from
  those words, CPSM and TEXA, so the digest is the one `native_palette` gives; a palette not met
  before (or pushed out) is converted by it as before. Over the route: 1,871,465 lookups, 264
  conversions (before: one for each lookup).
- `load_clut`'s CSM1 CT32 loads go a row of 16 entries at a time, read where the page's words lie
  (`clut_row32`: eight 8-byte copies put a row's words in entry order, then one loop splits them
  into the low and high halves `g->clut` keeps and counts the halves that change): the same
  entries and the same count. A run of words that wraps at the end of memory is copied first.

Checked on the PC against the base:

- the frame hash over the route at 2x: all 9,780 fields equal (vertices, indices, commands and
  each draw's texture key, which holds the digest);
- the picture route at 2x, Native VU off: the eleven fields identical;
- `test_gs`, new `clut_loads`: 3,777 random loads (16 and 256 entries, CT32 and CT16, every CSA and
  CLD, 141 past the end of memory, a load repeated with and without new words) against the
  entry-by-entry rule: the entries, `clut_writes` and `state_version` equal after every load. The
  test passes on the old `load_clut` too, and five deliberate errors (a row's words exchanged,
  the rows not reordered, rows below CSA loaded, the high halves not counted, no wrap) each fail
  it;
- `check.py quick` passes. The core is unchanged (host code only).

## Timed on the phone

Finding 195's method: the Mi 11i with the screen on and every other app closed, finding 156's
route (fields 300 to 14000, uncapped), fields a second and each thread's CPU work by scene from
the game's timeline; the builds of one comparison interleaved in one session, the phone's lock,
the clean-up and the throttle's restore around each. Release-signed APKs over the installed
release (`adb install -r`).

### Each change in turn (finding 195's profile on every build)

t612 2x, Native VU off, no mirror (2026-11-11 01:31 to 01:51, one run each: base, change 1,
changes 1 and 2, changes 1 to 3). Fields a second:

| scene | base | change 1 | 1 and 2 | 1, 2 and 3 | the last against the base |
|---|---:|---:|---:|---:|---:|
| race start | 62.5 | 60.8 | 66.4 | 65.9 | +5.5 % |
| mid-race | 77.2 | 75.4 | 81.1 | 80.7 | +4.5 % |
| the race | 72.1 | 70.6 | 76.4 | 75.7 | +4.9 % |
| the route | 68.1 | 67.0 | 72.9 | 72.5 | +6.5 % |

The threads' work, ms a field:

| scene | thread | base | change 1 | 1 and 2 | 1, 2 and 3 |
|---|---|---:|---:|---:|---:|
| race start | VU | 13.67 | 14.22 | 12.74 | 12.88 |
| race start | GS | 14.11 | 12.11 | 11.57 | 10.76 |
| race start | game | 10.75 | 10.89 | 10.64 | 10.59 |
| race start | render | 10.11 | 10.13 | 10.00 | 10.24 |
| mid-race | VU | 11.10 | 11.48 | 10.49 | 10.59 |
| mid-race | GS | 11.24 | 9.60 | 9.26 | 8.63 |

Each change's measured gain:

- **Change 1 (the strips): the GS thread -2.0 ms a field at the race start (-14 %), -1.6 mid-race.**
  No fields a second from it: the VU thread limits, and it got slower ("Surprises", 6).
- **Change 2 (the kernels): the VU thread -1.5 ms a field at the race start (-10 %), -1.0
  mid-race; fields a second +9 % at the race start and +7.5 % mid-race** against the build before
  it, +6 % and +5 % against the base. In the harness: -10.8 % and -9.4 % of the kernels' cycles.
- **Change 3 (the palettes): the GS thread -0.8 ms a field at the race start (-7 %), -0.6
  mid-race.** No fields a second (-0.5 to -0.8 %, within the runs' spread).

The same builds under the mirror, an hour earlier (base three runs, the others two): the base
slower by 11 %, the ratios the same.

| scene | base | change 1 | 1 and 2 | the last against the base |
|---|---:|---:|---:|---:|
| race start | 54.2 (54.2 / 54.3 / 54.0) | 53.2 (52.8 / 53.6) | 57.4 (57.0 / 57.7) | +5.9 % |
| mid-race | 66.3 (66.4 / 66.3 / 66.3) | 65.4 (65.3 / 65.6) | 69.4 (69.6 / 69.3) | +4.7 % |
| the route | 60.7 (60.2 / 61.1 / 60.8) | 59.9 (59.4 / 60.4) | 64.1 (64.6 / 63.6) | +5.7 % |

### The last build against the base

The lane's last code (`49ee5211`) with a profile recorded on it against `main` 652a734b with
finding 195's (each APK as a release would be built). The profile: the profile-collecting APK,
two unthrottled runs of the route with sound, Native VU off and on (99.9 and 111.4 fields/s
instrumented), merged 2 : 1; 62,271 functions; `dhd-197.profdata`, kept outside the repository
like finding 195's (`docs/BUILDING.md`).

No mirror; 2026-11-11 01:51 to 02:49; two runs each at 2x, interleaved (off: base, last, base,
last; on: last, base, last, base), one each at 4x. Fields a second:

| stand-in | Native VU | scene | base | the last build | gain |
|---|---|---|---:|---:|---:|
| t612 2x | off | race start | 61.5 (61.5 / 61.5) | 65.9 (66.4 / 65.3) | **+7.1 %** |
| t612 2x | off | mid-race | 77.0 (77.0 / 77.0) | 80.2 (80.4 / 79.9) | +4.1 % |
| t612 2x | off | the race | 71.4 (70.9 / 71.9) | 75.5 (75.8 / 75.2) | +5.7 % |
| t612 2x | off | the route | 67.7 (67.6 / 67.8) | 72.4 (72.4 / 72.3) | **+6.8 %** |
| t612 2x | on | race start | 74.5 (74.7 / 74.2) | 75.0 (75.3 / 74.7) | +0.8 % |
| t612 2x | on | mid-race | 91.7 (91.7 / 91.7) | 91.5 (90.6 / 92.5) | -0.2 % |
| t612 2x | on | the race | 88.0 (88.0 / 88.0) | 87.9 (87.3 / 88.5) | -0.1 % |
| t612 2x | on | the route | 83.6 (83.6 / 83.5) | 84.4 (84.4 / 84.3) | +1.0 % |
| 4x | off | race start | 51.6 | 51.5 | -0.2 % |
| 4x | off | the route | 64.4 | 65.0 | +0.9 % |
| 4x | on | race start | 49.3 | 49.3 | -0.1 % |
| 4x | on | the route | 61.6 | 61.7 | +0.2 % |

The threads' work at t612 2x, ms a field:

| Native VU | scene | thread | base | the last build | |
|---|---|---|---:|---:|---:|
| off | race start | VU | 13.89 | 12.91 | -0.98 |
| off | race start | GS | 14.21 | 10.52 | -3.69 |
| off | race start | game | 10.89 | 10.77 | -0.13 |
| off | race start | render | 10.09 | 10.25 | +0.17 |
| off | mid-race | VU | 11.10 | 10.66 | -0.44 |
| off | mid-race | GS | 11.25 | 8.42 | -2.83 |
| on | race start | VU | 10.66 | 10.60 | -0.06 |
| on | race start | GS | 9.55 | 8.39 | -1.17 |
| on | mid-race | VU | 8.79 | 8.88 | +0.08 |
| on | mid-race | GS | 7.76 | 6.76 | -1.01 |

Read:

- **Off, the CPU-bound stand-in gains 7 % at the race start (61.5 to 65.9) and 6.8 % over the
  route.** The base's 61.5 is finding 195's 61.9 again.
- **The GS thread does a quarter less** (-3.7 ms a field at the race start, off) and now idles 5
  ms a field there; **the VU thread 7 % less** (-1.0 ms), and it alone limits: busy 14.6 of a
  15.2 ms field (the game thread 12.2, the render thread 9.2).
- **On, nothing moves** but the GS thread's work (-1.1 ms: the palettes, and the strips the
  translated batches do not replace). The kernels that still run with Native VU on are faster in
  the harness, and the VU thread's work is the same: "Surprises", 6 again.
- **At 4x the GPU limits**, as in finding 195: no change.
- The new profile itself is worth little here: the same code with finding 195's profile ran the
  race start at 65.9 and the route at 72.5 in the session before ("Surprises", 2).

## The ranked cost table (the branch with every change)

Finding 195's method on the last build (`49ee5211`, its own profile), t612 2x: each thread's
samples by stage (simpleperf at 1000 Hz for 6 s at the race start and mid-race, inlined frames
resolved) times the thread's CPU work in the same scene of the timing runs. Fields a second and
the threads' work, ms a field:

| scene | Native VU | fields/s | field | game | VU | GS | render | GPU a frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| race start | off | 65.9 | 15.18 | 10.77 | **12.91** | 10.52 | 10.25 | 7.03 |
| race start | on | 75.0 | 13.33 | **10.58** | **10.60** | 8.39 | 10.57 | 7.87 |
| mid-race | off | 80.2 | 12.47 | 9.24 | **10.66** | 8.42 | 8.61 | 5.65 |
| mid-race | on | 91.5 | 10.93 | **9.02** | **8.88** | 6.76 | 8.93 | 6.50 |

The costs, ms a field, largest first (in brackets: finding 195's table, the base):

| thread | what | race start, off | race start, on | mid-race, off | mid-race, on | most of it in |
|---|---|---:|---:|---:|---:|---|
| VU | VU1 native kernels | 6.10 (7.63) | 2.30 | 5.58 (6.52) | 2.86 | `native_geometry_loop_impl` 21 %, `native_geometry_260_impl` 8 %, the SIMD multiplies 5 % |
| GS | draw recording | 5.86 (6.15) | 2.62 | 4.32 (4.71) | 2.20 | off: `native_strip_set` 28 %, `native_strip_loop` 26 %, `native_record` 11 % |
| game | the game's own code (wasm2c), other than below | 3.81 | 3.85 | 3.44 | 3.48 | spread; the memory access helpers 8 % |
| game | VU0 (macro and micro mode) | 3.19 | 3.04 | 2.53 | 2.47 | its loads 15 %, `run_fast_program` 9 %, `f32x4_eq` 8 % |
| render | the Vulkan driver (Adreno) | 2.96 | 3.08 | 2.29 | 2.48 | no symbols |
| VU | Native VU: batches translated for the GPU | | 2.58 | | 1.65 | `mesh_build` 92 % |
| VU | VU1 translated blocks | 2.48 (2.36) | 1.60 | 1.57 (1.46) | 1.09 | `lower_cached` 10 %, `commit_upper_lazy_proven` 10 %, `aot_find` 9 % |
| render | wgpu-core (validation, tracking, replay) | 1.90 | 2.25 | 1.90 | 1.93 | the trackers 7 %, the storage 7 %, `Queue::submit` 6 % |
| VU | VIF unpack and dispatch | 1.87 (1.78) | 2.13 | 1.52 (1.47) | 1.63 | `stream_wl1` 25 %, `stream_form` 19 %, `vif_feed` 15 % |
| GS | GS packet decode and registers | 1.83 (4.69) | 1.34 | 1.91 (4.03) | 1.51 | `gs_feed` 26 %, `packed_strip_game` 23 % |
| game | the scene walk and its packets | 1.84 | 1.86 | 1.78 | 1.60 | `func_0021B8A8` 33 %, `func_002628A0` 14 % |
| GS | GS VRAM transfers | 0.68 | 1.55 | 0.56 | 1.12 | `write_ct32` 48 %, `gs_mark_pages` 19 %, `native_owner` 14 % |
| render helper | the Vulkan driver (Adreno) | 1.18 | 1.50 | 0.74 | 0.98 | no symbols |
| render helper | wgpu-core | 1.21 | 1.46 | 0.69 | 0.97 | `encode_render_pass` 24 % |
| render | memcpy, memset | 1.30 | 1.31 | 0.99 | 1.10 | `memcpy` 87 % |
| render | kernel | 1.15 | 0.98 | 0.97 | 0.97 | spin unlocks 14 % |
| GS | memcpy, memset, memcmp | 0.32 (0.16) | 1.13 | 0.30 (0.14) | 0.84 | off: `memcmp` 56 % (the palettes' words, the CLUT memo) |
| render | the allocator | 0.88 | 0.94 | 0.69 | 0.65 | `je_free` 25 % |
| GS | textures, CLUT, palettes | 0.85 (1.89) | 0.92 (2.10) | 0.75 (1.65) | 0.70 (1.62) | `clut_row32` 24 %, `native_palette_digest` 20 %, `native_palette_words` 16 % |
| render | recording the picture (`dhd_render`) | 0.88 | 0.83 | 0.69 | 0.77 | `Native::render` 45 % |
| VU | VU1 dispatch and interpreter | 0.80 (0.68) | 0.43 | 0.57 (0.55) | 0.35 | `run` 80 % |
| VU | kernel | 0.66 | 0.67 | 0.46 | 0.48 | `_raw_spin_unlock_irqrestore` 60 % |
| GS | kernel | 0.64 | 0.48 | 0.32 | 0.22 | `_raw_spin_unlock_irqrestore` 47 % |
| VU | memcpy, memset | 0.58 (0.66) | 0.55 | 0.58 (0.52) | 0.55 | `memcpy` 71 % |

Read, against finding 195's table (Native VU off, race start):

- **The GS thread's two vertex rows are 7.7 ms where they were 10.8** (-3.1): the packet decode
  1.8 from 4.7, the recording 5.9 from 6.2 (the fused loop's samples are in the recording's row:
  `native_strip_set` and `native_strip_loop`).
- **The palette path is 0.85 ms where it was 1.89** (0.7 to 0.9 in every column, from 1.6 to
  2.1), plus 0.16 of `memcmp` that is new: what is left is the CLUT loads themselves and the
  gathering and comparing of a palette's words.
- **The VU1 kernels are 6.1 ms where they were 7.6** (-1.5; 5.6 from 6.5 mid-race). The VU
  thread's work fell by less (12.9 from 13.8): its kernel time is 0.66 ms where the base's
  recording has 0.26 (5.1 % of the thread's samples against 1.9 %, `_raw_spin_unlock_irqrestore`
  3.1 % against 0.8 %: where a wake-up's cost lands), and the blocks, the unpack and the dispatch
  are 0.1 each above.
- Off, the three vertex rows of finding 195 (18.5 ms) are 13.8.

## What is left

With Native VU off the VU thread alone limits the CPU-bound stand-in now: at the race start it
is busy 14.6 of a 15.2 ms field while the GS thread idles 5 ms. So only VU thread work buys
fields a second there; ranked by what the measurements say:

1. **The VU thread waking the GS thread** ("Surprises", 6): 0.4 ms a field of kernel time came
   onto the VU thread as the GS thread got faster and began to sleep between packets, and the
   VU thread's work rose by as much in every comparison where only the GS thread changed. Spinning
   instead of sleeping is not the answer on this stand-in (finding 148 tried
   `DHD_FIFO_ADAPTIVE=0`: worse). Not tried: the producer waking later, once the packets
   published are worth a wake or a marker follows; the GS thread's 5 ms of idle time has room
   for it. Worth about 3 % at the race start if all of it comes back.
2. **The VU1 kernels' rest** (6.1 ms a field at the race start). The mesh loop is 308
   instructions a vertex, 67 of them NEON register moves (the program's own pipeline moves,
   doubled where `v` and `c` differ) and 29 stack loads and stores; with no clamp at all it is 284.
   Four in ten vertices only feed triangles the ADC bit skips, but the program computes them
   before it knows. The two particle loops are not converted (2E48 and 0E10: 3.5 % and 1.9 % of
   the mid-race kernels' cycles, a third of that at most from their clamps).
3. **What is not a kernel on that thread**: the translated blocks (2.5 ms; program 0040's set-up
   is 7 % of the race start's VU1 cycles in the harness, 9,000 cycles a run, with no kernel), the
   VIF unpack (1.9), the dispatch (0.8), the copies (0.6).
4. **Native VU on** stays the larger answer where the GPU does not limit: 75.0 against 65.9
   fields/s at the race start, 84.4 against 72.4 over the route. There the game thread and the VU
   thread limit together (10.6 ms each).
5. The GS thread's rest is headroom now, not speed, while the VU thread limits: the recording
   and the decode are 7.7 ms at the race start, the palette path 0.85.

## Decisions

- **The fused path is for the game's one register list only.** The route's 13.6 million strip
  calls have no other; the general list path stays for any other list and is not slowed.
- **The hook is a global beside `gs_mesh_hook`, not a member of `struct Gs`**: the core's state
  trace holds that struct's size, and a new member would change the core.
- **`gs.c` counts the ADC bits itself** (a pass of one load a vertex) instead of taking the counts
  from the backend's loop: the GS state then depends on `gs.c` alone, and the unit test compares it
  with the qword path whatever the hook does.
- **The mirror was never this lane's to stop on its own word** ("Surprises", 1 and 5). While it
  ran, what did not need the game was measured by the kernels' harness, in cycles. When the lead
  said its panel was closed and its server was still there, two sessions were timed under it
  (base and changes interleaved, the server's and the codec's CPU time recorded with every run):
  ratios only. The lead then allowed its three phone processes to be killed by PID between
  sessions (2026-11-11 01:25: the server's PID; its shell and its helper went with it; nothing on
  the PC was touched). Every number in "Result" is from the sessions after that.
- **Each build is timed with a profile recorded on its own code**: the base with finding 195's,
  the lane's last build with one recorded on it (`dhd-197.profdata`; "Surprises", 2 and 6). The
  changes one by one were timed with finding 195's profile on every build, which holds their new
  functions back: read those as each change's direction and the threads' work, not as its worth.
- **The palette digest is kept as it was, and found by comparing the CLUT words**, not replaced
  by a hash of the words: the texture keys stay the same numbers (so the frame hash compares
  them with the base's), and two palettes are the same one only when every word read is.
- **The particle loops (2E48, 0E10) stay as they are**: 3.5 % and 1.9 % of the mid-race kernels'
  cycles, a third of that at most from their clamps, and their sources need rules the tool does
  not have (NMSUB, the MR32 shuffles). Not worth a second profile recording.
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
   15 %, the compositor 31 %. Neither is this lane's to stop, so no timed run was made then; the
   app's dhd.ini was read (md5 41477ea7..., as the lead gave it) and kept as the lane's first
   backup.
2. **A function added after the profile was recorded lands outside the hot code.** The APK with
   change 1 has `native_strip` and `gs_feed` (changed, so its profile is refused) at 0x17e..., far
   from the profiled functions at 0x50...: both are compiled without counts. Change builds made
   with finding 195's profile are therefore handicapped against the base, and the lane's last
   build needs a profile recorded on its own code (`docs/BUILDING.md`). Measured at the end
   ("Timed on the phone"): the last code with its own profile has `gs_feed`, `load_clut` and
   `native_strip` back among the hot functions, and is 0.5 to 1 % ahead of the same code with
   finding 195's profile at the race start: small, because the loops this lane changed are
   straight-line code. (Finding 195's +4 % was from every static function being found at all.)

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
   device hub were still running, the forward still listed (a second mirror session's forward
   beside it, its server gone: that one had been closed). Idle while the screen is static (1.6 %
   of a core), encoding again once the game draws: over a 300 s run its server took 26 s of CPU,
   the hardware codec 45 s. What it costs, from the same base APK with and without it: the route
   60.7 against 68.1 fields/s (-11 %), the race start 54.2 against 62.5, every thread's work up 8
   to 11 % (the VU thread's 15.2 against 13.7 ms a field). The ratios between builds held: the
   build with changes 1 and 2 was +5.9 % at the race start under it and +6.2 % without. It ended
   at 01:25, by PID, on the lead's word.
6. **A faster GS thread alone made the game slower.** Change 1 takes 2.0 ms a field off the GS
   thread at the race start, and the build with only that change is 1.5 to 3 % slower than the
   base in every scene, in three rounds (two under the mirror, one without). The VU thread limits
   (busy 15.4 of a 16.0 ms field in the base), and its own work went up, 13.7 to 14.2 ms, with no
   line of its code changed; the GS thread's idle time went from 1.2 to 4.7 ms a field. Not
   proven. What fits: the GS thread now sleeps between packets, so the VU thread's publishes
   find it asleep and pay for the wake (`gs_publish`, `wake`), and a thread that sleeps gives its
   core up to others. The same, smaller, with change 3 (GS -0.8 ms, VU +0.1). The last build's
   profile agrees: the VU thread's kernel time is 5.1 % of its samples at the race start where
   the base's recording (finding 195's) has 1.9 %, 0.66 against 0.26 ms a field, most of it where
   a wake-up's cost lands ("The ranked cost table"). The gain of changes 1 and 3 is CPU time and
   GS headroom, not fields a second, while the VU thread limits.
7. **The driver's negative control had gone blind.** `vu1_seq_diff.py --negative` breaks one MADD
   to show that the comparison can fail. After the kernels were converted it broke a helper none
   of them calls, and reported 0 runs differing. Repointed at `native_madd_c` (`6c83e696`,
   "Change 2").

## Result

Branch `perf/phone-fable-2`, not merged. Three changes, all host code (the core is main's, core id
daa92ea5cda6b962): the game's strips decoded and recorded in one pass, eleven VU1 kernels with
their operands kept clean where they are written, and a palette's digest found again by its CLUT
words with the CLUT loaded a row at a time.

On the Mi 11i as the t612 stand-in, the last build with its own profile against `main` 652a734b
(two runs each at 2x, interleaved; one at 4x; no mirror), fields a second:

| stand-in | Native VU | scene | base | the last build | |
|---|---|---|---:|---:|---:|
| t612 2x | off | race start | 61.5 | 65.9 | +7.1 % |
| t612 2x | off | the route | 67.7 | 72.4 | +6.8 % |
| t612 2x | on | race start | 74.5 | 75.0 | +0.8 % |
| t612 2x | on | the route | 83.6 | 84.4 | +1.0 % |
| 4x | off | race start / the route | 51.6 / 64.4 | 51.5 / 65.0 | none (the GPU limits) |
| 4x | on | race start / the route | 49.3 / 61.6 | 49.3 / 61.7 | none |

- **Native VU off, the default: 61.5 to 65.9 fields/s at the race start**, the CPU-bound
  stand-in's slowest scene: 10 % above 60 where the base has 2.5 %.
- The GS thread works a quarter less (-3.7 ms a field at the race start), the VU thread 7 % less
  (-1.0 ms). The VU thread alone limits now; the GS thread idles 5 ms a field.
- By change: the kernels gave the fields a second (+9 % at the race start against the build
  before them); the strips and the palettes gave GS time only (-2.0 and -0.8 ms), and a faster GS
  thread cost the VU thread 0.4 to 0.55 ms ("Surprises", 6): the first thing to look at next
  ("What is left", 1).
- Checked on the last code: boot and race state traces equal to `main`'s (600 and 5,000 rows),
  the picture route identical at 2x and 4x with Native VU off and on, the frame hash of all 9,780
  fields of the route at 2x equal, the kernels against the interpreter on the PC and on the phone
  (plain and with special values), `check.py port` passes, the release APK builds.
- The release APK is built with the profile recorded on this code (`dhd-197.profdata`, kept
  beside finding 195's, outside the repository): `build_apk.py --pgo-use`, `docs/BUILDING.md`.
- The phone is as it was found: the 0.7.15 release APK reinstalled (the same file), the app's
  dhd.ini as it was (md5 equal), no file of the lane left, the throttle restored.
