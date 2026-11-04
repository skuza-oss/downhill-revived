# Round 32: VU1 thread speed (native 07F0 kernel, VIF stream unpack)

Status: current
Checked: 2026-11-04

Goal: headroom for 120/144 Hz and no slow-downs in heavy scenes, with
game state and VU1 output exact. All numbers are `DHD_RENDER=native`, uncapped frame clock,
the bench race (`port/tools/bench_race.py`).

## Where the time goes

New measuring tools (all opt-in, nothing changes when unset):

- `bench threads:` line at the end of every bench window (port/gs/present.c): CPU ms per
  field for each named port thread (`game`, `vu`, `gs`, `native`, `sound`) and the process;
  `bench_race.py` reports the median as "CPU ms per field". Threads register their names with
  `host_thread_name` (port/hle/host.c).
- `DHD_PROFILE_THREAD=vu|gs|native|...` points the sampling profiler (`DHD_PROFILE`) at that
  thread instead of the game thread.
- `DHD_VU1_PROFILE_FIELDS=first-last[/step]` times VU1 per microprogram over a field window
  (the window must end before the bench exits, e.g. 6000-6990), optionally one row set per step
  fields. `DHD_VIF_STATS=field` logs the UNPACK forms (format, mask, mode, CL/WL) of one field.

Bench segment 6000-7000, before this round (b10d7660): the three busy threads each used about
11 ms of CPU per field (game 11.6, gs 11.6, vu 11.3) at 78 fields/s (12.8 ms a field).

- VU thread (samples): VU1 code 50% (`ps2_vu1_run` with the native kernels inlined 15%, AOT
  blocks, `native_clip_run` 6%, `retire_flags` 4%), VIF unpack 18%, the kernel signature
  `memcmp` 4%, waiting 11%.
- Game thread: 50% waiting in `graphics_wait`: 74% of that at `sceGsSyncPath`
  (func_002433A8) called from the frame end (func_001B4618 in the main loop func_001B40A0),
  25% at the deferred-readback guard (flare Z, func_002286F8). The game's own loop is
  sequential: update, submit, `sceGsSyncPath`, flip at vsync. So the EE and the VU worker
  overlap little: the frame costs roughly EE work plus the VU1/GS tail after the last submit.
- VU1 per microprogram (6000-6990, ms per field of 7.6): 0078/1040 1.25 (already native, 5.1
  us a call), 07F0/1040 0.98, 07F0/1CB0 0.56, 1638/1040 0.54, 0020 (skinning, MSCAL) 0.48,
  2E48 (overlay A) 0.45, 2DC8/1040 0.35, 0078/1CB0 0.32, then many under 0.3.

Load over the race (`DHD_VU1_PROFILE_FIELDS=3000-9590/100`, calls per field): the start
(3000-3300, about 2,400 VU1 runs a field, the heaviest) and 8600-9100 are the heavy stretches;
6000-7000 is mid-weight. These are the three bench segments below. A power-up moment could not
be identified from the data alone; 8600-9100 is the heaviest late-race stretch.

## Changes

1. **Native kernel for the 07F0 mesh loop** (`native_geometry_800`,
   port/runtime/vu1_native_extra.h, dispatch kind 64 for VI13 0x100 in vu0_interp.c): the loop
   08B0-0930 run as SSE code with the exact helpers (clamped arithmetic, sticky flags, Q
   pipeline, flag queue). It declines before writing anything (vertex distance 0, not a
   multiple of 3, over 341 vertices, or a pending Q) and the run continues pair by pair.
   Replay on the crowded-start capture: 07F0/1040 7.74 -> 2.20 us a run (3.5x), 07F0/1CB0 5.86
   -> 3.60 (1.6x: fewer vertices, setup dominates).
2. **Kernel signature check cached** per kernel and VU1 code version (only for code at the
   game's address, 0x11008000), instead of a `memcmp` of the microcode on every run (4% of the
   VU thread).
3. **VIF UNPACK from the stream** (`unpack_stream`, port/hle/vif.c): any format, masked or not,
   any STMOD, when WL <= CL and nothing is buffered: fields read at a bit offset, the mask
   decoded once per UNPACK, rows and columns as the generic path; a vector cut by the end of a
   transfer is buffered as before. `vif_unpack_reference=1` forces the generic path.

## Exactness

- `port/tools/vu1_seq_diff.py` (new; harness port/runtime/vu1_seq_diff.c): replays every
  captured VU1 run (`DHD_VU_SEQ`) from its real state with the native kernels off (the exact
  AOT/interpreter path, the referee) and on; the whole register file, the 16 KB data memory and
  every kicked GIF packet (bytes and address, in order) must be identical. Three captures,
  32,000 runs: crowded start (fields 3477-3480), 6300 and 8750: **0 differing**; every kernel
  kind ran (07F0: 874 + 2,204 + 1,692 runs).
- `port/hle/test_vif.c` `unpack_fuzz`: 200,000 random UNPACK streams (formats, masks, USN,
  STMOD 0-3, CL/WL 1-6 each, counts, start addresses, row/column registers, random DMA chunk
  splits) through the generic path and the fast paths; the 16 KB data memory must match:
  **0 differing** (16.1 M vectors).
- `port/tools/gate_native_renderer.py` on 48f90d0f: EE dumps at every moment match the
  reference and between arms, 1,162 of 1,162 readback records match: **GATE MATCH**.

## Benchmark

`build/port/r31/bench_all.sh` (local): b10d7660 (the code before this round, plus the thread
report) against 48f90d0f, 3 interleaved runs per segment, one game at a time, window focused,
under `build/port/BENCH.lock` taken only once the PC was idle (total CPU 6-10% before each
segment, no foreign dhd.exe during the runs). Medians:

| Segment | fields/s before -> after | frame time | worst 1% (ms) | CPU ms per field, game / vu / gs |
|---|---|---|---|---|
| 6000-7000 (bench) | 84.2 -> 96.8 (+15.0%) | 11.87 -> 10.33 ms (-13%) | 18.3 -> 16.4 | 10.8 / 10.5 / 10.6 -> 9.0 / 8.2 / 9.3 |
| 3000-3500 (start, crowded) | 72.3 -> 81.4 (+12.6%) | 13.84 -> 12.29 ms (-11%) | 20.2 -> 19.2 | 12.6 / 12.6 / 12.5 -> 11.0 / 11.0 / 10.9 |
| 8600-9100 (heaviest late stretch) | 81.1 -> 98.4 (+21.3%) | 12.33 -> 10.16 ms (-18%) | 16.5 -> 14.7 | 10.4 / 11.2 / 11.1 -> 8.8 / 9.3 / 9.2 |

Spreads: 3.0 / 3.2 / 0.6 fields/s before, 16.9 / 4.3 / 0.8 after; the 16.9 is one slow first
run (80.3) at 6000-7000, the other two were 96.8 and 97.2. The game and GS threads drop too:
they spent part of their time waiting for (spinning on) the VU worker. An earlier sampled run
taken while another build used the CPU is discarded.

## Go/no-go

- 07F0 mesh loop: GO. 3.5x on its main key (07F0/1040; 1.6x on 07F0/1CB0, where setup
  dominates), and the round's frame time is 11-18% better on all three segments. The frame-time
  gain is the three changes together; they were not benched one by one (a single noisy live run
  of the kernel and cache alone is not a valid benchmark and is not reported).
- 0078 (the top program, 16% of VU1 time): its loop was already native (3.3x); the rest is setup,
  the XGKICK tail and clipping. Not changed this round.
- 1638 (7%): already native (2.3x). Skinning 0020/0010 (8%): not done; next candidate.
- Cheap wins: the cached signature check and the VIF stream unpack (VIF went from 18% to 16% of
  the VU thread samples; the per-vector function call is gone, the per-field work remains).
  EE/VU overlap: not done (see Next).

## Next

- EE/VU overlap (tried in finding 048 and reverted; shipped later as deferred vsync, finding 090
  HO1): the game thread waits half its time for the pipeline at the frame-end
  `sceGsSyncPath` and at vsync. Packets are already copied into the ring, so the game's
  buffers are free at once; what needs the drain is presentation, the VBLANK handler's GS
  state (CSR FIELD, display latch) and readbacks. Queuing the vsync work as a ring marker
  (presented by the worker) and answering CSR from EE-side state would let the EE build frame
  N+1 while VU1/GS finish frame N. Largest remaining lever; needs its own determinism check.
- The skinning loop 0468-0570 (MSCAL 0020/0010 in overlay B, about 55 us a call, 8% of VU1
  time) is the next kernel candidate; then 0078's setup and tail (5.1 us a call with the loop
  native), `native_clip_run` and `retire_flags`.
