# 111: GPU submission overlap and same-field native Z probes

Status: current
Checked: 2026-11-04

2026-10-28. Branch `perf/gpuoverlap`, based on `prof/android-pcsx2` and `perf/phone120-measure`
(`f50127553`). No wgpu-hal or framebuffer-cache changes. Number checked against main and every
local/remote perf branch: 103 is the framebuffer-cache lane, 104 is reserved, 105 was free.

## Contract and experiments

The native probe already maps asynchronously. `native_zread_after` takes the newest completed
previous copy, then submits the current copy. `zread_take` uses `PollType::Poll`, not a blocking
readback wait. The exact probe has a different contract and does block. Finding 108's description of
the native probe as a readback wait must therefore be distinguished from measured elapsed time
inside its nonblocking poll/maintenance call.

Two experiments, both disabled by default:

- `DHD_ZREAD_FOLD=1`: snapshot the probe request after preparing the field's
  surfaces, append the compute pass and copy after the picture's draws, then
  map after submission. Publication stays after present. The current probe tag
  is explicitly excluded from that take; finishing the current copy early
  cannot expose it to the game a field early.
- Also `DHD_GPU_OVERLAP=1`: keep the picture encoder open until the present pass
  is appended, then submit picture, probe and present together. A skipped
  present flushes at `zread_take`. The C pacing call and `surface.present` keep
  their existing order. This is an experiment for the ordinary native render
  loop, not an interpolation/browser setting.

`DHD_SUBMIT_POLL=1`, with `DHD_RSTAT=1`, separately times a nonblocking maintenance poll immediately
before picture submission (`prepoll`). `DHD_GPU_TIMING=1` requests supported timestamp features and
measures picture execution with a three-slot asynchronous query/readback ring. Neither diagnostic
waits for a free ring slot. GPU timestamps and the extra maintenance poll are excluded from the
ordinary A/B runs.

## Exactness: do not let the standard trace hide the native probe

`state_trace_run.py` sets `DHD_EXACT_PROBE=1`. A standard trace PASS proves that the core and exact
path stayed unchanged; it does not prove that scheduling of the default native probe stayed
unchanged. This work additionally compares `DHD_GS_READBACK_TRACE` transcripts with
`probe_trace_diff.py`: every record, metadata field and returned byte, including repeated reads in a
field. No field shift, coverage tolerance, mask change or dropped read is allowed.

Completed checks:

- Windows x64 base versus experiment, 5,000 headless race fields, seed 81257,
  scale 2, 4:3, widescreen off: `trace_columns.py` PASS on all 13 columns,
  existing three-word mask unchanged.
- Windows DX12 hidden desktop, 5,000 fields with the **native** probe: all
  1,076 readbacks identical on their original fields, and all state-trace
  columns identical. Both processes exit 0.
- A repeated Windows baseline, with field captures, returns the same 1,076
  readbacks as its first run.
- The unchanged phone baseline, two native-probe runs (the second includes
  captures), returns 1,076 reads in both but differs in three: fields 1698,
  3939 and 3940. RAM differs from field 1698, and VU0/GS stream/registers from
  3940. This is a control failure: the native probe's newest-completed policy
  is timing dependent even without this optimization. It must not be hidden
  behind an exact-probe trace PASS.

Further checks:

- Phone headless, 5,000 fields against x64: `trace_columns.py` PASS on every
  column, mask unchanged.
- Phone experiment versus the **first** native baseline: all 1,076 reads and
  every state column match. Versus the repeated/capture baseline: the same
  three reads differ as in the baseline control, with the same downstream
  differing columns. All comparisons are retained; none is selected away.
- Phone captures at fields 2000, 3000, 4000 and 4990, seed 81257, scale 2:
  byte-identical BMPs, zero differing pixels. Windows captures at the same
  four fields are also byte-identical.

The experiment remains opt-in: matching one of two varying baselines does not establish the strict
same-values-on-same-fields contract in all runs.

## Where submit blocks

An additional 1x paced diagnostic run uses `simpleperf --trace-offcpu`, 500 Hz CPU sampling and
DWARF call graphs. The 104.962-second profile contains 554,686 samples with zero lost. On the native
thread it accounts for 76.654 CPU seconds and 27.168 off-CPU seconds. `vkQueueSubmit` contains 7.314
CPU seconds and 12.560 off-CPU seconds; 12.219 of the latter follow `vkQueueSubmit ->
gsl_command_issueib_timeline_sync -> ioctl_kgsl_gpu_command`. For the picture's submit alone the
driver contains 4.724 CPU seconds and 11.493 off-CPU seconds. Native probe start/take contain only
0.308/0.239 off-CPU seconds. This places most submission descheduling in the driver/kernel GPU
command path, not a native-probe map wait or a presentation acquire.

The phone's kernel is `5.4.210-qgki-g6e74b92dfc49`. Kernel symbol addresses are restricted. Off-CPU
time includes scheduler preemption, so it is not all proven GPU wait. The exact kernel wait
primitive cannot be named from this recording. Public KGSL source permits submission to sleep for
context-queue capacity; that is a possible explanation, not a proven identification on this
firmware. No kernel, phone setting, queue limit or scheduling policy was changed.

GPU timestamps and a pre-submit nonblocking poll, GO-aligned means:

| scale | picture GPU ms | finish ms | submit after pre-poll ms | pre-poll ms | present submit ms | native probe take ms |
|---|---:|---:|---:|---:|---:|---:|
| 1x | 3.182 | 5.049 | 2.142 | 0.344 | 1.589 | 0.849 |
| 2x | 6.637 | 5.674 | 4.420 | 0.470 | 1.882 | 0.913 |

Both diagnostics have median CPU4/CPU7 clocks 1.3248/1.3056 GHz. GPU means use thirteen 240-picture
query blocks per run in GO+360..GO+3600. The 1x run includes profiling overhead; neither diagnostic
is an A/B speed result. An extra maintenance poll does not eliminate the submit cost. Combining
submits reduces calls but does not make the driver/kernel call itself asynchronous.

## Paired speed measurements

`a` is baseline (both switches absent), `b` sets both switches to 1. Runs use `DHD_FPS=120`;
uncapped additionally sets `DHD_FRAME_CLOCK=1` and `DHD_VSYNC=0`. Each comparison covers
GO+360..GO+3600, requires full timeline coverage, and averages only complete 240-picture timing
blocks inside that window. Pairs run a/b, b/a, a/b. Medians below are medians of the three run
means, not pooled per-field medians. Render time is C `frame` wall time, including handoff wait; it
is not render-thread CPU time.

The interrupted third 2x paced baseline was rerun with its partner. Continuation runs install the
final candidate APK for both arms, leaving both flags absent in a and setting them in b. Earlier
pairs installed separate baseline and candidate APKs. The final source mirror's tracked renderer/GS
files were hash-compared with the assigned checkout before packaging. CPU clocks and temperatures
are sampled every approximately three seconds; new runs wait before starting if zone43 exceeds 65 C.

| Configuration | pairs | baseline fields/s | both fields/s | change | baseline render ms | both render ms |
|---|---:|---:|---:|---:|---:|---:|
| 1x paced | 3 | 82.3 | 81.2 | -1.3% | 11.897 | 11.792 |
| 1x uncapped | 3 | 72.5 | 77.4 | +6.8% | 13.221 | 12.153 |
| 2x paced | 3 | 58.5 | 65.2 | +11.5% | 16.765 | 14.667 |
| 2x uncapped | 3 | 49.3 | 56.7 | +15.0% | 20.192 | 17.197 |

Per-run GO-aligned means; times below are ms/field. Clocks and temperatures are medians of samples
inside that same window. `present` removes the batched finish/submit stopwatch nesting in b, so the
split does not double-count it.

| run | CPU4/7 GHz | zone43/battery C | fields/s | build | finish | qsub | present | probe | other |
|---|---|---|---|---|---|---|---|---|---|
| s1p1a | 1.32/1.31 | 60.2/38.0 | 72.0 | 0.934 | 4.976 | 2.763 | 2.702 | 1.506 | 0.166 |
| s1p1b | 1.32/1.31 | 60.8/38.0 | 75.5 | 1.228 | 4.693 | 3.882 | 1.219 | 1.090 | 0.162 |
| s1p2a | 1.32/1.31 | 60.0/38.0 | 90.8 | 0.677 | 4.301 | 2.080 | 2.308 | 1.362 | 0.157 |
| s1p2b | 1.32/1.31 | 60.5/38.0 | 81.2 | 1.107 | 4.714 | 3.630 | 1.216 | 0.972 | 0.153 |
| s1p3a | 1.32/1.31 | 60.2/38.0 | 82.3 | 0.776 | 4.724 | 2.218 | 2.591 | 1.423 | 0.165 |
| s1p3b | 1.32/1.31 | 60.5/38.0 | 81.2 | 1.084 | 4.728 | 3.585 | 1.226 | 0.956 | 0.154 |
| s1u1a | 1.32/1.31 | 60.2/38.0 | 70.8 | 1.020 | 5.184 | 2.821 | 2.971 | 1.324 | 0.168 |
| s1u1b | 1.32/1.31 | 60.0/38.0 | 75.1 | 1.178 | 5.012 | 3.771 | 1.218 | 0.919 | 0.156 |
| s1u2a | 1.32/1.31 | 59.8/38.0 | 72.5 | 1.020 | 4.843 | 2.832 | 2.994 | 1.360 | 0.172 |
| s1u2b | 1.32/1.31 | 60.6/38.0 | 77.7 | 1.184 | 4.565 | 3.770 | 1.213 | 0.927 | 0.161 |
| s1u3a | 1.32/1.31 | 60.2/38.0 | 73.6 | 1.026 | 4.642 | 2.846 | 2.927 | 1.380 | 0.173 |
| s1u3b | 1.32/1.31 | 60.5/38.0 | 77.4 | 1.169 | 4.945 | 3.794 | 1.204 | 0.885 | 0.156 |
| s2p1a | 1.32/1.31 | 58.2/38.0 | 60.2 | 0.891 | 5.743 | 5.011 | 3.121 | 1.502 | 0.160 |
| s2p1b | 1.21/1.19 | 58.4/38.0 | 65.1 | 0.919 | 5.285 | 6.297 | 1.264 | 0.763 | 0.139 |
| s2p2a | 1.21/1.19 | 57.9/38.0 | 58.5 | 0.930 | 5.884 | 5.032 | 3.170 | 1.583 | 0.166 |
| s2p2b | 1.21/1.19 | 58.2/38.0 | 65.2 | 0.996 | 5.321 | 6.384 | 1.282 | 0.796 | 0.143 |
| s2p3a | 1.21/1.19 | 58.5/38.0 | 58.0 | 0.931 | 6.043 | 5.039 | 3.260 | 1.543 | 0.175 |
| s2p3b | 1.21/1.19 | 58.5/38.0 | 66.2 | 0.930 | 5.138 | 6.242 | 1.256 | 0.747 | 0.144 |
| s2u1a | 1.21/1.19 | 58.8/39.0 | 53.6 | 1.183 | 6.351 | 5.803 | 3.236 | 1.660 | 0.187 |
| s2u1b | 1.21/1.19 | 59.2/39.0 | 56.7 | 1.339 | 6.225 | 7.217 | 1.396 | 0.820 | 0.200 |
| s2u2a | 1.21/1.19 | 57.9/40.0 | 49.0 | 1.001 | 5.959 | 8.015 | 4.508 | 0.678 | 0.168 |
| s2u2b | 1.21/1.19 | 59.8/40.0 | 57.3 | 1.331 | 6.121 | 7.205 | 1.398 | 0.789 | 0.197 |
| s2u3a | 1.21/1.19 | 57.2/40.0 | 49.3 | 1.005 | 5.916 | 7.725 | 4.688 | 0.686 | 0.172 |
| s2u3b | 1.21/1.19 | 59.4/40.0 | 56.0 | 1.312 | 6.352 | 7.222 | 1.386 | 0.793 | 0.202 |

At 1x paced, pair changes are +4.9%, -10.6%, -1.3%: no consistent gain. At 1x uncapped they are
+6.1%, +7.2%, +5.2%; the repeatable gain is about 1 ms per field on the render thread. At 2x paced
they are +8.1%, +11.5%, +14.1%; at 2x uncapped +5.8%, +16.9%, +13.6%. The second/third uncapped
baselines put more time in qsub/present, despite the same median CPU clocks. All samples are
retained. The first 2x paced pair crosses a CPU clock change; the other two pairs repeat the benefit
at matching median clocks.

These results support a batching benefit in uncapped and 2x operation, not a universal paced benefit
or the original estimate of eliminating a synchronous native map. Driver submit time remains
substantial and can move between submits.

The optional fold-only pair was queued but never acquired the phone lock. No fold-only result is
claimed. The three-pair matrix above had already completed before the scope cut.

Final packaging: `build_apk.py --jobs 4 --offline` passed using the final candidate native library
(Android host build passed earlier). APK SHA-256:
`dff2201a702a3ebf2b3cf8a97ce89759aa9930c5dc9102e784b2a5b098cd3afe`. The final source mirror's
tracked renderer and GS sources match this checkout. No default or source code changed during this
continuation. Existing final Windows smoke exits 0 and its 5,000-field state trace passes all 13
columns. The saved phone experiment comparison was rechecked: all 5,000 state rows and 1,076 native
reads match the first baseline; all four capture BMPs hash-identically to the baseline captures. The
native transcript comparator self-test passes. The baseline repeat mismatch described above remains;
no universal native-probe exactness claim is made. No new final-APK phone trace was run after the
scope cut: neither default changed, so the conditional recheck was not required.

Every completed continuation phone run exited successfully and printed `ini restored byte for byte`
before releasing its lock. The cancelled optional run was still waiting behind another lane's lock,
had not installed/launched anything, and needed no restoration. The other lane's lock was left
intact. The branch is ready to merge as opt-in experiments and diagnostics, with repeatability and
default promotion deferred to the merged-build measurement.

## Default decision

Neither experiment becomes a default. The 1x paced result is inside pair noise; uncapped and 2x
gains do not remove the native-probe exactness limitation above. One matching native transcript plus
matching captures is evidence of a successful run, not proof that the timing-dependent
newest-completed policy preserves all same-field values. No mask, comparison tolerance or probe
publication rule was relaxed. Both switches remain opt-in. Since 0.7.1 `0` turns one off and any
other value on (before, any value, `0` too, turned it on); C (`native_gpu_switch`) and Rust
(`zread::switch`) read them the same way. Measured on an M1 Air (Metal) they cost about 10% at 2x,
so even once the probe timing is proven exact they would be an Android-only default.

`DHD_GPU_OVERLAP` is gated on `zread::folded()` in `Pass::new`. An overlap-only run therefore
exercises baseline behavior rather than an independent experiment; it is omitted instead of being
reported as an isolated overlap measurement.

## Reproduction and choices

The unchanged portable wasm core is reused from finding 108. Builds use at most `--jobs 4`. Windows
uses finding 089's private hidden desktop launcher. `/sdcard/PS2/` is untouched.

Safe choice without asking: leave experiments off until native readbacks and pixels satisfy the
requested same-field contract. An exact-probe-only PASS does not authorize enabling them.

## Sources

- Existing findings 052, 089, 090, 091, 097 and 102; the local pinned wgpu
  29.0.3 source, especially `wgpu-core/src/device/queue.rs`,
  `device/resource.rs`, `device/life.rs` and wgpu-hal's Vulkan queue submission
  implementation (read only). Queue submission calls maintenance with
  `PollType::Poll`; retirement can still perform expensive resource cleanup.
- [wgpu issue 9470](https://github.com/gfx-rs/wgpu/issues/9470): a concurrent
  waiting device poll can hold the fence lock needed by submission. A possible
  mechanism to check, not evidence that it is happening in our native path.
- [wgpu Queue documentation](https://wgpu.rs/doc/wgpu/struct.Queue.html):
  uploads are executed with the next submission; writes cannot be freely
  moved across a submission boundary.
- [Khronos queue documentation](https://docs.vulkan.org/guide/latest/queues.html)
  and [submission specification](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html):
  submission ordering and synchronization requirements. No external code was
  copied or adapted.

- [Qualcomm KGSL dispatcher source](https://github.com/qualcomm-linux/kgsl/blob/gfx-kernel.le.0.0/adreno_dispatch.c)
  and [Android kernel KGSL dispatcher source](https://android.googlesource.com/kernel/msm/+/0dee0d1411e4ba837089a769a5bcce57a5a14df2/drivers/gpu/msm/adreno_dispatch.c):
  possible driver-side queue-capacity waits. This is not an assertion that
  either revision is the phone's exact kernel source.

## Merged build (perf/phone120-merged)

The unchanged portable wasm and local VU microcode inputs came from the prior lane.

**PASS:** Android versus x64, 5,000 race fields, seed 81257, scale 2, 60 Hz exact-probe/headless
recipe from finding 110: all 13 columns identical (menu, phase, clock, ram, scratch, hw, cpu, vu0,
gs_stream, gs_regs, gs_vram, sound, code). Existing three-word mask unchanged; no ignored columns.
x64 exit 0; Android completed 5,000 rows.

Lead/user dropped cross-run pixel and capture/readback comparisons as invalid: starting gate
positions and timing vary. One BEFORE capture session finished, the second was interrupted, and
queued comparisons were cancelled. No control verdict or merged native-readback equivalence is
claimed. The interrupted session's original ini was restored and byte-checked before releasing its
lock. One selected screenshot, `merged-mid-race.png`, was saved during ON 2x after logged field
10595 (GO 6022), beyond the timing interval. Rider, terrain, sky and HUD are visible with no obvious
missing render target. This is a visual spot check only. ON 1x also requested a field-6000 capture
before GO; that image is not used for validation or comparison.

OFF APK SHA-256: `94b1cde4f6a18e907f870ad9705947a11a17d617bd6f2d0948645f74241a861d`, byte-identical
to the output in the clean prior `phone120/wt` tree at `f50127553ab41d434289381c47b41a3fc086b4cf`
(finding 107/108 tooling, no VU/framebuffer-cache/overlap work).

Four uncapped runs in requested order: `DHD_FPS=120 DHD_FRAME_CLOCK=1 DHD_VSYNC=0`, seed 81257, 4:3,
widescreen off. Throughput and thermal samples use GO+360 through GO+3600 (3241 fields), with
complete 240-picture RSTAT blocks. Clocks and temperatures below are interval medians of sampled
current cluster frequencies and sensors. All runs GO at field 6022.

| Run | fields/s | CPU0/4/7 GHz | zone43 / battery C | finish ms | queue.submit ms | present ms |
|---|---:|---|---|---:|---:|---:|
| off1 | 91.4 | 1.805 / 1.670 / 1.670 | 65.5 / 35.0 | 3.803 | 2.541 | 1.378 |
| on1 | 82.3 | 1.805 / 1.325 / 1.306 | 61.2 / 36.0 | 2.799 | 3.097 | 6.082 |
| on2 | 78.7 | 1.805 / 1.325 / 1.306 | 63.1 / 36.0 | 3.322 | 6.739 | 10.243 |
| off2 | 58.9 | 1.498 / 1.325 / 1.306 | 61.2 / 36.0 | 6.046 | 5.586 | 1.653 |

Preflight zone43: OFF 1x 46.9 C; ON 1x 55.9 C; ON 2x 50.6 C; OFF 2x 50.2 C. All started below 60 C.
Thermal policy changes clocks during runs; single pairs do not isolate a clock-normalized speedup or
establish sustained 120 fields/s. Overlap moves submission cost between RSTAT buckets; full RSTAT
parts are retained in `raw/go_window.json`. Both the field-6000 capture request and selected
screenshot fall outside the timing interval.

The merged exact-probe state check passes; no renderer code was changed. This reduced scope does
**not** establish the strict native-probe scheduling contract required to recommend both switches on
by default. Earlier native baseline variability and the opt-in conclusion above remain. The tested
code can be merged with switches opt-in; default-on safety is not established.

The interrupted-session restore is recorded in `interrupted-restore.txt`. No `/sdcard/PS2` access.
No APKs, traces, captures or game data tracked. 