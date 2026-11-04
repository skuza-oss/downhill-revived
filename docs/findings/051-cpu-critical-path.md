# Round 34: where a race field's CPU time goes, and the first fixes

Status: current
Checked: 2026-11-04

Round 34. Goal: more game fields per second and lower worst-case fields on the CPU
(Ryzen 5 7600, RTX 3070), game state exact. All numbers are `DHD_RENDER=native`, uncapped frame
clock, the bench race (`port/tools/bench_race.py`), fields 3000-9100, on an idle PC with the game
window focused, one game at a time (`build/port/r30/benchlock.sh`).

## Measuring tools (new this round, all opt-in)

- `DHD_TIMELINE=1|path.csv` (port/hle/graphics_timeline.h): per field, how long the game (EE)
  thread waited for the VU worker and for the GS worker, how long the VU worker sat idle (no
  packet) or blocked on a full GS ring, and how long the GS worker sat idle. QPC ticks around the
  existing waits, nothing else. The bench prints `bench timeline:` lines (mean and median per
  column, busy ms per thread, EE wait by call site) and the CSV has one row per field. Busy
  times are wall minus waits, so they include the spinning part of a wait (the queues spin 4096
  pauses before sleeping) only where the wait is not timed.
- Profiler samples carry the virtual field (`DHP4`); `port/tools/profile.py --fields a-b` keeps
  one window, `--callers REGEX` names the first caller outside the runtime, `--addresses REGEX`
  lists IPs as symbol+offset.
- `DHD_VU0_STATS=N`: interpreted VU0 microprograms by entry (calls, TSC ticks).
- `PS2_VU_COVERAGE` (test builds): VU1 pairs run in AOT blocks against the interpreter per pc,
  printed by `bench_vu1_programs`.

## The profile (main at 66838753, one run of each kind)

Per field, ms (timeline, means):

| fields | fields/s | EE busy | EE waits VU | EE waits GS | VU idle | VU busy | GS busy |
|---|---|---|---|---|---|---|---|
| 3000-3500 | 78.0 | 5.72 | 7.00 | 0.11 | 2.12 | 10.71 | 5.94 |
| 6000-7000 | 94.6 | 5.09 | 4.30 | 1.18 | 2.45 | 8.12 | 6.71 |
| 8600-9100 | 95.1 | 4.57 | 5.92 | 0.03 | 1.22 | 9.30 | 4.57 |

The GS ring was never full. EE waits are at `sceGsSyncPath` (frame end, 5 calls a field, 4.1 ms)
and at the readback guard before flare Z (one field in eight, about 6 ms each).

How a field is built: the game updates (physics, collision: VU0 macro and micro code), then
submits the frame's packets, then waits at `sceGsSyncPath` until the VU and GS workers are done.
So the VU worker idles while the EE updates and the EE idles while the VU finishes:

    wall = VU idle (EE update before the first packet) + VU busy  (+ a GS tail at 6000-7000)

Every window fits that within 0.1 ms. The critical path is therefore (1) the VU worker's busy
time, 8-11 ms, and (2) whatever the EE does before its first packet, 1.2-2.5 ms. EE work after the
first packet overlaps the VU and is off the path; the GS worker is near-critical only in the
middle window (busy median 8.1 of 10.6 ms).

### (1) VU worker, 8.1-10.7 ms a field

Sampling (`DHD_PROFILE_THREAD=vu`, share of the thread's samples, waits excluded from the
right column):

| | 3000-3500 | 6000-7000 | 8600-9100 |
|---|---|---|---|
| `ps2_vu1_run` (dispatch + the native geometry kernels inlined) | 23.6% | 20.1% | 19.2% |
| AOT blocks (`aot_block_*`, top: 74, 568, 173, 555) | ~28% | ~25% | ~30% |
| `vif_feed` (UNPACK, inlined) | 15.3% | 14.9% | 16.3% |
| `native_clip_run` | 4.7% | 6.7% | 7.1% |
| `retire_flags` | 3.6% | 3.0% | 3.9% |
| `graphics_gif_feed`, memmove | 1.9% | 1.9% | 2.2% |
| waiting (driver, `consume` spin) | 14.6% | 22.0% | 13.0% |

VU1 time by microprogram (`DHD_VU1_PROFILE_FIELDS=3000-9090/100`, ms a field): 6.3-8.7 in all,
about 2,000 runs a field, spread wide. Largest: 0078/1040 1.83 (start) / 1.09 (mid); the
skinning MSCAL 0020 plus 0010 1.50 / 0.68 / 0.42 at 60 us a run; overlay 1 2E48 0.48 / 0.40;
2DC8/1040 1.31 and 17E0/1040 0.93 late in the race. With the VU1 captures in
build/port/vu-captures every pair runs in an AOT block (coverage 100%), so `ps2_vu1_run`'s own
samples are the native kernels and the dispatch, not interpreted pairs.

UNPACK forms a race sends (`DHD_VIF_STATS`, to field 6600): about 10,000 UNPACKs and 170,000
vectors a field. S-8 masked and unmasked with CL 3 WL 1, V2-16 masked CL 3 WL 1, V4-5, V4-8,
V3-32 and V3-8 (CL 4 WL 4) and V4-32 make up nearly all; mode 0 throughout. 75% of the vectors
went through the general stream path (masked, or fields narrower than a word).

### (2) EE update, 1.2-2.5 ms a field before the first packet

Game thread samples (6000-7000): `graphics_wait` 42% (spin), driver waits 10%,
`ps2_vu0_run` (interpreted VU0 microprograms from `vcallms`) 15.3%, the inline-asm runtime
(`run`, `ps2_asm`, `lookup`, `ps2_vu_madd`, `ps2_exec`) 14%, then recompiled game code, the
sound mixer 1.2%. At start the VU0 interpreter is 21%. The sampled stacks point at the collision code
(func_00213C08 ray tests and the vector helpers around it) for much of the inline-asm time;
the stack scan is heuristic (possible return addresses, no unwinding).

### GS worker

At 6000-7000 the host-to-local upload path (`image_data`, `gs_write_pixel`,
`gs_pixel_address`) was 24% of the GS thread's samples, with `native_draw` 13%, `gs_feed` 11%,
`gs_write` 7%.

### Ranked by expected gain on the critical path

1. VU worker: VIF UNPACK, about 1.3-1.5 ms a field (fixed below).
2. VU worker: the skinning program (MSCAL 0020/0010, 0.4-1.5 ms) and 0078/1040's setup and
   tail (finding 046), as native kernels.
3. EE: VU0 microprograms in the interpreter (about 1.4 ms of EE a field) and the inline-asm
   interpreter (about 1.3 ms); on the path only for the part before the first packet (the
   VU0 result below shows that part is small).
4. GS worker in the middle window: uploads (fixed below).
5. Readback guard: about 0.8 ms a field on average, 6 ms on the fields it hits (finding 048).

## Changes

1. **VIF UNPACK specialised per format** (port/hle/vif.c, e365de79): the stream path runs with
   the format and mode as constants for the forms above (one instantiation each), position,
   cycle and row registers in locals rather than the `Vif` struct, which data-memory stores
   could alias. A field starts at a multiple of its width, so it never straddles two words and
   the two-word read is gone. The whole-word fast path is subsumed. A local bench of the race's
   mix (build/port/r34/vifbench, not committed): 1.27 -> 0.65 ms a field.
2. **GS host-to-local uploads whole pixels** (port/gs/memory.c, gs.c, b71fdb77): one call per
   run of pixels with the format as a constant (`gs_write_pixels`), instead of bytes through a
   bit accumulator and a generic address per pixel; the idea of a per-format writer comes from
   PCSX2's `GSLocalMemory` WriteImage routines (no code taken).
3. **VU0 microprograms through translated blocks** (port/tools/vu1_aot.py, vu0_interp.c,
   cf3e621f), opt-in with `DHD_VU0_AOT=1`: the same generator emits `aot0_*` blocks for the
   VU0 package; the code words are compared on every dispatch, since EE stores can change VU0
   code.

4. **Native kernel for the skinning loop 0468-0570** (port/runtime/vu1_native_extra.h
   `native_skin_468`, kind 128, 20a4fc6a): MSCAL 0010 and 0020 branch to the setup at 03C8
   and then a 34-pair-a-vertex loop that blends a vertex from three bone matrices by weight and
   transforms and lights its normal. The loop reads no flags, Q, P or clip, and the VU stalls a
   read until its producer lands, so the pairs run in program order with the exact SSE helpers
   (each upper before its lower: 0510's MADDAz reads vf5 before the MOVE replaces it; no lower
   reads its own pair's upper result). Last MAC from 0570's MADDw, sticky flags as the other
   kernels. It declines before writing when a DIV or EFU result is pending; the run then goes
   pair by pair. Replay: 0020/0020 57.0 -> 18.8 us, 0010/0010 48.6 -> 18.6 us.

5. **Cheaper FMAC operand clamp, no result clamp** (port/runtime/vu0_interp.c `operand4`,
   vu1_native_geometry.h, b36aea01). Timing each kernel inside its runs (a local rdtsc copy of
   the runtime, build/port/r34/kt) showed the kernels take about 90% of their runs, so their
   setup and tail are not worth attacking; inside them, a test build without any clamp (inexact,
   discarded) ran the kernels about 35% faster. Every VU1 FMAC applied `arithmetic4` (11 SSE
   steps) to each operand and again to the result. Under ps2_vu1_run's MXCSR (toward zero, FTZ,
   DAZ, exceptions masked), DAZ already reads a denormal operand as the signed zero arithmetic4
   makes of it, so an operand only needs the exponent-255 clamp (`operand4`, 5 steps); the
   operation then has finite operands, so its result is finite and normal or zero (toward-zero
   overflow gives the largest finite magnitude, FTZ flushes) and arithmetic4 of it is the
   identity. Used by the interpreter's VU1 upper path (so every AOT block) and by the kernels'
   helpers; OPMULA/OPMSUB (scalar operands) keep their result clamp.

   The first version differed from the old build in 33 of 12,000 start-capture runs (vu1_seq_diff
   was clean, because it compares two paths of one build, both changed alike): in an AOT block
   the decoded words are constants, so `MULw vfN, vfM, vf0w` became `x * 1.0`, which the
   compiler folds to `x`, and a denormal operand passed through untouched (vf26.w = 0x00000F50
   instead of 0). An empty asm on the clamped operand hides it from the compiler, so every FMAC
   is computed by the SSE instruction under the VU MXCSR. That also rules out the compiler
   evaluating an all-constant FMAC (vf0 with a constant I) under its own rounding.

6. **No underflow/overflow MAC tests** (2ab40cf5): for the same reason no FMAC result is
   nonzero below FLT_MIN or has exponent 255, so the U and O MAC bits (and the kernels' sticky
   overflow) are always 0; `fmac_flags4` stops computing them. Every FMAC of every AOT block
   gets shorter.

## Exactness

- VIF: the `test_vif` fuzz (200,000 random UNPACK streams, every format, mask, mode, CL/WL,
  chunking, against the per-vector reference) passes, also queued (`DHD_MTVU=1`); a
  deliberately wrong S-8 value makes it fail (3,518 trials differ).
- GS uploads: `test_gs` upload fuzz, 3,000 transfers in all 13 formats, GIF IMAGE packets split
  at random and HWREG writes, two GS instances (new path and `image_reference=1`): VRAM, page
  writes, host-dirty pages, pixel counts and transfer state identical; a deliberate y offset
  fails it.
- VU0 blocks: `port/runtime/vu0_aot_diff.c` over the 104 local VU0 captures (13 entries) and 8
  seeded variants of each: 936 runs, the whole register file, both 4 KB memories and the VU1
  file identical; a deliberate VI1 flip in every block makes 288 of 312 differ.
- `port/tools/gate_native_renderer.py` on 62c2d7d9 (VIF + GS uploads): MATCH (EE dumps at
  GO+0/2/12/16/3600/4500 against the canonical reference and between arms, 1,147 Z readbacks).
  Again with `--env DHD_VU0_AOT=1` on the native arm: MATCH.
- Skinning kernel: `port/tools/vu1_seq_diff.py` over three race captures (a new crowded-start
  one at field 3478, build/port/r34/vu-seq-3480, and round 32's 6300 and 8750): 36,000 runs,
  197 of them through the new kernel, 0 differing (registers, 16 KB data memory, every GIF
  packet). Running 0510's MOVE before its MADDAz makes all 59 skinning runs of the start
  capture differ. The gate on 20a4fc6a: MATCH (all EE moments, 1,136 Z readbacks).
- Operand clamp: `test_vu1.c` runs 20,000 programs of ADD, SUB, MUL, MADD, MSUB, MULi, ADDw
  and MULw on raw operand bits biased to denormals, exponent 255, overflow and underflow,
  against the reference clamp-op-clamp under the VU MXCSR: 0 wrong. Without the exponent-255
  clamp it fails (and so does the existing NaN-operand check). A local 200M-vector fuzz of the
  same identity: 0 differing. Against the previous build (a per-run hash of registers, data
  memory and GIF packets, local seq-diff copy): all 36,000 runs of the three captures identical,
  with the kernels off and on. `test_vu1_pcsx2.py --aot` (pinned PCSX2 interpreter): 1,584
  cases, 0 failures. The gate on b36aea01: MATCH (all EE moments, 1,163 Z readbacks).
- MAC tests: against the previous build, all 36,000 capture runs identical; test_vu1 and
  test_vu1_pcsx2 --aot pass; the gate on 2ab40cf5: MATCH (all EE moments, 1,156 Z readbacks).

## Results

Bench: base = 66838753 (main before these changes, with the timeline), c1 = 62c2d7d9 (VIF +
GS uploads, plus the UI rounds' commits, which don't run in a race), c1aot = c1 with
`DHD_VU0_AOT=1`. 3 interleaved runs each, fields 3000-9100 in one race with `DHD_TIMELINE`, the
windows cut from the per-field CSV (build/port/r34/abwin.py); medians over runs, spread of
fields/s in brackets, all runs focused, 0 foreign dhd.exe, CPU 17-19% before each.

| fields | base fields/s | c1 fields/s | base p99 / worst 1% | c1 p99 / worst 1% |
|---|---|---|---|---|
| 3000-3500 | 79.2 (79.0-80.8) | 81.5 (80.8-83.3) | 18.70 / 19.78 | 18.23 / 18.66 |
| 6000-7000 | 95.3 (95.0-96.4) | 110.8 (110.7-112.2) | 15.65 / 16.31 | 13.32 / 14.42 |
| 8600-9100 | 95.0 (93.6-95.8) | 100.5 (100.0-102.6) | 14.46 / 15.42 | 13.75 / 14.57 |
| 3000-9100 | 104.7 (104.7-105.9) | 110.9 (110.8-112.6) | 15.80 / 17.36 | 14.96 / 15.99 |

Busy ms a field (means): VU 10.76 -> 10.50 (start), 8.12 -> 7.74 (mid), 9.37 -> 8.87 (late);
GS 6.57 -> 4.79 in the middle window, where it had been close to the VU's. The middle window
gains most because the upload fix removes the GS tail there.

The VU0 blocks are exact but not faster in the race: EE busy falls 0.2-0.35 ms a field, wall
time doesn't (3000-9100 110.6, spread 109.6-111.5), and the VU's idle time before the first
packet is unchanged (1.75 vs 1.74 ms at the start). The VU0 programs run in the part of the
EE's work that overlaps the VU, off the critical path. They stay opt-in.

Second bench, c1 against c2 = 20a4fc6a (c1 + the skinning kernel), 3 interleaved runs each
(one c2 run 99% focused, kept; the others 100%; 0 foreign dhd.exe; CPU 16-18% before each):

| fields | c1 fields/s | c2 fields/s | c1 p99 / worst 1% | c2 p99 / worst 1% | VU busy c1 -> c2 |
|---|---|---|---|---|---|
| 3000-3500 | 83.1 (83.0-83.5) | 88.0 (87.7-88.1) | 17.95 / 18.44 | 16.52 / 17.42 | 10.30 -> 9.37 |
| 6000-7000 | 112.9 (111.7-113.2) | 114.5 (114.3-114.6) | 13.38 / 13.92 | 13.24 / 13.53 | 7.58 -> 7.22 |
| 8600-9100 | 100.9 (96.0-101.0) | 105.1 (104.8-106.1) | 14.25 / 14.55 | 13.23 / 14.09 | 8.83 -> 8.32 |
| 3000-9100 | 112.0 (112.0-112.8) | 116.3 (115.9-116.6) | 14.80 / 15.96 | 14.33 / 15.33 | 7.80 -> 7.28 |

c1 measured 1-2% faster in this session than in the first (the PC's state between sessions);
chained, the round so far is about +11% at the start, +20% in the middle, +10% late, +11% over
the whole race.

So the critical path is the VU worker's busy time even more than estimated: VU idle is 0.8-1.7
ms a field and EE speed-ups after the first packet don't show. VU1 programs by size before the
skinning kernel (start window, ms a field): 0078/1040 1.83, the skinning MSCAL 0020 + 0010 1.50
(now about a third), 1430/1040 0.50, 0040/12E0 0.47, 0040/1040 0.45, 0040/0040 0.40, 1638/1040
0.39; late in the race 2DC8/1040 1.31 and 17E0/1040 0.93. Without a native kernel so far (AOT
only, replay 1.0x): 2DC8/1040 (DIV/Q, status-flag reads and a clipping branch in its loop),
overlay 1's 2E48 (about 96 us a run, FMAND reads and DIVs), 06F0/1040, 1430/1040.

Third bench, c2 against c3 = b36aea01 (c2 + the operand clamp), 3 interleaved runs each, all
100% focused, 0 foreign dhd.exe, CPU 16-18% before each, nothing else building or running:

| fields | c2 fields/s | c3 fields/s | c2 p99 / worst 1% | c3 p99 / worst 1% | VU busy c2 -> c3 |
|---|---|---|---|---|---|
| 3000-3500 | 88.3 (87.7-88.5) | 102.3 (101.9-102.9) | 17.19 / 17.60 | 14.99 / 16.02 | 9.33 -> 7.52 |
| 6000-7000 | 113.9 (113.7-115.9) | 132.0 (126.9-132.1) | 13.07 / 13.56 | 12.11 / 13.00 | 7.24 -> 5.85 |
| 8600-9100 | 104.1 (103.6-105.4) | 120.1 (116.1-122.7) | 13.65 / 14.62 | 12.20 / 12.67 | 8.33 -> 7.04 |
| 3000-9100 | 115.6 (115.4-115.7) | 135.9 (134.4-135.9) | 14.36 / 15.48 | 12.43 / 13.57 | 7.33 -> 5.97 |

+16% at the start and in the middle, +15% late, +18% over the race. It speeds up every VU1
program, the AOT ones included, which are about half the VU1 time (replay of the three
captures with the kernels on: 38.0/40.1/41.1 -> 27.7/30.1/32.3 ms). Chained over the round
(different sessions): about +29% at the start, +39% in the middle, +26% late, +30% over the
race (104.7 -> 135.9 fields/s; worst 1% 17.36 -> 13.57 ms).

Fourth bench, c3 against c4 = 2ab40cf5 (no U/O MAC tests), 3 interleaved runs each, all 100%
focused, 0 foreign dhd.exe, CPU 16-18% before each:

| fields | c3 fields/s | c4 fields/s | c3 p99 / worst 1% | c4 p99 / worst 1% | VU busy c3 -> c4 |
|---|---|---|---|---|---|
| 3000-3500 | 103.8 (102.3-104.2) | 111.6 (109.5-112.6) | 14.42 / 14.84 | 13.42 / 14.15 | 7.39 -> 6.46 |
| 6000-7000 | 132.1 (131.5-132.7) | 141.3 (140.5-142.7) | 12.05 / 12.80 | 11.67 / 12.46 | 5.82 -> 5.08 |
| 8600-9100 | 121.6 (120.5-122.4) | 136.1 (135.7-136.9) | 12.26 / 12.65 | 10.61 / 10.80 | 6.96 -> 6.03 |
| 3000-9100 | 136.2 (135.3-137.0) | 149.8 (148.9-150.3) | 12.28 / 13.18 | 11.49 / 12.35 | 5.95 -> 5.15 |

+8% at the start, +7% in the middle, +12% late, +10% over the race. Chained over the round:
3000-3500 79.2 -> 111.6 (+41%), 6000-7000 95.3 -> 141.3 (+48%), 8600-9100 95.0 -> 136.1
(+43%), 3000-9100 104.7 -> 149.8 fields/s (+43%), worst 1% 17.36 -> 12.35 ms. At the start the
EE (5.6 ms busy) is now close behind the VU (6.5 ms).

Where the time goes on c4 (sampling profiler, VU thread, 3000-3500): waiting for the EE 23%,
vif_feed 13% (spread over the UNPACK forms), the 0078 kernel 10%, the queue's consume 6.5%,
ps2_vu1_run's dispatch 4.7%, the kernels' clipping path (`native_clip_run`, still pair by pair)
4.2% (7.1% over the race: 30-42% of the 1638/17E0 vertices clip), retire_flags 2.6%, then AOT
blocks of 1-2.5% each. EE thread at the start: VU0 microprograms (interpreted) 30%, waiting for
the VU 29%. With the VU faster, VU0 AOT was benched again (c4 against c4 + `DHD_VU0_AOT=1`, 3
runs each): 3000-9100 148.3 against 149.6 fields/s, 3000-3500 111.3 against 111.2, inside the
spread (one c4aot run at 114 fields/s, the other two at 149.6; medians shown, nothing
dropped). EE busy falls 0.3 ms a field, the wall time doesn't follow yet; it stays opt-in.

Replay timings in a local harness move by up to 1.5x when unrelated code in the same
executable moves (an rdtsc probe around the kernel call made the 0078 kernel 40% slower in one
build and not another; the first run of a fresh executable is often slow too), so a replay is a
guide; the race bench decides.

## Sources

- PCSX2 `GSLocalMemory.cpp` (WriteImage per format, block/column swizzle tables), read for the
  structure of the upload path.
- PCSX2 MTVU and MTGS threads (`MTVU.cpp`, `MTGS.cpp`) for the worker/queue split this port
  already follows.
