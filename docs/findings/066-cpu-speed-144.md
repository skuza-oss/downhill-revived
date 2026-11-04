# 066 — CPU speed toward 144 real steps/s

Status: partly superseded by 090 (VU0 AOT opt-in; the i686 build)
Checked: 2026-11-04

A round on the 32-bit build (`build_clean.py`, the r46 gate; both retired, finding 088). Still in
the port: the native VU1 kernels 1430, 2DC8, 0E10 and 2E48 and the native VU0 entries
(`port/runtime/vu1_native_*.h`, `vu0_micro.c`). Changed: VU0 AOT, opt-in here, is built always
and runs every VU0 entry without a native version (`port/tools/vu0_aot.py`, finding 090 SP1), and
144 real steps a second were reached (finding 090).

Pure host performance work; the simulation clock and matching `src/` are unchanged. The real
simulation-rate work is separate.

## Conditions and baseline

Read STATE rounds 30–46 and findings 032, 046, 048, 049, 051, 052. This is historical context, not
this branch's baseline. Target is well above 144 fields/s with worst 1% near/below 6.9 ms.

Validation uses locally supplied retail inputs and captures. No disc image, captures, generated
assembly or proprietary data is tracked.

No full-race benchmark is valid under those conditions. Local microbenchmarks below are explicitly
shared-PC measurements, and do not establish race gains. Evidence is ignored under build/port/r66/.

## Step 1: acquire loads for queue observations

Both copied graphics FIFOs polled every counter using InterlockedCompareExchange with zero operands.
That performs a locked read/modify/write even when merely observing the counter. Replaced those
reads with clang's atomic acquire load. Interlocked publication exchanges, sleep announcements and
wake protocol remain: the full barrier before checking the peer's sleeping/waiting flag is necessary
to prevent a missed wake. An acquire read of published/retired positions retains ownership ordering
for packet bytes. This does not reorder queue execution.

Validation:
- test_split_graphics.py: split FIFO/wrap/copy/provenance/fences, zero failures;
  VIF integration and 200,000 random UNPACK trials (16,140,614 vectors), zero differences.
- Negative control: flip bit 0 of a copied packet after memcpy; FIFO test exits 1
  with one failure. Local source/executable retained, production untouched.
- Synthetic queue bench extends the existing test to 300,000 packets in each
  phase, removes its deliberate millisecond sleeps, and retains all hash/order
  checks. Three interleaved runs, process wall seconds including startup:
  base 0.4421 / 0.6231 / 0.6674; acquire 0.3733 / 0.4144 / 0.3767.
  Medians 0.6231 -> 0.3767 s. This is a threaded queue stress workload on a busy
  PC, not a single-thread replay or live race benchmark.
- A second interleaved set: base 0.4882 / 0.6445 / 0.4610; acquire
  0.4900 / 0.4110 / 0.5399; cache-line separated counters
  0.5777 / 0.3758 / 0.5550. Counter separation gives no consistent benefit and
  is dropped. Busy-PC noise prevents treating the first set as a reliable size
  of improvement; the change still eliminates locked RMWs from read-only polls.

Rejected VIF experiments (not shipped): WL=1 constant specialization and SIMD mask selection. A
local race-mix replay has 10,000 commands/168,445 vectors. WL=1 specialization was slower; SIMD mask
selection did not consistently beat baseline. Generated/local harness code remains ignored. No
correctness claim is made for these discarded prototypes.

Research: LLVM's Atomic Instructions and Concurrency Guide (https://llvm.org/docs/Atomics.html)
documents acquire ordering and x86 atomic loads lowering to MOV. Microsoft's
InterlockedCompareExchange documentation
(https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedcompareexchange)
documents its full memory barrier. No third-party code copied.

## Final verification and measurements

The integrated four-kernel checks and default clean gate now pass. See final evidence below.
Uncontended race evaluation remains pending; no deadline claim.

## Step 2 candidate: packed VU1 CLIP comparisons

`vu_clip.h` implements the existing signed-encoding CLIP rule with SSE2 integer comparisons, packs
the positive/negative masks to 16-bit lanes, then extracts the interleaved six xyz bits. Both VU1
upper instructions and the native kernels use it. VU0 retains its existing scalar float comparisons.
No flag timing, stall, queue, arithmetic or exception semantics change.

A raw-encoding independent scalar reference covers one million randomized vectors, biased bounds
toward exponent zero and 255: zero differences. The existing VU1
instruction/timing/continuation/XGKICK tests also pass. Deliberately shifting the packed negative
mask by six instead of seven makes 508,064 trials differ and the test exit 1. These are raw integer
comparisons, so FTZ/DAZ and the host's NaN arithmetic cannot alter the result.

Single-thread standalone microbenchmark, 200 million noinline helper calls per arm, three
interleaved pairs on the busy shared PC: scalar 0.649295 / 0.933543 / 0.872243 s; packed 0.593733 /
0.534953 / 0.598294 s. Medians 0.872243 -> 0.593733 s (32% lower). This measures the helper only;
full VU1 replay and race numbers are still required before claiming a frame win.

## Step 2 disposition: rejected after full replay measurement

Three interleaved single-thread sequence replays per arm (10 timed repeats per captured run, 12,000
real invocations per capture). Median totals per 12,000 invocations, milliseconds, native kernels
enabled:

| capture | baseline | packed CLIP in all paths |
| --- | ---: | ---: |
| crowded start 3480 | 47.753 | 47.752 |
| middle 6300 | 53.844 | 55.501 |
| late 8750 | 58.789 | 65.358 |

AOT-only totals were 129.218 -> 118.954, 134.026 -> 123.084, and 105.909 -> 124.323 ms respectively.
No consistent native-mode win: rejected. The packed helper, its new fuzz, and every runtime change
from step 2 are removed in the following commit. The working candidate has no VU arithmetic,
clipping, kernel, timing or dispatch change.

Two further local-only variants were also rejected (same three-capture, three-interleaved-run
method; each matched the exact replay state/GIF output):

| capture | base / flag-shuffle (native ms) | base / scalar readiness stores (native ms) |
| --- | ---: | ---: |
| 3480 | 57.533 / 56.732 | 57.506 / 61.416 |
| 6300 | 59.778 / 62.206 | 58.399 / 56.803 |
| 8750 | 71.066 / 72.068 | 64.578 / 68.970 |

The flag variant kept packed CLIP only in the AOT instruction path, retained scalar kernel clipping,
and replaced MAC reversal table loads with an SSE lane shuffle. The readiness variant kept baseline
arithmetic and replaced the partial readiness-array read/blend/write with scalar lane stores.
Neither has a consistent gain. No change to normal register stores was tried: their full vector
stores deliberately avoid partial-store forwarding stalls (existing runtime comment). No new native
kernel was invented without reliable profile and throughput evidence.

Verification performed on the discarded packed-CLIP candidate:
- 36,000 real sequence runs: zero register, 16 KB data-memory or ordered GIF
  differences. All eight kernel kinds exercised. Negative control flips VI1
  after the native execution: 30/30 runs differ (comparator exits 1).
- Independent pinned PCSX2 oracle: all 1,584 cases, 32 entries, zero failures.
  A VI1 flip in a local oracle adapter fails both the PCSX2 and reference-
  interpreter comparisons (2 differences, exit 1).
- Fixed an existing oracle-tool link error: the reference object now renames
  its unused ps2_vu_aot_load symbol as it already renames its run functions.
  The ordinary --aot command previously failed with a duplicate symbol. No
  checks, expected values, case count or exclusions were changed. The repaired
  reference object was linked and all ordinary cases run by oracle_run.py;
  the complete ordinary tool command is to be rerun on the final source.

## Additional validation and dependency setup

Longer queue stress test, three million packets per phase (12 million callbacks), three interleaved
runs, same busy shared PC and all hash/order checks retained: base 3.5045 / 3.9810 / 3.6017 s;
acquire 2.9564 / 2.4615 / 2.3266 s. Medians 3.6017 -> 2.4615 s, 31.7% lower process wall time
including startup. This is a queue microbenchmark, not evidence of 31.7% faster simulation. Assembly
from the same two test builds contains 77 read-only cmpxchg sites before and zero after; the six
true interlocked exchange sites and all pause sites remain. Counter separation remains rejected.

Final unchanged-runtime validation: the repaired ordinary `python port/tools/test_vu1_pcsx2.py
--aot` passes all 1,584 cases / 32 entries and the VU1 instruction suite. VIF negative control
changes S-8 stream values: 13,666 of 200,000 trials differ and the test exits 1, while the
unmodified queued VIF suite has zero differences.

Clean builds freshly compile all 3,797 game units. SDL3 uses the main checkout's already built
3.4.18 static dependency cache with its existing exact recipe stamp and original pinned headers. The
baseline's first build had finished all game units and spent several minutes configuring the same
SDL release; its own remaining link process was stopped by PID, the matching dependency cache was
seeded, and build_clean.py --rev 9b018854 --skip-compile performs the baseline link. The candidate
is a fresh full build (no --skip-compile). No game-object basis from the main checkout is used.

One final local ISA experiment uses the otherwise unchanged VU runtime compiled with SSE4.1, leaving
all game/EE objects at their original SSE2 flags.

| capture | baseline / SSE4.1 (median native ms) |
| --- | ---: |
| 3480 | 54.294 / 51.595 |
| 6300 | 64.954 / 53.225 |
| 8750 | 66.187 / 65.406 |

The spread is large: the respective base/ISA triples are 57.66/54.29/45.18 vs 51.60/69.58/49.56;
62.31/65.21/64.95 vs 53.23/62.32/51.43; and 74.47/66.19/57.29 vs 65.41/51.01/72.35 ms. Every
sequence comparison matches. The middle-window result warrants a private VU-only ISA race
comparison; no new CPU requirement or ISA setting is in the tracked build. The private relink
replaces only vu0_interp.c.o in the clean candidate's object response file, with strict linking (no
/force), and stays under build/port/r66/sse41-game/.

## Step 3: native 1430 mesh/light loop (dispatch kind 256)

Added `port/runtime/vu1_native_1430.h`: the 1510–1620 loop used by program 1430 (VI13 = 0x288) runs
with the existing exact SSE arithmetic, integer MIN/MAX, conversion and clipping helpers. It
preserves upper/lower instruction ordering, DIV-zero handling, masked stores, the final MAC and
sticky flags. FSAND in the loop reads only the new DIV's D bit; FCAND reads the CLIP issued four
pairs earlier. The closed clipping helper returns through VI5 to 15E0.

The kernel declines before writing for unsupported vertex counts or a pending Q, reserves a
conservative instruction budget, and cannot silently fall back after direct writes: clipping failure
or an over-budget count is an error. The exact-word guard covers its entire loop and both existing
clipping ranges; the guard cache is still keyed by microcode version. The generator derives the word
guard from the local ELF and production loads it from the player's disc. No microcode words are
committed. Coverage counters now have nine typed slots; the sequence capture's eight-word input
header is unchanged.

The default native mask becomes 511. `DHD_VU1_NATIVE_MASK=255` retains all prior kernels with this
one disabled; `DHD_VU1_NATIVE=0` retains the complete AOT referee.

Prototype exactness:
- All 36,000 real invocations across the three race captures match every
  register, all 16 KB of data memory and ordered GIF bytes against AOT.
  New-kernel coverage is 132 + 190 + 145 = 467 invocations.
- Extracted those 467 real inputs and replayed eight variants (3,736 checks):
  ordinary state; tiny clip bound; zero matrix W (zero DIV denominators); both;
  and four combinations of reduced clip bounds / shifted projection offsets.
  Every variant has 467 new-kernel hits and zero differences. Forced cases
  explicitly report 42/42 and 84/84 clipped vertices in their first two calls,
  with no rollback or fallback. This proves the clipping path actually runs.
- Negative control flips a data word after only the new kernel executes:
  exactly 132 of 12,000 start-capture invocations differ; the checker exits 1.
- An initial private harness adaptation accidentally changed the capture header
  array size when extending coverage. It failed with a register-size mismatch;
  fixed by changing only the named coverage arrays. No production/capture
  format change was made.

Median time per 1430/1040-or-1CB0 call (microseconds):

| capture | previous kernels | plus 1430 | speedup |
| --- | ---: | ---: | ---: |
| 3480 | 12.729 | 2.802 | 4.54x |
| 6300 | 14.907 | 3.002 | 4.97x |
| 8750 | 16.613 | 3.170 | 5.24x |

Whole-capture native totals are noisy/mixed (51.783/56.541, 62.187/56.538, 65.353/73.043 ms). They
are not a frame-throughput claim.

The tracked implementation adds an accurate issued-pair count across clipping, a conservative
near-limit guard and a hard remaining-step check. The ordinary sequence tool and independent oracle
are being rerun on this tracked version, followed by a new committed clean build and unchanged r46
gate.

## Step 4: native 2DC8 late-race loop (dispatch kind 512)

Added `port/runtime/vu1_native_2dc8.h`, loop 2F00–2FC0 (VI13 = 0x5BD). This was finding 051's large
unoptimized late-race program. It uses the same exact arithmetic/conversion helpers and closed
clipping routine.

Two ordering details are explicit:
- FSAND at 2F30 sees the MAC produced at 2F10 (four issue slots earlier), plus
  the current DIV D bit. The kernel keeps that result separately from sticky
  aggregation. No intervening register read can stall after that producer.
- The SQ at 2F88 reads the old VF19.w while the upper ADDx commits the new one.
  The first prototype stored the new W and failed 70 start-capture checks:
  registers matched, but VU memory and GIF bytes did not. The corrected order
  passes. The failing executable/object remain the real negative control.
- The final DIV at 2FA0 has issued five slots at loop exit and completes in
  seven. Q remains pending for two slots. The exact-word guard also covers
  2FC8–2FD0 and the common 0FF8–1038 tail, which has no premature MAC/status
  observation. A clipping DIV is completed before a replacement DIV starts.

The production mask is now 1023 (ten coverage slots). Mask 511 disables only 2DC8; mask 255 disables
both new kernels. Every loop, clipping range and tail assumption is checked against the player's
uploaded microcode; a changed code version invalidates the cache. No matching check or gate
exclusion is relaxed.

Prototype exactness:
- Three real captures: 36,000 checks, zero differences; new 2DC8 coverage
  85 + 162 + 774 = 1,021 invocations, with all prior kernels retained.
- Twelve variants of those 1,021 real inputs: 12,252 checks, zero differences.
  The first eight exercise smaller bounds, shifted offsets, zero matrix W and
  forced clipping (44/44 clipped vertices in a representative first call).
  Four more use a huge clip bound to prove the unclipped paths: zero, positive
  and negative normal/MAC results, then a zero denominator. All 1,021 inputs
  exercise the new kernel in every variant; the unclipped variants report
  zero clipped vertices. Data memory and every GIF packet are still compared.
- Negative control: the real within-pair store-order error above gives 70
  differing invocations out of the 12,000 start-capture runs (85 new-kernel
  calls); this is not a manually forced pass/fail or relaxed comparison.

Three interleaved single-thread replays, same executable with masks 511/1023, 10 timed repeats per
invocation, busy shared PC, median whole target-call us:

| capture | previous kernels + 1430 | plus 2DC8 | speedup |
| --- | ---: | ---: | ---: |
| 3480 | 28.500 | 18.183 | 1.57x |
| 6300 | 56.401 | 20.968 | 2.69x |
| 8750 | 21.088 | 7.438 | 2.84x |

Whole-capture native totals were 106.103 -> 128.459, 78.291 -> 74.313, 77.315 -> 60.058 ms, with
large shared-PC variation. These remain local replay measurements, not race FPS or deadline claims.
The ordinary tools are being rerun against the integrated source before its clean-build gate.

## Completed gate so far

Clean queue-only commit e65990ca (all 3,797 game units freshly compiled) passes the unchanged
default r46-plain gate: all 14 EE comparisons, 1,116 matching Z readbacks through field 9037.
SHA-256 begins 9485caaccd209a9a. The subsequent 1430 source passes the ordinary 36,000-run sequence
tool and all 1,584 independent PCSX2 cases; its clean build e36d1fa8 completed with 3,797 units and
SHA-256 beginning 3a6ec3adfe3bec8c. Its gate is in progress. The retail rebuild MATCHES all eight
checked sections. Final two-kernel clean build, default gate and guarded race campaign follow; no
reference recording, new exclusions, source decompilation claim, merge, push or publication.

## Step 5: native particle sprite loop (0E10)

The remaining replay table identifies program 0E10 as 146 complete calls across these captures (515
inner groups of up to 49 sprites). Added the guarded 0EE0..0FC8 loop, kind 1024, with the original
AOT setup, group loop and GIF tail. Its guard and tail words use overlay A provenance; the existing
clipping emitter continues to use overlay B. The production loader fills both from local vutext; no
proprietary instruction words enter tracked files or the executable.

The helper keeps DIV Q's seven-slot availability, both four-slot CLIPs before FCAND, raw MR32 UV
rotation, masked writes and the unusual clipped-last-sprite fallthrough (a second MADD/ADDA plus VI6
advancement). Out-of-range counts and pending Q decline before writes. It is explicitly VU1-only.
Default native mask is now 2047; 1023 isolates this kernel, 255 disables all three additions.

Proofs: the private prototype matches all 36,000 real sequence checks, with 226/134/155 new groups
and every old kernel retained. Twelve variants of all 146 inputs (1,752 checks, 515 new groups per
variant) also match: baseline, small/huge clip bounds, zero matrix W with either bound,
positive/negative particle sizes, alpha zero/INT_MAX/-1, UV NaN/Inf bit patterns, and altered finite
coefficients. The forced small-bound first group clips 98/98 corners; the huge-bound counterpart
clips zero, including the zero-divisor case. Every register, all data memory and ordered GIF output
remain compared. Negative control: flip one data word only after this kernel ran: 50 of 12,000
start-capture calls differ and the checker exits 1; the other calls are untouched.

Same-executable masks 1023/2047, three interleaved single-thread runs, 10 timed repeats per
invocation, busy shared PC. Median whole target-call microseconds:

| capture | previous kernels | plus 0E10 | speedup |
| --- | ---: | ---: | ---: |
| 3480 | 35.147 | 7.545 | 4.66x |
| 6300 | 25.927 | 5.125 | 5.06x |
| 8750 | 21.783 | 4.560 | 4.78x |

Whole-capture native totals: 25.359 -> 23.775, 27.621 -> 25.543, 26.323 -> 25.153 ms. These are
replay measurements under contention, not uncontended race numbers or evidence of meeting a 6.9 ms
deadline. The integrated ordinary sequence tool also passes all 36,000 checks. The independent
oracle, full clean build and renderer gate follow this commit.

The two-kernel clean 8d544317 gate has now passed: 14 EE comparisons and 1,131 matching Z readbacks
through field 9052. The sequence harness confirms both new geometry kinds executed. Independent
PCSX2: 1,584 cases, no failures; forced-input tests were relinked against the integrated oracle
runtime and pass.

A QueryThreadCycleTime timing prototype was discarded: two system calls around every tiny invocation
distort the workload and it yielded no paired evidence. Microsoft documents these as thread CPU
cycles (user plus kernel), not elapsed wall time:
https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime
. No cycle result is used as performance evidence.

## Step 6: projected particle quads (2E48)

The post-0E10 replay table still shows 2E48 as an expensive entry. Its loaded code is overlay A: a
trial mapping it to the similarly addressed overlay B 2DC8 loop correctly finds no matching word
guard and adds no native hits. That experiment was discarded; no loose address-only dispatch is
shipped.

Added overlay A's actual 2F90..30B8 loop as native kind 2048, up to 41 quads. AOT still performs
setup, group counts, alternating output buffers and XGKICK. The two DIVs feed different corners:
2FE0 consumes the old Q while issuing its replacement. FMAND at 3098 sees the XY MAC result issued
at 3078, four pairs earlier, rather than the newer MULs. Both CLIPs are available at FCAND. 3068's
SQ reads the old VF8 before the upper MADD commits; clipped quads mark both output W fields. Raw
MR32 and masked writes preserve all untouched bits. The guarded tail contains no early flag read.
Pending Q or unsupported counts decline before writes. All four additions are on by default (mask
4095); mask 2047 disables just 2E48, 255 disables all additions.

Private prototype: all 36,000 real calls match, 144/252/168 new inner groups. Twelve variants of all
110 target calls (1,320 checks; 564 new groups in each) match complete register/data/GIF state:
small/huge bounds, zero matrix W, zero/large size words, raw point flags 0/2/FFFF, NaN/Inf
coefficient bits, and changed finite coefficients. The first forced-bound group clips 82/82 corners,
while the normal first group clips 4/82 (point/MAC flag paths remain). Negative control flips a data
word only after kind 2048: all 50 start target calls differ, the other 11,950 calls do not; checker
exits 1.

Same-executable masks 2047/4095, three interleaved single-thread runs, 10 timed repeats, busy shared
PC, median target-call microseconds:

| capture | previous kernels | plus 2E48 | speedup |
| --- | ---: | ---: | ---: |
| 3480 | 42.416 | 9.264 | 4.58x |
| 6300 | 138.321 | 25.171 | 5.50x |
| 8750 | 123.440 | 21.984 | 5.62x |

Whole-capture native totals: 44.660 -> 41.851, 52.847 -> 46.314, 47.470 -> 38.477 ms. These busy-PC
replay numbers cannot establish race FPS. The ordinary sequence/oracle tests and full clean-build
gate follow integration.

Additional boundary check: unchanged GS suite passes all memory/GIF/CLUT tests, including 3,000
upload fuzz transfers / 6,657,394 pixels, zero differing. A local fast-transfer-state corruption is
the failing negative control.

## Integrated four-kernel results

Ordinary vu1_seq_diff.py: 36,000 real calls, zero differences, every old/new kind exercised. Fresh
untraced integrated runtime: 19,060 forced-input checks (3,736 + 12,252 + 1,752 + 1,320), zero
differences. All registers, all 16 KB of data and ordered GIF bytes/addresses compared, no
exclusions.

Unchanged test_vu1_pcsx2.py --aot in a separate output directory: 1,584 cases / 32 entries, zero
failures; instruction/timing/continuation/XGKICK tests zero failures. Pinned public source hashes
and checks unchanged. Additionally every real captured target call converted to the oracle's
ordinary input format: 1,744 comparisons against both PCSX2 and the reference interpreter, zero
differences. Positive new-kernel coverage: 467 1430, 1,021 2DC8, 146 0E10, 110 2E48 complete target
calls. Current oracle negative control on real 0E10 flips VI1 after native execution: PCSX2 plus
interpreter mismatch, two differences, exit 1. GS fuzz negative control corrupts fast transfer count
on trial 0: ordinary comparator reports one differing transfer, exit 1. Retail check_image.py rerun
MATCHES all eight sections; src/ unchanged.

An initial integrated variant rerun linked the oracle's tracing-enabled object (getenv per pair even
when tracing is off). Stopped after completed passing cases and replaced with a fresh current
runtime using ordinary untraced sequence-tool flags. All 19,060 cases above completed; no check was
relaxed.

### Batched thread CPU cycles

A local sequence harness uses two QueryThreadCycleTime calls around the whole capture, replacing the
rejected per-invocation probe. Each input is validated native off/on, followed by 50 native replays
with restored registers/data. Per-call QPC probes and exact-mode timed repetitions are omitted.
Includes file reads, copying/restoration, output collection and comparisons common to both masks.
One thread, busy shared PC; raw user+kernel thread cycles exclude descheduled wall time. Never
converted to seconds or race FPS.

Same executable, masks 255 (old kernels) / 4095 (all additions), three interleaved runs per
12,000-call capture, AB/BA/AB order, medians:

| capture | old kernels, cycles | all additions, cycles | reduction |
| --- | ---: | ---: | ---: |
| 3480 | 8,316,333,770 | 7,273,080,133 | 12.54% |
| 6300 | 8,875,664,278 | 7,502,410,198 | 15.47% |
| 8750 | 9,667,340,773 | 7,574,768,188 | 21.65% |

These include unchanged copying/checking overhead. This measures replay work, not the live EE/VU1/GS
critical path or its deadline. Remaining replay cost is dominated by existing 0078
geometry/clipping, where helper experiments above yielded no retained improvement. Further
EE/GS/sound changes need an uncontended live profile; no speculative shortcut is added.

### Clean build gates

All game units freshly compiled, normal release flags, no recomp-basis override. Default r46-plain
reference, exclusions/checkers unchanged; all 14 EE comparisons and every Z read MATCH in each case:

| commit | content | matching Z reads | through field |
| --- | --- | ---: | ---: |
| e65990ca | queues | 1,116 | 9037 |
| e36d1fa8 | plus 1430 | 1,113 | 9034 |
| 8d544317 | plus 2DC8 | 1,131 | 9052 |
| 7c996d9c | plus 0E10 | 1,136 | 9057 |
| 451bab30 | plus 2E48 | 1,090 | 9011 |

Four-kernel clean: 3,797 compile / zero errors; SHA-256
0fe8db89e0f2462ade211dcd4bcf8430a068152466163cd0a7e078165d73a000. Final committed report revision
will be clean-built/gated again.

### Race campaign

Full evidence preserved under build/port/r66/race-discarded-0210/ before reusing the fresh campaign
names.

## Final rejected experiment: 0078 setup

Finding 046's next candidate was the setup at 0088..0168 around the existing 0078 loop. A private
helper handles the untransformed branch only, declining before writes for the extra 0FA8 transform,
unsupported counts or pending Q. It matches all 36,000 captured calls, with 3,464 / 2,220 / 240
setup hits. Eight variants of 7,700 target inputs (61,600 checks) also match: baseline, forced
transform fallback (zero setup hits), forced simple branch, zero matrix W, forced clip bound, zero
depth parameters, and both handedness branches. Standalone target medians improve only 1.64% / 1.68%
/ 9.43%; whole replay medians 22.606 -> 22.579, 23.222 -> 23.095, 23.618 -> 23.587 ms are
negligible.

A fused setup+geometry prototype avoids an extra register save/reload. It also matches all 36,000
real and 61,600 variant checks. Same-executable masks 4095/8191, three interleaved single-thread
replays, 10 repeats, busy shared PC: 0078 whole target-call medians 2.368 -> 2.187, 2.893 -> 2.752,
2.741 -> 2.367 us (7.63% / 4.87% / 13.65%). Whole-capture medians 23.988 -> 23.020, 24.236 ->
24.099, 24.861 -> 24.409 ms.

But the batched thread-cycle test (same conditions as above, 50 replays, AB/BA/AB) fails to show a
consistent whole-workload gain:

| capture | setup off, cycles | fused setup on, cycles | change |
| --- | ---: | ---: | ---: |
| 3480 | 6,204,762,383 | 6,233,074,055 | +0.46% |
| 6300 | 6,544,014,718 | 6,326,043,896 | -3.33% |
| 8750 | 6,415,964,945 | 6,633,281,126 | +3.39% |

Both prototypes are discarded. No setup bit, helper, dispatcher or signature change is retained. The
shipped mask remains 4095 and the four-kernel source 451bab30 remains unchanged. A favorable per-key
wall time is insufficient to justify mixed/regressing whole-thread work. This exhausts the current
measured kernel/helper candidates; the remaining work is uncontended live evaluation.

## Strict O3 compiler trial (discarded)

While waiting for an isolated race slot, compiled only the current VU runtime with /clang:-O3
instead of O2. Same SSE2, -mno-fma, SSE scalar arithmetic and -ffp-contract=off; no fast math. The
compiler -### output confirms effective -O3. Ordinary 36,000-call differential replay matches all
registers/data/GIF.

The stronger result is that all 2,402,834 emitted code-section bytes are identical to the freshly
compiled O2 runtime (concatenated COFF code sections SHA-256
922c90ff44322af0d6130efaf1940f2e128dd0a5a8fb9ca9ec475c20befe26f7). No compiler-flag change is
retained. Object metadata differs, code does not. Batched cycles, same 50-replay AB/BA/AB protocol,
three medians:

| capture | O2 cycles | O3 cycles | change |
| --- | ---: | ---: | ---: |
| 3480 | 5,475,474,308 | 5,575,785,477 | +1.83% |
| 6300 | 5,811,585,282 | 5,537,351,417 | -4.72% |
| 8750 | 5,549,619,870 | 5,742,188,552 | +3.47% |

Identical code exposes several-percent shared-machine variation even in thread cycles
(cache/resource/power contention still matters). The larger four-kernel 12.5-21.6% batch reductions
and 1.6-5.6x target-call gains remain useful evidence, not precise live-race effect sizes. Small
helper gains were correctly rejected. Research: Clang's command guide distinguishes O3 from fast
math: https://clang.llvm.org/docs/CommandGuide/clang.html ; contraction control:
https://clang.llvm.org/docs/UsersManual.html#controlling-floating-point-behavior . No external code
copied.

The latest committed report revision 5bc51679 has now been clean-built too: 3,797 game units, zero
errors, normal release flags; manifest SHA-256
565781ac6c081e9b8cce6264605d1cd15819a8362c455d67dfe322a8011b0ed9. Default r46-plain gate MATCHES all
14 EE comparisons / 1,094 Z reads through field 9015. --env DHD_VU1_NATIVE_STATS=1 affects only the
native arm's logging; the production log positively records kinds 256, 512, 1024, 2048, proving that
the production word-loader guards activate all four new kernels. No comparison or reference changes.
Its completed gate moved to the same F: artifact folder. Gate output constraints and comparison code
remain unchanged.

## Clipping schedule specialization (discarded)

A private VM probe extracts VF read masks, component-write latencies and divider latency from the
actual pair_stalls/upper/lower/commit helpers for all 386 clip pairs. A worklist enumerates
readiness/Q states over both conditional edges and conservatively all 18 valid indirect return
labels plus caller return. Entry is mature after the existing NATIVE_SAVE drain. 506 abstract
states, all 386 pairs reachable, no missing edges: 375 PCs have a constant stall, 11 retain variable
0/1 stalls. Constant nonzero stalls are 22C0=1, 23C8=3, 23E8=1; other constant sites are zero.
Private generated code replaces only those proven checks by the same cycle advance, retaining all
readiness writes, flag handling, divider updates and value operations. All 36,000 real sequence
comparisons match. No runtime or generator source change is retained.

Three interleaved single-thread batches, 50 replays / input, same ordinary O2 runtime source and
masks, busy shared PC, AB/BA/AB, median raw thread cycles:

| capture | normal | static-stall prototype | change |
| --- | ---: | ---: | ---: |
| 3480 | 5,691,003,579 | 5,817,651,447 | +2.23% |
| 6300 | 5,838,423,075 | 5,903,465,370 | +1.11% |
| 8750 | 5,918,707,170 | 6,553,102,376 | +10.72% |

Rejected on whole-workload performance. The cause is not established; fewer VM checks alone is not
evidence of a live CPU win. Data/metadata and generated code stay ignored under
build/port/r66/clip-schedule/.

The guarded race campaign obtained one accepted base run at 03:13-03:14 UTC: 145.782 fields/s, p99
11.668 ms, worst 1% 13.047 ms, 100% focus; no game before, during or after. This is one sample, not
a three-run median. CPU probe execution began after both attempts had ended; no accepted race run
overlapped this private experiment. Process and file timestamps are recorded locally.

## EE profile follow-up

Existing native-arm gate profile (5bc51679, DHD_PROFILE=1, interpreted VU0), read with profile.py
and its matching clean-build map. Shared-PC gate samples; these rank costs, not uncontended race
timings:

| fields | samples | ps2_vu0_run | asm run | lookup | sound render |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3000-3500 | 936 | 31.9% | 7.3% | 1.7% | 1.2% |
| 6000-7000 | 1,730 | 22.8% | 14.0% | 2.4% | 1.6% |
| 8600-9100 | 698 | 24.2% | 12.2% | 1.6% | 3.6% |

Lookup already caches compiled asm by literal pointers and is a small cost; no parser/cache change.
VU0 is the largest remaining EE cost. Finding 051 already proved its opt-in AOT blocks exact but
twice found no race win because that EE work overlapped VU1. The current faster VU1 kernels may
change this; the guarded campaign adds a separate DHD_VU0_AOT=1 arm, three interleaved runs, same
executable and settings. No VU0 default or arithmetic change is made without a measured current-race
benefit and its mandatory exactness checks.

## Signature audit: guard the 1430 exit tails

A late audit found that the 1430 kernel retires its bulk MAC/status effects before returning to the
interpreter. The retail branch/delay slot and common GIF tail contain no early flag read, but the
original signature did not cover those words. Guard the short exit through 1630 and the full common
GIF tail before entering this kernel. A changed tail now declines before any direct writes. The
kernel end address, arithmetic, default mask and retail execution remain unchanged.

Concrete negative controls replace the lower instruction at 1630, and separately 0FF8, with FMOR
VI15,VI0. Each old executable incorrectly accelerates all 467 1430 inputs and differs on 255. Each
corrected executable declines all 467 and matches the interpreter on every input. This checks a real
signature failure, beyond deliberately corrupting an output. The 2DC8 tail guards remain equivalent.

After tightening: all 36,000 real sequence comparisons and all 19,060 forced variants match, with
unchanged positive retail coverage of all four kernels. The independent PCSX2 suite passes 1,584
cases across 32 entries, plus the instruction, timing, continuation and XGKICK checks (zero
failures).

### Final guarded runtime revalidation

The stricter 1430 signature retains all 1,744 real target-call matches against both PCSX2 and the
reference interpreter: kind 256/512/1024/2048 positive coverage is 467/1,021/146/110. Rebuilt oracle
negative control flips VI1 on a real accelerated 1430 call: it reports two differences and exits
nonzero. All eight retail ELF sections MATCH again.

Fresh build_clean.py build of 62ad9972 (3,797 game units, ordinary O1 game / O2 runtime flags, no
recomp-basis) passes the default r46-plain gate: all 14 EE comparisons and 1,102 Z reads through
field 9023 MATCH. Native-arm logs show all four added kernel kinds active with the tightened
signatures.

A fresh untraced O2 replay executable built from the guarded runtime repeats the batched cycle
protocol above: 50 replays/input, 12,000 inputs/capture, three interleaved AB/BA/AB runs, same
executable and masks 255/4095, single thread, shared PC. Medians:

| capture | old kernels, cycles | all additions, cycles | reduction |
| --- | ---: | ---: | ---: |
| 3480 | 8,356,588,836 | 7,380,274,928 | 11.68% |
| 6300 | 8,877,840,029 | 7,448,207,475 | 16.10% |
| 8750 | 9,269,675,063 | 7,301,814,338 | 21.23% |

This is the latest guarded-runtime measurement. It corroborates the earlier batch result without
asserting a live deadline improvement. The fresh race campaign uses this clean executable and new
files, with three interleaved baseline/final/VU0-AOT arms; earlier isolated or contaminated attempts
are not included in its medians.

### Final EE profile ranking

Read the guarded 62ad9972 native gate profile with its matching clean map. These are sampled
instruction-pointer shares from a gate with capture/statistics logging, on the shared PC; they are
cost rankings, not clean race CPU timings. Stack-word caller output is a heuristic, not an unwound
call tree.

| fields | samples | VU0 micro | asm run | lookup | sound render |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3000-3500 | 1,052 | 33.5% | 7.0% | 0.9% | 1.8% |
| 6000-7000 | 1,677 | 26.0% | 15.6% | 2.2% | 1.7% |
| 8600-9100 | 775 | 23.2% | 12.6% | 2.6% | 2.8% |

Graphics waits account for 14.7%, 10.8%, 21.4% respectively. Lookup already caches translations. No
speculative sound, cache, guest-source or compiler change is retained. Native VU1 0078
geometry/clipping remains a large replay cost; the measured helper/setup/scheduler alternatives
above did not produce consistent whole-capture wins.

## Live race results before the GS staging follow-up

The requested three-run live median is **not available**. Two baseline/optimized pairs passed all
declared game-isolation checks. No overlapped run contributes to these numbers; no slow accepted run
is dropped. The earlier 145.782 baseline and all earlier candidate attempts remain separate.

Conditions: bench_race.py, one game instance (its bootstrap and child processes), unpaced virtual
frame clock, native renderer, sound off, settings=none, seed 81257, ordinary recorded
navigation/race inputs, fields 3000-9100, 100% focus. Pre-run CPU load was 16.7-18.5%. No own build,
gate, oracle, replay benchmark or other CPU-heavy work overlapped. Other non-game activity on the
shared PC is not excluded throughout a run, so these are game-isolated measurements, not
exclusive-PC timings.

Baseline is clean 9b018854; optimized is clean 62ad9972 (the queue/four-kernel implementation,
before the separately measured GS staging follow-up below). Executable hashes, start/end UTC, focus,
environments and process observations are in the local race-guarded/runs.json. Accepted order: B0,
F0, exploratory AOT0, B1, F1.

| run | fields/s | p99 ms | worst 1% ms |
| --- | ---: | ---: | ---: |
| B0 | 153.410 | 11.179 | 12.238 |
| F0 | 172.718 | 9.587 | 10.283 |
| B1 | 152.231 | 11.282 | 12.141 |
| F1 | 158.455 | 9.973 | 10.925 |

Pair throughput gains are 12.59% and 4.09%; worst-1% reductions are 15.97% and 10.02%. These are
**two pairs, not a three-run median**. Candidate variation is material and is reported rather than
removed. Both accepted whole-window optimized runs exceed 144 fields/s on average, but the 6.9 ms
tail target is unmet. A stable 144-step deadline has not been demonstrated.

Focused windows, calculated from each accepted timeline (minor whole-window rounding differs from
the benchmark's own printed summary):

| fields | B0 -> F0 fields/s | B1 -> F1 fields/s | B0 -> F0 worst 1% ms | B1 -> F1 worst 1% ms |
| --- | ---: | ---: | ---: | ---: |
| 3000-3500 | 111.633 -> 125.948 | 110.851 -> 122.916 | 13.938 -> 11.663 | 14.241 -> 11.815 |
| 6000-7000 | 140.734 -> 152.897 | 139.013 -> 144.034 | 12.649 -> 10.664 | 12.362 -> 11.180 |
| 8600-9100 | 140.175 -> 165.715 | 140.406 -> 130.362 | 12.144 -> 8.952 | 12.013 -> 13.930 |

The late F1 window regresses; it is retained. Timeline busy intervals include scheduling, so they do
not establish an intrinsic kernel regression or its cause. The start remains below 144 fields/s in
both candidate runs, even as its tail improves. The remaining 0078 geometry/clipping cost and
EE/packet latency need another measured improvement before claiming stable 144.

Against F0's 172.718/9.587/10.283 it is neutral. Remaining AOT repetitions were dropped to
prioritize the mandatory B/F pair; this single sample is not proof of a benefit or regression. VU0
AOT remains opt-in; no VU0 change is shipped.

At this stage the retained wins were the acquire queue observations and four exact VU1 kernels; the
later GS staging win is measured separately below. All further helper/setup/compiler/scheduler
prototypes tested above failed to show a consistent whole-capture benefit and are not shipped.

Acceptance of the final revision additionally requires its own fresh build_clean.py build and
default r46-plain renderer gate, with native kernel statistics proving execution. The final
executable/manifest and gate result carry that revision's hash; the handoff reports the result. No
reference, checker, exclusion, retail matching rule or error condition was weakened.

## GS follow-up: gather complete GIF qwords

A final GS-thread sample was taken with the existing DHD_PROFILE_THREAD=gs mode, clean d246fd05 and
its matching map. The default renderer gate of this revision already MATCHES: 14 EE comparisons /
1,109 Z reads through field 9030, with all four new VU1 kinds active. It stops at the final g+4500
capture. Its trace reaches field 7788, so there is no 8600-9100 sample; that empty window is not a
zero-cost measurement. The two covered windows show:

| fields | samples | external/wait | GS consumer | native_draw | gs_feed | gs_write |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 3000-3500 | 4,430 | 52.6% | 16.6% | 11.1% | 7.5% | 4.4% |
| 6000-7000 | 6,524 | 30.6% | 20.2% | 13.2% | 11.7% | 6.4% |

Address histograms plus disassembly place almost all middle-window consumer samples in the existing
PAUSE/poll loop, not packet copying or retire stores. The 4,096-spin policy stays: reducing it
without a game-isolated latency comparison could save CPU while making packet wakeups slower.
Native-draw samples are diffuse. The next focused candidate is the measured 7-12% GIF parser: it
copied and counted each 32-bit word separately before processing one complete 128-bit qword.

Change port/gs/gs.c to gather a whole qword with memcpy when have=0 and four words remain. Partials
still use the scalar path. Input overlapping the current GsPath conservatively uses the scalar path,
avoiding overlapping memcpy and preserving sequential alias semantics. The callback still sees the
same p->words address and have=4 before gif_qword; tag/packed/reglist/image decode, register writes,
native rendering and their ordering are unchanged. No SIMD math, new ISA requirement, spin, fence or
rendering approximation is introduced. No external code was adapted for this change.

Exactness:
- 18,000 synthetic packet comparisons, all paths and modes with random word
  splits; complete GS state (including VRAM, palette, pending transfer and GIF
  path state) matches. 118,008 bulk qwords execute in this validation build.
- 48 alias cases and five empty/invalid-path cases match.
- Six real frozen GS captures: 4,814 records, 1,174,599 qwords; 962,406 bulk
  qwords execute despite randomized 1-31-word input splits. Complete state is
  compared at prefixes and the end; ordered transfer/draw/readback callback
  records are compared byte for byte for every record, not only by a hash.
  Callback records include qword bytes, path state/have, source path, primitive
  type, vertex bytes and GS registers. All match.
- Fast-path bit corruption fails immediately. A separate negative control
  changes have only during the callback and restores it before decode: final
  parser semantics remain otherwise unchanged, but the ordered-byte comparison
  correctly fails on callback state (84/84 bytes, first packet).
- Add a small permanent test for every split of a two-qword packet (including
  empty pieces), callback storage/have/bytes, and input aliasing staging memory.
  Updated test_gs: 3,000 upload fuzz cases / 6,657,394 pixels, zero differences;
  all memory/GIF/transfer/CLUT/scanout/readback tests pass. Fast staging
  corruption makes the suite fail; the independent pixel-transfer negative
  control reports one differing upload and exits nonzero.

Timing is a single-thread CPU-only GS-model replay, not a native-renderer or live-race benchmark. It
excludes the native recording/GPU backend (a draw callback stands in for it), loads each capture
before timing, restores the same complete initial GS state and replays its records 50 times.
Restores, register/transfer work and callback dispatch common to both arms remain timed;
validation/trace hashing and file reads do not. Same executable, unchanged scalar staging oracle
versus new staging, three interleaved AB/BA/AB runs per capture, two QueryThreadCycleTime calls
around each batch, medians of raw thread CPU cycles. Never converted to elapsed seconds or game FPS.

| capture | scalar cycles | qword cycles | reduction | second-series reduction |
| --- | ---: | ---: | ---: | ---: |
| gs-g+0 | 654,630,855 | 593,436,841 | 9.35% | 9.77% |
| gs-g+2 | 627,761,588 | 562,066,473 | 10.46% | 8.65% |
| gs-g+12 | 640,536,793 | 594,544,773 | 7.18% | 8.32% |
| gs-g+16 | 656,663,503 | 600,237,312 | 8.59% | 10.67% |
| gs-g+3600 | 445,181,357 | 393,187,312 | 11.68% | 10.00% |
| gs-g+4500 | 570,726,271 | 490,585,855 | 14.04% | 10.15% |

The second interleaved series attempted coalescing adjacent same-path records up to 64 at a time.
These captures have no such adjacent records, so record counts and workloads remain identical: it is
a replication series, not a second input shape. Both series improve every capture (7.18-14.04% in
the first, 8.32-10.67% in the second), so retain the gather fast path.

The two accepted live B/F pairs above predate this GS addition. No extra live FPS gain is claimed
for it. The final source revision must be clean-built and pass the unchanged default r46-plain gate
after this addition as well. The remaining queue spins are intentional latency work; no tuning is
retained without a valid latency comparison. Other rejected prototypes stay unshipped.

After the GS addition, test_split_graphics.py is rerun against the current production sources:
FIFO/wrap/copy/provenance/fences and VIF integration pass; 200,000 UNPACK trials / 16,140,614
vectors have zero differences. The retail image tool again MATCHES all eight sections. The final
clean build/gate is required after committing this measured GS improvement and its tests.

## Round 51 continuation: VU0

The clean 060383e5 CPU executable passes its final default r46-plain gate:
14 EE comparisons and 1,101 Z reads through field 9022 MATCH. Rechecking that
same executable with DHD_VU0_AOT=1 also MATCHES: 14 EE / 1,108 Z reads through
9029. No default, reference, exclusion or clock change is made for AOT.
Fresh VU0 block replay: all 104 captures / 13 entries with 32 seeded variants
(3,432 full CPU/data/VU1-state comparisons) match the interpreter. The pinned
PCSX2 interpreter independently matches 936 cases both without and with AOT.

VU0 statistics in the diagnostic native gate, at three million calls, rank 0000 at 23.4% of VU0
ticks, 05E8 at 20.7%, 02E0 at 17.5%, 08D0 at 9.6%. These are shared-PC diagnostic rankings, not
isolated timings. Add exact native kernels for the first three complete entries in
port/runtime/vu0_native.h:

- 0000..00B8: two transformed culling bounds, with five delayed flag reads.
- 02E0..04E8: transformed bounding volumes, retaining raw MR32 rotations,
  masked ABS/moves, the data-memory load, intermediate ACC, and the four
  distinct MAC producers observed by the final FMANDs.
- 05E8..0758: transpose the local basis and transform its axes by four matrices.
  Each FMAC contributes to sticky flags; final current flags and ACC match.

The native path runs under the existing toward-zero/FTZ/DAZ mode, with the existing operand clamp
and separate SSE2 multiply/add instructions. No fast math or FMA. Full exact-word signatures,
including every E delay slot, are loaded from the player's disc by the existing generator/loader.
They are compared on every VU0 call because EE stores can change VU0 code. Unloaded signatures and
nonzero VI0 decline before writes. DHD_VU0_NATIVE=0 restores the interpreter (or opt-in AOT);
tracing/interpreter-only modes also decline. No microcode words are tracked or embedded in
production.

Initial integrated validation: three million raw-bit CPU/data cases match the separately compiled
original interpreter; 48 + 132 + 94 individual instruction-word mutations all decline and match,
including errors from invalid mutated instructions. All nonzero-VI0 controls decline. The
independent PCSX2 comparator matches 792 captured/seeded cases across these entries. The permanent
harness checks positive native execution, whole CPU state and all 4 KB of data memory. A
culling-result corruption fails immediately; the final harness also provides a separate failing
result control for each kernel. An early private fuzz mistakenly randomized VI0, which the old
interpreter reads directly; the production guard handles this unsupported state. The fuzz now tests
hardware-valid VI0=0 and separately proves that guard.

Three interleaved single-thread batches, same executable, two million restored calls per arm and
entry, AB/C then C/B/A then A/B/C, QueryThreadCycleTime around each whole batch; median raw thread
cycles:

| entry | interpreter | AOT | native | AOT/native |
| --- | ---: | ---: | ---: | ---: |
| 0000 | 3,821,528,338 | 2,938,625,625 | 885,123,868 | 3.32x |
| 02E0 | 14,057,993,580 | 8,581,103,075 | 1,715,022,375 | 5.00x |
| 05E8 | 10,543,217,400 | 9,280,726,769 | 2,022,347,916 | 4.59x |

No own game, compilation, gate or other timing job overlaps these batches. Input reads precede
timing; restoring CPU state and runtime call overhead are included equally. Every native batch
positively reports 2,000,001 hits; the other arms report zero. These are kernel-work measurements,
not race FPS. The earlier private wall-time pilot overlapped a gate and is not used.

Research: the existing pinned PCSX2 VU0 interpreter was reread for paired upper/lower ordering,
stalls and final pipeline flush:
https://raw.githubusercontent.com/PCSX2/pcsx2/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2/VU0microInterp.cpp
. No external implementation was copied; GPL oracle sources remain in build/.

### Slow-field evidence

Reanalyzed every accepted historical timeline, without dropping outliers. 39/61 (F0) and 25/61 (F1)
slowest-one-percent fields fall in 3000..3499; the corresponding baselines have 43/61 each. Fields
6285 and 9004 recur among the worst in every accepted arm. F0's worst-percent mean is 10.283 ms: EE
busy 6.432, EE waiting VU 2.776, EE waiting GS 1.076; F1 is 10.925 ms: 6.745 / 3.045 / 1.136
respectively. No VU/GS ring-full time is observed. The tail spans all field residues modulo eight,
so the flare readback alone does not explain it. These busy intervals include scheduling, not just
CPU work.

The existing clean gate's EE profile has only seven samples at 9003..9005; 6284..6286 is mostly
external/wait. Its stack-word caller labels are heuristic and do not prove a loading, particle or
rider cause. At 3245..3274 VU0 is 36.1% of 72 samples and graphics_wait 26.4%, supporting the VU0
work above. No scene-content attribution is invented from these samples. Existing 0078
setup/fusion/static-clip-schedule candidates remain rejected by their recorded whole-replay results.
The deferred-vsync implementation in finding 048 already passed exactness but regressed late-race
throughput; it is not enabled without a fresh isolated latency benefit.

Final production-runtime validation, clean build and default gate follow.

### Two frequent short VU0 entries

Extend the same exact guarded method to 07B8..0800 (translated-point squared length; final ACC.x
retains x*x+y*y while VF6.x adds z*z separately) and 0808..0828 (three-row direction transform,
homogeneous W=1). Masked stores retain all other lanes, especially ACC.w and VF6.yzw in the first
routine. These entries account for another 6.3% / 3.2% of the diagnostic VU0 ticks. Their complete
signatures include the cross-MPG boundary at 0800.

Final five-kernel validation: five million raw-bit cases, complete CPU and 4 KB memory comparison,
all 304 instruction-word mutation guards, invalid VI0 controls, and 1,320 real/seeded independent
PCSX2 cases MATCH. Each oracle process explicitly requires exactly one native hit before comparing
its complete existing register/flag/data outputs. Boundary selection uses high PRNG bits so the
LCG's short low-bit cycle cannot suppress edge encodings. Every VF and data word mixes arbitrary
bits with signed zeros, subnormal limits, finite limits, infinities and NaNs. All five separately
corrupted kernel results fail on trial zero; the negative command exits 1.

Same-executable three-arm interleaved batches, now twenty million restored calls per short
entry/arm, three runs, same CPU-cycle protocol and shared-PC conditions as above. No own compilation
or game overlaps timing:

| entry | interpreter cycles | AOT cycles | native cycles | AOT/native |
| --- | ---: | ---: | ---: | ---: |
| 07B8 | 16,269,536,030 | 9,663,883,920 | 4,785,428,212 | 2.02x |
| 0808 | 11,826,047,921 | 7,985,927,468 | 4,118,755,725 | 1.94x |

Every native batch reports 20,000,001 hits; other arms zero. Retain both. All five default native
kernels can be disabled independently of VU0 AOT with DHD_VU0_NATIVE=0. Per-entry hit counts are
diagnostic-only under the existing DHD_VU0_STATS reporting interval. Native arithmetic does not
change the EE clock, matching source, thread/queue order or any game setting.

### Final CPU integration and production-object replay

Clean build_clean.py source 73ec1bb6 freshly compiles all 3,797 game units, zero errors, ordinary
release flags and no recomp-basis. Executable SHA-256:
a282259f0dd973301d0f80ca0e5eabb47860a296d9974cba7df0fbf7299b64ef. The unchanged default r46-plain
renderer gate MATCHES all 14 EE comparisons and 1,118 Z reads through field 9039. Diagnostic native
counts positively exercise all five VU0 entries and all four earlier VU1 additions. Current-source
VU1 sequence replay: 36,000 real calls, zero differences, all twelve kinds exercised. Independent
PCSX2 VU1: 1,584 cases / 32 entries, zero failures, plus instruction/timing/continuation/XGKICK
checks. Split graphics/VIF: zero failures, 200,000 UNPACK trials / 16,140,614 vectors exact. The
retail-image tool again MATCHES all eight sections.

A second replay links the actual unmodified clean-build runtime object, SHA-256
ca18ae46ee5ab6412c15f941a710f49c438ff7406a0b342e45bd4f0576fb3c51. Unlike the ordinary embedded test
harness, it loads signatures from the local vutext image through the production loader before
timing. Same three arms, two million restored calls per entry/arm, three interleaved batches, median
raw thread cycles. Own gate has finished; no own game, compilation or other timing job overlaps.

| entry | interpreter | AOT | native | AOT/native |
| --- | ---: | ---: | ---: | ---: |
| 0000 | 3,858,098,872 | 3,108,394,185 | 966,846,333 | 3.21x |
| 02E0 | 13,633,561,824 | 8,919,490,250 | 1,664,661,975 | 5.36x |
| 05E8 | 9,065,874,615 | 10,345,240,291 | 1,913,929,949 | 5.41x |
| 07B8 | 2,385,275,841 | 1,254,062,538 | 612,204,615 | 2.05x |
| 0808 | 1,623,539,049 | 1,041,084,429 | 494,670,531 | 2.10x |

Every native batch reports 2,000,001 hits, others zero. In this noisy series 05E8 AOT is slower than
the interpreter; no AOT default is inferred from it. The isolated race queue uses one clean
executable and explicit native-off, native-off+AOT, and native-on arms. Historical live results
above remain the only accepted before/after race table and predate both the GS staging follow-up and
these VU0 additions. The steady 144 / 6.9 ms tail target is still unproved. AOT stays opt-in
(here; built always since finding 090 SP1).

Next deliverable is the requested throwaway combined test branch, merging the then-latest committed
FPS work, with its own clean build and 60 gate.

### Combined test branch preparation

The sole content conflict is docs/STATE.md: retain both appended histories, in order. No runtime
conflict. Git's committed host.c has mixed line endings despite text/eol=lf, so checkout alone
leaves it dirty. No stash or source-overlay commit. Restore those exact bytes on return to the CPU
branch. The FPS branch's documented high-rate equivalence limits still apply to its 144 mode. 