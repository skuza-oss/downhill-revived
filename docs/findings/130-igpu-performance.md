# 130: Integrated GPU performance

Branch `perf/igpu`, based on local `feat/perf-log` (`0003e65`). Work in progress.

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
Uncapped uses `DHD_FRAME_CLOCK=1`. Sound is off; the harness has its own memory card.
The 4096 MiB developer scale guard is needed for actual 6x/8x. All runs reached GO at field 3214.
The window contains 1620 fields, GO+180 < field <= GO+1800. FPS is 1000 / mean wall ms;
1% low is 1000 / the mean of the 17 slowest fields. GPU figures average complete 60-frame
timestamp blocks inside that window. Units are ms/frame. Every recorded point is one valid run.

| GPU / API | Scale | FPS paced | 1% low | GPU ms paced | FPS uncapped | 1% low | GPU ms uncapped |
|---|---:|---:|---:|---:|---:|---:|---:|
| integrated / dx12 | 1 | 59.93 | 51.47 | 2.644 | 262.98 | 114.02 | 2.322 |
| integrated / dx12 | 2 | 59.61 | 36.76 | 5.992 | 173.46 | 110.71 | 5.368 |
| integrated / dx12 | 4 | 50.85 | 15.77 | 18.741 | 49.70 | 35.77 | 18.996 |
| integrated / dx12 | 6 | 19.97 | 9.04 | 49.259 | 23.97 | 12.03 | 40.458 |
| integrated / dx12 | 8 | 13.93 | 8.66 | 71.077 | 13.79 | 9.14 | 71.796 |
| discrete / dx12 | 1 | 59.93 | 55.58 | 1.087 | 290.55 | 155.94 | 0.372 |
| discrete / dx12 | 8 | 59.73 | 39.27 | 4.838 | 291.29 | 159.35 | 2.428 |
| integrated / vulkan | 2 | ? | ? | ? | 144.94 | 111.83 | 5.316 |
| integrated / vulkan | 8 | ? | ? | ? | 14.84 | 10.68 | 66.185 |

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

Vulkan on this iGPU is 16.5% slower in throughput at 2x (GPU time almost unchanged),
but 7.6% faster at 8x. The selected AMD adapter and Vulkan backend are confirmed in both logs.
No conclusion about other GPUs or old drivers follows from these two points.

The monitor stays connected to the RTX. These rates include the hidden-desktop presentation path;
pass timestamps exclude work outside the native passes. This setup cannot isolate the cost of
cross-adapter transport or fully reproduce a visible desktop compositor. Repeat with the monitor
on the motherboard to measure that difference.
