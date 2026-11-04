# 030 - Round 22: VU1 recompiler experiments (2026-10-15)

Status: current
Supersedes: parts of 029
Checked: 2026-11-04

Continuation of finding 029. Starting commits: 9cc58bf8 (MTVU default),
a087f597 (zero name buffer), e714d282 (whole-register vector moves).
The acceptance threshold is at least 3% faster in the 32-capture replay bench,
with unchanged results. Rejected prototypes stay in ignored build output only.

## Oracle case accounting

The default suite is 33 seeds (0 and 2..33) for 32 entry fixtures and 16
mesh draw fixtures: 33 * (32 + 16) = 1584. Positional capture arguments add
one case each. The earlier 1616-case run included the 32 files in
`build/port/vu-captures-race`. No generated fixture was removed. These
experiments explicitly include all 32 captures and check 1616 cases.

## Step 1: block-local SSE registers - dropped

The prototype specialized the interpreter's VF access helpers for a nonescaping
`__m128 regs[32]` local, with constant indices. Registers loaded on first possible
use, flushed at block exit, errors, XGKICK callbacks and enabled per-pair traces.
Instruction semantics came from the existing helpers, including partial writes
and the upper/lower pair ordering.

Same session, 32 captures x 3000:

- Baseline: 1.277 / 1.267 / 1.265 s.
- Locals: 1.485 / 1.426 / 1.487 s (slower).
- All hashes: `6DB1972798276582`.
- PCSX2 oracle: 1616 cases, zero failures; synthetic instruction/timing tests pass.

Dropped under the 3% rule. The conservative load/writeback policy did not pay off in this i386 build. No production code is retained.
Race/image gates and paced measurements were not run for this rejected prototype;
they cannot establish a performance benefit missing from the bench.
Local evidence: `build/port/bench/r22base`, `r22locals`,
`build/port/tmp/r22locals-oracle.log`, `r22locals-emitter.py`.

## Step 2: generation-time dependency and Q/P bounds - dropped

The generator decoded all source-component masks and tracked the issue timestamp
of each in-block VF write. Later source stalls used that timestamp plus four,
instead of reloading component-ready memory. Q and P issue latencies were also
tracked as constant offsets from their producer's issue time; unknown incoming
pipeline state retained runtime checks. Q publication and flag ordering stayed in
the reference helpers. No arithmetic changed.

- Bench: 1.301 / 1.287 / 1.278 s, against 1.277 / 1.267 / 1.265 s.
- Hash unchanged: `6DB1972798276582`.
- PCSX2 oracle: 1616 cases, zero failures; synthetic timing tests pass.

Dropped: no 3% gain. No race/image or paced measurement of this rejected prototype.
Evidence: `build/port/bench/r22timing`, `build/port/tmp/r22timing-emitter.py`,
`build/port/tmp/r22timing-oracle.log`.

## Step 3: whole-overlay flag liveness - analysis only, dropped

A conservative CFG analysis traversed both overlays (1803 and 1613 pairs),
including delay slots, conditional successors, indirect exits, flag readers,
XGKICK observers and drained end state. It tracked replacement age (at least
four issued pairs), MAC replacement, sticky-status reset by FSSET, and clip
history replacement by FCSET or four subsequent CLIPs. Unknown indirect paths
and independently entered delay slots remain live.

It found **zero complete MAC/status or clip updates** safe to remove under this
model. Intermediate MAC bits can be dead while sticky status remains live.
Removing that complete update would change the oracle's end state. Therefore
no deletion was emitted and arithmetic4/clamping is untouched. This is not a
proof that a more precise per-bit analysis or flag coalescing cannot help.

Dropped before code generation: this conservative pass emits identical code
(0% expected gain). No distinct fps or oracle result is claimed for it.
Local analysis: `build/port/tmp/r22flagcfg.py`.

## Step 4: larger blocks with fused branch/end delay slots

The block limit increases from 8 to 32 pairs. A simple delay slot following a
branch or E bit runs in the same generated function (at most 33 pairs). The
branch decision is saved before the delay slot; the run loop then applies the
completed transition immediately, or ends the program after an E slot.

- XGKICK remains a boundary, since its callback may inspect/change microcode.
- A slot containing another control transfer, E or XGKICK retains the existing
  one-pair transition path. Independently entered delay slots remain available.
- Exact code-word validation, interpreter fallback, issue cycles, Q/P, flags,
  end state, trace points and instruction-budget checks remain intact.
- Unique generated functions: 906 -> 718.

Validation:

- Bench (32 captures x 3000): 1.210 / 1.217 / 1.196 s, versus the baseline's
  1.277 / 1.267 / 1.265 s; about 5% faster by the medians.
- Hash unchanged: `6DB1972798276582`.
- PCSX2 oracle: **1616 cases, 32 entries, zero failures**, including 32 race
  captures; synthetic instruction/timing/continuation tests pass.
- Six generator boundary regressions pass (`port/tools/test_vu1_aot.py`).
- Race dumps at GO+0/+2/+12/+16 match ref22 using the existing host-pointer and
  0x2B461C exclusions. These are race-clock ticks (60/s), not elapsed seconds.
- All four race BMPs and four menu BMPs are byte-identical. Menu captures are
  at fields 600/660/720/780 with the menu navigation prefix.
- The image gate prints `IMAGE CHECK MATCH`; `python tools/check_image.py
  build/dhd.elf` also prints `MATCH` for all eight retail sections.

The first private build accidentally replaced the earlier `--unit` list with
a second option, omitting the race seed hook. Its seed was 81958 instead of
81257 and the comparison correctly failed. Rebuilding with all six intended
units in one list restored the hook. No tolerance or reference changed.

Evidence: `build/port/r22branch/dhd.exe`, `build/port/bench/r22branch`,
`build/port/tmp/r22branch-oracle.log`, `build/port/compare/r22branch-fixed`,
`build/port/tmp/r22branch-fixed-gates.log`.

## Paced results and remaining work

Same measurement driver/windows as findings 027-029, sequential runs, with the
user's other PC activity uncontrolled. Menu window: wall 20..40 s; race window:
20..80 s after navigation step 9. Each run ends at its 210 s timeout.

| State | Menu fps, twice | Race fps, twice | Replay bench (3000 repeats) |
|---|---|---|---|
| Inherited r22c (worker default + vector moves) | 36.2 / 31.3 | 42.3 / 42.1 | 1.277 / 1.267 / 1.265 s |
| Step 1 SSE locals, rejected | not measured | not measured | 1.485 / 1.426 / 1.487 s |
| Step 2 static timing, rejected | not measured | not measured | 1.301 / 1.287 / 1.278 s |
| Step 3 conservative liveness, no deletions | unchanged code | unchanged code | no distinct build |
| Step 4 longer blocks + fused slots, retained | **38.9 / 36.3** | **43.7 / 43.9** | **1.210 / 1.217 / 1.196 s** |

Both paced runs completed their bounded interval without a crash. Their exit
status is from the driver's deliberate taskkill, not an application fault.
Rejected steps were stopped at the bench/analysis gate, so no distinct paced
fps is claimed for them. The retained step meets all exactness gates.

Executable SHA-256:
`2f4b4b3cbc3e267bb61d3807bbddf32baf4589d63534e18df9f0829fea1cf858`.
Paced logs: `build/port/speed/r22branch-1`, `build/port/speed/r22branch-2`.

A separate 20 s external worker-thread sample began **after** the second run's
fps window: 5743 samples, zero context-read failures. Of sampled worker time:
VU 52.9%, GS/renderer CPU 16.5%, VIF 9.4%, worker consume wait 6.4%, identified
flush/draw waits 1.4%, unattributed external code/waits 13.0%, CRT ~0.3%.
This is worker-thread sampling, not a complete frame-time decomposition.
Evidence: `build/port/tmp/r22worker.profile` and `r22worker.log`.

**60 fps is not reached.** Next: measure wall time per worker stage and assess
VU1/GS overlap against its fence/copy cost. More exact flag optimization needs
per-bit liveness/coalescing that preserves sticky state; the conservative
whole-update elimination attempted here is insufficient. A new VF-local
attempt would need precise read/write sets and dirty tracking, rather than
revisiting this rejected conservative cache unchanged.

Reference semantics and gate: the existing pinned PCSX2 interpreter adapter,
[PCSX2 VU1microInterp.cpp](https://github.com/PCSX2/pcsx2/blob/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2/VU1microInterp.cpp)
and [VUops.cpp](https://github.com/PCSX2/pcsx2/blob/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2/VUops.cpp).
No upstream implementation was copied.

## Round 23: program-level profile

New opt-in `DHD_VU1_PROFILE` writes per-window CSV keyed by overlay, original
MSCAL entry, current entry and MSCNT flag. XGKICK/GS callback time is measured
separately and subtracted from VU time. Windows count presented frames once,
not once per key. `port/tools/profile_vu1.py` summarizes the CSV.
`port/tools/bench_vu1_programs.py` strictly validates and replays each local
capture, reporting VU and GIF time separately (no silently skipped fixtures).

Paced live race, wall seconds 90..150: 2504 presented frames / 60.000 seconds,
41.73 fps with profiling enabled. This is a profiling run, not an acceptance
speed measurement. Leading keys:

| Overlay / entry / current / MSCNT | Calls/frame | VU ms/frame | Share of wall frame | GIF ms/frame |
|---|---:|---:|---:|---:|
| B / 0078 / 1040 / yes | 516.17 | 4.5824 | 19.12% | 1.5508 |
| B / 0078 / 1CB0 / yes | 111.30 | 1.2074 | 5.04% | 0.7245 |
| B / 1638 / 1040 / yes | 78.55 | 0.8278 | 3.45% | 0.2369 |
| B / 17E0 / 1040 / yes | 82.41 | 0.6325 | 2.64% | 0.2568 |
| B / 0040 / 1040 / yes | 112.79 | 0.5979 | 2.50% | 0.5446 |
| B / 0020 / 0020 / no | 11.31 | 0.5922 | 2.47% | 0 |

The equal-weight replay is not the live workload distribution: its expensive
single captures include B0010/B0020 (57.07/55.33 us, skin preprocessing) and
A29C0 (47.15 us), but these are not the largest live frame costs. B1040's
captured invocation costs 13.29 us VU + 0.88 us GIF. Original MSCAL history is
not in the old capture format; this B1040 capture has VI13=0x11, the dispatch
selected by B0078. Live CSV supplies the actual call distribution.

Selected target: B0078's B1040 continuation, whose VI13 dispatch enters the
geometry path at 0x0088. The hot loop 0x0170..0x0258 transforms vertices,
builds texture/colour/position output and computes culling/clip state. Native
work will be guarded, keep exact AOT setup/clipping fallback/GIF submission,
and preserve complete persistent state. No visual approximation is selected.

Profile evidence: `build/port/r23profile/programs.csv`,
`build/port/vu-program-bench/programs.csv`. Profiling run completed without a
crash. The code and VIF upload disassembly were read from the user's local
microcode; no game instructions are committed.


## Round 23: native geometry prototype rejected

The guarded SSE prototype specialized B0078's B1040 continuation, retaining
AOT setup, clipping and packet submission. It speculated over 0x0170..0x0258
using local VF/ACC vectors and a private copy of the 16 KB VU memory, then
committed the final state only if every vertex avoided the clipping path.
No approximate output or guest-state relaxation was selected.

This was the wrong speculation boundary for the captured workload. All ten
attempts fell back: the first seven encountered the clipped vertex at memory
index 731 (starting at 713, 716, ..., 731); subsequent attempts hit 734, 737
and 740. Thus preceding vertices were recomputed repeatedly before AOT did
the same work. **Zero native commits were exercised** by the 32 fixtures.
The prototype was removed from the source tree, not enabled by default.

Same executable, alternating native=0/1, 100,000 repeats of B1040 per sample,
with diagnostic logging disabled; VU time excludes the GS callback:

| Trial | Exact AOT us | Prototype us | AOT / prototype |
|---|---:|---:|---:|
| 1 | 13.143930 | 18.053894 | 0.728x |
| 2 | 12.782364 | 17.799994 | 0.718x |
| 3 | 12.744249 | 18.191460 | 0.701x |

Median 12.782 -> 18.054 us, about 41% slower. The required 3x program speedup
fails, so the experiment stops at this gate. No candidate paced fps, 10%
whole-frame gain, live candidate readbacks/images, split-screen comparison,
or new reference-dump pass is claimed. Last accepted paced race remains
43.7/43.9 fps (menu 38.9/36.3), from round 22. The separate profiling run is
41.73 fps with instrumentation and is not a second acceptance measurement.

`compare_vu1_native.py` runs both switches in fresh processes with identical
capture inputs. The replay harness records every XGKICK address and complete
GIF qword stream, then the entire ps2_cpu (only two host pointers zeroed),
16 KB VU memory and CPU GS state. All 32 captures / 64 sections MATCH. This
covers fallback equivalence only; it does **not** establish the correctness
of an accepted native loop. The tool explicitly warns that native coverage
must be checked separately. With no GPU backend attached, its GS snapshots
also do not establish rendered pixels or real Z/colour readback equivalence;
those still require live gates before any future native implementation ships.

The opt-in profiler and reusable snapshot comparator are retained. No native
kernel or temporary probe is committed. Local evidence (not tracked):
`build/port/vu-native-bench/repeat-*.csv`,
`build/port/vu-native-compare/snapshots/results.json`,
`build/port/vu-native-reasons/reasons.txt`. Rejected source is archived only
under `build/port/vu-native-bench/rejected-source`.

Next proposal for review: handle clipping inside the native kernel, or commit
completed vertices at a precisely reconstructed AOT boundary rather than
rolling back the entire loop. First obtain representative unclipped and
clipped captures with positive native-path coverage. Keep the full state and
readback requirements; do not spend live race runs on a known slower kernel.
Stop for review as required by the failed go/no-go gate.


After removing the prototype, the AOT oracle passes **1616 cases, 0 failures**:
1584 generated fixtures plus all 32 captures. The former 1584-only invocation
omitted the captures; no generated cases were removed. Full replay benchmark:
1.201 / 1.216 / 1.212 s, hash `6DB1972798276582` unchanged. Retail image check
prints MATCH for all eight sections. Logs: `build/port/tmp/r23-final-oracle.log`
and `r23-final-bench.log`. No dhd.exe remains running. No new normal game build
or menu/race image gate was run after rejection; the accepted round-22 visual
and state evidence remains the last live evidence.


## Round 24: unclipped native ceiling

The round-23 failure measured speculation/fallback, not native throughput.
The new isolated benchmark captures the first B1040 loop-entry pipeline state
at 0x0170, widens VF13.w to FLT_MAX on both paths, and executes only through
0x0258. Microcode and the AOT blocks are unchanged. Input reset is outside the
timed region. Native writes directly to VU memory, with no memory snapshot,
CPU copy, rollback, or fallback. This fixture is a ceiling experiment, not a
replacement for the captured race or its reference images.

100,000 calls per sample, ten vertices/call, alternating AOT/native:

| Trial | AOT ns/vertex | Native ns/vertex | Speedup |
|---|---:|---:|---:|
| 1 | 193.388 | 56.564 | 3.42x |
| 2 | 188.079 | 56.896 | 3.31x |
| 3 | 185.604 | 56.948 | 3.26x |

All calls complete, native coverage 100% of these vertices. CPU, 16 KB memory,
and drained MAC/status/clip flags match the AOT result; both execute 293 pairs.
The 3x feasibility gate passes. This is not live native coverage or a full
program speedup. Tool: `port/tools/bench_vu1_geometry.py`; local output:
`build/port/geometry-ceiling/result.txt`. The pipeline capture hook compiles
only under PS2_VU_GEOMETRY_BENCH and is absent from game builds.


### Round 24: full clipped replay

A second prototype writes directly into VU memory and ports the clipping
control flow (1948..1AD8, 1CC0..2730) into a generated, directly threaded C
function. It has no AOT dispatch/fallback inside that function and never
restarts the geometry loop. It still uses the exact SIMD arithmetic, divider
and register-readiness helpers. Thus this is a real execution measurement,
but not a claim that the clipping code has reached its attainable ceiling.

The initial version preserves clipping's flag pipeline. Same executable,
100,000 B1040 invocations per sample, alternating native off/on, VU-only time:

| Trial | AOT us/program | Native us/program | Speedup |
|---|---:|---:|---:|
| 1 | 12.850678 | 11.041071 | 1.16x |
| 2 | 12.926597 | 10.974126 | 1.18x |
| 3 | 12.769796 | 10.906201 | 1.17x |

All 32 captures / 64 GIF and state sections MATCH. Positive coverage on the
target capture: one call, ten vertices, four clipped vertices, zero rollback,
zero fallback. The other 31 fixtures are outside the selected entry/dispatch;
their matching output is not counted as native coverage. Evidence:
`build/port/r24-native-check/snapshots`, `build/port/r24-native/repeat-*.csv`.

A follow-up removes the clipping MAC/CLIP queue where every consumer sees the
latest value: MAC read 1970 follows SUBA 1950 by four issue slots; CLIP read
1A58 follows 1A38 by four slots, later FCORs see that same result; FCGET 2600
runs through 25F8 after the caller's branch/delay slot, sufficiently after its
CLIP. No status reads occur in this closed clipping routine. Sticky MAC/status
bits remain exact. The generator asserts the complete flag-read address set.
This removes flag queue bookkeeping, not arithmetic clamping or divider timing.
Initial result: AOT 13.030 us, candidate 10.411 us, approximately 1.25x, still
below the required 3x full-program gate.

The production build does not include the experiment: it requires the explicit
PS2_VU_NATIVE_GEOMETRY_BENCH define. `bench_vu1_programs.py --native-geometry`
compiles it; add `--clip-pipeline` for the original flag-queue version. The
native env switch is then used by the comparator. Normal AOT compilation is
unchanged. Local private live executables were built before applying this
compile-time containment, solely to measure coverage and fps.

### GIF/GS inspection

B0078/1040's measured 1.55 ms/frame is the entire XGKICK callback, including
GS decode, primitive submission and backend work; it is not all packet parsing.
The isolated callback with no GPU backend is about 0.8 us per captured program,
so that replay cannot establish a 1.55 ms saving on the live renderer.

The implementation already batches triangles, caches texture-page sets and
suppresses unchanged GS state-version updates. A candidate inexpensive change
is caching the palette independently of unrelated draw-state changes: gpu.c
currently rebuilds all 256 palette entries whenever state_version/context
changes. Correct invalidation must include CLUT/TEX0/TEXA and uploads. Another
candidate is a full-qword GIF feed path, preserving partial-feed semantics and
record_transfer callbacks; compiler inlining may already remove much of the
word-loop overhead. Neither has a measured speedup here. Removing flushes at
texture feedback/aliasing hazards would not be a safe cheap optimization.
No GS implementation change is made this round.


### Round 24 final results and decision

The final benchmark-only version (early clipping flags) again matches all 32
captures / 64 sections. Target coverage is 10/10 native vertices, including
4 clipped, with no rollback or fallback. Three repeated program measurements:

| Trial | AOT us/program | Native us/program |
|---|---:|---:|
| 1 | 12.835771 | 10.205028 |
| 2 | 12.889092 | 10.197597 |
| 3 | 12.884878 | 10.417307 |

Median speedup **1.263x**, not 3x. Evidence:
`build/port/r24-final-native/repeat-*.csv` and `snapshots/results.json`.

Two sequential 210-second paced live runs used the original clipping flag
pipeline version, before the benchmark-only compile guard was added. Both
complete without a crash. Measurements use the established windows: menu
wall 20..40 s, race 20..80 s after navigation step 9. No compilation or replay
benchmark ran during those windows (compilation began only after the second
race window or during the tail after the first window).

| Implementation | Menu fps | Race fps |
|---|---:|---:|
| Last accepted round-22 AOT, earlier runs | 38.9 / 36.3 | 43.7 / 43.9 |
| Direct-write native + clipping pipeline | 47.0 / 47.3 | **46.0 / 46.0** |

The race result is about 5% above the earlier accepted runs, not the required
10%. PC background activity remains uncontrolled; menu navigation traverses
the fixed wall window, so its difference is not a steady-scene speedup claim.
The early-flag variant was stopped at its failed program-speed gate and was
not given a separate live fps claim.

Last flushed live coverage counters:

| Run | Native / target calls | Native vertices | Clipped vertices |
|---|---:|---:|---:|
| 1 | 4,444,160 / 4,444,160 | 170,858,886 | 11,944,050 |
| 2 | 4,423,680 / 4,423,680 | 170,080,828 | 11,924,742 |

Coverage is **100% of targeted calls and their vertices at these checkpoints**,
including menu/loading and race. Intermediate race checkpoints also show
native_calls == target_calls. Every completed call writes all its vertices;
clipped vertices use the directly threaded clip function, not an AOT fallback.
The counters flush every 4096 calls, so these are checkpoint totals rather
than exact shutdown totals. Race logs are `build/port/speed/r24-native-1` and
`r24-native-2`; zero rollback/fallback and no application crash were reported.

Native-off production AOT: **1616 cases, zero failures** (1584 generated plus
32 captures), benchmark **1.200 / 1.193 / 1.195 s**, unchanged hash
`6DB1972798276582`. Retail ELF image: MATCH, all eight sections. Evidence:
`build/port/tmp/r24-final-oracle.log`, `r24-final-bench.log`.

**No-go on performance.** The experiment is retained behind a benchmark-only
compile define; it is absent from normal game builds and is not the default.
GO+0/+2/+12/+16 live dump/image comparison and lens-flare readback comparison
were not run after the speed gates failed; no new live exactness claim is made.
The 32-capture comparator has no GPU backend, so its state MATCH cannot replace
those gates. No split-screen acceptance claim is made. No later key was changed.

Next proposal: keep the measured SIMD loop and replace the remaining clipping
pipeline/register-memory bookkeeping with a more specialized SIMD clip kernel,
with exact Q and persistent flags preserved. The unclipped result establishes
that native execution can help; the clipped implementation here does not yet
meet the program or frame-time requirements. Stop for review.

## Round 25: native geometry in the normal build

The round-24 feasibility result is accepted: its ten-vertex, four-clipped
capture overweights clipping compared with the live workload (about 38 vertices
per call, 7% clipped). The live VU milliseconds per presented frame, rather
than that single capture's program ratio, now decide whether to retain a kernel.

The direct-write B0078/1040 kernel is compiled normally, default on;
`DHD_VU1_NATIVE=0` selects the exact AOT referee. Trace mode retains AOT.
The generated microcode signatures still guard the loop and both clip regions.
No generated instruction words or captures are committed.

Live acceptance (`build/port/compare/r25-base`, `gate_vu1_native.py`):
- All four EE dumps at GO+0/+2/+12/+16 match ref22 and native-off under the
  existing ref22 host-pointer/run-varying-word exclusions.
- Four race BMPs and four menu BMPs are byte-identical on/off.
- Exact readback transcripts match: 669 records, all Z reads, through virtual
  GS field 3291; the last race checkpoint is field 3231. The transcript includes
  metadata and bytes, not only hashes or values rounded to text.
- Native-off PCSX2 oracle: 1616 cases, zero failures. This is 1584 generated
  cases (33 seeds times 48 cases) plus 32 captured entries; none are skipped.
- Native-off three benchmark hashes remain `6DB1972798276582` (1.333, 1.245,
  1.286 seconds; compilation overlapped part of this correctness run, so these
  times are not a controlled performance comparison).
- Retail image check: all eight sections MATCH. No application crash.

The live gate is reusable and bounded; it stops each process tree before
starting the next, checks the four existing reference moments, and requires
readback coverage through the last captured virtual field.


Paced measurements, sequential runs of the same `r25-base` executable, no
compilation/benchmarks in the measurement windows:

| Mode | Menu fps (two runs) | Race fps (two runs) |
|---|---:|---:|
| Native off | 35.9 / 32.5 | 42.9 / 43.0 |
| Native on | 46.4 / 47.0 | 45.8 / 45.7 |

Race gain is about 6.5%; retain the exact kernel as the default. Menu windows
cross navigation states and are not a steady-scene performance claim. Timing
windows remain menu wall 20..40 s, race 20..80 s after navigation step 9.

### Live per-program measurement and worker occupancy

Matched 90..150 wall-second profiling windows (`r25-live-profile/off` and
`on`) use the same executable, and exclude representative-capture I/O.

| Key | Calls/frame off / on | VU ms/frame off / on |
|---|---:|---:|
| B0078/1040 | 528.10 / 537.48 | 4.7476 / 2.1617 |
| B0078/1CB0 | 114.46 / 116.80 | 1.2558 / 1.2777 |
| B1638/1040 | 85.31 / 90.15 | 0.8682 / 0.8754 |
| B17E0/1040 | 84.53 / 85.87 | 0.6701 / 0.6868 |
| B0040/1040 | 101.99 / 100.68 | 0.5116 / 0.5058 |

The retained kernel cuts its live VU cost by 54.5% (2.20x), with slightly more
calls per presented frame in the faster run. Whole-worker wall busy time is
90.30% off and 86.26% on; thread CPU occupancy is 94.35% / 90.47%. These are
different measures: busy accounting excludes queue/event bookkeeping, whereas
GetThreadTimes includes it. Native-on GS/backend callbacks total 18.160 s in
60 s, or 6.637 ms/frame. The worker remains a credible limit, so a second FIFO
for GS/D3D will be prototyped with explicit fences at every EE readback.

### Additional geometry loops and representative captures

The sampler retains four 32..48-vertex inputs per dispatch/continuation after
the profile window. The four B0078/1040 inputs contain 167 vertices, 12 clipped
(7.19%): 44/6, 35/0, 44/0, 44/6. Local samples are in
`build/port/r25-representative`; no game data is tracked.

The kernels cover the 0170, 1708, 18B0, 1110 and 13A0 loops. B0078/1CB0
reuses 0170. B1638 and B17E0 share the 1948 clip routine with distinct return
boundaries; B0040 retains the prior dispatch, including the 20C/260 lighting
loops. The 13A0 loop has no clip branch. Each kernel writes directly to VU
memory and retains exact VF/VI/ACC/Q/I and flags; code signatures guard entry.
`DHD_VU1_NATIVE_MASK` selects bits 1 (0170 via 1040), 2 (0170 via 1CB0),
4 (1708), 8 (18B0), 16 (1110), 32 (13A0), for isolated comparisons.

All original 32 captures and all 32 new captures match GIF and persistent
state. The new replay's counters prove execution of all six selected paths,
including clipped paths, with no rollback or fallback. Mean VU replay times
across four representative inputs, microseconds per program (not live gates):

| Dispatch / continuation | AOT | Native |
|---|---:|---:|
| 011 / 1040 | 9.163 | 7.267 |
| 011 / 1CB0 | 10.021 | 7.090 |
| 2C9 / 1040 | 8.887 | 6.921 |
| 2FE / 1040 | 5.939 | 4.390 |
| 20C / 1040 | 7.753 | 6.191 |
| 260 / 1040 | 5.393 | 3.986 |

Live gates for all six paths (`build/port/compare/r25-all`): four reference
EE dumps MATCH under existing exclusions, four race and four menu BMPs
byte-identical, 670 exact Z readbacks through virtual field 3292 MATCH. All
reported per-key live native call counts equal target counts, with no fallback.
The separate 1CB0/1638/17E0-only gate also matches (`r25-extra`). Native-off
oracle remains 1616/0 and all three bench hashes remain `6DB1972798276582`;
retail image remains MATCH in all eight sections.

Live VU costs with all kernels enabled (60 s, 2759 frames):

| Key | Calls/frame | VU ms/frame, original native / all native |
|---|---:|---:|
| B0078/1040 | 540.83 | 2.1617 / 2.3703 |
| B0078/1CB0 | 117.68 | 1.2777 / 0.6544 |
| B1638/1040 | 91.81 | 0.8754 / 0.4980 |
| B17E0/1040 | 86.48 | 0.6868 / 0.4492 |
| B0040/1040 | 100.04 | 0.5058 / 0.3032 |

The original kernel becomes slightly slower in the larger executable/profile,
but the added keys each improve, and total VU cost falls from 9.606 to 7.973
ms/frame (AOT-only was 12.202). Worker busy is now 79.89%, CPU occupancy 83.41%,
GS/backend 6.738 ms/frame. Samples cover slightly different virtual progress
within the same wall window; calls/frame are shown to expose that variation.

Expanded-native paced race: **46.3 / 46.3 fps** (menu 49.2 / 49.2), versus
original-native 45.8 / 45.7 and AOT-only 42.9 / 43.0. All runs are sequential,
without compilation or replay benchmarks during their measurement windows.
Retain the added exact kernels; the default mask is now 63. The gain remains
modest at whole-frame level, so the GS/VU overlap experiment is the next step.

### Separate GS worker prototype

Opt-in `DHD_GS_WORKER=1` adds a copied GIF FIFO after VIF/VU execution.
VU1 retains ownership of its memory/registers; XGKICK copies each complete
EOP-delimited packet before that memory can be reused. PATH1/2/3 packets keep
one producer order. Per-draw provenance travels in the packet rather than
being read from shared live VIF fields. The GS worker alone consumes queued
GS work; EE presentation and readbacks run only after both FIFOs retire.
`sceGsExecStoreImage` also fences the DIRECT packet that sets up its readback.
FINISH/SIGNAL/LABEL remain ordered GS operations, and virtual vblank still
fences both workers. Suspect-capture mode retains the single worker because
it synchronously inspects GS counters inside the VU callback.

Design references, no source copied:
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MTVU.cpp
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MTGS.cpp

`test_split_graphics.py` passes copied-packet reuse, FIFO order, provenance,
4-KB ring wrap, 32-bit publication-counter wrap, producer backpressure,
wrapped VU-memory XGKICK and completion fences (12,000 recorded qwords).
The VIF integration test also passes uploads, MSCNT, DIRECT and exact depth
readback. These tests do not replace the live image/state/readback gate.

Split-worker live gate (`build/port/compare/r25-split`): four EE checkpoints
match ref22 and split-off, four race and four menu BMPs are byte-identical,
675 Z readbacks match through virtual GS field 3297. No crash or unexplained
state difference. Paired paced timings, same executable and sequential runs:

| GS worker | Menu fps | Race fps |
|---|---:|---:|
| Off | 49.4 / 43.0 | 46.2 / 46.2 |
| On | 55.5 / 53.6 | 47.0 / 47.1 |

Retain the exact, repeatable gain (~1.8%) and enable the GS worker by default;
`DHD_GS_WORKER=0` keeps the prior single worker. This is far less than the
previous estimate, and the late-race section still runs near 43 fps. A
main-thread frame breakdown is being measured before further queue changes.

Split profile (`r25-live-profile/split`, 60 s / 2783 frames): VIF/VU worker
busy 57.90%, CPU 62.39%; GS worker busy 30.75%, CPU 37.11%. Main-thread
virtual-vblank work totals only 0.809 s, including 0.144 s pacing and 0.288 s
presentation. 59.186 s falls between vblanks, so pacing/presentation is not
the late-race limit. Statistical late-race samples (120..150 s, 5166 samples)
show 39.0% in external work with graphics_wait as the first possible game
return address, and 35.8% with ps2_vu0_run. These are sampled candidate return
addresses, not a fully unwound call tree.

The VU0 VU1-register snapshot currently uses the full graphics fence. A
VU-only fence is safe to test because the GS queue owns copies of its GIF
inputs and never reads live VU registers. `DHD_VU_FENCE_ONLY=1` selects that
snapshot fence while all GS readbacks, presentation and control synchronization
retain the full fence. The queue test now deliberately blocks the GS consumer
and verifies the VU fence returns, then releases GS and checks the full fence.

The narrower VU snapshot fence measured **46.7 / 46.9 fps**, versus
47.0 / 47.1 with the retained full fence. It did not improve performance and
was removed before committing. No live exactness gate was claimed for this
rejected probe. The main-thread timing instrumentation remains available as
DHD_FRAME_PROFILE; full synchronization remains the production behavior.

### Palette cache measurement and final decision

The opt-in prototype cached the converted 256-entry palette using CPSM/CSA,
TEXA and an exact copy/compare of the 512-entry loaded CLUT. The existing
D3D11 WARP raster tests passed, including palette/readback cases. Two paced
runs of the same executable and GS/native settings measured **47.1 / 46.9
fps** (menu 54.0 / 55.6), versus cache-off **47.0 / 47.1**. No repeatable gain.
The prototype was removed; no GS palette-cache change is retained or committed.
Its live image/readback gate was not run after this failed performance gate,
and no broader exactness claim is made for the rejected cache.

All requested candidate keys were evaluated and retained only where live VU
cost decreased. The retained split is exact but small; neither the narrower
fence nor palette cache improves paced performance. Steady 60 fps is not yet
reached. Next work should measure waits at the specific EE/VIF/GS synchronization
sites and GPU readback/feedback boundaries, then optimize a measured critical
path. Do not remove authoritative-state fences based only on aggregate waits.

### Final-build diagnostic isolation

The first clean rebuild with main-thread frame profiling compiled in failed
the unchanged ref22 RAM gate: 493 words at GO+0. Every differing word had an
identical low 16 bits and was the union value in a gzip Huft table. The prior
word's e/b fields identify literal/length entries; func_00240CB8 in the matched
00240270 unit writes `r.v.n` (16 bits) into an uninitialized local union, then
copies the whole Huft. Its unused upper half therefore exposes host stack
history. All four race BMPs and 669 readback records still matched the accepted
build exactly. This is not evidence of a renderer or decoded-value difference.

The strict comparison was not widened. Main-thread frame instrumentation is
now compiled only with `DHD_FRAME_PROFILE_BUILD`; the runtime environment
variable selects its output only in such diagnostic builds. Normal builds
compile every frame mark away. The gzip padding issue itself is left for a
separate determinism fix with an explicitly reviewed canonical reference;
no magic padding values or new comparison exclusions are introduced here.

With the frame probe compiled out, the clean normal build again passes the
strict unchanged gates (`build/port/compare/r25-clean`): all four EE moments
match ref22 and native-off, all eight BMPs are byte-identical, and 671 exact Z
readbacks match through virtual field 3293. The optional diagnostic build was
also compiled with DHD_FRAME_PROFILE_BUILD after isolation. Final native-off
oracle is 1616/0; the bench hash remains `6DB1972798276582`; all 64 original
and representative GIF/state replays match. The retail image matches all eight
sections, and both queue/VIF tests pass. No accepted gate is waived.

### Round 25 final normal build

`build/port/r25-clean/dhd.exe`, no probe options: paced race **46.9 / 47.0 fps**,
menu **55.2 / 54.6 fps**. Both 180-second runs complete without an application
crash. No compilation or replay benchmark overlaps either measurement window.
The later race still runs near 43 fps; this is not a steady-60 result.

Final native-plus-split live VU profile (2783 frames):

| Key | Calls/frame | VU ms/frame |
|---|---:|---:|
| B0078/1040 | 546.89 | 2.2453 |
| B0078/1CB0 | 119.28 | 0.6321 |
| B1638/1040 | 94.97 | 0.5490 |
| B17E0/1040 | 87.87 | 0.4832 |
| B0040/1040 | 98.88 | 0.3026 |

Per-key GIF callback times in this split profile are queue/copy time, not GS
execution time. The separate GS worker's measured busy time is 6.629 ms/frame.
All test game processes have ended. Round 25 stops for review; the next plan
is the specific synchronization/readback critical path, plus a separately
reviewed canonicalization of unused Huffman-table bytes before changing the
reference. The native and GS referee switches remain available.
