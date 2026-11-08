# 088 — Cleanup: one 64-bit build on the Rust renderer

Status: current
Supersedes: parts of 015, 018, 020, 021, 023, 024, 038, 043, 053, 081
Checked: 2026-11-04

The 32-bit (i686) pipeline, the Direct3D 11 renderer and the C wgpu-native renderer are retired (the
last 32-bit release is tagged `v0.3.1`). Each cleanup task has a section here.

## CL3: the 64-bit build is the release build

### What changed

- `port/tools/package_release.py` makes the players' zip from the 64-bit build:
  `python port/tools/build_wasm.py`, then `python port/tools/package_release.py` (or `--wasm PATH`,
  or `--exe` a ready `build_host64.py --gs-rust` dhd64.exe with its `link.rsp`). It runs
  `build_host64.py --gs-rust` (x64 SDL3 from `sdl3.py`, output in `build/release-work/`), names the
  exe `dhd.exe` in the package and writes `build/release/DownhillRevived-<version>-win.zip`, the same
  files unzipped in `build/release/DownhillRevived-<version>/`, and `latest.json`.
- Checks kept: version.h = the exe's `dhd-port X.Y.Z` string; no absolute path (SDL's USB vibration
  path list excepted, as before for dhd64.exe), no server but `dhd.skuza.app`, no game data (retail
  ELF runs, file names), text files without other servers or local paths; dxcompiler.dll (now x64)
  by its pinned SHA-256; imports only Windows DLLs; the Rust library linked in (`link.rsp`: exactly
  one `dhd_render.lib`) passes `rust_lib_problems`. Replaced: build_clean.py's manifest check (an
  i686 exe built from a committed revision) by a refusal when `port/` or `src/` has uncommitted
  changes to tracked files. Removed: the wgpu-native path exemption (`gs_libraries`,
  `without_library_paths`) and the i686/`--impl c` paths.
- Notices: `THIRD-PARTY-NOTICES.txt` in the zip is `port/release/THIRD-PARTY-NOTICES.txt` plus a
  generated "Rust crates linked into dhd.exe" section (`rust_notices`). No cargo-about here, so it
  comes from `cargo metadata --offline --locked --features native --filter-platform
  x86_64-pc-windows-msvc` in `port/render`, walked from `dhd_render` along normal dependencies,
  skipping proc-macro crates (they run in the compiler only): 61 crates. Each is used under the
  first of MIT, Zlib, ISC, BSD, Apache-2.0 it offers; the text is the crate's own licence file from
  the cargo registry (identical texts printed once with the crates they cover); five crates ship no
  file (gpu-descriptor, gpu-descriptor-types, profiling: MIT text with their authors; spirv: Apache,
  the shared text; hexf-parse: CC0, none needed). The Apache License 2.0 text (from a crate's
  LICENSE-APACHE) ends the file. A crate with another licence stops the package. The static file
  drops wgpu-native and webgpu.h and adds wgpu, the Rust standard library, WABT (wasm2c and its
  runtime, Apache-2.0), wasi-libc (licence notice from github.com/WebAssembly/wasi-libc: LICENSE and
  cloudlibc/LICENSE) and SIMDe (its COPYING); SDL now names video and audio too; Windows libraries
  say Direct3D 12.
- `port/hle/update.c`: the `_WIN64` gates ("update: off in the 64-bit build") are gone; the 64-bit
  dhd.exe checks for updates at start (boot.c `host_service_update_at_start`, same path as before)
  and after the online menu's Update Now.
- Online: no 64-bit gate or "untested" flag in `port/net` or `port/hle/online*.c`; the `__wasm__`
  branches in `online_menu.c` are the host-thread versions of the same jobs (finding 082). Nothing to
  remove. Not run.
- `port/release/README.txt`: Direct3D 12 or Vulkan (WARP otherwise); an update replaces the
  release's other files too (dxcompiler.dll). `docs/BUILDING.md` and finding 054 §6 name the new
  commands.

### Migration from 0.3.1 (32-bit)

What 0.3.1's updater (`update.c` at `v0.3.1`) does, and what the 64-bit package gives it:
1. It asks `api.github.com/repos/skuza-oss/downhill-revived/releases/latest` and reads the
   `latest.json` asset (or, without one, a `DownhillRevived-*-win.zip` asset with a sha256 digest).
   The package still writes `latest.json` and `DownhillRevived-<version>-win.zip`: publish both as
   before.
2. It downloads the zip, checks the sha256, unpacks it with tar.exe and looks for `dhd.exe` in the
   unpacked folder or one folder below. The zip is still `DownhillRevived-<version>/dhd.exe`: the
   64-bit exe keeps the name.
3. It renames its running dhd.exe to dhd.exe.old, moves the new dhd.exe in, copies every other plain
   file of the folder (README.txt, LICENSE.txt, THIRD-PARTY-NOTICES.txt, dxcompiler.dll) over the old
   ones, never dhd.ini, dhd.log, iso-check.txt or folders (saves\), and starts the new exe. In 0.3.1
   this is the first process (before the relaunch), which never loads dxcompiler.dll, so the x86 DLL
   is replaced by the x64 one. The new exe deletes dhd.exe.old at its first start.
4. dhd.ini and saves\card1 are read by the same host code (`host.c`), so settings and saves carry
   over. Left behind: 0.3.1's D3D11 shader cache in `%LOCALAPPDATA%\DownhillRevived\shaders` (a few
   files, unused now); nothing deletes it.
5. Afterwards the 64-bit dhd.exe updates itself the same way. One difference: it is a single process
   (no relaunch, `host64/bridge.c`), so the online menu's Update Now installs from the game's own
   process, which has dxcompiler.dll loaded; a loaded DLL can't be overwritten. `copy_side_files`
   now renames such a file to `NAME.old` and copies the new one; `clean_up` deletes `*.dll.old` at
   the next start. At start (update_at_start) nothing is loaded yet.

Release steps: bump version.h, `build_wasm.py`, `package_release.py`, publish zip + latest.json as
`v<version>` from the public repo, then raise `MIN_VERSION` (now 0.3.1) when 0.3.1 should stop
racing (32-bit and 64-bit exes have different hashes, so they never share a room anyway).

### Check

`package_release.py --jobs 2` on this branch, with the gate-X64 dhd_core.wasm (built from 12025b835,
five commits older; two module files differ, `port/runtime/vu0_interp.c` and
`vu1_native_geometry.h`): exit 0 in 7.5 min (build_host64 58 objects). Zip 22,444 KB with
`DownhillRevived-0.3.1/` dhd.exe (x64, 45,857,792 bytes), README.txt, LICENSE.txt,
THIRD-PARTY-NOTICES.txt (1,607 lines), dxcompiler.dll (x64, pinned); latest.json beside it. All
imports are Windows DLLs (D3DCOMPILER_47/d3d11 still imported until CL2's removal lands). The exe
holds the official update URL and no "update: off in the 64-bit build". No game run, no network.

## CL1: the 64-bit and browser builds stand alone; the 32-bit pipeline is gone

### What the 64-bit build took from link.py, and where it comes from now

`build_wasm.py` imported `link.py` and read its outputs (`build/port/link`, `build/port/recomp`,
`build/port/obj`, or another checkout's copy of them). Each input now comes from the checkout,
`asm/` and `assets/vutext.bin`:

- C++ retail names (`VARIANT`, `retail_name`): moved to `wasm_retail_names.py`.
- The recompiler's fixed point (the asm-function list, the library regions, the libc names, the
  `llvm-nm` helper): moved into `build_wasm.py`.
- The names the 32-bit link stubbed (`missing.c`): a constant, `MISSING` (the 16 libmpeg `mpc.o`
  routines `_ri0_*`/`_rix_*`, five functions, `kCopy`, `kFindAddress`: the 23 trap stubs).
- The absolute data names from `dhd.map`: every `D_<hex>` name a declaration uses is its PS2 address
  (data.py's rule). The 41 names below the image the 32-bit map listed are exactly these.
- The constant-`eax` table of the 32-bit objects: its 83 entries were all 0, which is what the ABI
  thunks and `wasm_postlink.py`'s adapters return without it. Removed.
- The "recompiled only in wasm" check compared with the 32-bit build's recompiled set. It is now
  "recompiled although a game unit defines it in C" (`build/wasm/recompiled-with-c.txt`): a
  recompiled function that a failed unit's source or a C++ definition (by retail name) defines
  stops the build.
- `vu1_aot.inc`/`vu1_native_clip.inc`: generated by `vu1_aot.generate` in `build_wasm.py`
  (`build/wasm/vu1aot/`, for `vu0_native_words`), `build_host64.py` (its output folder; the
  `--generated-dir` option is gone), `build_host64_linux.py` (`<out>/gen`) and `host64_census.py`.
  The native host builds always compile `vu0_interp.c` with `PS2_VU_AOT`.
- `build_host64.py --sdl-include` defaults to `sdl3.py`'s x64 build
  (`build/port/sdl3/3.4.18-x86_64`). `sdl3.build()` builds x86_64 by default; the 32-bit test
  programs of `test_runtime.py` and `test_text_input.py` ask for i686 explicitly.

### Removed

- Files: `port/tools/link.py`, `build_clean.py`, `build_compare_port.py`, `test_x87.py`,
  `wasm_host_census.py` (it parsed `link.py`'s source), `port/runtime/fp_trap.c`, `fp_trap.h`,
  `test_float.c`, `port/launcher/Launcher.ps1` (only `build_clean.py` packaged it).
- The x87/FP-trap runtime's uses: the `-include fp_trap.h` of the wasm build, `boot.c`'s
  `ps2_fp_trap_init`, `kernel.c`'s `ps2_fp_trap_enable`, `port.h`'s `ps2_x87_checkpoint`,
  `recomp.h`'s include, `recomp.py`'s x87 checkpoint emission, the stack/tag observation patches in
  `patches.py` (`001AD1C0.c`, `001AD700.c`, `0023AE78.c`), the x87 parts of `test_float.py`,
  `test_runtime.py` and `test_rider_time.py`.
- `kernel.c`'s `hle_missing` (only `link.py`'s `missing.c` stubs called it).
- `codeids.py`'s map and COFF readers (only the wasm tables remain); `pctarget.IMAGE_BASE`;
  `sdl3.py`'s 32-bit default and `-msse2`; `host_boundary_audit.py`'s `missing.c` reader.
- Docs: `docs/BUILDING.md` names the 64-bit and browser builds, README.md's PC line too.

### Kept on purpose

- `pctarget.py`'s 32-bit values: `abi_check.py`'s source audit, `build.py` and the native unit tests
  (`port/tools/test_*.py`) compile for them.
- `build_wasm.py` still assembles the data image (`data.py`'s x86 assembler) as a COFF object:
  `data.check_image` and `data.write_fixups` check the placement and the retail bytes on it.
- `float_returns.json`, `abi_calls.json` and the scalar contracts: the wasm build reads them.
- `wasm_postlink.py`'s `--eax-tsv` option (its test covers it; nothing passes it now).
- `recomp.py`'s defaults (`build/port/...`), which its tests use; `build_wasm.py` passes its own.

### Still pointing at the old build (left as they are)

- Harness tools whose default exe or map is `build/port/link/dhd.exe` or `dhd.map`, all with
  `--exe`/`--map` options: `addr2sym`, `compare_pcsx2`, `pcsx2_retail`, `race_compare` (and its
  `build_compare_port.py` hint), `lockstep_local`, `online_race`, `profile`, `state_diff`,
  `state_trace_run`, `visual_sweep`, `wasm_hazards` (reads the 32-bit map for object names),
  `audio_benchmark` and `port/hle/bench_sound.c` (32-bit mixer benchmark),
  `gate_native_renderer` (its references are `build_clean.py` executables).
- `port/online/` (Settings.cmd, Online.ps1, Play-online.cmd, README.txt): the old online kit for the
  32-bit dhd.exe, packaged by nothing; Settings.cmd starts the deleted Launcher.ps1.
- `build_host64.py`'s `--wgpu` help names link.py (the renderer removal's line).
- `#ifndef __wasm__` branches in module code and `_WIN64` else branches in host code: not reviewed
  one by one, left.
- Older findings and STATE.md rounds describe the 32-bit build as it was.

### Check

No game run.

- `build_wasm.py -j 2`: exit 0 in 454 s. 3,815 of 3,815 game units, 712 recompiled functions (5
  failed in the recompiler, as before), 23 trap stubs, no signature conflict after the thunks,
  strict link ok, offset scan clean, `recompiled-with-c.txt` empty.
- Against the module built from the 32-bit inputs (sha256 `9edca5dc...`): `fixups.c`, `stubs.c`,
  `thunks.c`, `abi_thunks.tsv`, `recompiled.txt`, `vu0_native_words.inc` and the offset scan are
  identical. The module has the same 40,752 functions; one body differs: `rc_func_001B6708` is 198
  bytes smaller. That is the removed x87 checkpoint calls (empty inline functions on wasm, but they
  changed clang's code for the function): its generated C with the 80 checkpoint pairs put back
  compiles to exactly 198 bytes more.
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2`: exit 0,
  `DownhillRevived-0.3.1-win.zip` (22,444 KB), dhd.exe x64, imports only Windows DLLs.
- `port/web/build.py build/wasm/dhd_core.wasm --renderer rust`: exit 0 (the threaded build), 136
  imports match `host_api.h`.

## CL2: the Rust renderer is the only GS renderer

### What changed

- Removed (18 files, 7,740 lines): the Direct3D 11 renderer (`port/gs/gpu.c`, `gpu_native.inc`,
  `gpu_hw.inc`, `gpu_win32.h`, `raster.hlsl`), the C wgpu-native renderer (`gpu_wgpu.c`,
  `gpu_wgpu_native.inc`, and `port/tools/wgpu.py`, its library download and pin), the backend
  switch (`gpu_backend.inc`/`.h`: `DHD_GS_BACKEND`, `DHD_GS_NO_FALLBACK`'s exit 3), the D3D11-only
  tests (`test_hw_renderer.c`, `test_hw_coverage.c`/`.hlsl`, `port/tools/test_hw_renderer.py`,
  `test_hw_coverage.py`, `gate_hw_renderer.py`) and `port/render/gen` (`gen.py`,
  `gen_native_tables.c`: they read `gpu_wgpu_native.inc` and wgpu-native's `webgpu.h`; the tables
  they made stay pinned in `port/render/tests/data`).
- `port/gs/gpu.h` is the whole renderer interface: `gs_gpu_init(Gs *)` takes no device,
  `gs_native_vsync` no swap chain. `gpu_rust.c` defines it directly (no backend table);
  `gpu_null.c` (headless Linux) follows it. native_core.inc's hooks keep gpu.h's names, except
  `gs_native_vsync`, renamed to `rust_native_vsync` because it still takes the swap chain the
  browser glue passes (NULL).
- Adapter and video memory without D3D11: `gpu_rust.c` takes DXGI's first adapter (the one
  `D3D11CreateDevice(NULL, ...)` used to choose) and hands its PCI ids to `dhdr_device_init`. When
  that adapter is Microsoft's Basic Render Driver (1414:8C), or `DHD_WGPU_ADAPTER=fallback` is set,
  the renderer uses WARP. The texture-pack budget and the native scale limit read the adapter's
  `DedicatedVideoMemory` from the same DXGI description (`texpack_open(device, video_memory)`).
- `present.c`: SDL's window, then `gs_gpu_init` and the renderer's surface on the window. Gone:
  the swap chain, exclusive fullscreen (`DHD_WINDOW=fullscreen` is borderless over the whole
  monitor), the frame-latency waitable, which only the D3D11 chain had (so native_core.inc's
  `gs_frame_latency_wait` call is gone too), `DHD_GS_HARDWARE`, `DHD_FLIP_MODEL` and
  `DHD_GPU_TRACE`. A PC with neither DX12 nor Vulkan gets the "couldn't start its graphics" box,
  which now asks for Direct3D 12 or Vulkan. WARP still covers a PC with no graphics driver.
- `replay.c` and the test programs (`test_gpu`, `test_depth`, `test_present`,
  `test_native_transfers`, `test_native_stress`, `test_gpu_stubs`) no longer make D3D11 devices.
  `test_depth` compares the CPU's Z with raster.wgsl's exact Z only (the `d3d11_vs_cpu` column is
  gone; D3D11 matched the CPU on every case).
- Build: `build_host64.py` always compiles `gpu_rust.c` (now in `host64_census.HOST_UNITS` instead
  of `gpu.c`) and links `dhd_render.lib`. `--wgpu`, `--gs-rust` and `DHD_GS_IMPL` are gone, and so
  are `d3d11.lib` and `d3dcompiler.lib`; `dxgi.lib` and `dxguid.lib` stay for the adapter choice.
  dhd64.exe imports neither d3d11.dll nor D3DCOMPILER_47.dll any more. `package_release.py` calls
  `build_host64.py` without `--gs-rust`, and its `SYSTEM_DLL` no longer allows those two.
- Harnesses (Rust only; no `--impl`, `--arch` or `--backend`):
  - `test_gs_wgpu.py` runs test_gpu (with VRAM logs `vram-<device>-wgpu.bin`), test_depth and
    test_present.
  - `replay_gs.py` runs one replay.
  - `test_native_transfers.py` runs `wgpu` and `wgpu-no-dual` and compares them.
  - `gs_verdict.py` has no `impl` any more: every run must print `gs: renderer dhd_render`, and
    the only backend it accepts is wgpu.
  - `harness_negative.py` has Rust baselines. The "fallback" negative became "start" (a renderer
    that does not start exits nonzero and is red). The "renderer" negative drops the dhd_render
    lines, and a new "backend" negative reports d3d11.
  - `compare` leaves the goldens' d3d11 items (`RETIRED`) out of the comparison and out of the
    meta count. It still checks that they are intact, and a d3d11 item in a run fails.
  - The goldens' d3d11 and wgpu items were identical where they were compared at recording, so the
    Rust path is held to the same bytes as before.
  - `gs_golden.py selftest` and `gs_golden_selftest.py` point their d3d11 mutations at wgpu items
    and add retired cases (a retired item in a run, or changed or missing in the golden).

### Left as it is

- Other owners' files: CL1's i686 tools still name removed files: `link.py` (gpu.c,
  gpu_wgpu.c, wgpu.py, raster.hlsl), `build_clean.py` (DHD_GS_IMPL), `test_runtime.py` (raster.hlsl,
  gpu.c; it also links test_capture/test_gs, which link only for i686), `build_compare_port.py`
  (raster.hlsl, d3d11.lib) and `wasm_host_census.py` (it reads link.py's unit list).
  `docs/BUILDING.md` still says `build_host64.py --gs-rust` (one word, next to CL1's lines).
- `port/web` (not touched; BS2 and BS3 work there):
  - `gpu_rust_web_native.inc` still defines an empty `gs_frame_latency_wait` and passes NULL
    for native_core.inc's swap-chain parameter. Both can go together with that parameter.
  - `gpu_js_web.c` and `www/gpu.js` (the JS renderer) mention D3D11 in comments.
- Locked or recorder-hashed files keep their history comments: `gpu_exact.inc`, whose optrace
  recorder embeds its own blob SHA and whose header still names gpu_wgpu.c; the Rust crate's
  module comments (`as gpu_wgpu.c's ...`); `dhd_render.h`; NTRACE.md; and
  `tests/native_keys.rs`/`native_arenas.rs` (which name gen_native_tables.c).
  native_core.inc's comments still cite raster.hlsl as the origin of its rules.

### Check

Builds only (no program run):
- `build_host64.py build/wasm/dhd_core.wasm --num-outputs 8` with the x64 SDL3: dhd64.exe links
  (39 host objects). Its imports are ADVAPI32, GDI32, IMM32, KERNEL32, OLEAUT32, SETUPAPI, SHELL32,
  USER32, VERSION, WINHTTP, WINMM, two api-ms-win-core sets, bcrypt, bcryptprimitives, dxgi, ntdll
  and ole32.
- The Rust crate with test-hooks: a cold rustgs build.
- Compiled with the harnesses' own build functions: test_gpu, test_depth, test_present,
  test_native_transfers, test_native_stress, replay (test_gs_wgpu, replay_gs,
  test_native_transfers), plus harness_negative's test_gpu and test_native_transfers.
  test_capture and test_gs compile for x64.
- `port/web/build.py build/wasm/dhd_core.wasm --renderer rust`: the browser builds.
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2` on 4b419d6fc: exit 0, zip 22,370 KB.
  dhd.exe imports only Windows DLLs, dxgi.dll among them and neither d3d11.dll nor D3DCOMPILER_47.dll.

## CL4: fewer tools and shorter docs

### Removed

- `port/tools` (151 -> 131 tracked files), 20 tools and the C programs only they built (29 files,
  about 5,200 lines):
  - 32-bit only: `addr2sym.py` (the i686 fixed-base map; `host64_rva.py` names x64 addresses),
    `audio_benchmark.py` + `port/hle/bench_sound.c` (32-bit mixer benchmark), `native_census.py`
    (i686 syntax census; `build_wasm.py`'s census replaced it), `profile.py` (DHD_PROFILE's DHP4
    records hold 32-bit code addresses; `sample_profile.py` profiles x64), `race_proof.py` and
    `visual_sweep.py` (read the i686 child process's EE memory at 0x08000000), `wasm_hazards.py`
    (i686 vs wasm divergence census over the i686 objects and map), `bench_vu1_geometry.py`,
    `bench_vu1_programs.py` (`clang-cl -m32` benchmarks; with `port/runtime/bench_vu1_geometry.c`,
    `bench_vu1_geometry_kernel.h`, `bench_vu1_programs.c`) and `compare_vu1_native.py`.
- `port/gs/native_keys.inc`: nothing compiles it since `port/render/gen` went (CL2); its tables
  stay pinned in `port/render/tests/data`.
- `port/online/` (Online.ps1, Play-online.cmd, README.txt, Settings.cmd): the old friends' online
  kit; nothing packages it and Settings.cmd started a deleted script.
- `test_runtime.py` no longer reads `port/gs/raster.hlsl` (gone) or builds the D3D11 `test_gpu`
  (gpu.c, d3d11.lib); `test_gs_wgpu.py` runs test_gpu on the Rust renderer.

### Harness defaults

- `compare_pcsx2.py` (`--exe`, unless `gs-replay --from-dumps`) and `race_compare.py` (`--exe`)
  require the exe; `pcsx2_retail.py` and `compare_pcsx2.py` run it in its own folder (they ran it
  in `build/port/link`). DHD_RACE_SYNC, DHD_FRAME_CLOCK, DHD_AUTOPAD_FRAMES and the capture
  switches are run-time switches of the shared HLE, so these work on dhd64.exe. Not run.
- `lockstep_local.py`, `online_race.py` and `state_trace_run.py` default to
  `build/host64-exe/dhd64.exe` (build_host64.py's output); `state_diff.py`'s symbols default to
  the core's map, `build/wasm/dhd.map` (it takes the `func_`/`D_` names, which hold the EE address).
- `port/hle/host.c` comments: crash and watch reports go to `host64_rva.py`; dhd.ini's online keys
  are `port/net`'s.

### Docs

- README.md's layout names `port/` and `online/server/`; BUILDING.md names the renderer's tests.
  Both already described only the 64-bit and browser builds.

### Kept on purpose

- `bench_race.py`, `bench_x64.py` (x64 timing), `audio_report.py`, `audio_gain_report.py`
  (read DHD_SND_WAV / DHD_SND_GAIN_TRACE output, still in sound_debug.h), `profile_vu1.py`
  (DHD_VU1_PROFILE), `field_bob.py` (findings 060, 086), `shader_compile_time.py` (wgpu's DX12
  compile of raster.wgsl), `wgsl_parens.py` (Tint's grammar), `passthrough.py`, `rules_audit.py`
  (source audits), `gs_fixture.py` (synthetic GS packets for replay_gs), `codeptrs_guard.py`
  (state traces), `vu1_seq_dep.py`/`vu1_seq_diff.py`, the `host64_*` tests.
- Self-contained tests that test_runtime doesn't run but that test shipped code (they build
  themselves, some for i686 with clang): `test_defined_producers`, `test_fps_envelopes`,
  `test_input_edges`, `test_huft`, `test_ps2asm_fast`, `test_split_graphics`, `test_text_input`,
  `test_inflate_status`, `test_wasm_retail_names`, `test_async_services`, `test_owned_process`,
  `test_particle_timing`, `test_vu1_aot`.

### Still naming removed things (left)

- Locked history comments: the Rust crate's `native/arenas.rs`, `native/pipelines.rs`,
  `tests/native_keys.rs`, `tests/native_arenas.rs` and the pinned JSONs' `source` name
  native_keys.inc and gpu_wgpu_native.inc.
- `port/hle/host.c`'s DHD_PROFILE sampler still writes DHP4 (32-bit addresses) and has no reader
  now; the comparison tools still set DHD_PROFILE. It can go with its `profiler` thread.
- `bench_x64.py` still offers an i686 label and handles the 32-bit relaunch (harmless).

### Check

`build_wasm.py` not run (nothing it imports changed). `package_release.py --wasm
build/wasm/dhd_core.wasm --jobs 2` on 1b6995cdd (the gate-X64 dhd_core.wasm, built at d40f885f5):
exit 0, zip 22,371 KB, dhd.exe x64 with the Rust renderer, imports only Windows DLLs. No game run.

## CL5: the browser draws only with the Rust renderer

### What changed

- Deleted `port/web/www/gpu.js` (123 lines, the JS/WebGPU device ops) and `port/web/gpu_js_web.c`
  (85 lines, its provider of `gpu_exact.inc`).
- `port/web/build.py`: `--renderer` accepts only `rust` (kept so the callers that pass
  `--renderer rust` still work); the Rust module is always built; the stale-renderer cleanup of
  the js build is gone. `--no-threads` still gives the one-worker build.
- `port/web/build_graphics.py`: no `--renderer` (only build.py called it); `gpu_rust_web.c` and the
  sharp font are always linked; `check_imports` keeps the Rust rules (declared `dhd_render`
  imports, `env.web_gpu_await`, none of the old `web_gpu_*` device ops).
- `port/web/check_web_layout.py` (locked, P0-6): the renderer module is always required (the
  renderer-less js build is gone); `--require-renderer` is accepted and does nothing. The rules
  are unchanged and the check is stricter; `--selftest` passes (6 fixtures).
- `port/web/www/worker.js`: no `createGpu`, no js branches (present, status, imports, start).
- `port/web/serve.mjs`: the `/gs/` route for `raster.wgsl`/`present.wgsl` is gone (only gpu.js
  fetched them; the Rust module has its shaders compiled in).
- `run_headless.mjs`, `headless_graphics.mjs`: only the `dhd_render` and `web_gpu_await` stubs.
- `port/tools/web_gs_replay.mjs` (locked, B3): replays on the Rust renderer only (the default was
  js); `--renderer rust` is accepted. `gs_golden.py record` calls it without `--renderer`, so it now
  replays on Rust.
- `gpu_rust_web.c`, `port/gs/replay_web.c`: comments no longer name gpu.js/gpu_js_web.c.
- `graphics_common.js`, `graphics.js`, `main.js`, `index.html`, `check_browser.mjs`: nothing in
  them was js-renderer only, so they are unchanged.

### Left as it is

- `port/gs/gpu_exact.inc` (locked) still names gpu_js_web.c and gpu.js in its header comment.
- The Rust crate's module comments (`exact/buffers.rs`, `encoder.rs`, `readback.rs`,
  `web/init.rs`) describe their behaviour "as gpu.js's"; older findings (083-087) describe the js
  path as it was.

### Check

Builds only, no browser or game run, with gate-X64's `dhd_core.wasm`:
- `port/web/build.py build/wasm/dhd_core.wasm --no-threads`: exit 0; `check_web_layout.py
  build/web`: OK (core, graphics, renderer).
- `port/web/build.py build/wasm/dhd_core.wasm --renderer rust` (threaded): exit 0, reactor imports
  `dhd_render` 24, `env` 3; `check_web_layout.py build/web`: OK, every memory shared.
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2` (for the port/gs changes): exit 0,
  dhd.exe x64, imports only Windows DLLs.

## CL7: no shadow legacy host; shared build plumbing

### What changed

- The x64 builders compiled `host_win32.c` and `host.c` with 14 `-D<name>=host64_legacy_<name>`
  renames, so the old i686 definitions were built and thrown away. They are gone, and so are the
  rename lists in `build_host64.py` and `build_host64_linux.py`:
  - `host_win32.c`: the fiber table, the fixed-stack fibers of state traces (`run_on_stack`,
    `fiber_entry`), the fxsave/fxrstor fiber switch, `host_stack_ranges`, `host_fp_mode`, the RAM
    file mapping (`host_ram_create`, `host_ram_view`, `host_mem_commit`, `host_mem_reserved`) and
    the `_WIN64` conditional around the async services (no 32-bit build is left);
  - `host.c`: `host_relaunch_reserving` (the suspended child with its reservations) and the
    `__security_check_cookie` reporter; `host_service_host_relaunch_reserving` returns -1, as it
    did on x64; `bridge.c`'s unused `host_relaunch_reserving` and its `hle.h` line;
  - the native callback-address wrappers (`host_callback_*`, `host_stack_ranges_copy`,
    `host_callbacks_drain`) in `host_win32.c` and `host_posix.c`: the generated `imports_gen.c`
    implements them. `gen_host64_imports.py` no longer falls back to a native
    `host_callbacks_drain` for a module without callbacks (it was empty).
  The real definitions are the host64 glue's (`fiber_x64.c`/`fiber_posix.c`, `bridge.c`,
  `guest_memory*.c`, `fp_mode.c`).
- Thread naming has one implementation, `host.c`'s `host_thread_name`: it records the thread for
  `host_thread_cpu` (the bench's per-thread CPU) and now also sets the OS thread name
  (`SetThreadDescription` / `pthread_setname_np`), which `diagnostics.c`'s copy did. Before, only
  host.c's own "game" thread reached the CPU table on x64 (the others went to diagnostics.c), so
  `DHD_BENCH`'s "bench threads" line now lists vu, gs, native, sound and the others too.
- `port/tools/port_source.py`: `apply_patches` and the 16-byte alignment of aggregate locals, once,
  for `build.py` (kept: `abi_check.py`, `test_defined_producers.py` and `test_runtime.py` use it)
  and `build_wasm.py`. Both add the module to their object-cache key, so the first build after
  this misses once.
- `prepare_wasm2c.py` gains `module_units` (wasm2c with module name `core`, or the copied WABT
  units), `check_launcher_exports` and `prepare_units`; both host builders use them. Windows keeps
  its exact f32 literals, guard pages and wasm-rt.h rewrite; Linux keeps its explicit bounds checks.
- `build_wasm.py` no longer writes `build/wasm/w2c/dhd_core.c` (nothing read it) and has no
  `--no-wasm2c` option; findings 083 and 086 still show the option in the commands they ran.

### Left as it is

- `build_host64.py --fake` without `--sdl-lib` stops at the link: the fixture imports the text
  input and GS services, whose units need SDL3. With `--sdl-lib` it links.
- `memory.c`/`memory.h` comments still name `host_relaunch_reserving` (module code, i686 history).

### Check

Builds only, no runs: `build_wasm.py -j 2` (module identical to gate-X64's); `package_release.py
--wasm build/wasm/dhd_core.wasm --jobs 2`: dhd.exe x64, Windows DLLs only; `port/web/build.py`
`--no-threads` and `--renderer rust`: `check_web_layout.py build/web` OK both; `build_host64.py
--fake --sdl-lib ...` linked, then `host64_imports_test.py` (every wrapper compiles, the generator
checks pass); `test_async_services.py`'s harness compiled and linked; `build_host64_linux.py` in WSL
Ubuntu: dhd64 linked (null audio, no window).

## CL8: one copy of the Rust renderer's C glue and of the DevTools plumbing

### What changed

- New `port/gs/gpu_rust_shared.inc`, included by `gpu_rust_native.inc` (x64) and
  `port/web/gpu_rust_web_native.inc` (browser) in place of their own `native_core.inc` /
  `native_snapshot.inc` includes. It holds what the two sides had twice:
  - the device ops that are one `dhdr_exact_*` call each (`op_write`, `op_copy`, `op_batch`,
    `op_record`, `op_submit`) and `op_clip`, over `NativeExact` (`Rust` or `Web`), out of
    `gpu_rust.c` and `gpu_rust_web.c`;
  - `NativeView`, `NATIVE_TEXTURE_GPU`, `NATIVE_SURFACE_GPU` and the shared `NATIVE_GPU` members
    (each side adds its own through `NATIVE_GPU_SIDE`; member order and layout as before);
  - the replacements' device (`dhdr_replace_create`, `dhdr_texture_retain` / `_release`),
    `native_texture_create` / `_release`, `native_surface_resources`, `native_picture`,
    `native_present_options`;
  - `native_present_draw` (the present's rectangles, `dhdr_native_present`, its failure) and
    `native_create_rust` (creation's common half up to the replacements' device).
- Per side, as before: `op_read` (the browser suspends), `op_clock`, `op_draw_skipped`,
  `native_replacement_view` (texture packs), `native_match_window`, `native_present` /
  `native_show` (window and pacing, or the canvas), `native_read_display`, `native_init`'s own
  half (texture packs, scale limit, `DHD_SCALE_MAX`, `sync_range`), the browser's stubs.
- Comments that named removed files (`gpu_wgpu.c`, `gpu_wgpu_native.inc`, `gpu.js`,
  `native_keys.inc`, `gen_native_tables.c`, `raster.hlsl`) now say what is true now or mark the
  file as retired with where it last was: `native_snapshot.inc`, `native_core.inc` (raster.hlsl
  -> native.wgsl / raster.wgsl, which have those functions now), `gpu_rust_web.c`, `dhd_render.h`,
  22 modules of `port/render/src` and `tests/native_keys.rs` / `native_arenas.rs`, NTRACE.md's
  recorder paragraph (past tense).
  - The reference is render/rust commit `3882d5576` (the last with every one of them), not tag
    `v0.3.1`: the C wgpu-native renderer, the JS renderer, native_keys.inc and the
    table generator only ever lived on render/rust, so the 0.3.1 tag has none of them (it has
    gpu.c and raster.hlsl).
- New `port/web/cdp.mjs`: `startChrome` (profile, flags, the "DevTools listening" line, a 30 s
  limit that stops the browser), `connect` (the WebSocket call bookkeeping: `send`, `evaluate`,
  `events`) and `runPage`, the whole run of the two port/tools checks (page server on 127.0.0.1,
  Playwright's Chromium with the Dawn flags on a fresh `build/tint-check-profile/<pid>`, the tab,
  cleanup, exit status). `tint_check.mjs` and `web_gs_replay.mjs` run through `runPage`;
  `check_browser.mjs` (system Chrome, a target of its own, sessions) uses `startChrome` and
  `connect`. Each keeps its own page and checks.

Lines (against b29686be5): the C glue 1,211 -> 1,099 (gpu_rust.c 427 -> 390, gpu_rust_native.inc 265
-> 130, gpu_rust_web.c 212 -> 176, gpu_rust_web_native.inc 307 -> 188, new gpu_rust_shared.inc 215):
112 fewer; what both sides had alike is written once. The DevTools tools 331 -> 332: their plumbing
(about 60 lines, three times) is one copy now, but cdp.mjs carries its own documentation, so the
total stays level.

Locked files edited (dedupe or comment only, no behaviour change): `port/gs/gpu_rust.c`,
`port/tools/web_gs_replay.mjs`, `port/render/include/dhd_render.h`, `port/render/NTRACE.md`,
`port/render/src/lib.rs`, `port/render/src/test_hooks/*`, `port/render/tests/native_keys.rs`,
`native_arenas.rs`. `gpu_exact.inc` is untouched (its recorder hashes itself).

### Behaviour

None intended. Small differences in the tools' failure paths only:
- `tint_check.mjs` / `web_gs_replay.mjs` learn the DevTools port from Chrome's stderr line instead
  of the profile's `DevToolsActivePort` file (Chrome's stderr is piped and, with `CHROME_LOG`,
  copied to ours); a Chrome that never starts is stopped after 30 s (was: 20 s, then an exception).
- `check_browser.mjs` prints its "server pid, chrome pid" line once Chrome is listening, not right
  after the spawn; a WebSocket that fails to open now throws instead of hanging.

### Check

Builds only, nothing run, after merging render/rust (with PR1), gate-X64's `dhd_core.wasm`:
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2`: exit 0, dhd.exe x64, imports only
  Windows DLLs.
- `port/web/build.py build/wasm/dhd_core.wasm --renderer rust` (threaded) and `--no-threads`: exit
  0 each; `check_web_layout.py build/web`: OK (core, graphics, renderer) after each.
- `rustgs.py test --target x86_64-pc-windows-msvc -- --no-run`: every test compiles.
- The test programs that include the glue, through their harness build functions: test_gpu,
  test_depth, test_present (`test_gs_wgpu.build`), replay (`replay_gs.build`),
  test_native_transfers (`test_native_transfers.build`): all built.
- `node --check` on cdp.mjs, check_browser.mjs, tint_check.mjs, web_gs_replay.mjs.

## CL9: one sound backend per build; one benchmark driver

### Sound output

- `port/host/sound_out_common.inc` (new, 138 lines) holds what `sound_out_win32.c` (WASAPI) and
  `sound_out_sdl.c` (SDL3) had twice: the ring and its push, the counters and histograms,
  `DHD_SND_LATENCY_MS` parsing, percentiles, the `bench snd:` line, `trace_on` and the
  device-free part of the interface (`host_sound_open`/`write`/`reset`/`tick`). Each backend
  includes it once and supplies `host_start` (its output thread) and `host_bench_tag` (the end of
  the bench line: ` legacy` / ` sdl`). WASAPI keeps its WSOLA time stretch, its legacy loop and
  its device buffering; SDL keeps its stream buffering. The bench line and every log line are
  unchanged. 507 + 267 lines -> 389 + 153 + 138 (94 fewer). `sound_out_null.c` is unchanged.
- Backend selection: `host64_census.HOST_UNITS` (the Windows x64 list, used by `build_host64.py`
  and so `package_release.py`) now holds exactly one backend, `SOUND_UNIT`: WASAPI, or
  `sound_out_sdl.c` with `DHD_SOUND=sdl` at build time (needs `--sdl-lib`, which
  `package_release.py` always passes). `build_host64_linux.py` is unchanged: SDL with
  `DHD_SOUND=sdl`, else null.
- Found on the way: until now HOST_UNITS archived BOTH backends into `host64.lib` (since
  970f77a14), and lld-link resolved `host_sound_*` from `sound_out_sdl.c`: every x64 dhd64.exe
  built since then (gate-X64, CL1-CL5, PR1's maps) played through SDL audio, without the time
  stretch. The 32-bit releases (link.py, up to 0.3.1) used WASAPI unless `DHD_SOUND=sdl`. The x64
  exe now takes WASAPI again, as the released builds did; `DHD_SOUND=sdl` gives the SDL path.

### Benchmarks

- `bench_x64.py` (105 lines) is merged into `bench_race.py` (212 -> 252 lines; 65 fewer in all).
  `bench_race.py` keeps its race route (race_compare's NAV, pad, seed 81257), candidates, focus
  check, `--sound` and the interpolation/audio/thread summaries, and gains `bench_x64.py`'s menu
  scene (`--scene menu`, fields 400-1000), `--mode paced|flat` (flat, the old behaviour, is the
  default), `--minimized` (never takes focus) and `--headless`. The game image defaults to
  state_trace_run's when DHD_ISO is unset. With several scenes or modes the summary keys are
  `LABEL SCENE MODE`.
- Fixed: `focus_game` looked only for `dhd.exe` windows and so never focused a `dhd64.exe`
  (build_host64.py's output); it now takes `dhd.exe`, `dhd64.exe` and the candidate's own name.
- Gone with bench_x64.py: its i686 child-relaunch handling. Its race scene used state_trace_run's
  race pad and window 3800-4400; the merged race scene uses bench_race's (pass `--window` for
  another window). No script called bench_x64.py; STATE.md and finding 082 now name bench_race.py.
  `sample_profile.child_of` stays (sample_profile itself still uses it).

### Check

Builds only, no game or benchmark run, after merging render/rust:
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2`: exit 0, dhd.exe x64, imports
  only Windows DLLs. `host64.lib` holds one sound object (`sound_out_win32.c.obj`); the map
  resolves `host_sound_open`/`write`/`log_stack` and `snd_host_bench` from it, nothing from
  `sound_out_sdl.c`; the exe carries "989snd: WASAPI output started".
- `build_host64_linux.py` (WSL, `--num-outputs 8 --jobs 2`): null audio PASS (35 Linux host
  objects).
- `DHD_SOUND=sdl build_host64_linux.py` (WSL; SDL3 3.4.18 built for Linux from the local tarball,
  2 jobs): PASS (36 objects, SDL window/input and SDL audio); its archive holds
  `sound_out_sdl.c.o` only, the null build's `sound_out_null.c.o` only.
- `sound.c` + `test_sound.c` (`/DSND_NO_HOST`, x64 clang-cl `/c`) and `sound_out_sdl.c` for Windows
  (the `DHD_SOUND=sdl` unit, with the SDL3 headers) compile; not linked or run.
- `py_compile` of bench_race.py and host64_census.py; `bench_race.py --help` parses.

## CL6: dead code and dead tools

No behaviour change of the game, the exe or the browser.

- Dead tools: `port/tools/wasm_census.py` (i686-based, no caller; build_wasm.py's comment naming
  it), `port/hle/replay_sound.c` (no build lists it; test_sound.c and its helpers stay),
  `port/tools/test_fiber_float.py` and its two lines in test_runtime.py (it looked for
  `void *host_fiber_main`, gone), `port/web/check_float.mjs` (check_float_exact.mjs supersedes it).
- Checked-in generated files: `port/host64/imports_gen.c` and `exports_gen.h`.
  gen_host64_imports.py writes both into each build's output dir, and that dir is the first `-I`
  in build_host64.py and build_host64_linux.py (launcher.c's `<exports_gen.h>` finds it there;
  imports_gen.c's quoted include finds its own dir).
- host.c's DHP4 in-process sampler (Windows and POSIX), the `named_thread` lookup only it used,
  its startup wiring (SIGUSR2 handler, thread) and `DHD_PROFILE_THREAD`. `DHD_DUMP` keeps the
  game-thread handle it needs. `DHD_PROFILE` itself stays (gpu_rust.c and present.c renderer
  diagnostics), as does the named-thread CPU accounting (`host_thread_name`, `host_thread_cpu`)
  the benchmarks read. compare_pcsx2.py and pcsx2_retail.py no longer ask for a profile file.
- Small bits: `port/hle/frame_profile.h` and kernel.c's include and seven marks (nothing defines
  `DHD_FRAME_PROFILE_BUILD`); sample_profile.py's unused `direct_caller`; the dhd.ini keys
  `msaa`, `fxaa`, `filter`, `remap` (host.c and port/web/www/settings.js; an old dhd.ini with
  them now logs "unknown key"); dxc.py's x86 DLL entry and pin (only `dll('x64')` is used).
- Synthetic migration benchmarks: `port/tools/host64_bench.py` and `port/host64/bench_core.c`.
  host64_dispatch_test.py and test_wasm_postlink.py now take the toolchain paths from
  build_host64.py (ROOT, WABT, WASI, LLVM, MSVC, SDK, VERSION, SOURCE) and define RT/BINARYEN
  locally. wasm_postlink.py's custom eax table (`eax=`, `read_eax`, `retail_names`, `--eax-tsv`,
  the `eax_constants_used` report key) is gone: an integer caller of a void callee gets 0, the
  bytes the real build (which never passed a table) already produced. test_wasm_postlink.py's
  case 1 now expects 0. The indirect-call adaptation itself is unchanged.
- Diagnostic switches nobody sets: `DHD_TEST_PAD` (host.c's shared-memory pad merge),
  `DHD_SND_GRAIN_TRACE` and `DHD_SND_RNG_TRACE` (sound_debug.h and their calls in
  sound_grains.h and sound_rng.h).

- After merging CL7: host.c's POSIX `<fcntl.h>` and `<sys/mman.h>` (their last users, the test
  pad and the relaunch, are gone); the comments in port/runtime/memory.c and memory.h no longer
  name `host_relaunch_reserving` (the reservation is the wasm core's linear memory,
  bridge.c's `host_mem_reserved`).

Kept: state_trace.c's `"DHD_PROFILE"` entry in its environment list (the variable lives on). No
locked file was edited.

Checks (builds only): `build_wasm.py -j 2`, `package_release.py --wasm ... --jobs 2` (dhd.exe x64,
only Windows DLL imports), `port/web/build.py ... --renderer rust` threaded and `--no-threads` with
`check_web_layout.py build/web` OK after each, `build_host64_linux.py` in WSL (PASS, 136 imports, 35
Linux host objects), `py_compile` of every Python file touched.

## AND1: Android arm64 build

Finding 091. Shared code touched for arm64: `guard.rs` (FPCR), `surface.rs`/`ffi.rs`
(`DHDR_WINDOW_ANDROID`), `rustgs.py` and `check_rust_lib.py` (the Android triple, ELF members),
`sdl3.py --android`, `fiber_posix.c` (aarch64 frame fixed), `native_thread.h` (POSIX branch),
`gpu_rust.c`/`gpu_rust_native.inc`/`present.c` (non-Windows window size, surface reset, Android
lifecycle), `host.c`/`vif.c`/`keyboard.c` (`__aarch64__` with the x64 layout tests, pacing fences),
`graphics_queue.c`/`gs_queue.inc` (`fp_control`), `vu0_interp.c` (SIMDe on arm64), `font_sharp.c`.
New: `build_host64_android.py`, `port/host/android_main.c`, `backtrace_posix.h`, `port/android/`.
The x64 exe (`package_release.py`) and the browser build were rebuilt after it. After the device
runs: `sdl_host.c` (Android polls SDL's events, then sleeps 4 ms), `present.c` (the Android
lifecycle in an SDL event watch), `surface.rs` (Android: a lost surface fails the present), `host.c`
(the Android disc check before the core stops at SYSTEM.CNF), `host_posix.c` (Android: backslashes
to slashes in host paths; the log drained at exit).

## UI1: online flow, one centred menu layer, mouse

Finding 092. Online: Create/Join waits on ONLINE until the server has the player, then the rider
screen once and the room over it; back from the shop the title flight is skipped (the seek now comes
after the forced play that rewound it). `menu_page.c` is the one layer for the port's pages (centred
like the pause menu; button, value and text rows); PC SETTINGS, the exit question, ONLINE and the
race room use it. Mouse: `host_service_mouse_state` (HostMouse, picture fractions) from SDL3
(present.c, the renderer's picture rectangle) and the browser canvas (main.js/worker.js); hover,
click, right click and wheel in every popup the manager runs; the OS cursor hides when idle or
riding. No game logic changed: race-start state trace 3000/3000 rows match main's exe. Builds: wasm,
x64 package, both web builds with check_web_layout. 
