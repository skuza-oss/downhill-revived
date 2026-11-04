# 089: Where the time goes (x64 and browser, race)

Status: current
Checked: 2026-11-04

2026-10-25, branch `rr/PR1` (from `render/rust` 200241e37). A profile only. No game code was
changed. Two throw-away x64 variants were built outside the repo to measure one candidate (see
"Experiment" below).

## Summary

- **x64: the game (EE) thread sets the rate, not the GPU.** In a race the x64 build runs at
  118-131 fields/s at scale 2 and 128-131 at scale 8. The scale makes no difference, and the GPU is
  42% busy at scale 8. The EE thread is busy 7.2-8.0 ms per field. 144 fps needs 6.94 ms. The VU
  and GS threads are each about 60% and 50% busy.
- **The biggest single cost on the EE thread is the guest-memory alias fold that
  `port/host64/memory_access.h` inlines into every wasm2c load and store.** A variant without the
  fold runs the EE thread in 5.0 ms instead of 8.3 ms, a 40% cut, and reaches 145-152 fields/s,
  144.4 at scale 8. A variant that only moves the fold out of line, with identical results, cuts
  13% (to 7.25 ms). With the fold gone, the next limit is the VU1 thread at 5.1 ms, plus the EE
  waiting for it (1.3 ms a field).
- **Browser: about 52 fields/s in the race, with or without the 59.94 Hz pacing.** Both workers
  are almost saturated and wait for each other. The graphics worker is busy 16.3 ms a field and
  the game worker 15.2 ms. The graphics worker sets the rate, by about 7%. Their hand-off at the
  vsync costs about 2.6 ms a field: the game waits 3.7 ms and the graphics worker idles 2.6 ms.
- **The VU0 microprograms run in the interpreter inside the core module on both platforms.** They
  take 30% of the x64 EE thread and 24% of the browser game worker. About 71% of their time is in
  8 entries that have no native kernel.

## Method

Between that tree and 200241e37 only comments, tools and docs changed, so its wasm core and its
threaded web build (BS1-BS4 in) are current.

**x64 build.** `build_host64.py build/wasm/dhd_core.wasm --num-outputs 8 --sdl-lib ... --jobs 2
--debug`. `--debug` adds line tables and a PDB. The code generation is the release one: /O2
/clang:-O3, x86-64-v2. dhd64.exe sha256 `f8fb1f72...`. The Rust library comes from the rustgs cache
(release, no debug info), so its functions are named by the nearest link-map symbol. Those names are
approximate, but their library is not.

That desktop is never shown, so no window appears and no focus is taken. Power throttling (EcoQoS)
is switched off for the game process, since a process with no visible window may otherwise be
throttled.

Each run used:
- bench_race's recipe: `DHD_FRAME_CLOCK=1` (unpaced), `DHD_SETTINGS=none`, skip movies, no
  memory card, race_compare's `DHD_NAV` and autopad, `DHD_RACE_SYNC=1`, seed 81257.
- A pedalled race (`DHD_RACE_PAD=2:1200:cross`, state_trace_run's full pedal), so the rider rides
  as in play.

At 144 steps a second GO is at field 7139. `DHD_BENCH=7900-10900` (3000 fields, about 23 s) and
`DHD_TIMELINE` give fields/s and each thread's waits. Sampled runs start
`port/tools/sample_profile.py` 5 s after GO for the rest of the window. A scratch copy dumps every
sampled PC and keeps the module list while the process lives. Windows DLL addresses are named by
their nearest export. `nvidia-smi` logged GPU use every 100 ms during the sampled runs.

The sampler reads a thread's PC when the thread used more than about 20 us of cycles since the last
tick (one tick about 1.5 ms). PCs in `NtWaitForSingleObject`, `NtWaitForAlertByThreadId`,
`NtUserMsgWaitForMultipleObjectsEx` and the ring polls (`idle_wait`, `position`) are counted as
waits.

It attaches to both workers (the graphics worker is nested in the game worker, so auto-attach is set
on the worker's session too) and takes DevTools CPU profiles of both at once: 500 us sampling, 20 s
from field 4000.

Each run used:
- The race query: race_compare's NAV and seed, `DHD_RACE_PAD=2:1200:cross`. The race was on
  screen at 45 mph at the end.
- Scale 2, the native renderer, the threaded build.
- `--unpaced` serves `worker.js` with its pacing period replaced by 1 ms (Fetch interception; the
  file is not changed) to measure capacity.

Modules are told apart by their functions, not their hashed URLs. A native frame under
`dhd_render.js` counts as a WebGPU call.

**Timing rule.** Before every measurement `quiet.sh` waited until no clang, clang-cl, lld-link,
cargo, rustc, wasm-opt or build_*.py ran, polling once a minute. Other load on the PC was not
excluded:
- `ProBrcp` used about 0.9 of a core throughout.
- Total CPU load before the A/B runs was 29-93%.

So single fields/s numbers move by up to 15%, and several runs are much slower (marked below). The
EE thread's busy ms per field is the steadier measure.

**PC.** Ryzen 5 7600 (6 cores, 12 threads), RTX 3070 (D3D12 through wgpu 29), Windows 11.

## x64: numbers

| run (race, fields 7900-10900) | fields/s | p50 / p99 ms | EE busy | VU busy | GS busy | EE waits (VU + GS) |
|---|---|---|---|---|---|---|
| scale 2, timing | 118.1 | 8.29 / 12.89 | 8.00 | 5.13 | 4.32 | 0.29 + 0.19 |
| scale 2, sampled | 125.8 | 7.69 / 12.92 | 7.52 | 4.77 | 4.01 | |
| scale 8, timing | 131.4 | 7.42 / 11.52 | 7.16 | 4.70 | 3.93 | 0.26 + 0.19 |
| scale 8, sampled | 128.0 | 7.63 / 11.92 | 7.39 | 4.75 | 3.97 | |
| scale 8, sound on (dummy device), sampled | 127.7 | 7.68 / 11.79 | 7.42 | 4.77 | 3.97 | |
| bench_race recipe (finding 066's pad, 60 steps/s, 3000-9100), scale 2 | 136.0 | 7.00 / 13.69 | 7.13 | 3.98 | 3.41 | |

Busy and wait times are ms per field (`bench timeline`). The process uses 2.36 cores. Displayed
frames equal fields (118 and 131 fps).

The bench-recipe row is not comparable with finding 066's i686 results (153-173 fields/s): the code
is older there, the window was in front, and the load on the PC differed. It suggests that the
wasm2c x64 build is somewhat slower than the old i686 build was.

**Threads.** The sampler's work share (waits excluded) times the field time gives ms per field,
scale 8:

| thread | does | work ms/field | waits |
|---|---|---|---|
| `game` (EE) | the wasm2c core, VU0 microprograms, HLE | 6.35 | 0.45 ms in graphics_wait (spinning, then events) |
| `vu` | VIF1 unpack, VU1 (AOT blocks, native kernels), XGKICK into the GS ring | 3.86 | about 40% of its samples are waits for packets |
| `gs` | the GS front end (gs.c, gpu_exact.inc for the Z probe, native_core.inc recording) | 3.46 | 41% waits for packets; Z-probe readback map wait about 0.6 ms/field (1 readback a field) |
| `native` | the Rust renderer: native_picture, wgpu, submit and present | 1.04 | 58% of its samples are in a condition-variable wait (no frame queued). It does not wait on the GPU: fence waits (`NtWaitForSingleObject`) are 0.4% |
| `sdl` | the window's event loop | 0.02 | |
| `sound` | (off; with the dummy device: idle) | ~0 | |

**Areas, scale 8** (ms per field, summed over threads; waits omitted):

| area | ms/field |
|---|---|
| game core (wasm2c module code, including VU0 and the inlined load/store helpers) | 4.14 |
| host side of the core's memory access (`host64_memory_address`, inlined into the core) | ~1.43 |
| GS front end | 3.19 |
| VU1 | 2.99 |
| VIF1 unpack | 0.66 |
| GPU driver (nvwgf2umx) | 0.52 |
| C runtime (memmove, memset) | 0.47 |
| OS calls (heap, event signalling) | 0.46 |
| Rust renderer (dhd_render, wgpu) | 0.30 |
| D3D12 runtime | 0.12 |

**EE thread by group, scale 8** (non-inlined function, % of the thread's samples):
- VU0 30.3%: `ps2_vu0_run` 25.7%, `ps2_vu_madd` 3.0%, `native0_transform` 1.4%.
- Recompiled game functions (`rc_func_`/`func_`) 24.3%: 0021B8A8 4.1%, 00213C08 2.9%, 002628A0
  2.2%, 00213658 1.0%.
- Recompiler runtime 18.6%: `ps2_asm` 3.7%, `lookup` 3.5%, `run_fast_program` 2.8%, `ps2_exec` 1.8%.
- Other module code 6.3%.
- libc in the module 6.2%: `memcmp` 4.5%, which is VU0's code-signature compare on every
  vcallms.
- Waits for the VU and GS threads 7.9%.
- SPU2 0.7%.

Across these groups, the inlined memory helpers make up 22% of the thread (`host64_memory_address`
21.7%, and `i32_load_*`/`store` 9-12% as inline frames). The NaN canonicalisation of promote/demote
(`wasm_quietf`/`wasm_quiet`) makes up 8%, and `common_getenv_nolock` 0.5%: a getenv on a hot path.

**Top 25 functions, scale 8, all threads, waits excluded** (% of all samples, ms/field):
1. `host64_memory_address` 5.86%, 1.43
2. `w2c_core_ps2_vu0_run` 2.48%, 0.61
3. `nvwgf2umx.dll` 1.97%, 0.48
4. `stream_form` (VIF1) 1.81%, 0.44
5. `native_record` 1.69%, 0.41
6. `operand4` (VU1) 1.67%, 0.41
7. `i32_load_unchecked` 1.60%, 0.39
8. `vertex_kick` 1.46%, 0.36
9. `wasm_quietf` 1.41%, 0.34
10. `gs_write` 1.39%, 0.34
11. `memmove` 1.28%, 0.31
12. `packed` (GIF) 1.13%, 0.27
13. `queue_flags` (VU1) 0.94%, 0.23
14. `w2c_core_memcmp` 0.83%, 0.20
15. `wasm_quiet` 0.83%, 0.20
16. `gif_qword` 0.80%, 0.20
17. `fmac_flags4` 0.79%, 0.19
18. `native_geometry_loop` 0.77%, 0.19
19. `pixel_address` 0.77%, 0.19
20. `w2c_core_lookup` 0.72%, 0.18
21. `native_exact` 0.67%, 0.16
22. `retire_flags` 0.63%, 0.15
23. `i64_store_unchecked` 0.55%, 0.13
24. `simde_wasm_v128_any_true` 0.53%, 0.13
25. `native_result` 0.51%, 0.13

Small items in the GS thread: `native_field_trace` 0.8% and `native_log` 0.7% of the thread run with
tracing off.

**GPU.** nvidia-smi, mean / median / max utilisation during the sampled 25 s. The desktop, with
- Scale 2: 27 / 23 / 97% at a median 1110 MHz.
- Scale 8: 42 / 41 / 58% at 2055 MHz.
- The no-fold variant at scale 8 and 144 fields/s: 61 / 59 / 99%.

The render thread never waits on a fence. The GPU is not the limit at 144. At scale 8 it is the next
resource to watch: about 60% busy at 144, so roughly 200+ fps.

**VU0 programs** (`DHD_VU0_STATS`, the last 5.9M VU0 calls of the race; ticks include the stats' own
rdtsc):

| entry | share of VU0 ticks | path |
|---|---|---|
| 08D0 | 20.3% | interpreter |
| 0180 | 18.3% | interpreter |
| 0000 | 9.1% | native (cull) |
| 05E8 | 8.7% | native (basis) |
| 0C50 | 7.8% | interpreter |
| 0CD0 | 7.6% | interpreter |
| 02E0 | 6.3% | native (bounds) |
| 0BD0 | 6.1% | interpreter |
| 04F0 | 5.3% | interpreter |
| 0B40 | 3.8% | interpreter |
| 07B8 | 3.3% | native (length) |
| 0238 | 2.1% | interpreter |

About 71% of the VU0 time is interpreted. The core module is built with `PS2_VU0_NATIVE` only: the
AOT blocks (`DHD_VU0_AOT`, proven to match on i686 in findings 051 and 066) are not in the wasm
core.

## Experiment: the memory alias fold

`memory_access.h` folds guest addresses in the RAM mirror windows (0x2xxxxxxx, 0x3xxxxxxx, the low
views) to canonical RAM on every access. It inlines a window test plus the full fold. The OS already
maps all six views onto the same pages (`guest_memory.c`). The fold exists so that the C compiler
never sees two different addresses for one location (finding 082).

Two scratch builds re-ran the recorded core-unit compile commands of the profiled exe, with one
- **mem**: `host64_memory_address` returns `base + address` and nothing else. The exe shrinks from
  45.7 MB to 20.9 MB. This is a timing probe: it relies on the OS views and gives up the
  compiler-alias guarantee.
- **mem2**: the same function with the fold moved into a `noinline, cold` function, so each access
  inlines only the window test. Results are identical to the original. 28.9 MB.
- **quiet** (control): `wasm_quietf`/`wasm_quiet` return x unchanged.

All variants reached GO at the same field (7139) with the same navigation log. Values are EE busy ms
per field, with fields/s in brackets:

| round | base | mem | mem2 | quiet |
|---|---|---|---|---|
| 1 | 8.15 (115.8) | 4.64 (155.0) | | 12.62 (73.4)* |
| 2 | 9.25 (102.6) | 6.89 (109.4) | | 9.65 (95.3) |
| 3 | 11.43 (80.7)* | 5.38 (125.5)* | | 9.94 (92.2) |
| 4 | 7.76 (121.5) | 4.92 (149.0) | | 8.03 (114.0) |
| 5 | 8.35 (113.3) | 5.55 (133.5) | | |
| 6 | 9.27 (102.3) | 4.80 (152.2) | 50.2 (13.6)* | |
| 7 | 8.47 (111.9) | 5.00 (147.6) | 7.09 (129.9) | |
| 8 | 8.01 (118.0) | 5.01 (145.6) | 7.41 (124.7) | |

- Median EE time: base 8.3, mem 5.0 (-40%), mem2 7.25 (-13%).
- quiet shows no gain.
- mem at scale 8, sampled: 144.4 fields/s. EE busy 5.19 ms, VU 5.12, GS 4.33. The EE now waits
  1.27 ms a field for the VU thread.

The fold's cost is mostly code size (half the executable) rather than the test itself. The core's
memory accesses run into the millions per field: the browser counted 2.9 M non-proven ones per field
in BS3.

## Browser: numbers

Headless Chrome, scale 2, the race (GO at about field 3650). Fields/s are measured from the page's
status:

| run | 3800-4000 | profile window 4000-5100 | 5100-5600 |
|---|---|---|---|
| paced (as shipped) | 50.2 | 52.8 | 56.1 |
| unpaced (period 1 ms) | 49.9 | 53.1 | 51.8 |

Pacing makes no difference, so the race runs at capacity: about 52 fields/s, 18.9 ms a field. The
profiler did not slow it.

**Game worker** (unpaced; the paced run is within 1 point; ms per field at 53.05 fields/s):

| area (self time) | % | ms/field |
|---|---|---|
| VU0 (`ps2_vu0_run`, `ps2_vu_madd`, native0 kernels) | 23.7 | 4.47 |
| Atomics wait for the graphics worker (`web_x_host_service_graphics_wait`) | 19.4 | 3.67 |
| recompiled game functions | 18.5 | 3.49 |
| recompiler runtime (`ps2_asm`, `lookup`, `ps2_exec`) | 13.6 | 2.56 |
| core float layer (exact EE/VU0 rounding: `web_dmul`, `sum`, `vadd`, `vmul`, ...) | 11.1 | 2.10 |
| SPU2 sound (`sound_tick`, reverb, ADSR) | 4.4 | 0.83 |
| bulk memory (`web_copy`/`web_fill`) | 2.5 | 0.47 |
| HLE/other | 2.5 | 0.47 |
| libc (`memcmp` 1.7%) | 1.7 | 0.32 |
| queue copy (gqueue `feed`) | 0.9 | 0.17 |
| memory wrappers (BS3 left) | 0.3 | 0.05 |
| JS, the boundary, idle | 0.6 | 0.1 |

Top 25:
1. `ps2_vu0_run` 20.28%
2. `web_x_host_service_graphics_wait` 19.45%
3. `ps2_asm` 5.03%
4. `web_helper_web_dmul` 3.46%
5. `func_0021B8A8` 3.39%
6. `sound_tick` 2.65%
7. `func_00213C08` 2.26%
8. `web_helper_web_copy` 2.12%
9. `ps2_vu_madd` 2.04%
10. `memcmp` 1.69%
11. `web_helper_sum` 1.66%
12. `lookup` 1.54%
13. `func_002628A0` 1.39%
14. `web_helper_web_dsub` 1.29%
15. `web_helper_web_vadd` 1.19%
16. `native0_transform` 1.13%
17. `web_helper_web_vmul` 1.09%
18. `rc_func_00213658` 1.00%
19. `ps2_exec` 0.94%
20. `web_helper_web_dadd` 0.94%
21. `reverb_sample` 0.75%
22. gqueue `feed` 0.67%
23. `rc_func_0021CC44` 0.67%
24. `func_0022DE40` 0.60%
25. `snd_adsr_tick` 0.56%

**Graphics worker:**

| area (self time) | % | ms/field |
|---|---|---|
| VU1 (`run_micro`, `native_clip_run`, `aot_block_*`) | 38.2 | 7.20 |
| GS front end (`native_draw`, `gs_feed`, `gs_write`, `image_data`, `load_clut`, `gs_pixel_address`, `native_claim`) | 27.4 | 5.16 |
| idle (Atomics.waitAsync: no packet queued; readback maps) | 13.6 | 2.57 |
| WebGPU calls (`writeBuffer` 6.2%, `setBindGroup` 0.4%, ...) | 7.6 | 1.43 |
| VIF1 unpack (`vif_feed`) | 6.6 | 1.25 |
| other reactor code | 2.5 | 0.47 |
| queue (`web_q_run`) | 1.8 | 0.34 |
| Rust renderer wasm | 0.6 | 0.11 |
| JS glue (`dhd_render.js`, graphics.js) and the boundary | 1.1 | 0.2 |

Top 25:
1. `run_micro` 20.66%
2. (idle) 13.63%
3. `native_draw` 11.10%
4. `native_clip_run` 6.65%
5. `vif_feed` 6.62%
6. `gs_feed` 6.22%
7. `writeBuffer` 6.20%
8. `gs_write` 5.60%
9. `image_data` 1.99%
10. `web_q_run` 1.79%
11. `load_clut` 1.54%
12. `aot_block_173` 1.10%
13. `aot_block_555` 1.10%
14. `gs_pixel_address` 0.92%
15. `aot_block_170` 0.91%
16. `native_claim` 0.81%
17. `aot_block_172` 0.77%
18. `aot_block_564` 0.77%
19. `aot_block_50` 0.73%
20. `aot_block_640` 0.41%
21. `aot_block_64` 0.41%
22. `setBindGroup` 0.39%
23. `wasm-to-js` 0.36%
24. Rust `wasm-function[331]` 0.30%
25. `aot_block_44` 0.30%

**Which worker sets the rate.**
- The graphics worker is busy 16.3 ms a field and the game worker 15.2 ms.
- The game blocks 3.7 ms a field at the vsync until the graphics worker has finished the field.
  The graphics worker then idles 2.6 ms until the game queues the next field's first packets.
- So the graphics worker sets the rate by about 1.1 ms a field (7%), and the serial hand-off adds
  about 2.6 ms.
- With perfect overlap the rate would be about 61 fields/s (1/16.3 ms). With only the graphics
  worker faster it would be 66 (1/15.2 ms).
- Per field, the browser's game side costs 2.05x the x64 EE thread (15.2 vs 7.4 ms). Its graphics
  side costs 1.9x the x64 VU + GS threads (16.3 vs 8.6 ms).

## What limits each target

- **x64, 144 fps (6.94 ms a field).** The EE thread: 7.2-8.0 ms busy plus 0.45 ms waiting for the
  VU and GS threads. The GPU, the renderer thread and the scale are not limits: scale 8 runs as
  fast as scale 2. For a steady 144 the p99 also has to come down: 11.5-12.9 ms today, from
  irregular EE spikes. Once the fold is gone, the VU1 thread (5.1 ms) and the EE-to-VU hand-off
  (1.3 ms of EE waiting) come next.
- **Browser, 60 fps (16.7 ms).** Both workers are within 10% of it, but they run in series at the
  vsync. Overlapping them, or taking about 2.5 ms out of each, reaches 60.
- **Browser, 120 fps (8.3 ms).** Each worker has to halve:
  - The game worker has to drop from 15.2 to under 8.3 ms. Its parts are VU0 4.5, game
    functions 3.5, recompiler 2.6, float layer 2.1 and sound 0.8 ms.
  - The graphics worker has to drop from 16.3 to under 8.3 ms. VU1 plus VIF1 alone is 8.5 ms.

## Optimisations, ranked

The "share" is what the change removes from the limiting thread or worker, measured where it says
measured, otherwise estimated from the profile.

1. **Memory alias fold out of the hot path** (`memory_access.h`, `prepare_wasm2c.py`).
   - Measured: -13% of the EE thread with the fold out of line (exact; 1-2 h). The upper bound
     without any fold is -40%, which reached 144-152 fields/s.
   - Getting most of the 40% while keeping exactness needs more. Fold literal alias addresses when
     the C is prepared (they are constants), and keep the fold only where one function could see
     two views of one location at known distances. Everything else becomes `base + address` over
     the OS views. 8-12 h with a state trace and a race check.
   - x64 only (and the Linux host, which shares the header).
2. **VU0 off the interpreter in the core module.**
   - Build the core wasm with the VU0 AOT blocks on: they exist and were proven on i686. 6-10 h
     with the VU0 diff checks.
   - Add native kernels for 08D0, 0180, 0C50, 0CD0, 0BD0, 04F0 and 0B40: 71% of VU0 time, 3-5 h
     each, 25-35 h.
   - Estimate: -15% of the x64 EE (about 1.1 ms) and -15% of the browser game worker (2-2.5 ms;
     the browser's VU0 also pays the exact float helpers).
   - Both platforms; the core is the same wasm.
3. **Browser: VU1 and the GS in separate workers, or the core one field ahead of the vsync.**
   - The graphics critical path would fall from 16.3 to max(VU1 + VIF 8.5, GS + WebGPU 6.9) ms.
     Together with closing the 2.6 ms hand-off, the rate becomes game-worker-bound at about
     62-66 fields/s, +20-25%.
   - Needs a wasip1-threads build of the reactor (BS2's open item). 20-30 h.
   - On x64 the same idea (a deeper hand-off at the vsync) is worth up to 1.3 ms of EE waiting
     after item 1. 10-20 h.
4. **VU1 flag pipeline** (`queue_flags`, `retire_flags`, `fmac_flags4`, `native_result`: about 11%
   of VU work; `operand4` 6-8%; VIF `stream_form` 7%).
   - Let the AOT blocks skip MAC/status flag work no later instruction reads (a liveness pass in
     `vu1_aot.py`), and speed up the hot UNPACK forms.
   - Estimate -15-25% of the VU1 thread or worker: about 1 ms on x64, the next limit after item 1;
     1.5 ms in the browser. 10-20 h.
   - Both platforms.
5. **Guest functions still run by `ps2_asm`, and `lookup`/`ps2_exec`/`run_fast_program`.**
   - These are 12% of the x64 EE and 13.6% of the browser game worker.
   - Find the hot asm-run guest functions (a guest-PC count in `ps2_asm`) and recompile or
     decompile them; shorten the dispatch.
   - Estimate -5-8%. 10-20 h. Both platforms.
6. **VU0 signature `memcmp` and the hot getenv.**
   - Compare the VU0 code words only when VU0 micro memory may have changed (a write epoch on the
     VIF0 MPG and EE store paths into it), and cache the getenv.
   - Share: -4.5% of the x64 EE today (1.7% after item 1), -1.7% of the browser game worker.
   - 2-4 h. Both platforms.
7. **GS front end in native mode** (x64 GS thread 3.5 ms, browser 5.2 ms).
   - The exact GS still emulates local memory for uploads (`gs_write_pixels`, `pixel_address`,
     `write_pixel`, `native_exact`) for the Z probe. Limit that to what the probe reads, and skip
     `native_field_trace`/`native_log` when tracing is off.
   - Estimate -10-20% of the GS side. 8-16 h.
   - Both platforms, but off the critical path on x64 until items 1 and 4 are done.
8. **Renderer upload format** (indexed strips or a smaller vertex).
   - Browser `writeBuffer` is 6.2% of the graphics worker, about 3.9 MB of vertices a field;
     halving it is -3%.
   - x64: 0 (the render thread works 1 ms a field and is never the limit).
   - 6-10 h; the picture ABI and shaders are locked files.
9. **VU1 with IEEE floats and SIMD in native mode on x64.**
   - About 0. On x64, VU1 already runs as SSE under the VU's MXCSR (toward zero, FTZ, DAZ), so its
     rounding costs nothing. IEEE would only swap `operand4`'s opaque barrier for `ieee_clamp4`,
     which has about the same op count.
   - The browser needed BS1 only because it emulated toward-zero rounding in binary64. 2-4 h,
     x64 only. Not worth doing.
10. **VU1 plus GS primitive setup as wgpu compute shaders.**
    - Browser: removes up to 13.6 of the 16.3 ms in the graphics worker (VU1, VIF1, GS front end),
      which leaves the game worker as the only limit.
    - x64: moves about 7 ms of VU/GS CPU off the CPU, but it is off the EE path until items 1 and 4.
    - 150-300 h and high risk: the VU1 microprograms branch and XGKICK in order, the GS state is
      sequential per draw, and the exact Z-probe readbacks have to stay exact. Only worth it for
      120 in the browser once items 2-6 are in.
    - Both platforms.
11. **wasm2c NaN canonicalisation** (`wasm_quietf`, 5-8% of the EE's samples): measured no gain
    (the quiet variant). Drop.

Likely outcomes:
- **x64:** items 1 and 6 bring 144 within reach. Items 4 and 2 then give the margin, including
  the p99.
- **Browser:** items 3, 2 and 4 bring 60. 120 needs all of 2-6 plus 10, or a much faster core.

## Raw data

- `x64/<run>/`: game.log (bench lines), profile.json (every sampled PC), report.txt, areas.json,
  timeline.csv, gpu.csv.
  - Runs `s2-time`, `s2-prof`, `s8-time`, `s8-prof`, `s8-stats` (`DHD_PROFILE`), `s8-snd-prof`,
    `s2-vu0` (`DHD_VU0_STATS`), `bench60-s2`, `ab-*-N` (the A/B), `mem-s8-prof`.
- `exp/{mem,mem2,quiet}/`: the variant builds.
- `web/{paced,unpaced}-{worker,graphics}.cpuprofile` (open them in DevTools), `*.txt`/`*.json`
  summaries, histories, screenshots.
- `tools/`:
  - `hidden_run.py`, `x64run.sh`, `quiet.sh`, `ab.sh`: hidden-desktop runs and the build check.
  - `sample_profile_raw.py`, `x64prof.py`, `pe_exports.py`: the sampler and its analysis.
  - `drive.mjs`, `webprof.py`: the browser runs and their analysis.
  - `variant_build.py`, `mem2_patch.py`: the variant builds.

Sources: findings 051, 066 (VU0 AOT, the i686 race numbers), 082 (why the fold exists), 086 W4 and
087 (browser methods); `port/tools/sample_profile.py`; Chrome DevTools Protocol
(Target.setAutoAttach with flatten for nested workers, Profiler, Fetch); Microsoft docs for
CreateDesktopW, STARTUPINFO.lpDesktop, SetProcessInformation(ProcessPowerThrottling) and
QueryThreadCycleTime; nvidia-smi's query options. 
