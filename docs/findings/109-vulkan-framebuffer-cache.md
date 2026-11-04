# 109: Vulkan framebuffer reuse across encoder resets

Status: current
Checked: 2026-11-04

2026-10-28, branch `perf/fbcache`, based on `prof/android-pcsx2` and `perf/phone120-measure`
(`f50127553`). Number checked against main and every local perf/* branch before reservation; 102 was
the highest existing finding.

## Cause and implementation

Finding 108 traced about 4.5 ms/field of Adreno CPU work to framebuffer creation and destruction.
wgpu-hal 29.0.3 cached VkFramebuffer only until reset_all, so each of about 18.5 render passes per
field paid again on the next field.

The renderer vendors the unmodified crates.io wgpu-hal 29.0.3 source in its own commit, then patches
only its Vulkan implementation. Cargo patches that crate to the local path; all dependency versions
remain pinned, and builds keep `--locked --offline`. The only lock change removes HAL's registry
source and checksum. rustgs hashes vendor inputs so its object cache cannot return an unpatched
renderer for a patched build.

The cache belongs to DeviceShared, under one mutex, with a maximum of 256 lookup entries and
arbitrary eviction on a miss at capacity. Every encoder pins each distinct framebuffer it records,
using Arc, until its command pool is reset or destroyed. Eviction releases cache ownership;
outstanding commands keep their framebuffers alive. No shaders, attachments, passes, draw order,
transitions, load/store operations or synchronization are changed.

## Lifetime and threading rules

- Lookup, creation, eviction and view invalidation use the same mutex. Command
  pools remain encoder-owned and are never accessed by another thread.
  HAL still requires the borrowed attachment view to stay alive throughout pass
  recording; wgpu-core's resource ownership/snatch guards enforce this. The
  cache lock does not authorize concurrent destruction of a borrowed view.
- Keys retain the upstream render pass, extent, attachment handles and unique
  view identities. destroy_texture_view removes all lookup entries containing
  the view's identity before destroying its VkImageView. Encoder-owned temporary
  views invalidate their entries on destruction too. Handle reuse cannot match
  a prior view's identity.
- Encoder pins are lifetime references, not lookup entries. HAL already requires
  submitted commands to finish before reset_all, and command buffers cannot
  outlive their encoder. Pins are released after pool reset/destruction. Cache
  eviction therefore also works before a recorded buffer has been submitted.
- As before, HAL view destruction requires all submitted uses complete; recorded
  commands referring to a destroyed view cannot subsequently be submitted.
  Pins may retain an invalidated framebuffer object until reset, but cannot
  make it discoverable again or keep its texture/view alive.
- Cache lookup entries are bounded at 256; necessary outstanding command
  lifetime references are additional, bounded by distinct framebuffers in those
  commands. The cache owns no textures, views or DeviceShared Arc.
- Framebuffer Drop holds an ash dispatch-table clone. Encoder Drop clears its
  pins while its DeviceShared Arc is alive. DeviceShared Drop clears the cache
  before destroying render passes and the VkDevice. There is no ownership cycle.

## Upstream research and licence

Checked v30.0.1 and current upstream trunk's Vulkan command.rs: both drain framebuffers in reset_all
and uses vkCreateFramebuffer/vkCmdBeginRenderPass, with neither imageless framebuffer creation nor
dynamic rendering. Its module-level text describing a device cache and imageless framebuffers is
stale. GitHub issue/PR searches found open [#8032](https://github.com/gfx-rs/wgpu/issues/8032),
discussing retention, and [#9754](https://github.com/gfx-rs/wgpu/issues/9754), discussing
temporary-view cleanup; neither supplies this fix. Historical lifetime work is
[#7847](https://github.com/gfx-rs/wgpu/pull/7847).

The imageless search also found the older [#1701](https://github.com/gfx-rs/wgpu/pull/1701)
(view-usage handling) and [#7596](https://github.com/gfx-rs/wgpu/pull/7596) (rendering to 3D
slices). These are historical implementations/discussions, not an imageless path present in 29.0.3
or 30.0.1. A bounded cache keeps the existing attachment and render-pass path; adding extension
negotiation and another rendering path would be a larger change.

Source: [upstream
command.rs](https://github.com/gfx-rs/wgpu/blob/trunk/wgpu-hal/src/vulkan/command.rs). The vendored
crate's provenance is commit `4cbe6232b2d7c289b6e1a38416a6ae1461a22e81` (crates.io
.cargo_vcs_info.json). Original MIT OR Apache-2.0 licence files are preserved. The local cache is an
original patch, not a backport. See `port/render/vendor/wgpu-hal/PATCH.md` for the concise
maintenance contract.

## Checks and measurements

Verified builds and renderer checks:

- `python port/tools/build_host64_android.py build/wasm/dhd_core.wasm --jobs 4`
  and `python port/android/build_apk.py --jobs 4 --offline`: pass, pinned 29.0.3
  family, NDK 29.0.14206865. APK version 0.6.4 / code 604.
- `python port/tools/rustgs.py test --target x86_64-pc-windows-msvc --features test-hooks`:
  **33 passed**, including ABI, device_smoke, native_arenas, native_keys,
  native_textures and the new framebuffer_cache regression. Focused native_keys,
  device_smoke native and framebuffer_cache reruns also pass.
- The Vulkan regression creates 300 distinct attachment views and records them
  before submitting, exceeding the 256-entry bound while commands still need
  their objects. It checks every RGBA byte after submission, reuses views across
  pool recycling, repeats view destruction/recreation three times, and drops
  its owned device. No validation-scope errors or wrong bytes.
- Windows `build_host64.py ... --jobs 4 --num-outputs 8`: pass, including
  rustgs/check_rust_lib gates. Hidden-desktop races (finding 089's
  CreateDesktopW method) complete fields 6000-6200 on DX12 and Vulkan on an
  RTX 3070. Selector: `DHD_WGPU_BACKEND=vulkan`; default DX12. Vulkan emits
  unrelated loader messages about installed overlay manifests; no VUIDs.
- `rustgs.py tree`: the native graphs remain exactly 29.0.3; the browser graph
  contains no HAL/core/naga. Comparing the vendor with the registry crate shows
  exactly four modified Vulkan source files, with all other backends identical.
- The validation APK was built with `--extra-lib` and VVL 1.4.363. Its libmain.so
  is byte-identical to the measured patched APK. Before/after portable wasm
  inputs are byte-identical. SDL libraries differ only in their 20-byte GNU
  build IDs; all executable bytes match.

### Phone protocol and completed measurements

Mi 11i / Snapdragon 888 / Adreno 660, serial `1ae461e`, `DHD_FPS=120`, seed 81257, bench_race's
deterministic race route. A is finding 108's unchanged APK (`f50127553`); B is the patched Vulkan
HAL. Paced uses the ordinary field clock; uncapped adds `DHD_FRAME_CLOCK=1 DHD_VSYNC=0`.

Paced pairs ran A/B, B/A, A/B; uncapped each retained one A/B pair.

Like finding 108, all rates and RSTAT parts use each run's GO+360..GO+3600 interval, with complete
240-picture RSTAT blocks. The enclosing bench window is 3700-11000, to retain the whole GO interval.
Clock/temperature samples are selected over the same interval; the first pilot predates adding the
sample's field number, so its clock/temperature uses the enclosing bench window instead (its clock
is constant there).

The lead reduced scope during the active second patched 1x uncapped run: retain completed pairs, one
pair per remaining configuration, and one before/after pixel comparison. The active extra run
completed; `b2s1u` below is supplemental and excluded from paired medians. The two waiting-runner
restarts changed lock retry policy before acquiring the phone; no game measurement failed.
Three-pair measurement of all configurations is deferred to the merged build.

Run labels: a/b = before/after; digit = pair; s1/s2 = scale; p/u = paced/uncapped. The table lists
all completed timing runs (execution order within pair 2 was B/A). All timings are milliseconds per
field.

| run | finish ms | queue.submit ms | submit total ms | fields/s | cpu4 / cpu7 GHz | zone43 C | battery C |
|---|---:|---:|---:|---:|---|---:|---:|
| a1s1p | 4.118 | 2.446 | 6.584 | 90.3 | 1.67 / 1.67 | 62.8 | 37.0 |
| b1s1p | 2.011 | 1.965 | 4.001 | 98.0 | 1.32 / 1.31 | 61.2 | 37.0 |
| a2s1p | 5.253 | 2.855 | 8.135 | 71.9 | 1.32 / 1.31 | 60.2 | 38.0 |
| b2s1p | 2.143 | 2.175 | 4.343 | 88.4 | 1.32 / 1.31 | 60.2 | 38.0 |
| a3s1p | 4.762 | 2.408 | 7.192 | 80.5 | 1.32 / 1.31 | 60.2 | 38.0 |
| b3s1p | 2.328 | 2.615 | 4.972 | 77.6 | 1.32 / 1.31 | 60.6 | 38.0 |
| a1s2p | 4.156 | 4.368 | 8.542 | 78.2 | 2.11 / 2.15 | 61.0 | 29.0 |
| b1s2p | 1.925 | 4.685 | 6.633 | 80.5 | 1.67 / 1.67 | 63.5 | 33.0 |
| a2s2p | 5.368 | 5.027 | 10.419 | 62.3 | 1.32 / 1.31 | 58.5 | 37.0 |
| b2s2p | 2.350 | 4.334 | 6.712 | 75.3 | 1.32 / 1.31 | 60.5 | 36.0 |
| a3s2p | 6.103 | 5.034 | 11.162 | 57.2 | 1.21 / 1.19 | 58.8 | 40.0 |
| b3s2p | 1.917 | 2.794 | 4.736 | 95.5 | 1.21 / 1.19 | 62.8 | 40.0 |
| a1s1u | 5.843 | 4.581 | 10.454 | 63.3 | 1.21 / 1.19 | 58.5 | 40.0 |
| b1s1u | 2.777 | 2.726 | 5.538 | 72.9 | 1.21 / 1.19 | 59.6 | 39.0 |
| b2s1u | 2.758 | 2.756 | 5.550 | 74.4 | 1.21 / 1.19 | 60.5 | 40.0 |
| a1s2u | 5.839 | 5.877 | 11.749 | 56.2 | 1.32 / 1.31 | 58.8 | 37.0 |
| b1s2u | 2.447 | 4.285 | 6.762 | 76.5 | 1.32 / 1.31 | 63.5 | 36.0 |

Paired medians (three pairs for paced; one pair for uncapped):

| configuration | finish A/B | queue.submit A/B | submit total A/B | fields/s A/B | cpu4/cpu7 GHz A; B | zone43 C A/B | battery C A/B |
|---|---|---|---|---|---|---|---|
| 1x paced | 4.762/2.143 | 2.446/2.175 | 7.192/4.343 | 80.5/88.4 | 1.32/1.31; 1.32/1.31 | 60.2/60.6 | 38.0/38.0 |
| 2x paced | 5.368/1.925 | 5.027/4.334 | 10.419/6.633 | 62.3/80.5 | 1.32/1.31; 1.32/1.31 | 58.8/62.8 | 37.0/36.0 |
| 1x uncapped | 5.843/2.777 | 4.581/2.726 | 10.454/5.538 | 63.3/72.9 | 1.21/1.19; 1.21/1.19 | 58.5/59.6 | 40.0/39.0 |
| 2x uncapped | 5.839/2.447 | 5.877/4.285 | 11.749/6.762 | 56.2/76.5 | 1.32/1.31; 1.32/1.31 | 58.8/63.5 | 37.0/36.0 |

CPU clocks and temperatures are per-run interval medians; the summary takes medians of those values.
Clocks vary substantially and throughput is noisy. The cache lowers finish in every paired run; 1x
paced goes from 4.118/5.253/4.762 to 2.011/2.143/2.328 ms. That does not establish a stable 120
fields/s or isolate every throughput change from thermal policy.

### Remaining verification and conclusion

The phone Vulkan validation-layer run and simpleperf profile were **skipped by explicit lead/user
decision** when measurement scope was reduced. The VVL APK was built, but no successful phone VVL
session is claimed. Likewise, this branch has no patched simpleperf evidence that
vkCreateFramebuffer/vkDestroyFramebuffer have disappeared from the hot path. Finding 108 supplies
the baseline profile; finish timing and the implementation support the expected reduction, but do
not replace the skipped profiler check. Existing desktop renderer regression and platform checks
above remain valid.

**Cross-run pixel method dropped as invalid by lead/user decision.** Riders can start at different
positions in the gate, and run timing changes simulation progress and what is on screen. GO-relative
or race-clock alignment does not establish equivalent state. The earlier 460/460 differing frames
therefore do not isolate a framebuffer-cache defect. The before-versus-before control and all
further capture/readback comparisons were cancelled at the scope change; no repeatability verdict is
claimed. Correctness for the merged build rests on the 5,000-field Android versus x64 state trace,
all 13 columns with the existing mask unchanged (finding 111). One merged-build screenshot is for
visual inspection only. Renderer code was not changed during this test task.

Both capture sessions completed their bench window and the runner's original `dhd.ini`
restore/byte-comparison before releasing the lock. All completed timing sessions used the same
cleanup. No `/sdcard/PS2` data was touched. The preflight zone43 check waited when above 65 C;
clocks/temperatures were sampled per run. Local evidence is `pixel-results.json`,
`pixel-clock-results.json` and the two frame archives under the scratch directory below.

The bounded device cache substantially reduces encoder.finish CPU cost across all measured
configurations without changing the rendering path. Throughput improves in the configuration
medians, with thermal and single-pair limitations above. The complete three-pair matrix and skipped
phone instrumentation belong to the merged-build follow-up. The cross-run pixel method was
subsequently dropped as invalid by the lead; merged-build state-trace verification and measurements
are recorded in finding 111.

Input game data is hardlinked read-only; the existing measuring lane's unchanged portable wasm input
is copied. 