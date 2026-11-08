# Round 33: game logic overlapping the drawing (deferred vsync), measured and not shipped

Status: partly superseded by 090 (the revert: deferred vsync shipped in 090 HO1)
Checked: 2026-11-04

Reverted here because VU1 was then the bottleneck; the same design shipped later as deferred
vsync on the native hosts' graphics workers (finding 090 HO1, `host_service_graphics_threaded()`
returning 2). Read HO1 for what runs today; the measurements below are the 32-bit build's.

Tried, measured and reverted here. The code is commit f80b4d61 (reverted by 7fcb47b9; the
launcher exe stays the round 32 build). Goal: let the EE (game) thread run the next field while
the VU and GS threads are still drawing the last one, as PCSX2's MTGS/MTVU do, with everything
the game can observe exact: readbacks (the flare Z probe), CSR/FINISH/SIGNAL/vblank state, VU1
memory, and their order relative to game code. It was exact (the gate passes) but not faster:
+4% in a mid-weight stretch, nothing at the race start, 4-5% slower late in the race. All
numbers are `DHD_RENDER=native`, uncapped frame clock, the bench race (`port/tools/bench_race.py`).

## Where the EE waited

`DHD_SYNC_STATS=1` (f80b4d61, port/hle/graphics_sync_stats.h) times every EE wait for the graphics
threads by call site over the bench window and prints `sync stats:` lines at its end: calls,
calls that found work still queued ("pending") and wait time, per field. Before this round
(DHD_SYNC_STRICT=1, which keeps the old waits), segment 6000-7000, per field: 5.5 ms of EE wait,
4.1 ms of it at `sceGsSyncPath` (5 calls a field, 1.9 of them with drawing still queued), 1.4 ms
at the deferred-readback guard (the flare Z read), the rest under 0.1 ms. vsync itself found the
queues empty: the frame end had already drained them.

## What the EE can observe of the drawing

The graphics threads change only GS/VIF/VU1 state, never PS2 RAM (apart from readbacks, which
are written on the EE). The EE sees that state only through:

- recompiled/asm loads and stores to VU1 memory (0x11008000-0x11010000), VIF1 registers and
  FIFO, GIF registers and the GS privileged page 0x12001000-0x12001100: `ps2_memory_address`
  drains the threads first (`ps2_graphics_sync`); so do VPU_STAT/FBRST through cfc2/ctc2
  (`ps2_vu1_regs_sync`);
- `sceGsExecStoreImage` (port/hle/graphics.c), which drains before and after the transfer and
  guards the destination pages until the read is decoded;
- plain C code: only func_0020BED0, which reads CSR bit 13 (FIELD), and the display-register
  stores in func_0021F000/func_0020B938.

Everything else, including how far along the drawing is, the EE cannot see. So a wait whose
caller reads none of the above before the next fenced access can go: the fence still runs, at
the same point of the game code, and finds the same state.

## Changes (f80b4d61)

- **sceGsSyncPath** (port/hle/dma.c) no longer waits: it polls the kernel (the virtual frame
  clock still advances, +1024 cycles a call) and returns 0, the value retail returns once PATH1-3
  are idle. Any later observation of the GS state fences anyway.
- **Ring markers** (port/hle/graphics_queue.c, gs_queue.inc): `graphics_mark(payload, bytes)`
  queues a small payload in order with the packets, through the VU ring and on into the GS ring
  (`GS_PATH_MARK`); `graphics_mark_handler` runs it on the GS owner thread once everything sent
  before it has been drawn. `graphics_wait_mark(n)` waits for marker n only.
- **Deferred vsync** (port/hle/kernel.c, port/gs/present.c): vsync queues the GS's VBLANK
  (`gs_vblank`: CSR.VSINT, FIELD, the readback field) and the picture (`gs_vsync_marked`: the 15
  display registers as the VBLANK handler left them, copied at vsync, so later EE writes don't
  leak in) as markers instead of draining. At most one field is in flight: each vsync first
  waits for the previous field's last marker (its picture). PCSX2's MTGS does the same: its vsync
  is a ring packet carrying a copy of the privileged registers (`RingCmdPacket_Vsync`).
- **CSR**: the EE's CSR page is written only by the EE. At a deferred vsync it becomes
  provisional: the previous value with VBLANK applied (`| 8`, FIELD toggled), exact apart from
  SIGNAL and FINISH, which only the drawing sets. The first drain after it (`graphics_drained`:
  any fenced access, a readback, a CSR write) writes the exact value the VBLANK marker found.
  Until then the only CSR reads are the FIELD bit: func_0020BED0 (plain C) and sceGsSyncV.
- **sceGsSyncV** (func_00243CE8) is now an HLE transcription of the recompiled function with its
  CSR load unfenced (it uses only FIELD). link.py now counts `rc_` references as wanted, so the
  functions it calls are recompiled.
- Kept strict: SetVSyncFlag (the CSR is copied to RAM at vsync), `DHD_GS_CAPTURE`,
  `DHD_RACE_GSDUMP`, the unthreaded build, and the first field. `DHD_SYNC_STRICT=1` restores
  every old wait (sceGsSyncPath, vsync, readbacks) with the same code otherwise: the debug switch.

Buffer reuse: every packet is copied into the ring when it is fed (port/hle/graphics_queue.c), so the game may
overwrite its DMA buffers as soon as the feed returns; the queue test writes 0xCD over each
packet right after feeding it. Markers are copied the same way.

Presentation: in the threaded build the picture is now presented from the GS thread (from the
copied registers), not the EE. The native renderer's render thread, DHD_INTERP and the frame
pacing are unchanged: pacing still runs at vsync on the EE against the virtual clock, and online
play's catch-up clock still decides when vsync happens.

## Tried and dropped: earlier readback fetch

The 1.4 ms at the readback guard stays. Two variants moved the GPU staging copy's fetch onto the
GS thread so settling would wait only for a marker: at the next VBLANK marker, and right after
the store image. Both measured no change (the guard's wait became 1.34 and 1.32 ms a field, fps
the same within noise): the game uses the Z result almost at once, so the wait is the GPU
finishing the queued work and the copy, not the GS thread's backlog. Hiding it would need the
game to see the result later than retail does, which is not allowed.

## Results

Bench race, 3 interleaved runs per candidate, one game at a time under the bench lock, idle PC
(Ryzen 5 7600, 6 cores / 12 threads), window focused, no foreign dhd.exe. before = the round 32
launcher build (9a63ce9a), after = f80b4d61, strict = f80b4d61 with `DHD_SYNC_STRICT=1`. Medians;
fields/s, worst 1% frame time, CPU ms per field of the game (EE), gs and vu threads, and the
EE's wait for the graphics threads as a share of the field (`DHD_SYNC_STATS=1` was on for after
and strict in this table).

| segment | before | after | strict | worst 1% b / a / s (ms) | CPU game/gs/vu before | after | EE wait after / strict |
|---|---|---|---|---|---|---|---|
| 6000-7000 | 97.7 | 101.6 (+4.0%) | 95.8 (-2.0%) | 16.3 / 16.6 / 16.2 | 8.9 / 9.3 / 8.2 | 8.4 / 9.6 / 9.2 | 5.0 ms (51%) / 5.6 ms (54%) |
| 3000-3500 | 81.4 | 81.0 (-0.5%) | 79.1 (-2.7%) | 18.4 / 18.9 / 19.7 | 11.2 / 10.9 / 11.0 | 11.6 / 11.4 / 11.4 | 7.0 ms (57%) / 7.2 ms (57%) |
| 8600-9100 | 98.1 | 94.5 (-3.6%) | 95.7 (-2.4%) | 15.0 / 16.1 / 15.1 | 8.7 / 9.1 / 9.2 | 9.8 / 10.3 / 10.4 | 6.3 ms (60%) / 6.1 ms (58%) |

Again for 8600-9100 without `DHD_SYNC_STATS`: before 98.8, after 93.5 (-5.4%), strict 95.2
(-3.7%); worst 1% 15.2 / 16.3 / 16.2 ms. In 6000-7000 the game tick's maximum also rose from
about 17 to 22.5 ms in every after run.

Why it does not pay:

- The EE's wait does not go away, it moves: with sceGsSyncPath gone the EE now waits at the next
  vsync for the previous field's picture (5.3 ms a field in 8600-9100, 0.8 of the calls with work
  in flight). The frame is bound by the VU thread (about 10 ms of CPU a field at about 95-100
  fields/s: near 100% busy); letting the EE run ahead cannot make the VU thread finish sooner.
- The VU and GS threads get slower while the EE runs beside them: in 8600-9100 they used 10.3-10.4
  ms of CPU a field instead of 9.1-9.2 for the same work. The PC is not short of cores (2.9 busy
  cores of 12); the likely cause (not measured) is shared caches and memory traffic: the EE
  builds the next field's packets while VU1 reads the current one. Where the VU thread is the
  bottleneck that costs more than the overlap gains.
- Open: strict (the old waits, same binary) measured 2-4% slower than the round 32 build in
  every segment although its waits and object set are the same (only func_00243CE8 differs);
  code layout is the likely cause. It was not chased further.

The next lever stays VU1 throughput (finding 046's list: the skinning loop 0468, 0078's setup and
tail); overlap may become worth retrying once the VU thread is no longer the bottleneck.

## Verification (f80b4d61)

- `port/tools/gate_native_renderer.py` on the clean launcher build of f80b4d61: every EE state
  dump matches (g+0/2/12/16 in both arms, g+3600, g+4500), READBACK MATCH 1154 records and 1154
  Z reads through virtual GS field 9037, NATIVE RENDERER GAME-STATE GATE MATCH. The exact
  renderer arm, which the change affects (presentation moved to the GS thread), passed too.
- `port/tools/vu1_seq_diff.py` on the round 31 captures (6300 and 8750 s): 24,000 runs, 0
  differing.
- test_graphics_queue (threaded and inline) with marker checks: 429 markers in order, each run
  after exactly the packets queued before it, on the consumer thread; 0 failures.
  test_split_graphics: queue 0 failures, VIF fuzz 200,000 trials 0 differing.
- The link of the clean build: rc_func_00243CE8 from dma.c.o, no basis copy, 0 duplicates.
- Not done, as nothing shipped: the 120 Hz `DHD_INTERP` check by eye.

Kept on main from this round: `host_thread_name` stubs (the queue and VIF tests had failed to link
since round 32) and link.py counting `rc_` references as wanted (HLE code calling a recompiled
entry used to link to a silent stub under `/force`).

## Sources

- PCSX2 MTGS: vsync as a ring packet with a copy of the GS privileged registers
  (`RingCmdPacket_Vsync`, `GS_RINGTYPE_VSYNC`), the EE waits (`WaitGS`) only where it reads GS
  state, e.g. VIF1 FIFO reads:
  https://code.govanify.com/govanify/pcsx2/src/commit/5a4541f2f4c5eb7c89bf54eb0f0f76393e708b90/pcsx2/MTGS.cpp,
  https://github.com/PCSX2/pcsx2/commit/c6d8b52d2691d2a203f5a9cda7d4c9a72b871712
- Ring design history (EE write position independent of the GS read position):
  https://wiki.pcsx2.net/PCSX2_Documentation/Google_Code_svn_repository_comments_archive_2000_to_2499
No code was copied.
