# 148: The race start was a serial chain: the flare's Z read made the game wait for VU1

Status: current
Checked: 2026-11-04

2026-11-04, from local main `dd40407`. Target: 60 fps throughout races on the rooted Mi 11i
throttled to finding 127's low-end profile (`phone_throttle.py --apply t612 --big-max 1209600
--little-max 1497600`), at render scale 2x and 4x. Every speed run unlocked (`DHD_FPS=240`,
`DHD_VSYNC=0`, `DHD_FRAME_CLOCK=1`), non-PGO release APKs with line tables (code unchanged).

## Summary

The slow race interval of finding 147 is the race start (the first ~960 fields after GO at 240,
about four seconds of race clock with all riders in view). There the game thread and the VU1 thread
ran one after the other: the game waited 14.8 ms a field for VU1, VU1 idled 9.7 ms a field waiting
for the game. The wait was `sceGsExecStoreImage`, the lens flare's Z probe, which drained the whole
graphics queue before and after its read. Queuing that read behind the drawing (change 1) took the
race start from 31.5 to 47 fields/s; three GS and fill changes added ~8 % more.

Both builds in one session, unlocked, throttled; the second race perf line is the race start
(perf lines are 30 s each, so a faster build's lines cover more fields):

| scale | build | successive race perf fps | window 9000-14000 fields/s |
|---|---|---|---:|
| 2x | main | 59.4, **31.5**, 52.6, 59.8, 64.8 | 63.33 |
| 2x | branch | 67.0, **55.8**, 60.1, 66.4 | 65.81 |
| 2x | branch + PGO | 69.7, **58.8**, 65.8, 73.2 | 71.44 |
| 4x | main | 53.2, **31.4**, 47.2, 52.8, 54.8, 57.2 | 55.43 |
| 4x | branch | 54.9, **42.9**, 51.6, 54.5, 55.5 | 55.16 |
| 4x | branch + PGO | 55.0, **43.6**, 52.0, 55.5, 56.8 | 55.90 |

Race-start window (fields 6400-7300) at 2x, the per-change measure: main 30.5-31.5, branch
50.3-51.9 fields/s (+65 %). Finding 147's main + PGO at 4x: 53.4, 36.1, 52.2, 54.1, 54.3, 58.5
(window 55.88).

60 is not held throughout. At 2x every interval but the race start is at 60 or above (the race
start 55.8, with PGO 58.8). At 4x the GPU limits (15-19 ms a frame, below) and the window did not
move.

## Method

- The runner (scratch, not committed) as finding 147's: writes the run's `DHD_*` keys into the app's
  dhd.ini by root and puts the original back (md5 checked), closes third-party apps, cools 45-90 s,
  samples clocks, optionally records simpleperf, pulls dhd.log and named files.
- **Per-change A/B: the race-start window, fields 6400-7300** (GO is at 6321; 900 fields of the slow
  interval), 2x, alternated builds, two runs each. Run-to-run spread of one build there is about
  +-1.5 %. The mid-race window 9000-11000 for VU-only changes.
- **Before/after: window 9000-14000 and every 30 s race perf line**, both builds in one session.
- `DHD_TIMELINE` (each thread's waits per field), `DHD_VU1_PROFILE` (per microprogram), simpleperf
  (1 kHz flat, 500 Hz frame-pointer call graphs), `DHD_RSTAT` (render thread per picture),
  `DHD_GPU_TIMES` (GPU per pass kind), `DHD_GPU_STATS`, `DHD_PASS_TRACE`.

## Where the race start went (main, 2x, fields 6400-7300)

`DHD_TIMELINE`: field 31.7 ms; game busy 14.7, waiting for VU1 14.8 and for the GS 2.2 (3 waits a
field, 2.9 of them pending); VU1 busy 22.0, idle 9.7; GS idle 16.5. The call graph puts the game's
waits under `func_002286F8` (the flare's first touch of last frame's Z read) and the read itself
(`sceGsExecStoreImage`, `port/hle/graphics.c`): it waited for both workers to go idle, fed the store
packet inline on the game thread, waited again, read the GS registers and deferred the read; the
next frame's first touch drained the queue once more before settling. With the native probe the
answer is the render thread's newest finished Z copy anyway; the waits only kept the order.

CPU shares then (simpleperf): game 30 % (idle_wait 30 % of it, spinning), VU 28 %, GS 22 %, render
17 %. The VU thread spent 11 % in the kernel's wakeup path (`_raw_spin_unlock_irqrestore`), waking
the waiting game thread.

## Changes

1. **`2e8185a` Graphics: queue the lens flare's Z read behind the drawing.**
   `host_service_gs_store_image(packet, destination, bytes)` queues the store packet, a `MARK_STORE`
   marker and the PATH3 unmask in the VIF1 ring. The GS owner runs the read when it reaches the
   marker, as the game thread did after its waits (`gs_defer_read_image`, settled at once into a
   slot, or `gs_read_image`), and consumes FINISH. The game guards the destination pages as before;
   the first access (`graphics_settle`) waits only for that marker and copies the slot. The CSR page
   is exact at every fence (the last VBLANK or read marker's CSR, `marked_csr`). The read's size
   comes from the packet's own GIF registers (BITBLTBUF, TRXREG, TRXDIR); any other packet shape,
   no workers or no deferred VBLANKs, or `DHD_QUEUE_READS=0` keep the waiting path, and the browser
   keeps it (its stub returns 0). The first version fed the packet with `host_service_vif_feed`,
   which runs VIF1 inline on the calling thread: it raced the VU worker (the race stopped drawing,
   the state trace differed from field 1564). Fed through the queue it matches.
   Race start 31.5 -> 47.1 fields/s; the game's wait for VU1 14.8 -> 0 ms a field.
2. **`09291c7` GS: no repeated work for strip indices, CLUT reloads and palette keys.** A strip
   triangle right after one recorded in the same loop takes its first two indices from it and
   stores only the third vertex (a temporary check: 176 M such triangles of a Windows race, all
   reused vertices byte-equal); `load_clut` skips a reload of the same source bytes into the same
   entries; the texture key mixes a palette digest kept until the CLUT entries (new
   `clut_writes`), CPSM, CSA or TEXA change. 46.2-47.0 -> 48.5/48.5; GS 16.7 -> 15.5 ms a field.
3. **`f2a9dfd` Skip the zero fill of three hot locals.** The build zero-fills locals
   (`-ftrivial-auto-var-init=zero`, needed by the game, finding 081). `packed_strip`'s 10.6 KB
   vertex list (every PACKED strip tag; memset was 6.5 % of the GS thread), the VU run state in
   `run()` (prefix cleared explicitly, the 1 KB VU1 copy filled before use) and `load_clut`'s
   scratch now carry `PS2_NO_AUTO_INIT`. 47.9-48.5 -> 50.7/49.5; GS 15.6 -> 14.9, VU 17.4 -> 16.7,
   game 18.0 -> 17.2 ms.
4. **`3ae5282` GS: one cached converted palette, and a strip box grown by the new vertex.** The
   palette cache holds the converted entries, used by the key, the decode and converted textures'
   constants; after a triangle recorded just before, only the new vertex can move the box.
   51.28/50.27 vs 50.21/49.88 (+1.5 %, at the edge of the spread but in both pairs); GS 14.9 -> 14.6.

## Checks

- **Windows state trace** (`state_trace_run.py`, exact probe) after every change, against main's
  traces (`state_diff.py`): boot 600 rows and race 5000 rows match.
- **Recorded native frames**: a temporary hash of every handed frame's vertices, indices and
  commands (without pointers and padding), hidden-desktop deterministic race with
  `DHD_EXACT_PROBE=1` to field 9000: main against the branch head, **all 8,999 frames identical**;
  one build against itself identical too.
- Host tests (GS memory/GIF/transfer/CLUT, dumps, VU1, VIF, graphics queue, gamepad, touch): 0
  failures after each change. `test_vif.c` gained stubs for the two host services.
- **The phone's state trace** (finding 146's recipe: headless, exact probe, 2x, 4:3, 5000 race
  fields): the branch's APK against main's on the same phone, `trace_columns.py` PASS, all 13
  columns identical.

## Tried and not kept

| what | result |
|---|---|
| `-ffp-exception-behavior=ignore` for the host build (FP ops free to schedule; results identical) | 46.64/46.93 vs 46.98/46.22: none |
| `DHD_SPIN_US=0` after change 1 | 45.98 vs 45.71: none |
| the VU0 native routines' and the clip runner's `Vu` locals without their zero fill | 51.90/50.97 vs 51.68 (50.74/49.55 earlier): inside the spread |
| `native_geometry_loop` unrolled by two | mid-race 63.35/63.67 vs 64.02/63.68: none (code 7.0 -> 12.4 KB) |
| a strip's first triangle in the fast loop when the state is current | 50.97/51.68 vs 51.04/51.90: none (frames identical; a first version took the shortcut at k = 0 through `0 - 1 == UINT32_MAX`, which the frame hash caught) |
| `DHD_ZREAD_FOLD=1 DHD_GPU_OVERLAP=1` (finding 111) | 50.32/49.85 vs 51.61/50.92: worse, although the render thread's CPU fell 14.0 -> 12.4-13.0 ms |
| `DHD_FIFO_ADAPTIVE=0` (full spin before a worker parks) | 48.58/48.94 vs 50.73/51.67: worse |

## 4x: the GPU

At 4x the GPU limits the race start (19.3 ms a frame in that interval) and parts of the race
(15-17 ms). `DHD_GPU_TIMES` at the race start: draw passes 16.8 ms, scanout 1.36 ms, present 0.51 ms
a frame (the countdown before GO: draw ~10 ms). `DHD_GPU_STATS`: 12-16 Mpixels shaded per 3.67
Mpixel picture, overdraw ~4 as on finding 131's PC iGPU, most of it the bilinear, fogged,
dual-source blended terrain layers (in-pass timestamps cannot attribute time on this tiler).

Pass count is not the cost: a race picture opens the main 2048x1792 target up to five times, with
small surfaces between (`DHD_PASS_TRACE`), but a temporary experiment that reopened the pass every
40 draws (18 -> 29 passes a picture, each loading and storing its attachments) added only 0.33 ms
of GPU time: the Adreno renders these passes in system memory, so merging or reordering passes would
not pay. From the 2x and 4x GPU times about 3.6 ms a frame is fixed (scanout, present, clears,
probe) and ~15.7 ms scales with pixels at 4x. What is left is the per-pixel shading of the exact
picture, which finding 131's specialised pipelines and gather already brought near its floor; at
the profile's 315 MHz GPU clock 60 at 4x needs about 15 % less GPU time at the race start, and
nothing found here preserves the picture and gives that.

## What limits it now, and what is left

- **2x race start (55.8 fps, 18 ms a field):** a balanced pipeline now. Busy per field: game 15.5
  (plus ~2.6 ms spinning for the GS), VU 17.1, GS 14.8 (85-89 % on the A55s), render 11.2 + 4.3 in
  the present; each thread is idle or waiting 15-25 % of the field, the two-fields-in-flight limit
  coupling them. All four need cutting for 60 there (PGO does ~5 %: 58.8).
- **2x mid-race:** VU1 (`limit=cpu:vu`; mid-race busy 16.1 of a 16.4 ms field). Its programs are
  12.1 ms a field (`DHD_VU1_PROFILE`): the mesh loop (`native_geometry_loop`, kinds 0-2) ~44 %, the
  1638 kernel 8 %, the AOT programs at 0040/0020 (setup work, ~1 ms) and the particle programs;
  VIF unpack and decode ~13 % of the thread. The mesh loop gained nothing from unrolling (and in
  finding 147 nothing from cheaper clamps): native kernels for the 0040/0020 programs and a cheaper
  VIF path are what is left there.
- **Render thread (A55s):** per picture `encoder.finish` ~5 ms, the picture's submit ~3 ms and the
  present pass's ~3 ms (wgpu-core and the Adreno driver); it waits 2-5 ms a picture now, so it is
  next once the GS and VU1 drop. Candidates: a persistent upload ring instead of
  `Queue::write_buffer`'s staging buffer per call, fewer bind-group changes.
- **The game thread:** the hand-written renderer core `func_0021B8A8` (recompiled asm) 7.6 %, VU0
  work ~14 %.
- **4x:** the GPU (above).
- **PGO** is worth +8.5 % on the 2x window (65.81 -> 71.44) and +5 % on the race start on this
  branch; its profile came from one unlocked 2x run of fields to 7000 (21 MB profraw), as finding
  146 did. A release profile should cover more tracks and the menus.

## Phone state at the end

`phone_throttle.py --restore` printed RESTORED (every saved value read back); dhd.ini is
byte-identical to the saved original (md5 9af4f977637895d329ec56b472b53514); no run files left in
the app's folder; `/sdcard/PS2` untouched, no uninstall. Installed: the branch's release-signed PGO
build (code of `3ae5282`), SHA-256
671bb0d7c72981dc9522d2119c6268a243c97643a5a711a4770a8b659acfc2a0. The phone lock was removed.

## Sources

- Findings 081, 097, 108, 111, 127, 129, 131, 139, 146 and 147; the queue, VIF, GS and renderer
  sources; wgpu-core 29.0.3's `Queue::write_buffer` (a staging buffer per call), read only.
- [PCSX2's MTVU](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MTVU.cpp) (a copied command ring
  for VU1, the design graphics_queue.c already follows) and
  [Clang's `uninitialized` attribute](https://clang.llvm.org/docs/AttributeReference.html#uninitialized)
  with `-ftrivial-auto-var-init`. No external code copied.

Raw logs, profiles, APKs, traces and the temporary patches (frame hash, pass-split experiment,
rejected changes) are in the ignored scratch folder. No game data, keys or machine paths are tracked.
