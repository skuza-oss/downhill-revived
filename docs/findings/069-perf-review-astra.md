# 069 — Independent performance review of the combined CPU/FPS build

Status: current
Checked: 2026-11-04

Review date: 2026-10-18. Source: `fbb0fa536df6ee1a7c95fb6084231d1c6b749057` (`test/cpu-plus-fps`).

## Conclusion and interpretation

The strongest additional CPU opportunity is to specialize hot **EE inline-asm templates**, retaining
their exact arithmetic and register side effects. These still pass through a generic instruction
interpreter after parsing has been cached. This is a different layer from the VU0 microprograms
already optimized in finding 066. A second concrete, smaller opportunity is to stop timing VU0 calls
when the timing diagnostic is disabled.

For the crowded opening, VU1 throughput and the time before its packets arrive remain the larger
frame-time levers. Queue spinning is a separate CPU-efficiency opportunity, especially for slower
CPUs. None of the results below establish that every frame can finish within 6.944444 ms. Estimates
are explicitly separated from measured prototype results; the estimated gains cannot simply be added
because the threads overlap.

The supplied 184 FPS / 9.8 ms result is supported by the earlier **f4152bd7** combined-build
campaign, not a timing measurement of fbb0fa53. That campaign's 144-Hz opening was 136.032 real
updates/s, with worst-1% 10.276 ms. Reaching 144 on average needs about 0.407 ms off its 7.351 ms
average, whereas bringing that tail statistic below budget needs 3.332 ms (32.4%).

## Evidence and method

I read the state, local operating notes, build tools, performance history and rejected experiments,
then inspected the EE runtime/transform path, VU native kernels and dispatch, VIF, both graphics
FIFOs, GS parsing, native/exact rendering and readbacks, pacing, high-FPS helpers, sound, input,
disc and offline network boundaries. No external agent was delegated work.

There are three distinct kinds of evidence:

1. **Current-build sampled evidence, independently reanalyzed:** the saved native
   gate trace from fbb0fa53, decoded with that executable's own link map. The
   copied executable SHA-256 is
   `6d2e02b6b8a5e79650ac47f89fa38fa5d0afe0d71245565e8e82f9fe88d28ba4`.
   The gate enables captures/logging and ran on a shared PC. Its IP shares rank
   costs; they are not uncontended CPU-time percentages or fresh FPS results.
   Stack-word callers are heuristics, not unwound stacks.
2. **Earlier combined-build timings, independently reanalyzed:** all three
   Native renderer, sound output off, FPS=60, unpaced, seed 81257, same scripted
   and retain the slower third run. They are not relabeled as fbb0fa53.
3. **New isolated prototypes and checks:** under ignored `build/review/` in this
   checkout. No game data, captures, binaries or generated assembly are committed.
   Thread-cycle measurements use whole batches, not a timer around each helper
   call. They are not converted to milliseconds or FPS.

The shared PC had other games and heavy compilation. Readiness is checked and retried; noisy
microbenchmarks are labeled as such.

### Game-thread samples on fbb0fa53

| Virtual fields | Samples | Generic asm `run` | asm bind/lookup/exec and VU madd | VU0 micro entry | `graphics_wait` | External/wait |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 3000–9100 | 7,044 | 14.7% | 7.1% | 10.4% | 31.9% | 8.5% |
| 3000–3500 | 835 | 7.8% | See whole window | 12.7% | 32.9% | 18.4% |
| 6000–7000 | 1,294 | 13.8% | See whole window | 8.0% | 30.2% | 13.6% |
| 8600–9100 | 615 | 13.7% | See whole window | 8.5% | 41.8% | 5.4% |

Whole-window component shares are `ps2_vu_madd` 2.4%, `ps2_asm` 2.4%, `lookup` 1.6%, `ps2_exec`
0.7%. `memcmp` is another 1.6%; sound `render` is 2.1%. Heuristic callers of the generic asm group
include `00221888` (2.2% of all samples), `0022B028` (1.6%), `00211888` (1.1%), and collision
`00213C08` (1.0%). This identifies targets, not exact inclusive costs.

### Pipeline timing from the earlier combined build

Ranges below cover all three runs; milliseconds per field. Busy means **wall minus instrumented
waits**, so it includes scheduling delay. These columns overlap and must not be summed into a frame
time.

| Window | Wall | EE busy | VU busy | VU idle | GS busy | EE waiting for GS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Opening, 3000–3500 | 7.282–7.609 | 4.305–4.320 | 5.556–5.922 | 1.687–1.726 | 4.513–4.586 | 0.190–0.219 |
| Middle, 6000–7000 | 6.149–6.291 | 3.862–3.907 | 4.317–4.577 | 1.715–1.835 | 4.476–4.494 | 0.761–0.862 |
| Late, 8600–9100 | 5.908–6.203 | 3.371–3.417 | 4.802–5.100 | 1.104–1.110 | 3.839–3.880 | 0.295–0.306 |

On the middle window's worst 1%, EE→GS wait rises to 2.824–3.538 ms and GS busy to 7.348–7.849 ms.
At the opening's worst 1%, VU busy is 7.276–8.441 ms and VU idle 3.461–3.962 ms. Thus neither “all
slow frames are flare reads” nor “all slow frames are one VU kernel” is supported.

In C0's whole window, process CPU is 15.274 ms/field: game 5.271, VU 4.482, GS 4.751, native render
0.297. Wall is 5.422 ms/field. The CPU totals include spinning; they are not 15 ms of serial useful
work. Neither packet ring filled.

## Ranked remaining work

### 1. Specialize the hot inline-asm templates (highest-confidence substantial CPU opportunity)

**Where:** `port/runtime/ps2asm.c` (`lookup`, `ps2_asm`, `run`, `vu_arith`),
`port/tools/transform.py`, and port-only replacements for hot vector/collision helpers. About 21.8%
of current-build whole-window game-thread samples are in the generic asm group. Parsing is already
cached; caching parsing again is not the proposed change.

**Change:** generate or hand-specialize the hottest fixed templates into host operations with
constant registers, masks and operands. Start with the matrix helper `func_00221888`, then
`00211888`/`00211930` and measured collision sites. Keep the existing `ps2_float_*` and
`ps2_vu_madd` operations, order, masked lanes, ACC, operand-binding GPRs, zero register, aligned
quadword addressing, MMIO fences and stores. The scalar-to-vector arithmetic contract is not
changed. Use port transformations/runtime helpers; never alter matching `src/`.

**Prototype:** a straight-line transcription of the 15-instruction 00221888 template, preserving all
architectural register side effects. The standalone comparator checks **all `ps2_cpu` bytes, the
entire output arena and MXCSR** against the original interpreter for 200,000 cases: randomized CPU
state and raw float bits, exponent-255 values, signed zero, subnormals, four rounding modes, FTZ/DAZ
on/off, output/input aliasing, and unaligned addresses rounded as the original runtime does. Zero
differences. An intentional VF11 bit flip fails on trial zero. An early harness error used the wrong
constraint delimiter and failed; it was corrected to the actual transformed `r|r|r`, not hidden by
relaxing the comparison.

**Gain:** measured prototype results and integration outcome are recorded below. For a broader
hot-template pass, a planning estimate is **0.2–0.7 ms less EE CPU per field**, with **0–0.4 ms
frame-time benefit** depending on how much falls before VU submission. This assumes eliminating
roughly 30–60% of an approximately 20% sample-ranked EE cost; it is not an FPS forecast. One helper
alone is a smaller win. Require call counts or proper stack tracing before extending the list based
solely on heuristic callers.

**Risk:** medium. A replacement returning the right vector can still corrupt live VF/ACC/GPR state.
Mixed EE/VU rounding, pointer aliases, diagnostic hooks, and code-size effects need attention.
Full-state tests and the unchanged r46 gate are mandatory; preserve a generic fallback for
unsupported sites.

**Effort:** 3–6 agent hours to productionize and validate the first few helpers; 10–20 hours for a
small reusable template emitter and a broader measured pass.

### 2. Reduce repeated work inside existing VU1 geometry kernels

**Where:** `vu1_native_geometry.h`, `vu1_native_extra.h`, generated native clipping, and their
dispatch in `vu0_interp.c`. The opening still has about 5.6–5.9 ms VU busy time. Finding 066's final
twelve-kernel sequence measurements leave 0078 geometry/clipping as the dominant replay cost. Its
rejected setup, flag-shuffle, packed-CLIP and static-schedule experiments should not be repeated.

**Change:** try a *lane-aware finite-value analysis* within a whole native kernel. `native_result`
already establishes that FMAC outputs are finite/normal-or-zero under the kernel's MXCSR, yet
`native_add/sub/mul/madd` clamp each operand again. An operand just produced by a proven FMAC need
not be clamped again. Loaded values, integer bit reinterpretations, untouched masked lanes, loop
inputs and values returned by clipping must retain checks until independently proved. Keep
sticky/current flags, CLIP/Q scheduling, integer MIN/MAX and all stores.

**Gain:** unprototyped estimate **5–15% of an affected arithmetic-heavy kernel**; budget **0.2–0.6
ms/field** only if the current opening profile confirms at least 2–4 ms of eligible kernel work.
These are test targets, not established wins. Finding 051's deliberately inexact all-clamps-off
experiment was about 35% faster inside kernels; that is an upper-bound clue, not permission to ship
it or a measured result for this proposed analysis.

**Risk:** high. Partial writes and merges can reintroduce exceptional raw bits. The prior
constant-folding denormal bug in finding 051 is directly relevant: retain the barrier that forces
real arithmetic and do not introduce FMA or fast-math. Require raw-bit differential tests, all real
sequence outputs, positive clipping coverage, independent PCSX2 checks and r46 MATCH.

**Effort:** 6–12 agent hours for one kernel and its proof; 12–24 for broader coverage. This is the
most promising *frame-time* investigation, but less validated than item 1.

### 3. Adapt the FIFO spin budget to the wait being performed

**Where:** `graphics_queue.c:idle_wait`, both queue consumers, `graphics_wait`, and `gs_queue.inc`.
Current game-thread samples spend 31.9% in graphics_wait. The later GS profile in finding 066
localized its 16.6–20.2% consumer samples to PAUSE/poll, not copying. Both queues still use 4,096
pauses for every wait.

**Change:** distinguish consumer-empty, producer-full and frame-drain waits. Start by measuring
128/256/512-pause budgets for idle consumers, with the current publication, event announcement and
recheck protocol intact. Prefer elapsed-time or recent-arrival-based budgets over one
processor-dependent iteration count. A separately validated WaitOnAddress implementation could
remove timed event polling, but is not required for the first experiment. Preserve acquire
ownership, publication barriers, rollover and the possibility of two retire waiters.

**Gain:** estimated **1–3 ms aggregate CPU per field**, not 1–3 ms off wall time. The gap between
CPU totals and useful-stage intervals makes this credible as a CPU-efficiency target, but does not
itself measure removable spin. The effect on FPS may be neutral or negative; only latency-tail A/B
measurements can decide.

**Risk:** medium. Shorter spinning can add wake latency; a bad wake protocol can deadlock. Test FIFO
wraps, source reuse, publication races, multiple waiters and forced stalls, then r46 and paced-144
latency. Do not remove waits or return stale readbacks. The old cache-line padding experiment was
noisy and is not a proven win.

**Effort:** 2–4 agent hours for a budget-only experiment and measurements; 5–8 hours for a new wait
primitive with adversarial concurrency checks.

### 4. Remove disabled VU0 timing overhead (small, concrete cleanup)

**Where:** `ps2_vu0_run` always executes two `__rdtsc()` calls; only `vu0_stats` discovers that
`DHD_VU0_STATS` is off. This now surrounds very short native entries, including 07B8/0808. VU0 entry
work still accounts for 8.0–12.7% of the current sampled game-thread windows, although most of that
is useful work.

**Change:** cache whether timing is enabled, and execute the two timestamp reads and statistics
reporting only when enabled. Retain native hit counters, complete microcode guards, arithmetic,
MXCSR save/restore and call ordering.

**Gain:** new batch measurements below isolate this change. Expect a small EE CPU saving, not the
missing 144-Hz headroom by itself. Its priority is high relative to its implementation cost, low
relative to the frame-time budget.

**Risk:** low for gameplay, but nonzero code-layout and diagnostic risks. Validate stats
enabled/disabled, all five entries, and the actual integrated binary. Do not broaden this into
skipping signature checks or removing MXCSR operations without a separate proof.

**Effort:** 0.5–1 hour for the code; 1–2 hours including full validation.

### 5. Reduce per-primitive GS frontend and exact-readback bookkeeping

**Where:** `gs.c` vertex/register dispatch and `gpu_native.inc`'s `native_draw`, `native_exact`,
`native_record`, `native_log`. Historical combined GS busy is about 3.76 ms across the race and 4.48
ms in the middle window; middle-window tails make it critical. The current fbb0fa53 gate records
231,544,794 primitive callbacks versus 2,470,416 native commands over 8,493 frames: approximately
27,263 primitive callbacks but 291 commands per frame (mixed menu/race, not an isolated race
counter).

**Change:** profile the frontend subparts, then cache decoded draw invariants over a correctly keyed
GS state epoch and specialize common packed vertex streams. Avoid recomputing exact-write
eligibility, context/format fields and related metadata for every primitive where they are
unchanged. Keep vertex order, primitive boundaries, strip history, ADC, state changes, texture/VRAM
dependencies and both exact and native coordinate conventions. Existing command and texture caches
already work; this targets work before command batching.

**Gain:** unprototyped planning target **0.1–0.4 ms GS CPU per field** if a focused profile finds
10–20% repetitive metadata work. A frame benefit is most likely in the middle/GS-bound windows, not
necessarily the crowded opening. For the 2.8–3.5 ms tail waits, first distinguish CPU
replay/bookkeeping, exact-GPU execution, and staging waits; the wait itself is not all removable
work.

**Risk:** medium/high. `state_version` alone is insufficient for every relevant register; current
logging explicitly handles PRIM, PRMODE, PRMODECONT and XYOFFSET separately. VRAM aliases and exact
lens-flare depth are mandatory. Use ordered callback/state differential replay and the unchanged
renderer gate, plus image comparisons for native-only coordinate work.

**Effort:** 4–8 agent hours for focused instrumentation and one guarded fast path; 8–16 for broad
packed-stream handling. No batching rewrite is accepted without a measured saving.

## Checked and not worth prioritizing, or not yet demonstrated

- **Another wholesale compiler/ISA switch:** finding 031 already rejected game
  O2, recomp O2, AVX2 and ThinLTO for EE differences; VU PGO did not improve the
  race. Finding 066's strict O3 and later SSE4.1 trials do not justify a default.
  Fix producers of differences before reconsidering; never relax the gate.
- **More of the old VU helper tweaks:** packed CLIP, flag shuffles, scalar
  readiness stores, 0078 setup/fusion and clipping schedule experiments already
  failed whole-replay benefit tests. No new evidence rescues them.
- **VU0 AOT as a default:** the five common entries are now native. Prior live
  AOT results were neutral. Turning on another translator is not a fresh win.
- **Cache VU0 signatures without invalidation:** unsafe; ordinary EE stores can
  modify its microcode. The VU1 code-version cache already exists and is not
  evidence that the same VU0 shortcut is valid. Current memcmp share is only
  1.6% of EE samples anyway.
- **Larger rings or eliminating memcpy indiscriminately:** neither ring filled
  in the accepted combined timelines. Copies protect DMA-buffer reuse. The
  locked-load and GIF-qword improvements are already in this branch.
- **Blindly remove frame-end/readback waits or reapply deferred vsync:** finding
  048 passed exactness but regressed late throughput. Revisit only after a new
  profile shows a changed limiting stage. Never use stale flare depth or move
  a guest-visible result to a later frame.
- **D3D11 constant-buffer pooling/state-call deduplication:** source has one
  WRITE_DISCARD map per native command and redundant-looking state calls, but
  native render CPU was only 0.297 ms/field in C0. Even eliminating that whole
  thread's work would not repair the tail. A D3D11.1 constant-buffer arena with
  correct feature fallback is possible, but lower priority than the GS frontend.
  Low render-thread CPU does not prove the RTX 3070 has unlimited GPU headroom:
  GPU timestamps and exact-readback staging latency were not independently
  measured here. Shader/fill-rate changes therefore have no claimed saving.
- **Sound, disc, menus, input and offline network work:** sound render is 2.1%
  of current EE samples and already uses exact SIMD interpolation. Most disc
  streaming is loaded up front, input is already sampled/cached, and offline
  networking is not a measured hotspot. No broad rewrite is justified for this
  race target. The sound-off benchmark still advances sound state; skipping it
  would not prove a valid normal-play win.
- **High-rate `pow`/`powf`/trig memoization:** `port_decay`, `port_blend`,
  `port_particle_sum` and rotation repeat rate-dependent math at 120/144. A small
  cache keyed by raw input bits, effective rate and relevant FP mode could keep
  the original libm result, with misses using the exact existing expression.
  This is worth a separate high-rate call-count/profile pass, not a claimed
  win: no current 144-Hz profile establishes its share or cache-hit rate. Preserve
  live input variation, online 60 transitions, signed zero, FP status and all
  existing high-rate physics behavior. Do not replace pow by an approximation.
- **Generic 64-bit port, GPU skinning, broad physics parallelism or reduced
  update/AI rates:** architectural projects or behavioral changes, not supported
  near-term exact performance wins. The existing 32-bit layout and mutable
  register model need to remain intact for the measured candidates.
- **Treating gate logging as normal gameplay work:** 4.2% of all samples have
  a heuristic external-work return address in CRT text output. Gate RPC,
  capture and readback logging can distort rankings; that does not establish
  a normal-play logging optimization. Likewise the 0.7% `getenv` IP share is
  small and has no reliable caller attribution in this trace. Caching remaining
  launch-only diagnostic flags is reasonable housekeeping, not a demonstrated
  material frame-time win.

## Validation and artifacts

New build, microbenchmark, live-attempt and renderer-gate results are recorded in the completion
section below. That prior gate is not substituted for a changed prototype's gate.

Local evidence roots (untracked):

  `macro_probe.c`, timer variant, batch drivers/results, prototype response file
  and manifest, build logs, guarded live controller and captures.
- Earlier timings:
- Current input profile: `input-final-gate-fbb0fa53/native/1` in the first
  artifact root; it has DHP4 records, despite its extensionless filename.

Reproduce the sampled ranking with `port/tools/profile.py`, the copied
`build/review/base/gate-ee.profile`, `--map build/review/base/dhd.map`, and `--fields 3000-9100`.
`build/review/analyze_previous.py` derives the pipeline table from all three earlier CSVs; it does
not filter away outliers.

No new decompiled-function claim is made. The final tracked diff is documentation only; all matching
sources and retail-image tooling are unchanged. Prototype success would authorize a
productionization attempt, not prove global correctness outside the exercised fixtures and renderer
gate. Existing high-rate physics equivalence failures in finding 067 remain outside this performance
review.

## Completion evidence

### New build and batch measurements

A fresh build compiled all 3,797 translation units without errors, then linked the VU-AOT-enabled
runtime with 718 AOT blocks and no unresolved or duplicate symbols. Its executable SHA-256 is
`e2d8774730482dcee5a931e64be5fa6450cdc0f566dd78a34216325e5826746c`. The prototype is a strict relink
of that build, replacing only the matrix wrapper, generic-asm runtime object and VU0 runtime object.
Its SHA-256 is `7caa8dd165037b51a591d311a597eba65445870f7e9451c8edc2647528b7930c`. An A/B race must
compare these two builds, not compare the independently linked prototype against the earlier
canonical executable and attribute layout effects to the changes.

The following **microbenchmarks are noisy**, use thread cycles rather than elapsed time, and do not
establish game-frame gains. No owned game or build was running concurrently with them. Thread cycles
avoid charging descheduled time but do not remove shared-cache or frequency effects.

The matrix test used five alternating-order pairs of 2,000,000 calls per arm. Median cycles fell
from **1,663,783,874 to 370,164,946: 4.49x throughput, 77.75% fewer cycles**. All five pairs
strongly agree and produce the same checksum; the separate full-state randomized comparator passes
200,000 cases. This is evidence for removing generic dispatch from this helper, not evidence for a
4.49x game speedup. Raw batches are in `macro_cycles.log`.

The VU0 test used three alternating-order pairs per entry, 2,000,000 restored calls per arm, native
mode enabled, and AOT disabled. Every arm recorded 2,000,001 native hits including warmup. Complete
final CPU and 4-KB VU memory snapshots were byte-compared after normalizing the host memory
pointers; all five entries match. The comparison is not based only on a checksum.

| Entry | Baseline median cycles | Timing-disabled median cycles | Reduction |
| --- | ---: | ---: | ---: |
| 0000 | 876,787,232 | 856,444,602 | 2.32% |
| 02E0 | 2,160,586,840 | 1,987,798,269 | 8.00% |
| 05E8 | 1,826,885,559 | 1,767,488,316 | 3.25% |
| 07B8 | 606,122,157 | 536,379,108 | 11.51% |
| 0808 | 506,456,906 | 426,852,832 | 15.72% |

0000 and 02E0 each have a regressing pair, so their medians are weak evidence under this noise. The
short 07B8/0808 entries improve in every pair. The change removes a known fixed cost; its exact live
contribution still requires a quiet race and call counts. Raw triples and conditions are in
`timer_cycles.json`, `micro_run.log` and `micro_conditions.json`.

### Integrated correctness gate

The combined matrix-specialization/timing-disabled executable passed the **unchanged
`gate_native_renderer.py`, reference r46-plain**: all 14 EE comparisons MATCH, and all 1,147
readback/Z records MATCH through virtual GS field 9068. Both arms reached the final GO+4500 capture
(capture field 7715). The native arm presented 8,825 frames, so this exercised the native renderer.
The gate's failures list is empty. Its output is in `build/review/gate-prototype/result.json` and
`build/review/gate.log`.

This is the normal 60-Hz deterministic game-state gate, not a proof of every possible input, picture
equality, or high-rate physics equivalence. The gate ran on the shared PC; no FPS conclusion is
taken from its duration.

`tools/check_image.py` also reports MATCH for all eight sections of the existing local PS2
`build/dhd.elf` against the retail ELF. This was a read-only recheck of that existing image, not a
newly rebuilt PS2 image. `git diff fbb0fa53 -- src port` is empty in the review checkout; the
tracked review changes are documentation.

### Fresh guarded race attempts

The newly built, unchanged fbb0fa53 baseline completed one native, FPS=60, unpaced race with seed
81257 and fields 3000-9100. The controller polls ownership every 250 ms.

| Metric | Fresh baseline |
| --- | ---: |
| Real updates/s | 176.413 |
| Median frame time | 5.790 ms |
| p99 | 9.538 ms |
| Worst-1% mean | 10.474 ms |
| Maximum | 14.271 ms |
| Process CPU per field | 15.970 ms |
| Game / VU / GS / native CPU per field | 5.453 / 4.651 / 4.964 / 0.391 ms |

Whole-window mean wall time is 5.668 ms; wall-minus-waits is game 3.756, VU 4.495 and GS 3.884 ms.
The opening (3000-3500) has mean wall 7.841 ms, EE busy 4.519, VU busy 6.060, VU idle 1.781 and GS
busy 4.708 ms. Neither ring is full. These current-build observations support the stage priorities
above, with the same overlapping-stage and scheduling caveats. Another game started before the
prototype could launch; the guard waited rather than run an overlapping timing comparison. Raw
evidence is in `build/review/live/`.

The ten-minute guarded campaign ended with that one accepted baseline and no prototype timing run.
Other game instances or high system CPU occupied the remaining launch opportunities. Consequently
there is **no accepted fresh frame-time A/B**, no new VU/GS-thread profile, and no new paced-144
measurement. The matrix and VU0 gains above are batch-cycle measurements only; the other ranked
items remain estimates pending their stated profiling/proof steps. The controller exited normally
and no review-owned game remained running.

## Implementation campaign (requested after the review)

Candidates are attempted in the requested order: 4, 1, 3, 2, 5. Only exact changes with a measured
CPU/frame benefit will remain in the final build. The original review evidence above is retained as
historical evidence.

### Win 4: conditional VU0 timing

The production `ps2_vu0_run` now caches whether `DHD_VU0_STATS` requests timing and skips both
timestamp reads and the statistics call when it does not. Native hit counts, complete signature
guards, arithmetic and MXCSR handling are intact.

Validation on this implementation: all five entries pass one million raw-bit
CPU/memory comparisons each, all 304 changed-word guards and invalid-VI0 cases;
1,320 independent PCSX2 cases MATCH. All five deliberately corrupted results
fail immediately. A stats-enabled 0808 run also passes full-state comparisons
and prints the expected 1,000-call timing/hit totals. The unchanged r46-plain
gate MATCHES all 14 EE comparisons and 1,139 readback/Z records through field
9060. Artifacts: `build/review/win4-{diff,negative,gate}` and their logs.

The prior three-pair timing experiment supports removing this fixed overhead; a new guarded
production-object batch and the final replicated race campaign will determine retention. A separate
`build_clean.py --rev fbb0fa53` build is being prepared for the same-method final baseline
comparison.

## Research

No third-party code was copied. Sources informed the experiments and constraints:

- Microsoft, [QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime):
  measures thread user/kernel cycles; do not convert these counts into elapsed
  time. This is why batch ratios here are not labeled FPS or milliseconds.
- Microsoft, [WakeByAddressSingle](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-wakebyaddresssingle)
  and [WaitOnAddress](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress):
  address-based waits are an implementation option, not permission to discard
  the queue's memory-order or multiwaiter protocol.
- Microsoft, [dynamic resources](https://learn.microsoft.com/en-us/windows/win32/direct3d11/how-to--use-dynamic-resources)
  and [D3D11.3 specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm):
  constant-buffer offsetting and no-overwrite support have feature requirements.
  The small measured native submission cost keeps this below the CPU frontend.
- PCSX2's [threading VU1 documentation](https://wiki.pcsx2.net/PCSX2_Documentation/Threading_VU1)
  was located in search; direct retrieval returned HTTP 403. No new hardware
  behavior is inferred from unavailable text. Local findings 046/048/051/066
  supply the already-tested queue and exact VU contracts used in this review.

## Production implementation (revised acceptance)

The owned timing controller was stopped. Correctness is now the retention criterion; production
performance, wake latency and paced tails are **unmeasured**. Earlier exploratory timings above are
not measurements of these final changes. One final clean build and unchanged gate will cover the
integrated result.

### Win 1 included: inline-assembly templates

Complete parsed instruction shapes select matrix/direction (00221888), bounds min/max, vector
addition/subtraction and quad copies. Other programs retain the generic path; DHD_ASM_GENERIC=1
forces it. Binding, writeback, MMIO callbacks, register-zero behavior, masked lanes and PS2 float
operations remain exact. A diagnostic call census counted 99,245,790 calls across 1,133 sites in
fields 3000?9090: bounds 14,086,845; matrix 3,743,513; in-place vector addition 3,445,231; quad copy
3,221,425; several further hot vector add/subtract sites. This selected the families; these counts
are not timing evidence.

The production test passes seven templates x 200,000 full CPU/memory/MXCSR cases and 448
mapped-memory callback cases, including raw exceptional bits, aliases, four rounding modes and
FTZ/DAZ combinations. All seven corruption controls fail. The selector rejects 480 mutated matrix
instruction fields.

### Win 3 included: adaptive empty-consumer waits

Only the two empty FIFO consumers adapt their spin budgets (128?4096 pauses, initially 1024).
Arrival during spin targets twice that observed spin count plus 128; an exhausted budget halves.
Full/drain waits retain 4096 and their original timeouts. Interlocked publication, sleep
announcement, recheck and events are unchanged. DHD_FIFO_ADAPTIVE=0 restores the original budget.

Both policies pass the 4 KB-ring rollover/source-reuse stress test: full GS state including 4 MB
VRAM and all 12,000 callback records match direct execution. Both single-bit GS-state corruption
controls fail. Both policies also pass 200,000 VIF differential trials (16,140,614 vectors), upload,
double buffering, MSCNT and DIRECT checks. No wake-latency or paced-tail measurement was made, as
explicitly requested. Shorter idle spins may trade CPU use for wake latency; the unchanged
lost-wakeup protocol guarantees delivery, not a latency bound.

### Win 2 included: four all-lane accumulator clamps

Four geometry-loop MADD sites now omit only the ACC exceptional-value clamp. Each ACC is proved to
have all four lanes overwritten by preceding FMAC under toward-zero rounding with FTZ/DAZ, hence
finite normal/zero. The optimizer barrier remains. Partial XYZ cross-product W, loads, clipping
reloads and other operands keep their clamps; full-lane provenance is reestablished after clipping.
No FMA, fast-math, operation reordering or flag simplification is introduced.

All 36,000 real calls across the existing start/middle/late captures match full CPU state, 16 KB
data memory and every ordered kicked GIF byte, with every native kind exercised. A private one-bit
result corruption fails 748 of the first 2,000 start-capture calls (exit 1). The unchanged
comparator is used in both cases; --negative compiles a private corrupted header under build/. No
timed repeats or additional PCSX2 oracle round were run.

### Win 5 included: invariant exact-GS frontend metadata

A small per-renderer cache keys the complete raw ZBUF, TEST and FRAME words plus the effective ABE
bit. It caches format/base/width, write masks, ZTST NEVER and unconditional-write eligibility. Raw
keys also cover replay/context/PRMODE changes without relying on state_version. Dynamic VRAM
aliases, ownership, watch rectangles, band/field state, vertex order, coordinate calculation and
exact rendering remain on their existing paths. Eligibility is copied into locals before downstream
calls. The scope deliberately excludes packed-stream rewrites and changes to existing native
command/texture caches.

`test_native_frontend.py` compares 200,000 decoder results with the original formulas, then 20,000
primitive groups / 80,000 ordered callbacks with complete vertex/metadata records and full GS state
including VRAM. It exercises both contexts, PRMODE/PRMODECONT and offset/scissor changes; 133,314
cache hits and 106,686 misses occur. All match. A one-bit cached-result corruption fails immediately
on the hit path. The test covers frontend semantics; the final unchanged renderer gate additionally
checks the integrated GPU/readback path. All five items are included; **none has final-build
timing**.

### Final integration

Merged combined tip 7b74859c (adaptive offline timing) before the final build. Only docs/STATE.md
conflicted; both independent progress records were retained.

### Integration correction: keep the fast dispatcher out of line

The first integrated clean build (364de20e) failed the frozen-reference part of r46-plain: 33
identical word differences in both renderers at all four early checkpoints. The six exact/native EE
comparisons and 1,126 ordered readbacks matched each other. This is a failed gate, not an accepted
exception.

A bounded early-state diagnostic with DHD_ASM_GENERIC=1 retained the same 33 differences. The values
were host return addresses in quadword W slots that previously held 0xC0 or 3. Disassembly showed
that inlining the new dispatcher expanded ps2_asm's local frame from 4 to 32 bytes; legacy partially
initialized stack quadwords made that host-layout change observable in EE memory. Marking only
run_fast_program noinline restores the binding frame, preserves all five template families, and
makes the same early full-EE comparison MATCH. The 1.4-million-case assembly suite, callback cases
and seven corruption controls were rerun and pass. No state was patched, comparison relaxed, or
reference changed.

The failed integration requires a corrected final link and repeat of the final gate. The already
fresh 3,797 game objects are reused: src/, their build inputs and flags are unchanged;
build_clean.py --skip-compile rebuilds the runtime and link for the corrected commit. This avoids a
second full game-source rebuild. No performance timing is collected. The failed build is not the
launcher target.

### Final accepted handoff

**All five optimizations are included. No final-build timing was collected.** Code revision:
`4cbe3c71` (combined-tip merge `364de20e`, importing `7b74859c`). The single full clean compilation
built all 3,797 game units without errors; the corrected runtime/link rebuilt with build_clean.py
--skip-compile, using those unchanged fresh game objects. Link succeeds with 23 existing stubs and
no undefined symbols. Final executable SHA256:
`deb773b1e0f5c87aa049854b151393dfdaed4866dd62e8c47bc4ee923f3a3a53`.

The corrected final unchanged **r46-plain gate MATCHES**: all 14 EE comparisons; 1133 records, 1133
Z reads, through virtual GS field 9054. Both arms reach final capture field 7715. The earlier
failing artifact remains recorded above; no check or reference was weakened.

Implementation commits (requested order): `97634e61` (VU0 diagnostics), `dbc6e302` (assembly
templates), `404a04ec` (FIFO waits), `9bbcee2f` (VU1 clamps), `a1b5f8d1` (GS metadata); merge
`364de20e`; integration repair `4cbe3c71`. Local evidence is under build/review/:
win4-diff/negative/stats-on logs, win1-noinline-tests.log, win3-tests.log, win2-seq.log and
win2-negative.log, win5-tests.log, final-clean.log, final-corrected-link.log, final-retail.log and
final-confirmed-gate/. Captures/generated game inputs remain untracked.

No main merge, push or release.