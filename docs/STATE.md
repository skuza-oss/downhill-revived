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
  fields/s at 1x and 14 at 2x and cannot hold 60. `port/tools/phone_throttle.py --apply t612`
  makes the rooted Mi 11i match it at 1x paced 60 (39.6 fields/s, same thread split; `--restore`
  puts the phone back), with render scale ~2x/~5-6x standing in for the Mali's 1x/2x. On that
  profile two pacing changes and a GS fast path took 1x paced 60 from 39.6 to 55.0 fields/s
  (median field 16.65 ms); finding 127 has the breakdown and what is left for 60. A gamepad or the
  keyboard, or the touch controls (finding 141, branch `feat/touch-controls`; built and unit-tested,
  in-game checks pending): Thumb zones (floating stick, PEDAL with flick attacks, HOP / BRAKE / SLIDE,
  a trick stick in the air) and Classic pad, every touch plain pad input; the rules in
  `port/hle/touch.c`, drawn by the Rust renderer's overlay, the settings on SETTINGS > Touch Controls;
  the game is read for the rider's air flag and the race HUD hook only. `DHD_TOUCH_MOUSE=1` makes the
  mouse a finger on a PC. Online uses the same
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
borderless/windowed, Hor+ widescreen with the HUD anchored (finding 056), frame rate (a cap: the
game steps by the real time since the last frame at every rate, findings 067, 139), texture packs in PCSX2's format (finding 057), the sharp
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
  (`wasm_offset_scan.py`, run by `build_wasm.py`), `test_fps_race.py` for high rates,
  `rate_drift.py` (the race at 30-240 steps a second against 60) and `slow_race.py` (a slow PC
  made with `DHD_SLOW_FRAME`: game speed, bursts and sound; finding 139).
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

2026-11-02 (finding 132): the online room names tracks as the game does (retail's event names
read from the player's data image, e.g. "Mt. Midoule FR", "Mosh Pit (Arena)") everywhere it
shows one, and the track picker shows the selected track's loading-page picture, read from the
player's disc at run time (nothing in the repository; no generated art needed). Checked on the
hidden desktop: all 23 picker views, a race afterwards; the lead's checks are in the finding.

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
iGPU holds the nominal 60 fps target at 4x, up from baseline 3x. The dedicated-VRAM guard can
clamp high requests; `DHD_SCALE_MEMORY_MB` supplies a logged developer estimate for UMA measurements.
Exact bounded integer scanout, duplicated-row reuse and power-of-two texture pipeline specialisation
reduce 4x uncapped FXC GPU time from 18.81 to 11.73 ms (50.52 to 78.48 fps, fresh controls).
The full matrix's true 8x improves from 13.79 to 22.19 fps, GPU 71.80 to 43.81 ms; 5x/6x/8x
remain below 60. Final Vulkan is slower at 2x and effectively tied at 8x. Renderer/host GS tests,
oracle replays, strict fixed-stream 8x comparisons on both GPUs/APIs and boot traces pass.
Finding 130 contains the final paced/unlocked tables, 1% lows and pass splits; wall-time stalls
remain, and the RTX-connected monitor's transport cost is not isolated. Player defaults unchanged.

2026-11-02 (finding 133, issue #19): Mt. Liddellroch's ambience is no longer too loud. 989snd's END
grain (type 24) stops its sound and releases its voices (retail IOP 0x5A60 -> 0xFE04); the port only
ended the program, so `E_TREE_WHOOSH`'s wind loop, started at every tree passed, never stopped (35
copies at once a minute into the race; PCSX2: 5). Sound records' solo flag and instance limits
(byte +5, flags 8/0x10/0x20; 0x4A34, 0xF34C, 0x10038) now apply as in retail. Against a PCSX2
recording of the same race (a capture hook in the comparison adapter): Liddellroch FR race 0-60 s
-24.71 dBFS PCSX2, -22.54 before, -25.18 after; ambient voice power +13.3 dB -> +3.4 dB (most of
the rest is samples shared with rider sounds); Mt. Midoule FR -25.03 PCSX2, -24.77 before, -25.02.
`DHD_SND_VOICE_TRACE` dumps each sounding voice for such comparisons. State traces: the END fix
changes only the PCM column; with the limits the race trace's RAM also differs, in the sound queue
and handle words only (the game keeps the refused plays' replies).

2026-11-02 (finding 135): rooms are made near their creator and everyone can see where they are.
`/v1/rooms/new` gives the room's Durable Object a location hint from the creator's continent and
country (`online/server/src/region.ts`), the room learns its own colo from a Cloudflare trace, the
welcome and a new `where` message say where the player and the room are, and the public list
carries each room's region in the asker's region's order (0.7.6's Quick Race then prefers a near
room too). dhd.log gets `net:` lines: on joining (this PC's Cloudflare location, country, the
room's location), every 30 s in a room or race and on leaving (own ping median and worst, every
other player's ping and region; another player's country is never sent). The list rows and the room's header show the region
(`PLAYERS 2/10 - EU SERVER`). The list's ENDS IN / NEXT IN / RACING times no longer count from the
epoch before a room is joined (0.7.6 showed ENDS IN 29893031:32): the list carries the server's
`now`. Server tests 73/73; two games on hidden desktops logged every line.

2026-11-02 (finding 136): the resolution scale the GPU allows is no longer hidden. dhd.log has
one line with the scale in use, `capped` when it is under the one asked, the limit and its reason
with the figures (`resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (graphics
memory: ...)`), Match window too. PC SETTINGS offers every scale and marks the ones over the
limit (`8x (running 4x: GPU memory)`, footer `This GPU's memory allows up to 4x`); the launcher
reads the last run's line from dhd.log and says the same under Resolution (`Your GPU's memory
allows up to 4x; 8x runs at 4x`, choices `8x (runs at 4x)`). The cap's rule is unchanged. Boot
state trace unchanged.

2026-11-02 (finding 134, issues #20 and #27): controllers. A stick dead zone setting
(`stick_dead_zone`: Game, the PS2's own, about 36% of the travel, by default; or 5-50%, the travel
past it spread over the bytes that steer in proportion, `port/hle/stick.h`), in PC SETTINGS and a
new Controls tab in the launcher with a live picture of the controller's sticks and what the rider
gets. Button icons (`prompts`: Auto from player 1's controller, PlayStation, Xbox, Nintendo): the
game's face-button icons drawn as our own dark discs with the letter in the game's font, L1/R1/L2/R2
as LB/RB/LT/RT (L/R/ZL/ZR), at draw time in the text packet (`port/hle/prompts.c`). `DownhillRevived
--play` starts the game at once without the launcher's window (Steam shortcuts, the Steam Deck).
Boot state trace with default settings equal to next/0.7.7's.

2026-11-02 (finding 131): the native renderer's draw pass from first principles. A diagnostic,
`DHD_GPU_STATS`, gives GPU time and pixels shaded per kind of draw: a race frame shades about four
screens of pixels, two of them one blended terrain kind, at three times the cost per pixel of a
plain fill. Each draw now gets a pipeline specialised for its state (built on a worker thread; the
generic one draws until it is ready), texel bounds tests stay but cost what they must, and decoded
textures are R32Uint texels read by one gather per bilinear footprint. The Ryzen 5 7600 iGPU now
holds 60 fps paced at 5x (was 4x); GPU time per frame is 22-24% lower at every scale (4x uncapped
76.5 -> 102.6 fps, 12.02 -> 9.11 ms; 8x 21.5 -> 28.0 fps, 45.13 -> 35.30 ms; DXC alike). The RTX at
8x is unchanged (CPU-bound; GPU 1.92 -> 1.68 ms). Oracle goldens, the replay matrix on both GPUs
and APIs, `test_gs_wgpu.py` and the native-transfer tests pass; VRAM is identical everywhere. One
picture change remains for a decision: a `round()` that makes the generic and specialised shaders
agree draws some pixels one step differently on DX12 than before (none on Vulkan; DXC up to 0.6% of
a menu picture's pixels at 4x, about 0.003% of race pixels). The AMD iGPU varies a few pixels from
run to run (also before; no application race found, finding 131 lists the checks).

2026-11-02 (finding 137, issue #30): no more clones in online rooms. A game whose connection
dropped rejoins with the same hello; the server gave a member back by name only when that member
had no socket, but with a dead connection its old socket stays open on the server for minutes, so
the rejoin became a second member (and a host's rights stayed with the dead copy). Now a hello
takes back the same-named member whose socket has been quiet 4 s (waiting up to that when it spoke
just before), a 1006 drop outside a race keeps the member 20 s for its rejoin, and a socket silent
20 s is dropped. Server only: deploying it fixes 0.7.7. Server tests 81/81.

2026-11-02 (finding 138): the launcher's update check has a backup. When GitHub's releases API fails
(no connection to api.github.com, its 60-an-hour limit, 5xx, a bad answer), the launcher asks the
online server's new `GET /v1/latest`. That endpoint serves the newest release's `latest.json`,
fetched from GitHub's release download, kept fresh for 5 minutes, with the last good copy kept for
when GitHub fails, and checked to name only this repository's release assets. No release step.
A player's own `update_url` works as before, without a backup. The bottom bar now says what went
wrong in one line ("No internet connection.", "GitHub is limiting update checks from your
connection; try again in an hour.", "Couldn't reach GitHub or the backup server (your connection
timed out)...", "Update failed: the download couldn't reach GitHub (...)"). The full errors go to
the new `launcher.log`. Server tests 88/88, launcher tests 27 pass. The endpoint needs a server
deploy.

2026-11-02 (finding 139): one delta-time game clock. Every paced frame, at every frame rate (60
included) and online, the game steps by the real time since the last frame, between one step at
the selected rate and two fields, smoothed so the steps add up to wall time; no debt, no
fast-forward. It replaces the fixed 1/60 step at 60 (slow motion below 60 fps), the one-field cap
above 60 with its repaid debt (the speed ramps), the online extra fields and `DHD_PACE=wall`. The
mixer gets wall time, so the sound keeps its speed too. Slow PC made on the RTX desktop
(`DHD_SLOW_FRAME`): at 45 fps the game went from x0.75 to x0.999 at FPS 60 and 120, the worst
second at 85 fps from x0.87-x1.13 to x0.97-x1.04, the sound's time-stretching from 57 to 5-6 s a
minute. Two-field steps broke 12 of finding 067's per-frame counters (the post-race countdown never
ended, the action lock stuck, ...) and made riders "airborne" on every bump (at 30 steps a second
nobody finished the scripted race): fixed. The race at 30-144 steps a second now finishes with no
trend against 60 (player -7..+6%, AI mean -0.5..+4.1%: the race's own chaos, finding 067).
Deterministic runs keep the original clock: the state trace is unchanged. V-Sync keeps three swapchain buffers, so a missed refresh no longer halves 120 to 60.
Ghosts/replays stay off on the delta-time clock, now at 60 too (open).

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
  in Workers Analytics Engine. New rooms take a location hint from their creator's region, and
  the welcome, a `where` message and the list say where rooms and players are (finding 135).
  `MIN_VERSION` (`wrangler.jsonc`, now 0.7.0) turns older clients
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
