# 130: Integrated GPU performance

Status: current
Checked: 2026-11-04

Branch `perf/igpu`, based on local `feat/perf-log` (`0003e65`). Three exact renderer
optimisations retained; final measurements and validation are complete.

## Adapter selection

`DHD_WGPU_ADAPTER=integrated` uses DXGI minimum-power order, `discrete` uses
high-performance order, skipping software adapters. The existing default and WARP remain unchanged.
These are hardware preferences, not guarantees that a machine contains both GPU kinds; inspect
`gs: GPUs` for the actual device. Preference and PCI IDs are logged. The PCI IDs select the same
adapter for DX12 and Vulkan. Failure to enumerate the explicit preference fails device creation.

Research: Microsoft's [EnumAdapterByGpuPreference documentation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgifactory6-enumadapterbygpupreference)
sets out the ordering and Windows 10 1803 requirement. No external code was copied.

## Measurement choices

Use the hidden-desktop harness, its own memory card, seed 81257 and `bench_race`'s race recipe.
Disable GS screenshots so the normal graphics threads run. Use 60 fields/s, 4:3, no widescreen,
with uncapped runs using the virtual field clock and V-Sync off. Select every GPU explicitly.
Crop per-field timing to GO+180 through GO+1800 (3 through 30 game seconds at 60 fields/s),
following findings 108/112. Never use separate live-run pictures for correctness.

The monitor remains attached to the discrete GPU. Integrated-GPU presentation therefore includes
cross-adapter transport. Pass timestamps measure GPU passes, not the compositor's whole cost;
a motherboard-connected monitor would be required to isolate that cost cleanly.

Read findings 021, 045, 085/086, 096/097, 108, 111/112 and 129 before renderer work.
Feedback copies already copy the reachable texture rectangle; conversion caching and independent
conversion grouping already landed in finding 112. Measure before changing either further.

## Adapter change validation

`build_host64.py` passes. `test_gs_wgpu.py` reports VERDICT PASS (GPU, depth, present;
WARP and hardware, DX12 and Vulkan). Host `test_gs` passes: 6,000 upload fuzz transfers,
11,237,790 pixels, zero differences and zero failures. Unchanged versus adapter-selection build:
`state_trace_run.py boot` / `state_diff.py` match all 600 rows with the unchanged three-word mask.
An explicitly integrated DX12 native replay selects AMD PCI 1002:164e and replays both fields.
The default path still selects the NVIDIA adapter.

Native recorded-stream replay succeeds, but `gs_golden.py compare replay-native` refuses the
installed driver's adapter key: no `meta.json` exists for it. The available golden has an older
driver key. This is a validation limitation, not a passing golden comparison; no gate is weakened.
The replay CLI also compares a resolved output path against an unresolved build junction;
calling its existing build/clear/replay functions directly avoids that path-only CLI refusal.

## Measuring actual high scales

The first requested 6x run was invalid as a 6x result: its log explicitly clamped it to 4x.
The dedicated-VRAM guard underestimates usable shared memory on this integrated GPU. Added
`DHD_SCALE_MEMORY_MB`, a logged developer override for only that guard, with player defaults and
texture-pack budgets unchanged. High-scale measurements use 4096 MiB. This does not allocate that
amount or bypass texture-size limits. Verify actual scale in `perf: render`, not the requested setting.
The clamped run is retained only as a diagnostic and excluded from the matrix.
Build and the repeated renderer suite pass. The 600-field boot trace with the override matches.

Refreshed the native replay golden from the frozen oracle executable, verified against its
manifest SHA-256, on the current driver. The two recorded fields' pictures are byte-identical.
Created fresh metadata for this native replay group from those oracle outputs, with current
adapter/driver pins and hashes for all four items. The unmodified `gs_golden.py compare
replay-native` reports four items compared, metadata count four, identical, VERDICT PASS.
The old installed golden is preserved. This resolves the native golden validation limitation above;
it does not re-record unrelated golden groups.

## Baseline (unmodified renderer)

Ryzen 5 7600, AMD Radeon Graphics (PCI 1002:164e), RTX 3070 (10de:2484), 32 GB installed RAM.
Hidden 960x720 window, native renderer, 4:3, V-Sync off in both modes, 60 requested for paced runs.
DX12 developer builds here use FXC (no dxcompiler.dll beside these executables). Packaged builds
select DXC when that DLL is present; a separate DXC control is kept apart from this matrix.
Uncapped uses `DHD_FRAME_CLOCK=1`. Sound is off; the harness has its own memory card.
The 4096 MiB developer scale guard is needed for actual 6x/8x. All runs reached GO at field 3214.
The 60 setting uses the retail NTSC period (1001/60000 s in `port/hle/fps.c`),
so about 59.94 fps is the paced target. The window contains 1620 fields, GO+180 < field <= GO+1800. FPS is 1000 / mean wall ms;
1% low is 1000 / the mean of the 17 slowest fields. GPU figures average complete 60-frame
timestamp blocks inside that window. `DHD_GPU_TIMES=60` times every frame,
so both matrices include profiling overhead (normal play samples one frame in eight).
Units are ms/frame. Every recorded point is one valid run.
`gpu_ms` sums timed passes (finding 129); `span_ms` also includes gaps between the first and
last timestamp. Raw encoder copies outside passes are not individually classified by these
pass-kind figures. Presentation/compositor work beyond that interval is not measured by them.

| GPU / API | Scale | FPS paced | 1% low | GPU ms paced | FPS uncapped | 1% low | GPU ms uncapped |
|---|---:|---:|---:|---:|---:|---:|---:|
| integrated / dx12 | 1 | 59.93 | 51.47 | 2.644 | 262.98 | 114.02 | 2.322 |
| integrated / dx12 | 2 | 59.61 | 36.76 | 5.992 | 173.46 | 110.71 | 5.368 |
| integrated / dx12 | 4 | 50.85 | 15.77 | 18.741 | 49.70 | 35.77 | 18.996 |
| integrated / dx12 | 6 | 19.97 | 9.04 | 49.259 | 23.97 | 12.03 | 40.458 |
| integrated / dx12 | 8 | 13.93 | 8.66 | 71.077 | 13.79 | 9.14 | 71.796 |
| discrete / dx12 | 1 | 59.93 | 55.58 | 1.087 | 290.55 | 155.94 | 0.372 |
| discrete / dx12 | 8 | 59.73 | 39.27 | 4.838 | 291.29 | 159.35 | 2.428 |
| integrated / vulkan | 2 | — | — | — | 144.94 | 111.83 | 5.316 |
| integrated / vulkan | 8 | — | — | — | 14.84 | 10.68 | 66.185 |

GPU pass split, uncapped:

| GPU / API | Scale | Draw | Surface | Convert | Scanout | Present |
|---|---:|---:|---:|---:|---:|---:|
| integrated / dx12 | 1 | 1.988 | 0.000 | 0.071 | 0.059 | 0.205 |
| integrated / dx12 | 2 | 4.869 | 0.000 | 0.054 | 0.281 | 0.165 |
| integrated / dx12 | 4 | 17.120 | 0.000 | 0.066 | 1.416 | 0.394 |
| integrated / dx12 | 6 | 36.563 | 0.000 | 0.065 | 3.135 | 0.695 |
| integrated / dx12 | 8 | 64.357 | 0.000 | 0.066 | 6.131 | 1.243 |
| discrete / dx12 | 1 | 0.213 | 0.000 | 0.105 | 0.029 | 0.025 |
| discrete / dx12 | 8 | 2.043 | 0.000 | 0.064 | 0.259 | 0.063 |
| integrated / vulkan | 2 | 4.719 | 0.000 | 0.082 | 0.297 | 0.220 |
| integrated / vulkan | 8 | 59.515 | 0.000 | 0.066 | 5.292 | 1.312 |

Vulkan on this iGPU is 16.4% slower in throughput at 2x (GPU time almost unchanged),
but 7.6% faster at 8x. The selected AMD adapter and Vulkan backend are confirmed in both logs.
No conclusion about other GPUs or old drivers follows from these two points.

The monitor stays connected to the RTX. These rates include the hidden-desktop presentation path;
pass timestamps exclude work outside the native passes. This setup cannot isolate the cost of
cross-adapter transport or fully reproduce a visible desktop compositor. Repeat with the monitor
on the motherboard to measure that difference.

## Boundary and rejected draw experiment

Baseline 3x: paced **59.93 fps**, 1% low **54.63**, GPU **11.649 ms**; uncapped **80.04 fps**,
1% low **54.73**, GPU **11.748 ms**. Highest measured scale holding the 60-class paced target
before optimization is **3x** (4x is about 51 fps).

A bounded E=1 texture-coordinate shortcut removed float conversion/division and sub-texel work
for small coordinates, retaining the original path outside that range. It passed the renderer
suite, host GS tests, all 33 synthetic presented pictures (dual/fallback: no RGB or VRAM
differences), the frozen C oracle at 1x/2x/4x (12 items), and a strict before/after 8x replay
(four byte-identical items). It was nevertheless rejected on performance:

| 4x DX12, uncapped | FPS | GPU ms | Draw ms | Scanout ms |
|---|---:|---:|---:|---:|
| Original matrix | 49.70 | 18.996 | 17.120 | 1.416 |
| Bounded shortcut | 45.07 | 21.334 | 18.597 | 2.087 |
| Preserved baseline repeated afterward | 49.77 | 19.108 | 17.286 | 1.380 |

The repeated baseline settles the doubtful comparison: the shortcut regresses the measured
frame and is not retained. An unchanged pass also moved, so per-pass duration alone must not be
interpreted as an instruction-count measurement.

The frozen C oracle itself supports at most 4x. An attempted 8x oracle recording was detected as
a clamped 4x image by the unchanged golden checker (size mismatch), excluded, and preserved as a
diagnostic. Correct oracle goldens now cover 1x, 2x and 4x. At 8x, compare the same recorded stream
against a replay executable built from the pre-change renderer; do not compare separate game runs.

Shader arithmetic reference: the [WGSL specification](https://www.w3.org/TR/WGSL/).
No external shader code was copied. Arithmetic shortcuts must preserve integer/float conversions,
large wrapped coordinates, bilinear interpolation and blend rounding.

## Retained: bounded integer scanout and repeated-row reuse

The second-largest high-scale pass was scanout (6.131 ms at 8x uncapped). For scale 1–8 and
display dimensions at most 32768, use a separate compute entry point. The original pipeline is
retained for other inputs. No format, blend, filter or pass-order change is involved.

The original coordinate is `((origin + at/S) & 2047)*S + at%S`. This equals
`(masked_origin*S + at) % (2048*S)`. Its dividend is below 65536: masked origin contributes at
most 2047*8 and a valid invocation at most 32767. Use `ceil(65536/divisor)`, a multiply/shift,
and one integer quotient correction. The reciprocal overestimates the quotient by at most one
on that domain. All 524,288 dividend/scale combinations match ordinary integer remainder.
For the interlaced mapping, two output rows read exactly the same texel; one invocation now
loads it once and writes both rows, guarding an odd final row. Other row mappings stay intact.

Research: [Labor of Division, Episode I](https://ridiculousfish.com/blog/posts/labor-of-division-episode-i.html)
for reciprocal division and error bounds, plus WGSL's integer semantics. Implementation and the
bounded-domain derivation are original; no external code was copied.

| 4x DX12, uncapped | FPS | 1% low | GPU ms | Draw ms | Scanout ms |
|---|---:|---:|---:|---:|---:|
| Preserved baseline | 49.77 | 20.51 | 19.108 | 17.286 | 1.380 |
| Fast scanout | 52.06 | 36.77 | 18.239 | 17.067 | 0.718 |

FPS improves 4.6%, total GPU time falls 4.6%, scanout time falls 47.9%. Correctness checks:
host GS (zero failures), full `test_gs_wgpu.py` (PASS), synthetic presented transfers (33 pictures,
zero RGB/VRAM differences between dual/fallback), 12 oracle replay items at 1x/2x/4x identical,
all four before/after replay items at 8x byte-identical, and all three native init unit tests
(including the browser WGSL shift transformation) pass. No game/core code changed.

The second draw shortcut used only a uniform E=1 branch and preserved the integer/float round
trip for large coordinates. It also passed all pixel gates but measured 45.96 fps / 21.037 ms
at 4x uncapped, worse than the preserved baseline. It is rejected too. Neither shortcut remains
in the retained scanout change.


## Retained: specialise decoded unit-scale texture coordinates

The draw loop knows the final uploaded S/E scale pair. For ordinary decoded textures with
S=E=1, use a pipeline override to make those values compile-time constants. The original
coordinate, wrapping, bilinear, fog and blend expressions remain intact; there is no runtime
shortcut branch. Replacement sampler draws and other scale pairs retain the generic pipeline.
An internal key bit distinguishes the variants (at most two pipelines per original key).

API reference: [wgpu pipeline compilation options](https://wgpu.rs/doc/wgpu/struct.PipelineCompilationOptions.html).
No external code was copied.

Fresh idle-PC controls and the specialised renderer, 4x DX12 uncapped:

| Build / compiler | FPS | 1% low | GPU ms | Draw ms | Scanout ms |
|---|---:|---:|---:|---:|---:|
| Original / FXC | 50.52 | 36.26 | 18.812 | 17.004 | 1.356 |
| Scanout only / FXC | 51.80 | 35.51 | 18.278 | 17.093 | 0.733 |
| Scanout + unit texels / FXC | 68.88 | 28.27 | 13.836 | 12.074 | 1.148 |
| Original / DXC | 50.26 | 38.18 | 19.416 | 16.624 | 2.062 |
| Scanout + unit texels / DXC | 68.45 | 46.55 | 14.140 | 12.224 | 1.166 |

Versus scanout-only FXC, throughput rises 33.0% and total GPU time falls 24.3%.
Against the original DXC control, throughput rises 36.2% and GPU time falls 27.2%.
Per-pass clocks/durations can move even for an unchanged pass; these gains refer to total frame
GPU time and throughput. The 1% low does not improve uniformly and is reported without a claim
that all stutters are fixed.

Checks: full GPU/depth/present suite PASS; 33 synthetic presented pictures and seven stress
pictures have zero RGB/VRAM differences between dual/fallback; 12 C-oracle items at 1x/2x/4x
match exactly; all four before/after items at 8x match on Vulkan and separately DX12/DXC.
Six CPU pipeline-key tests, three shader transformation tests and host GS tests pass.
The recent original/scanout control was repeated on an idle PC; the fresh controls above
supersede the earlier 49.77/52.06 fps pair for evaluating the scanout gain.


## Retained: specialise power-of-two scaled surface reads

Extend the same pipeline constant to S=E in {1,2,4,8}, covering scaled render-target reads as
well as decoded unit textures. Unequal pairs, non-power-of-two scales and replacement sampler
draws stay generic. Only exact binary divisors are specialised; all original expressions remain.
The private key uses bits 25..28. Across resolution changes it can retain at most five variants
per original key (generic plus four scale values), rather than specialising arbitrary flags.

| 4x DX12 uncapped | FPS | 1% low | GPU ms | Draw ms | Scanout ms |
|---|---:|---:|---:|---:|---:|
| Unit only / FXC | 68.88 | 28.27 | 13.836 | 12.074 | 1.148 |
| Power-of-two surface reads / FXC | 78.48 | 54.84 | 11.732 | 10.587 | 0.691 |
| Unit only / DXC | 68.45 | 46.55 | 14.140 | 12.224 | 1.166 |
| Power-of-two surface reads / DXC | 72.61 | 51.30 | 12.778 | 11.593 | 0.730 |

This stage adds 13.9% throughput / 15.2% less GPU time on FXC, and 6.1% / 9.6% on DXC.
All three retained changes together reduce 4x GPU time by 37.6% on FXC and 34.2% on DXC
against the fresh original controls. These are single-run comparisons, not precision clock-normalised
microbenchmarks. No filter approximation or precision reduction is used.

Checks repeated for this stage: renderer suite, 33 presented synthetic pictures, seven replacement
stress pictures, all 12 C-oracle items at 1x/2x/4x, strict four-item before/after 8x comparisons on
Vulkan and DX12/DXC, six pipeline-key tests, three native shader transformation tests and host GS
all pass. This is the last renderer change in this round; the final matrix below measures this build.


## Final matrix

Same recipe, GO window, compiler and measurement definitions as the baseline. Unlocked
runs remain the performance comparison; paced runs establish the 60 fps boundary. The final
4x unlocked point is the already-recorded final-build control above, not a redundant rerun.
No measurements taken during the user's GPU hold are used; fresh original/scanout controls
were repeated after clearance. A permission marker is checked throughout every final run;
withdrawal stops the owned game and discards that run.

| GPU / API | Scale | FPS paced | 1% low | GPU ms paced | FPS unlocked | 1% low | GPU ms unlocked |
|---|---:|---:|---:|---:|---:|---:|---:|
| integrated / dx12 | 1 | 59.93 | 55.53 | 2.124 | 284.61 | 142.59 | 1.810 |
| integrated / dx12 | 2 | 59.93 | 55.55 | 4.043 | 239.69 | 131.88 | 3.645 |
| integrated / dx12 | 4 | 59.93 | 55.01 | 12.336 | 78.48 | 54.84 | 11.732 |
| integrated / dx12 | 6 | 37.73 | 15.81 | 25.311 | 38.34 | 28.10 | 25.108 |
| integrated / dx12 | 8 | 20.88 | 9.55 | 46.967 | 22.19 | 8.92 | 43.807 |
| discrete / dx12 | 1 | 59.33 | 28.61 | 1.020 | 293.71 | 159.27 | 0.334 |
| discrete / dx12 | 8 | 59.17 | 24.85 | 4.806 | 293.95 | 158.83 | 1.878 |
| integrated / vulkan | 2 | — | — | — | 182.46 | 68.05 | 3.853 |
| integrated / vulkan | 8 | — | — | — | 22.05 | 8.29 | 43.849 |

Additional boundary points:

| Scale | FPS paced | 1% low | GPU ms paced |
|---|---:|---:|---:|
| 3 | 59.62 | 36.41 | 7.734 |
| 5 | 52.57 | 14.21 | 17.715 |

Final GPU pass split, unlocked (ms/frame):

| GPU / API | Scale | Draw | Surface | Convert | Scanout | Present |
|---|---:|---:|---:|---:|---:|---:|
| integrated / dx12 | 1 | 1.509 | 0.000 | 0.075 | 0.017 | 0.208 |
| integrated / dx12 | 2 | 3.293 | 0.000 | 0.058 | 0.116 | 0.178 |
| integrated / dx12 | 4 | 10.587 | 0.000 | 0.061 | 0.691 | 0.392 |
| integrated / dx12 | 6 | 22.594 | 0.000 | 0.069 | 1.748 | 0.696 |
| integrated / dx12 | 8 | 39.373 | 0.000 | 0.064 | 3.139 | 1.232 |
| discrete / dx12 | 1 | 0.192 | 0.000 | 0.089 | 0.029 | 0.025 |
| discrete / dx12 | 8 | 1.590 | 0.000 | 0.065 | 0.165 | 0.057 |
| integrated / vulkan | 2 | 3.443 | 0.000 | 0.082 | 0.106 | 0.222 |
| integrated / vulkan | 8 | 40.127 | 0.000 | 0.063 | 2.359 | 1.300 |

For completeness, the paced pass splits follow. GPU clocks and queue behaviour differ
between pacing modes, so their durations need not equal unlocked durations.

Baseline paced:

| GPU / API | Scale | Draw | Surface | Convert | Scanout | Present |
|---|---:|---:|---:|---:|---:|---:|
| integrated / dx12 | 1 | 2.312 | 0.000 | 0.077 | 0.055 | 0.199 |
| integrated / dx12 | 2 | 5.422 | 0.000 | 0.074 | 0.277 | 0.220 |
| integrated / dx12 | 4 | 16.894 | 0.000 | 0.065 | 1.392 | 0.391 |
| integrated / dx12 | 6 | 43.998 | 0.000 | 0.072 | 4.199 | 0.990 |
| integrated / dx12 | 8 | 63.827 | 0.000 | 0.064 | 5.984 | 1.202 |
| discrete / dx12 | 1 | 0.644 | 0.000 | 0.254 | 0.064 | 0.125 |
| discrete / dx12 | 8 | 4.185 | 0.000 | 0.101 | 0.462 | 0.092 |

Final paced:

| GPU / API | Scale | Draw | Surface | Convert | Scanout | Present |
|---|---:|---:|---:|---:|---:|---:|
| integrated / dx12 | 1 | 1.833 | 0.000 | 0.080 | 0.015 | 0.196 |
| integrated / dx12 | 2 | 3.640 | 0.000 | 0.075 | 0.111 | 0.217 |
| integrated / dx12 | 4 | 11.085 | 0.000 | 0.075 | 0.789 | 0.386 |
| integrated / dx12 | 6 | 22.751 | 0.000 | 0.064 | 1.811 | 0.684 |
| integrated / dx12 | 8 | 41.178 | 0.000 | 0.079 | 4.083 | 1.628 |
| discrete / dx12 | 1 | 0.585 | 0.000 | 0.260 | 0.054 | 0.121 |
| discrete / dx12 | 8 | 4.267 | 0.000 | 0.116 | 0.332 | 0.092 |


## Result and remaining work

The highest measured scale holding the nominal 60 target rises from **3x to 4x**. Final 4x
is 59.93 fps paced (1% low 55.01); 5x is 52.57 fps and already exceeds the GPU frame budget.
Against the full original matrix, unlocked 4x rises 49.70 -> 78.48 fps with GPU time
18.996 -> 11.732 ms; 8x rises 13.79 -> 22.19 fps with GPU time 71.796 -> 43.807 ms.
The fresh original controls above independently confirm the 4x gain on FXC and DXC.

Final Vulkan is slower than DX12 at 2x: 182.46 versus 239.69 fps unlocked (23.9% lower
throughput). At 8x they are effectively tied: 22.05 versus 22.19 fps, GPU 43.849 versus
43.807 ms. Vulkan's baseline 8x advantage does not survive these DX12 improvements.
This is one AMD iGPU/driver, not evidence about the GeForce 920M or a 2019 driver.

The final AMD iGPU also passes strict before/after 8x fixed-stream comparisons on DX12/DXC
and Vulkan: all four items per backend are byte-identical. Combined with the frozen C oracle
at 1x/2x/4x, renderer tests, synthetic presented/stress checks, host GS and boot traces above,
no picture or game-state difference was found. No comparison uses two separate live runs.

Two doubtful timing points were repeated once. The first final 3x paced run had two wall-time
outliers of 690.23 and 385.82 ms despite GPU time of only 7.704 ms. Its complete window was
57.67 fps / 12.24 low. The first final RTX 1x unlocked point was 275.45 fps / 33.27 low,
GPU 0.392 ms. The repeated points are used in the final table; both first runs are preserved
as diagnostics. The repeat results are recorded below. No individual frame is removed from
any reported window, and no cause is assigned to the stalls without further profiling.

- Repeated integrated-dx12-3-paced: 59.62 fps, 36.41 low, 7.734 ms GPU.

- Repeated discrete-dx12-1-uncapped: 293.71 fps, 159.27 low, 0.334 ms GPU.


Stopped after three measured renderer optimisations. At final 4x and 8x, draw passes still
account for about 90% of GPU time. Manual bilinear sampling, GS wrapping/blending, depth
tests and overdraw remain. The exact expressions and precision are preserved. Non-binary
surface scales retain the generic texture-coordinate path. Further specialisation would need
broader coordinate/rounding coverage and the same recorded-stream gates. A GPU capture with
shader/bandwidth counters would distinguish instruction, sampling and overdraw costs inside draw.

Feedback copies are already bounded to reachable rectangles (finding 112); ordinary colour
targets are RGBA8, with Depth32Float for the current depth/Z-probe contract. Conversion costs
only about 0.06 ms in the final high-scale window. Replacing persistent-surface loads/stores
or narrowing depth needs a proven coverage/dependency/precision analysis; no safe measured
win was established here. Removing the second alpha draw on devices without dual-source
blending needs an exact alternative. The existing dual/fallback picture tests still pass.
Shader/pipeline compilation and wall-time stalls need separate CPU/pacing profiling; 1% lows
do not improve uniformly. The extra exact pipeline variants can increase creation/cache work.

The optional launcher suggestion is not implemented. Player adapter choice and first-start
scale defaults stay unchanged; a single small Ryzen iGPU is not a general calibration for all
integrated GPUs. The developer UMA-budget override is not enabled for players. True 6x/8x
measurements above use that override; otherwise this machine's guard clamps high requests
to 4x. No settings or installed game files were touched.

Windows DX12/Vulkan were built and checked; other platforms were not run. Browser shader
transformation unit tests pass, but this is not a browser/Metal performance claim. Monitor
transport was measured through the existing RTX-connected hidden-desktop path, not isolated;
a motherboard-connected visible-display comparison remains for a later hardware setup.
