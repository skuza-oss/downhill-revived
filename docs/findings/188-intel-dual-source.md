# 188: dual-source blending off on Intel GPUs

Status: current
Checked: 2026-11-09

Branch `fix/intel-dual-source` from `main` ac62c78d (0.7.14), started 2026-11-09. Merged into `main` at a97b42d7 on 2026-11-09; not yet released.

## Goal and acceptance

Issue #58 (Windows 11, Intel i5-1035G1: Ice Lake, Intel UHD Graphics G1, Gen11; driver
31.0.101.2145): a few seconds after the start the picture stops while sound and input go on. On
D3D12 the driver removes the device (`Present failed: The GPU device instance has been suspended`,
0x887A0005; finding 171 read the player's DxDiag: one `VIDEO_ENGINE_TIMEOUT_DETECTED` TDR per
start). The player's matched A/B on Vulkan (native renderer, borderless, 2x, V-Sync on, only
`DHD_WGPU_DUAL_SOURCE` changed): dual-source on froze after about 8 s
(`get_current_texture: validation error`, `presenting failed; no more pictures are shown`);
`DHD_WGPU_DUAL_SOURCE=0` ran menus and a race without a stop, on Vulkan and on D3D12 (their
DX12 run: race 57.4 fps of 60, `gpu_ms/frame=8.91`, limit gpu, at 2x). The driver update from
31.0.101.2121 to 2145 did not help.

Goal: the game turns dual-source blending off by itself on the affected Intel GPUs, decided once
from the adapter's information in one place for every platform, with one dhd.log line saying so.
`DHD_WGPU_DUAL_SOURCE=0` stays the manual override, `=1` forces it on.

Acceptance: a unit test of the rule; `python tools/check.py port` passes; a boot state trace equal
to the base build's (the native renderer only shows pictures; nothing the game reads changes).
Out of scope: making dual-source work on Intel, a device recreated after a loss (finding 171's
decision stands).

## Research (2026-11-09)

- **Mesa, Intel's open driver, has the hardware workarounds.** `src/intel/compiler/brw/
  brw_compile_fs.cpp` (Mesa main, fetched 2026-11-09), the fragment shader's SIMD width limit:
  "Wa_14017468336: If dual source blend is enabled do not enable SIMD32 dispatch ... The
  dual-source RT write messages fail to release the thread dependency on ICL and TGL with SIMD32
  dispatch, leading to hangs. The dual-source RT write messages may lead to hangs with SIMD16
  dispatch on ICL due some unknown reasons" (Mesa issue 2183), applied for `devinfo->ver` 11 and
  12: Ice Lake (Gen11) and every Gen12 part (Tiger Lake, Rocket Lake, Alder/Raptor Lake, DG1,
  Arc Alchemist, Meteor Lake). Xe2 (ver 20) has no such limit (Mesa 24.1: "intel/brw/xe2+: Allow
  dual-source blending in SIMD16 mode"). Older: Mesa 20.0 "intel/compiler: force simd8 when dual
  src blending on gen8", and Mesa bugs 107088 (i965) / 107306 (anv), "[GEN8+] Hang when
  discarding a fragment if dual source blending is enabled but shader doesn't support it", fixed
  in 2018 (anv commit 00fc56a68d21) by turning blending off for such shaders. So the hazard is in
  the hardware, Gen8 on, and worst on Gen11/12; a driver that misses one of these rules hangs the
  GPU, which is what #58's TDRs look like. Our dual-source shader (`native_dual.wgsl`) discards
  (the alpha test in `shade`) with a depth test, and writes both outputs.
- **Dolphin** (`Source/Core/VideoCommon/DriverDetails.cpp`, `BUG_BROKEN_DUAL_SOURCE_BLENDING`):
  Intel on macOS for OpenGL, Vulkan (MoltenVK) and Metal, and AMD's Windows OpenGL driver. Its
  note: dual-source outputs can crash drivers, sometimes in kernel mode. Dolphin's answer is
  separate shaders with and without dual-source outputs, not a vendor switch for the blend.
- **PCSX2** has no Intel dual-source rule in `GSDeviceVK.cpp` or `GSDevice12.cpp` (master,
  2026-11-09): it takes `dualSrcBlend` as offered. It does keep Intel GPUs off Vulkan for its
  automatic renderer choice ("Not using Vulkan for Intel GPU": OpenGL is faster on Linux,
  Ivy Bridge and Haswell drivers incomplete). Finding 171 already listed PCSX2 #345 and PPSSPP as
  older Intel dual-source bugs.
- **wgpu**: no open issue on Intel dual-source hangs (searched `gfx-rs/wgpu` "dual source");
  wgpu #849 is a D3D12 `DEVICE_HUNG` on an Intel HD 4000 the runtime saw nothing wrong with.
  Another wgpu-based project (PokeZ-ARecomp #208) reports Vulkan device loss on the same
  i5-1035G1 after 25 s to 4 min; it does not say whether it uses dual-source blending.

## Progress

- [x] 2026-11-09: finding reserved (4fe86df8)
- [x] 2026-11-09: the rule, the log line and the override (c1f66213): `port/render/src/device.rs`
  `dual_source`, `DHDR_DEV_DUAL_SOURCE` (64) for `DHD_WGPU_DUAL_SOURCE=1`
- [x] 2026-11-09: unit tests `device::tests` (2 pass); `abi_layout`, `test_dhdr_abi.py` OK;
  `device_smoke exact_hardware*` pass (RTX 3070, dual-source on as before)
- [x] 2026-11-09: `python tools/check.py port -j 6` OK (build_wasm, test_runtime, build_game,
  net_tests, test_gs_wgpu VERDICT PASS, test_wasm2c_chunks); boot state trace
  (`state_trace_run.py boot --env DHD_LAUNCHER=1`) of this branch's dhd64.exe against the 0.7.14
  release dhd.exe (= `main` ac62c78d): all 600 rows match
- [ ] the lead: merge; ask #58's player to confirm a build with it starts and races on D3D12
  without `DHD_WGPU_DUAL_SOURCE` (their log should show the `gs: dual-source blending off` line)

## How it works

- `device::dual_source(offered, vendor, flags)` (`port/render/src/device.rs`) is the one place:
  not offered: off; `DHDR_DEV_NO_DUAL_SOURCE` (`DHD_WGPU_DUAL_SOURCE=0`): off on every GPU; PCI
  vendor 0x8086 (Intel): off, unless `DHDR_DEV_DUAL_SOURCE` (`DHD_WGPU_DUAL_SOURCE=1`); else on.
  The native device creation (Windows, macOS, Linux, Android) asks for DUAL_SOURCE_BLENDING only
  when it says so. The browser's device asks for no features at all (`web/init.rs`), so it never
  had dual-source blending.
- dhd.log, when the Intel rule decided: `gs: dual-source blending off on Intel(R) UHD Graphics
  (Intel GPUs hang on it, finding 188); DHD_WGPU_DUAL_SOURCE=1 turns it on` (or `... on for X by
  DHD_WGPU_DUAL_SOURCE=1 ...`), next to the existing `gs native: GS blending without dual-source
  blending (a draw blending on As writes its alpha in a second draw)`.
- C (`port/gs/gpu_rust.c` device_init) only turns the variable into a flag: `0` NO_DUAL_SOURCE,
  any other number DUAL_SOURCE (an empty value counts as 0, as before). The native front end
  (`gpu_rust_shared.inc`) no longer reads the variable itself: it uses dual-source exactly when
  the device has it (`DHDR_FEAT_DUAL_SOURCE`).
- Without dual-source a draw that blends on the source alpha (As) is drawn twice: RGB with As /
  128, then the alpha channel (finding 084 W7; `native/render.rs`). The pictures match:
  `test_native_transfers.py` runs both paths and compares them pixel by pixel; finding 084 names
  the one case that could differ (blending on Ad with overlapping triangles of one draw), which it
  found can't happen.

## Decisions

- **Every Intel GPU, not a list of generations.** The evidence of hangs covers Gen8 (discard),
  Gen11 and Gen12 (SIMD16/32), that is nearly every Intel GPU a player has; what Intel's Windows
  driver does for each generation is not public, and a device-id table would need upkeep for
  every new part. Xe2 and later (Lunar Lake, Battlemage) have no known hazard, but they are few
  and the fallback costs one more draw for the As-blending draws only. Revisit if an Xe2 player
  measures a real loss.
- **One rule on every backend.** #58 hung on D3D12 and on Vulkan alike (one shader compiler and
  one hardware under both); Dolphin lists Intel on macOS too. Intel Macs are not a build target
  (the Mac build is arm64), so on Apple the rule never fires.
- **`=1` forces it on**: one more ABI flag (64), cheap, and it lets a player or a developer with
  an Intel GPU check whether a later driver is fixed. `0` still wins over `1` (both flags set:
  off).
- **The decision lives in Rust, beside the request**, so the device's feature is the only truth;
  the C front end's own reading of the variable was dropped (it could only agree or say less).
- **No new recovery path.** A lost device on any backend still goes through finding 171
  (`lost.rs`: wgpu's device-lost callback, the reason logged once, the box and exit code 1). The
  Vulkan freeze in #58 (`get_current_texture: validation error`) is wgpu's invalid-device error
  after a loss, which is the same callback's case; 0.7.12, where the player first saw it, did not
  have 171 yet.

## Surprises

- The native replay tool (`native_replay`) asks for a device the same way: a DUAL trace recorded
  elsewhere is refused on an Intel GPU now ("no dual-source blending") unless the rule changes;
  traces recorded on Intel are not DUAL. Left as is: a replay that hung the GPU would not help.
- `test_native_transfers.py`'s `wgpu` run means "dual-source where the GPU has it": on an Intel
  machine both of its runs now take the two-draw path, so the comparison there compares the path
  with itself. Left as is (no Intel test machine; forcing `=1` there could hang the GPU); on
  other GPUs nothing changed.

## Not measured

- This PC has no Intel GPU (NVIDIA RTX 3070 and an AMD Radeon iGPU), so the two-pass path's cost
  on Intel was not measured. Elsewhere it was small: finding 146, the Mi 11i at 4x,
  `DHD_WGPU_DUAL_SOURCE=0` 47.54 vs 47.94 fps (GPU 14.72 vs 14.78 ms), no change. The fix itself
  was not run on an Intel GPU; #58's player ran the same configuration by hand
  (`DHD_WGPU_DUAL_SOURCE=0`) on D3D12 and Vulkan without the freeze.

## Result

Filled in at the merge. So far: the rule in `device::dual_source` (Intel off, `0|1` overrides),
its dhd.log line, `DHDR_DEV_DUAL_SOURCE`; checks in Progress. Not run on an Intel GPU (none here);
the two-pass path's cost on Intel not measured (Not measured).
