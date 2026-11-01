# State

Goal: readable, **matching** C/C++ decompilation of Downhill Domination (PS2, USA `SCUS_971.77`):
source that rebuilds the retail executable byte for byte. Later it can serve as the base for a PC
port. PCSX2 is the behavioural reference.

Progress: `python tools/report.py` (objdiff report `progress/report.json`, README table).
2026-10-12: game 98.17% and engine 92.61% of code matched, 4,526 of 4,593 functions (the SDK and
libgcc regions are library code and stay assembly). Left: 28 functions in the queue, 7
hand-written, 164 eight-byte remnants that aren't functions.

## Phase 0 — compiler identification (done)
`docs/findings/002-compiler-sn-prodg.md`. Game code: **SN ProDG 3.01** — cc1 "2.95.3 SN BUILD
1.36" + SN assembler `ps2eeas`, **`-O2 -G8 -fopt-stack`**; C++ `-fno-exceptions` (finding 003).
Library regions are identified (finding 003, `config/libmatch.txt`) and not decompiled.

## Phase 1 — project skeleton (done)
`docs/findings/003-build-system.md`. The retail image rebuilds byte-identical from splat asm +
C/C++ (`python tools/configure.py && python -m ninja` → MATCH), checked by `tools/check_image.py`.
Tooling: `tools/queue.py` (work queue + claims), `tools/try_func.py` (per-function trial build,
with a fast compile-only check first), `tools/diff_func.py`, `tools/promote.py` (into `src/` only
if the whole image still matches; refuses TU merges that would turn C back into asm),
`tools/lost_check.py`, `tools/unit_merge.py` (merge existing units into a wider translation
unit), m2c drafts. Guide for contributors: `docs/WORKER.md`.

## Phase 2 — decompilation (finishing; 28 functions left)
Functions are claimed from the queue, decompiled one at a time against the retail bytes and
promoted only when `try_func` prints MATCH and the whole image still matches. Finding 004
covers how the assembler addresses small data, finding 007 what made trials fast.

2026-10-06: `func_00134CC8` and `func_00240358` promoted after exact function and
whole-image checks. The latter required integration into its existing full translation unit.
Finding 009 distinguishes an assigned-function implementation from a matching assembly stub,
and records serialization of report generation with source promotion. A fresh full-image check
matched all eight checked sections.

2026-10-11: six more matches (00137300, 00137B70, 00148F60, 001554C0, 00171B28,
00228BE8), including two translation-unit merges. The rebuilt image matches all eight
checked retail sections.

2026-10-21 (finding 074): the 25 functions not matched yet have behaviour-equivalent C under
`#ifdef NONMATCHING` next to their `INCLUDE_ASM` (18 new single-function units). The default build
keeps the asm and still matches; `python tools/configure.py --nonmatching` builds the C into
build-nonmatching/ (links, not checked; sizes and the moved ranges are in the finding).
func_00220DA0 is now listed in config/handwritten.txt (trapping adds, pmfhi/pmflo).

Open items:
- `engine` region 0x201000–0x243000 (1,202 functions): same toolchain and flags as game code;
  what it is (engine/middleware) is still open.
- Real TU boundaries: single-function units are merged when evidence appears (shared `$gp`
  data, `.rodata` ownership). func_001F24B8 and func_001C8380 have matching C that still needs
  its translation unit worked out before it can be promoted.
- Per-file flags: two small clusters were compiled `-O0 -fno-opt-stack` (0x21E9A8–0x21EAA0,
  0x2212A0–0x221608); rule in WORKER.md.
- Language: the game is C++ (exception tables, vtables, `this`-style calls). Matching is done in
  whatever form is fastest (mostly C, byte-identical to the method form); a later phase turns
  shared structs into classes and merges functions into reconstructed TUs.
- Hand-written assembly (e.g. func_0021B8A8: registers saved to the scratchpad, `$gp` used as
  scratch) stays `.s`.
- `.vutext`/DVP overlays (VU microcode) stay binary for now.

## Phase 3 — PC port: Downhill Revived (current)
Plan: `docs/findings/015-pc-port-plan.md`. Port code lives in `port/` and is applied at build
time; the matching sources and checks don't change. Build steps and requirements:
`docs/BUILDING.md`. Players' text: `port/release/README.txt`. The round-by-round log of how the
port got here (2026-10-12 to 2026-10-25) is in the findings (015-088) and in this file's history
(`git show v0.3.1:docs/STATE.md`).

Releases: 0.3.1 (2026-10-25) is the last 32-bit release (tag `v0.3.1`; it can always be
rebuilt from there). 0.4.0 is the first 64-bit release, the build below; 0.3.1's updater installs
it unchanged (finding 088, CL3).

### The builds
- One portable game core: `port/tools/build_wasm.py` compiles every `src/` unit (rewritten at
  build time by `port/tools/transform.py`, with the PC patches of `port/patches*.py`), the
  asm-only functions recompiled to C (`port/tools/recomp.py`), the module-side runtime and HLE
  and the data image's fixups into one wasm32 module, `build/wasm/dhd_core.wasm` (findings 081,
  082). Everything comes from the checkout, `asm/` and `assets/vutext.bin`.
- One build script for every platform (finding 099, Mac and Linux): `port/tools/build_host64.py
  --target win-x64|linux-x64|macos-arm64|android-arm64` (default win-x64), with what differs per
  target in its TARGETS table (compiler and flags, linker, SDL3's build, the Rust triple, the output:
  `build/host64-exe/dhd64.exe`, `build/host64-linux/dhd64`, `build/host64-macos/dhd64`,
  `build/android/lib/arm64-v8a/libmain.so`). The same host units everywhere (host64_census's, with
  the POSIX host and texture-pack stub off Windows, Android's start there), guard-page bounds checks
  and the plain call-depth global on every target, the Rust renderer everywhere (`gpu_null.c` is
  gone). Guest pages are 16 KiB on every platform (`HOST64_PAGE`: commits, the null page, the EE
  stack guards), so the committed layout does not change with the OS page.
- The EE kernel's fibers are one module on every platform (`port/host64/fiber.c`: the 64 slots,
  the guest and native stacks and their guards, the root; finding 099, "Fibers"); only the
  register switch differs per CPU and ABI (`fiber_win64.c`, `fiber_sysv64.c`, `fiber_arm64.c`,
  the table's `fiber`). Windows uses the same hand-written switch, not SwitchToFiber.
  `build_host64.py [--target T] --fiber-test` checks them alone (`selftest_fiber.c`).
- Windows x64: `port/tools/package_release.py` runs `build_host64.py` (wasm2c, the x64 host
  `port/host64` + `port/host`, the Rust renderer) and writes the players' zip
  (`build/release/DownhillRevived-<version>-win.zip`, `latest.json`) after its checks: version,
  no absolute path, no server but `dhd.skuza.app`, no retail bytes (`scan_retail.py`), Windows
  DLL imports only, the Rust library (`check_rust_lib.py`), pinned `dxcompiler.dll`, generated
  Rust crate notices. It builds only from committed source.
  Guest RAM mirrors: the OS views plus `memory_access.h`, with one inline window test per base
  value per access site (`prepare_wasm2c.py split_accesses`) and the fold out of line (finding 090 OP1).
- Browser: `port/web/build.py build/wasm/dhd_core.wasm` (into `build/web/`). The graphics side
  (VIF1/VU1, GS front end, renderer) runs in its own worker on shared memory by default
  (`--no-threads` for one worker; finding 087, BS2). It draws only with the Rust renderer
  (`--renderer rust` is still accepted; the JS/WebGPU renderer `www/gpu.js` is gone, finding 088
  CL5). Speed toward 60 fps: finding 087. Its PC SETTINGS (finding 087 WO1): resolution 1x-8x
  (applied at once), scaling, widescreen, font, frame rate (60-144 paced to the core's period).
  dhd.ini there is `dhd_settings` built on its own (`rustgs.py build --target
  wasm32-unknown-unknown --crate dhd_settings`, by build.py; its own memory: layout rule P1);
  `node port/web/check_settings.mjs [--iso PATH]` runs the crate's corpus and the game's settings
  page through it in headless Chrome against the native results (finding 102).
- macOS arm64 (`--target macos-arm64`, on a Mac with Xcode's tools; macOS 11 and later): the POSIX
  host with SDL3 static (Cocoa, Core Audio) and the Rust library (Metal, rustgs.py on the Mac).
  `package_release.py --platform mac` makes `Downhill Revived.app` (ad-hoc signed) in the -mac-experimental.zip.
  Built and run on an M1 (2026-10-27): finding 099. On the M1 Air the native renderer
  holds 59.9 fields/s on the title, in the menus and in a race at the player's settings (GPU about
  half busy); the 12 fields/s first seen were the exact renderer, which a run without dhd.ini
  gets (finding 099, "The M1's speed"). `DHD_GPU_TIMES=N`: the native renderer's GPU time by
  pass kind from timestamp queries, logged every N frames.
- Linux x64 (`--target linux-x64`, on Linux or in WSL, linked with lld): the shared host with SDL3
  (X11, Wayland, PipeWire/PulseAudio/ALSA) and the Rust library (Vulkan; rustgs.py on the machine,
  or cross-built on Windows for WSL). `package_release.py --platform linux` makes the -linux-experimental.zip.
- Android arm64 (finding 091): `build_host64.py --target android-arm64` makes `libmain.so` (wasm2c core, the
  POSIX host on arm64, the Rust renderer on Vulkan, SDL3 as `libSDL3.so`) and
  `port/android/build_apk.py` a debug-signed APK with Gradle (no Android Studio). On the Mi 11i:
  title, menus and a race draw right, background/foreground works; the first race measurement was
  about 25 fields/s, and findings 107/112 brought it to 115 fields/s at 1x uncapped (93 at 2x), so
  60 is reachable there at 1x. A low-end Unisoc T612 phone (Realme, finding 126) does 36-40
  fields/s at 1x and 14 at 2x and cannot hold 60. Gamepad or keyboard only (no touch). Online uses the same
  Rust client as Windows (`dhd_net`, below; not yet tried on the phone).
- Retired (finding 088): the 32-bit i686 build (`link.py`, `build_clean.py`, the x87/FP-trap
  runtime), the Direct3D 11 renderer, the C wgpu-native renderer and the browser's JS renderer.
  No D3D11 fallback.

### How the port runs the game
- `port/hle/` replaces the PS2 system: EE kernel (threads as fibers, semaphores, interrupts),
  crt0, SIF RPC, disc and file I/O (`DHD_ISO`), DMA, VIF0/VIF1, timers, pads, memory card
  (`mc.c`), the sound driver (989snd replaced by `sound.c`, finding 022), movies (`movie.c`,
  libmpeg2, finding 055).
- The game data image is read at start from the player's `SCUS_971.77` and checked by SHA-1
  (`game_data.c`); the exe holds only the words the port changes (finding 053, section 4).
- Inline asm runs on an emulated EE/FPU/VU0 register file (`port/runtime/ps2asm.c`, plus
  `ps2asm_aot.py`); EE float rounding follows PCSX2's (toward zero, FTZ/DAZ, finding 026,
  `eefloat.py`). VU0 microprograms are native (finding 016); VU1 runs AOT blocks (`vu1_aot.py`,
  MAC/status updates no flag read can see made lazily) and native geometry kernels
  (`vu1_native_clip.py`, `vu1_native_*.h`), checked against PCSX2's interpreter
  (`test_vu1_pcsx2.py`) and on captured run sequences against our interpreter
  (`vu1_seq_diff.py`, x64; finding 090 VU1R).
- GS (`port/gs/`): GS memory and registers in C; the Rust renderer (`port/render`, wgpu: DX12
  or Vulkan, WARP last; WebGPU in the browser) behind the shared C front ends `gpu_exact.inc`
  (exact: the PS2's pixels) and `native_core.inc` (native: upscaled, the default). Glue:
  `gpu_rust.c`, `gpu_rust_native.inc`, `port/web/gpu_rust_web*.{c,inc}`, with what both share in
  `gpu_rust_shared.inc` (findings 085, 086, 088).
- Host services (`port/host`): one code path on SDL3 (`sdl3.py`, static) for Windows, macOS
  (arm64), Linux (x64) and Android (arm64), finding 099 (2026-10-27, branch `desk/integrate`;
  round 2 with the Mac/Linux, fibers, texture pack, settings, glue, test and disc branches on
  `desk/integrate2`, 2026-10-28, checked on all five platforms; round 3 on `desk/integrate3`,
  2026-10-28: Discord Rich Presence (finding 106), DHD_GPU_TIMES, the error box on the main thread,
  the launcher ending on SIGTERM/SIGINT on macOS and Linux, and a pacing queue that never drained:
  a key reached the screen 90 ms after it was pressed on the title, 11 ms now; round 4 on
  `desk/integrate4`, 2026-10-28: online takes the USA disc's game files (dhd_disc, finding 098),
  the rider-select skip at high fps and the launcher's Linux parts from PR #13, Vulkan drivers
  with no conformance (dzn in WSL2); 132 host_api imports):
  - Threads, locks and atomics are SDL3's and C11's (`native_thread.h`; `win32_compat.h` is gone).
  - The OS services are one shared file (`host_sdl.c`: files, folders, clock and frame waits that
    sleep to a millisecond before and then spin, display, threads, the error box), with UTF-8 paths
    everywhere (`host_os.h`; dhd.exe's manifest sets the UTF-8 code page, so the environment the
    launcher passes is UTF-8 too). `host_win32.c` / `host_posix.c` keep only the machine parts:
    guest memory protection, the fault guard, FP control, the exe path and code range, memory
    regions, environment writes, the heartbeat and the exit.
  - SDL's loop is the process's main thread on every platform (`sdl_host.c`: macOS needs Cocoa
    there, and the error box runs there too); the game runs on its own thread (64 MiB stack). One window path (`present.c`): the only
    platform part turns SDL's window into the renderer's window (Win32 HWND, X11 or Wayland,
    Android's ANativeWindow, Apple's CAMetalLayer); wgpu runs DX12 or Vulkan on Windows, Metal on
    Apple, Vulkan on Linux and Android, in RGBA8 or BGRA8, whichever the surface offers
    (`rustgs.py check --target aarch64-apple-darwin`).
  - One sound output, SDL3's audio stream (`sound_out_sdl.c`; WASAPI, Core Audio, PipeWire/ALSA or
    AAudio under it); headless runs open no device. Controllers through SDL only (the XInput and
    raw-HID fallbacks are gone), the keyboard (finding 059), text input.
  - One network client, `dhd_net` in Rust (below). The game has no disc dialog: the launcher picks
    the disc (Android: its document picker, `android_main.c`).
  - dhd.log on every platform when the starter gave no console, terminal, pipe or file: in
    `DHD_DATA_DIR` (the launcher's data folder) when set, else beside the game; Windows copies it
    through a pipe and thread (finding 097), the POSIX hosts through one shared pipe and thread
    (logcat too on Android). Its first line names the release and the build (finding 098). The
    default texture-pack folder follows `DHD_DATA_DIR` as well.
  - Texture packs (finding 057, "One decoder"; branch `desk/texpack`): one `texpack.c` on every
    platform, over the host's folders and files, with the PNGs decoded by the Rust library
    (`png` crate, `dhdr_png_*`); WIC and `texpack_linux.c` are gone.
  The macOS parts follow public PR #6 (by dxdramar: the Metal surface, BGRA8, the main-thread
  loop); they are rebuilt here as shared code, a few `__APPLE__` lines in the POSIX files (the exe
  path, the Mach-O code range, guest RAM without memfd, the fiber switch's symbols, the arm64
  ucontext, thread names and times, the frame wait's time-constraint sleep).
  From public PR #13 (by n0buholic, Linux; branch `fix/pr13-ports`, finding 099 "From PR #13"):
  the high-FPS RIDER SELECTED / shop-flight skip (`patches_fps.py`, `test_rider_skip.py`), the
  launcher's "no update package for this platform" state, its restore on Wayland and Linux
  WebKitGTK defaults, and the game window's app id and icon on Linux and macOS.

### What players get
PC SETTINGS in the game's options, saved to dhd.ini (finding 064): resolution and scale,
borderless/windowed, Hor+ widescreen with the HUD anchored (finding 056), frame rate (60, or real
offline simulation steps at higher rates, finding 067), texture packs in PCSX2's format (finding 057), the sharp
font (finding 058), movies, vibration, EXIT GAME. The launcher (`port/launcher`, finding 093)
holds the settings, the rider name and the updates on every platform; the game has no updater
or start window of its own (2026-10-27). It decides the one data folder for the player's files
(the system's app-data folder for an installed copy: the Windows setup's `installed.txt`, an app
bundle, an AppImage; else the game's folder when it is a plain writable folder, else app data; passed
as `DHD_SETTINGS`, `DHD_MEMCARD_DIR` and `DHD_DATA_DIR`), updates from per-platform assets in
`latest.json` (a macOS update swaps the whole .app, an AppImage the whole file), and checks HTTPS with rustls and bundled
webpki-roots, the game's rule. dhd.ini has one implementation, the Rust crate `dhd_settings`
(`port/render/settings`, finding 101): its keys, defaults, choices, reading rules and line-keeping
saves serve the game (through the Rust static library; PC SETTINGS through a generated table), the
launcher and the browser (the crate's own small wasm module, `build/web/dhd_settings.wasm`;
`settings.js` keeps only IndexedDB and what the browser can't offer; finding 102). The rider
name's rule is the crate's too (the launcher, and the online menu through the same table). Discord Rich Presence (finding
106, setting `discord`, on by default): the profile shows the menus, the race with its track and
place, the career, an online room or race, through Discord's local IPC (`dhd_presence` in the Rust
library, no SDK; the core sends its state through `host_service_presence`). Online races: below. The port's own pages (PC SETTINGS, the exit question, ONLINE and the
race room) share one centred layer in the game's popup style (`menu_page.c`), and the mouse works
in them and in the game's own popups: hover selects, left click X, right click back, the wheel
scrolls or changes values (finding 092; the shell's own menus are not bridged yet).

### Checks and tools
- Texture packs: `python port/tools/test_texpack.py` (synthetic PNGs: PCSX2 names, the folder,
  every PNG kind's pixels and mips, damaged files, budget, a loader/eviction stress).
- Unit tests: `python port/tools/test_runtime.py` (after `build_wasm.py`). Test programs run where
  the game runs their code (`port/tools/testprog.py`, finding 104): the core's C as wasm32 guest
  programs through wasm2c, the host's on the 64-bit host. `abi_check.py --check` audits the game's
  calls as the wasm32 core compiles them.
- Renderer: replay goldens recorded locally (`gs_golden.py record`/`compare`; they hold pictures
  of the game, so they are not in the repository), `replay_gs.py`, `test_gs_wgpu.py`,
  `test_native_transfers.py`, `web_gs_replay.mjs` for the browser, `gs_verdict.py` (the pass rule)
  and `harness_negative.py` (its negative controls).
- Game state: per-field state traces (`port/hle/state_trace.c`, `state_trace_run.py`,
  `state_diff.py`, `trace_columns.py`; `codeptrs_guard.py`), the wasm offset scan
  (`wasm_offset_scan.py`, run by `build_wasm.py`), `test_fps_race.py` for high rates.
- Retail comparisons with PCSX2 (`compare_pcsx2.py`, `race_compare.py`, finding 024), online race
  runs (`online_race.py`), timing (`bench_race.py`,
  `sample_profile.py`); crash and trap reports name RVAs with `host64_rva.py`. The tools start the
  game and its helpers through `owned_process.py` (a job object or process group: the whole tree
  ends with the run, on a timeout, an error or Ctrl+C; finding 103).

### Release steps
Bump `port/include/version.h`, `build_wasm.py`, `package_release.py`, publish the zip and
`latest.json` as `v<version>`, then raise `MIN_VERSION` in
`online/server/wrangler.jsonc` and deploy when older versions should stop racing (finding 054,
section 6; finding 088, CL3). macOS and Linux (finding 093, Release): `package_release.py
--platform mac|linux --game PATH` on that machine (the mac zip is signed there), then
`--merge` its `latest.json` into the Windows one; publish every zip with the one `latest.json`.
Installers (finding 125, after 0.7.5): the same runs also make each platform's installer from the
checked files (`port/tools/installers.py`; `--no-installer` skips it): the per-user Windows setup
(`-win-setup.exe`, Inno Setup 7 via `innosetup.py`, `port/release/installer/windows.iss`), the
macOS DMG (on the Mac) and the Linux AppImage (`-linux-experimental.AppImage`, Tauri's bundle with
WebKitGTK inside; its `linux-appimage` entry goes into latest.json and `--merge` carries it).
Publish them next to the zips; installed copies keep the player's files in the system's app-data
folder and update from the zips (the AppImage from its own entry). None is code-signed.
Since 0.7.0 the mac and linux zips are `-mac-experimental.zip` and `-linux-experimental.zip`
(finding 093, "0.7.0": built and checked on all three, published 2026-10-28; Linux is linked on
Ubuntu 22.04 and runs on glibc 2.35 and later, which `package_release.py` checks).

2026-10-26 (finding 090, SP3): native GS vertex conversion and texture-page key iteration
cost less CPU; one x64 race A/B measured GS busy 4.571 -> 3.807 ms/field.
Exact, native and interpolated replay goldens and all 198 transfer golden items match;
x64 packaging and the Rust browser build/layout pass. Exact/probe paths and game core unchanged.

2026-10-26 (finding 090, SP6): fixed a retired-queue observation race that could wait after
completion; added opt-in per-field cycles and host/GPU stall attribution. One race pair observed p99 73.803 -> 17.763 ms (not an isolated speedup estimate), with unchanged-counter
timeouts 2,443 -> 513. All 4,500 race state-trace fields match with the existing mask; x64 packaging,
Rust browser build/layout and GS FIFO stress pass. Details and the legacy VIF fixture link limit
are in finding 090. No game core or GPU operation changed.

2026-10-26 (finding 090, SP7): x64 core and packaging pass. Two race pairs retain padded queue counters
and the 512-pause retired/full-ring spin policy: 104.548 -> 141.226 and
121.783 -> 124.349 fields/s. EE spin falls in both; second-pair p99 worsens
21.869 -> 24.024 ms. Run-to-run variability prevents an isolated speedup claim.
Full busy/spin/park tables and limitations are in the SP7 section.

2026-10-26 (finding 090, FPS1): at 240 the two counters that stayed per-update now
advance per field behind `port_fps_high()`: the particle/effects frame clock
`D_0029DBAC` (dust, snow and spray waves, glow phases) and the sound/skat queue age
`obj->x60C`; the rate audit (`DHD_RATE_AUDIT`, off by default) now measures them at a quarter
of their 60 Hz travel per update (0.250 and 1.002 against 1.000 and 4.004 before), which is
time-based motion at 240. Boot 600- and race 5000-field state traces against the base build
match in every column with the mask unchanged (3); x64 packaging, Rust browser build and layout
pass.
Measured words, attribution and the audit's false positives are in the FPS1 section.

2026-10-28 (finding 105): the exact renderer no longer runs out of GPU memory while minimised
(0.6.4 died at the menu after 30-60 s, 0xC0000409): each frame waits for the previous frame's
GPU work (`Exact::frame_end`), and an out-of-memory device error now ends with a log line, an
error box and exit code 1. 5 minutes minimised through menus and a race: memory flat.

2026-11-01 (finding 128): online rooms keep Ready across track changes (TRACK CHANGED line), MOSH
only by the host's pick, and the shop's bike, jersey and bike-colour pages online (issue 18).
Server tests 64/64; two local games on hidden desktops: each saw the other's choice in its log.

2026-10-31 (finding 124): PERU no longer logs "vu1: microprogram at 0x1F50 didn't end" (0.7.4,
0.7.5: about a million interpreted VU1 steps in a frame after earlier races). The weather object's
four per-view batch records were never initialised, so a zero-density emitter left an old particle
float there, which the microprogram read as thousands of particles (PCSX2 runs the same input as
long). A `port/patches.py` patch clears them in `func_00226D08`; with stale pool data seeded, a
PERU state trace of the fixed build equals the unfixed build's clean run from the constructor on.
Opt-in VU1 diagnostics: `DHD_VU_CAPTURE_FAILURE`, `DHD_VU_CAPTURE_ENTRY`, `DHD_VU_CAPTURE_EE`.

2026-11-01 (finding 129): every player's dhd.log says how fast the game ran and what held it back
(`port/gs/perf_log.c`, on by default, `DHD_PERF_LOG=0` off): at start the GPUs with their kind and
the one used, the present mode, and a `perf: render` line (scale, picture and window size, rate,
pacing, V-Sync, refresh, GPU timer); every 30 s of race or menus in front, and per kind at exit, a
`perf:` line with fps against the requested, 1% low, frames over 1.2x the budget, each thread's
CPU and busy ms per field, the render thread's wait in the present, the GPU's ms per frame (one
frame in 8 timed by DHD_GPU_TIMES' queries) and `limit=gpu|cpu:<thread>|vsync|none`. The graphics
queue's wait counters now count by default. Boot state trace unchanged; 1x uncapped 243.3 -> 241.1
fields/s (one run each). Windows only built and run; no lines in the browser.

2026-11-01 (finding 130): Windows `DHD_WGPU_ADAPTER=integrated|discrete` selects hardware through
DXGI's power/performance preference order, with the player default unchanged. The Ryzen 5 7600
iGPU baseline holds about 60 fps at 3x; requested higher scales can be clamped by the dedicated
VRAM guard, so `DHD_SCALE_MEMORY_MB` supplies a logged developer estimate for UMA measurements.
Exact bounded integer scanout, duplicated-row reuse and unit-texture pipeline specialisation
reduce 4x uncapped FXC GPU time from 18.81 to 13.84 ms (50.52 to 68.88 fps). Renderer/host GS tests, fixed-stream pixel comparisons and the
boot trace pass. Finding 130 contains the paced/unlocked matrix, Vulkan checks and limitations.

### Open
- Browser speed: about 25-30 fields/s on the title, 17-25 in a race before finding 087's work;
  the game's worker is CPU-bound.
- Finding 067's high-rate (120/144) AI and physics equivalence limits.

## Facts

- `.text` 0x10a000–0x263bc0 (~1.35 MB), stripped. C++ (only 4 TUs with exception info).
- 5,690 functions (splat): game 3,534, engine 1,202, sdk/libc/libm 802, libgcc 147, crt0 5.
- `_gp` = 0x2A40F0. Single PT_LOAD at file offset 0x1000.
- Disc: `SYSTEM.CNF` BOOT2 = `cdrom0:\SCUS_971.77;1`, VER 1.00.

## Environment
Windows (Git Bash or PowerShell); the build runs natively. Toolchain: `python
tools/setup_toolchain.py` (pinned + hashed in `toolchain.lock.json`: SN ProDG 3.01, ps2dev
binutils 2.45.1, SDK 2.4 libs for identification). m2c: clone into `tools/ext/m2c`. The retail ELF
is not in the repo: extract it from your own disc (`7z e "<USA iso>" SCUS_971.77` into `iso/`,
sha256 adfda7b7…f0cf0c).

## Online play
Shipped since 0.1.0; the original plan is `docs/ONLINE-PLAN.md` (historical). The round log is in
findings 039-047, 053-054, 065 and 072 and in this file's history.
- Server: `online/server/` (Cloudflare Workers + Durable Objects) at `wss://dhd.skuza.app`, the
  game's built-in default (dhd.ini `server =` or `DHD_NET_SERVER` override it). Lobbies, join
  codes, the public list, the race relay (10 players per room by default), desync reports, usage stats
  in Workers Analytics Engine. `MIN_VERSION` (`wrangler.jsonc`, now 0.7.0) turns older clients
  away with "update to play online". Protocol: `docs/online-protocol.md`. Steps:
  `online/server/README.md`.
- Client: `port/net/` (lobby client, hashes; `net.h`) over `port/net/ws.h`, the WebSocket and HTTPS
  client of every native build: the Rust crate `dhd_net` (`port/render/net`, tungstenite over rustls
  on ring, Mozilla's roots bundled by webpki-roots; no OS certificate store, no proxy), linked into
  the one Rust library (`dhd_render`'s native build, rustgs.py). The browser has its own WebSocket.
  Races use state sync, not input lockstep (finding 041 has why): each PC runs its own one-player
  race and shows the others, and on guests the host's AI riders, as puppet riders written from
  snapshots (`port/hle/puppet.c`, finding 044). Each player sends all its riders in one message.
  Grid, rider/bike looks, the countdown on the server's clock, hits between humans, finishes and
  results are shared (finding 047); a player who leaves becomes an AI rider.
- In the game's own menus: name and code entry, public list, rider picker (finding 065); restart,
  new level and rematch flows (finding 072). Create/Join waits on ONLINE, then the rider screen
  once and the room over it, with no bounce and no second title flight (finding 092); then the
  shop's own BIKES / JERSEYS / BIKE COLOR pages, every bike and jersey offered, each player's
  choice (bike colour too) worn by their puppet on the other PCs (finding 128).
- The room (finding 128): Ready means "count me in for the next race": a new track or computer
  riders on/off keep it, cheats on/off clear it; ready players see TRACK CHANGED. MOSH PIT (the
  arena) only comes when the host picks it (the server's pickOnly; NEXT / RANDOM skip it).
- The disc: online needs the USA disc's game files (the hash of every ISO 9660 file's path, size
  and contents, `dhd_disc` in port/render/disc, shared by the game and the launcher; redump
  30393's files), so a good dump that differs only outside the files is taken. An image whose
  files differ is turned away on ONLINE with lines saying why and what to do, and dhd.log has both
  hashes and the first different file (finding 098). dhd.log's first line names the release and
  the build.
- Tests: `port/net/build_tests.py` (unit and loopback), the server's own tests (`npm test`),
  `port/tools/online_race.py` (two local games through a relay, optionally `nettool` bots).
