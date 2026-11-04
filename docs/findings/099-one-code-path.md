# 099: one code path for Windows, macOS, Linux and Android

Status: current
Supersedes: parts of 022, 059, 065, 082
Checked: 2026-11-04

2026-10-27, branch `desk/integrate`: six lanes (sync, host, audio, net, present, release) each moved
part of the port onto shared code, and this merge puts them together with `fix/online-player-report`
(finding 098). The aim: as little platform code as possible, and no old path kept beside a new one.
The macOS parts follow public PR #6 (by dxdramar: the Metal surface, BGRA8, the main-thread loop),
rebuilt as shared code rather than merged as a separate port.

## What is shared now

| Part | One implementation | Deleted |
|---|---|---|
| Threads, locks, atomics | SDL3 + C11 (`port/gs/native_thread.h`, the units' own SDL calls) | `port/host/win32_compat.h` (375 lines) |
| OS services | `port/host/host_sdl.c` (464 lines): files, folders, clock, frame waits, display, threads, the error box; UTF-8 paths (`host_os.h`) | most of `host_win32.c` (531 -> 166 lines) and `host_posix.c` (766 -> 296) |
| Sound | `port/host/sound_out_sdl.c` (SDL3 audio stream; headless opens no device) | `sound_out_win32.c` (389), `sound_out_common.inc` (138), `sound_out_null.c` (57) |
| Controllers | SDL3 (`port/hle/gamepad.c`) | the XInput and raw-HID (DualSense/DS4) fallback in `host.c`, gamepad.c's no-SDL stub |
| Network | `dhd_net` in Rust (`port/render/net`, 536 lines: tungstenite, rustls on ring, webpki-roots), behind `port/net/ws.h` | `ws_winhttp.c` (293), `ws_winhttp.h` (31), `ws_posix.c` (653) |
| Window and loop | SDL on the main thread (`sdl_host.c`), the game on its own 64 MiB thread, one SDL window init in `present.c` | per-platform window code and the separate SDL thread |
| Texture packs | `texpack.c` over the host's folders and files, PNGs decoded by the Rust library (`png` crate, `dhdr_png_*`; finding 057, `desk/texpack`) | WIC, the Win32 listing and `wchar_t` paths in `texpack.c`; `texpack_linux.c` (94) |
| Disc choice | the launcher (Android: its document picker) | the game's file-open dialog (`comdlg32`) |
| The log | `host_console_init` in `port/hle/host.c`: one rule for where dhd.log goes | `android_main.c`'s own log pipe |
| TLS roots | bundled webpki-roots 1.0.11 in the game and the launcher | the launcher's `tls.rs` and rustls-platform-verifier (with schannel, security-framework, rustls-native-certs, openssl-probe) |
| Build switches | SDL3 linked and its headers on every unit, every build | `DHD_HAVE_SDL`, `SDL_UNITS`, `DHD_SOUND`, `--sdl` |

From `bab47b24c`: 2,071 lines in deleted files; platform `#if`s (`_WIN32`, `__ANDROID__`,
`__APPLE__`, `__linux__`, `_MSC_VER`) in `port/{hle,gs,net,host,runtime}` 91 -> 64; `host.c`'s
`#if`s 45 -> 28, `present.c`'s 21 -> 6.

## What stays per platform, and why

- `host_win32.c` / `host_posix.c`: guest memory protection and the access-fault guard (VirtualAlloc
  and vectored exceptions, mmap and signals), the FP control word (MXCSR, arm64's FPCR), the exe
  path and code range, memory regions, environment writes, the heartbeat, the exit. SDL has none of
  these.
- `present.c`'s `window_target`: SDL's window as the renderer's native handle (HWND, X11 or
  Wayland, ANativeWindow, CAMetalLayer). wgpu needs the raw handle.
- The log copier: Windows keeps its named pipe, overlapped reads and the TLS callback that writes
  the last lines in ExitProcess (finding 097, shipped in 0.6.4); the POSIX hosts share one pipe and
  thread (logcat on Android). Where the log goes is one rule for all: a console, terminal, pipe or
  file the starter gave keeps it; else `DHD_DATA_DIR/dhd.log`, else beside the game.
- `android_main.c`: the Android start (the app's files folder stands for the exe's, the disc copy
  through the document picker, the SDL hints).
- dhd.exe's manifest (`port/res/dhd.manifest`): the UTF-8 code page. Windows hands a program its
  environment in the ANSI code page, so the UTF-8 host could not open a non-ASCII `DHD_DATA_DIR`,
  `DHD_ISO` or `DHD_SETTINGS` (a probe with `DHD_DATA_DIR=...\data-Zürich é`: `ü` arrived as CP1252
  `FC`; with the manifest, ACP 65001 and `C3 BC`).

## Fixes made while merging

- `movie.c` reads the disc through `host_os_fopen` (UTF-8) and `host_fseek` (64-bit), no 1 GiB
  seek steps.
- `build_host64.py` builds sdl3.py's SDL3 before it compiles the host (a fresh tree has no
  headers yet).
- `build_host64_linux.py` links with lld: GNU ld on a tree under `/mnt/<drive>` (WSL's 9p) ran for
  minutes past the step's 300 s timeout; lld links the same objects in 2.4 s there (0.9 s into
  `/tmp`).
- `mc.c`'s backslash card paths already go through `host_sdl.c`'s slash rule (every `host_file_*`,
  `host_path_*`, `host_dir_*` call), so the card works on POSIX unchanged.
- The tests: `test_loopback.c` includes `windows.h` itself; the async-services harness gives
  `host_sdl.c` a plain `host_os_fopen`.

## Checks (2026-10-27)

- Windows x64: `build_host64.py` builds; `package_release.py` passes all its checks with the
  launcher built from this branch (zip 21.6 MB; dhd.exe imports Windows DLLs only, no WinHTTP or
  comdlg32). dhd.exe 26,373,120 bytes against 0.6.4's 25,009,152 (+1.36 MB: rustls, ring and
  tungstenite in the Rust library); DownhillRevived.exe 4,801,024 against 3,904,512 (rustls with
  bundled roots instead of the system's TLS).
- Android: `build_host64_android.py` builds `libmain.so` (27.1 MB) and `libSDL3.so`.
- Linux x64 (WSL): `build_host64_linux.py` compiles and links with SDL3 and the Rust library
  (47.6 MB) in 3 min 44 s from `/mnt/<drive>`; a 25 s headless run boots without a fault.
- `port/net/build_tests.py`: test_hash, test_json, test_loopback pass; nettool builds.
  `port/tools/test_async_services.py`: RESULT PASS. Launcher `cargo test`: 8 pass, and the live
  GitHub source with `--ignored`.
- Windows runs, minimised, started without std handles as the launcher does: dhd.log appears in
  a non-ASCII `DHD_DATA_DIR` with the version line first; SDL audio on WASAPI, 0 underruns in a
  900-field title window and a 600-field race window (59.94 fields/s); a DualSense is found
  through SDL; a race through `DHD_NAV`. nettool (dhd_net) reaches `wss://dhd.skuza.app` and lists
  the public lobbies.

## macOS and Linux (branch `desk/macos-linux`)

One build script, `port/tools/build_host64.py --target win-x64|linux-x64|macos-arm64|android-arm64`,
replaces `build_host64_linux.py` and `build_host64_android.py`: a table of what differs per target
(compiler flags, linker, SDL3's build, the Rust triple, the output), the same units and defines
everywhere (guard-page bounds checks, the plain call-depth global, exact f32 literals). `gpu_null.c`
is deleted: Linux links the Rust renderer (Vulkan on X11 or Wayland) like every other target.
`sdl3.py --posix` builds the static SDL3 on Linux and macOS and reads its link line from `sdl3.pc`;
`rustgs.py` builds `aarch64-apple-darwin` on a Mac; `build_launcher.py` and `package_release.py` run
there (`--platform mac` signs the app ad hoc).

The Mac code is 61 lines inside `__APPLE__` branches of the POSIX files (6 before), plus the
target's entries in the Python tools:

- `host_posix.c`: the exe path (`_NSGetExecutablePath`), the code range (`getsegmentdata`), and the
  frame wait's sleep. Plain `nanosleep` on macOS woke 4.6 ms late on average (8.2 ms worst) under
  timer coalescing; inside a time-constraint thread policy (16 ms period, 0.5 ms computation) 0.020 ms
  (0.043 ms worst), so `host_os_sleep_ns` sets the policy around the sleep. Elsewhere it is
  `SDL_DelayNS`.
- `guest_memory_posix.c`: guest RAM is a `shm_open` object, unlinked at once (no `memfd_create`).
- `fiber_arm64.c` (then `fiber_posix.c`): Mach-O symbol names (a leading `_`, no `.type`).
- `host.c`: the arm64 `ucontext` (`uc_mcontext->__ss`), `pthread_setname_np(name)`, thread CPU times
  from Mach's `thread_info`.
- `present.c` (all platforms): the render thread asked SDL for the window's pixel size each frame,
  an AppKit call Cocoa allows only on the main thread. An SDL event watch on the main thread now
  stores it in an atomic. Xcode's Main Thread Checker finds no issue in a 30 s run.

Page size: the guest's layout uses 16 KiB pages on every platform (`HOST64_PAGE`: commit rounding,
the null page, the EE stack guards), so it is the same on Windows (4 KiB OS pages), Linux and
Android (4 or 16 KiB) and macOS (16 KiB). Only `host_mem_protect` uses the OS page
(`sysconf(_SC_PAGESIZE)`); graphics.c's write tracking still guards 4 KiB, and on a larger OS page
the POSIX fault guard offers the fault for each 4 KiB part of it. The Windows fake-module fixture
(`--fake --run`) passes with the 16 KiB layout (selftest, 11 boundary negatives).

### Fibers: one module (branch `desk/fibers`, 2026-10-28)

The EE kernel's threads are fibers of one OS thread. `fiber_x64.c` (Windows fibers) and
`fiber_posix.c` (an asm switch for System V x64 and AArch64) each carried the whole lifecycle; they
are replaced by one module and three small switches:

- `fiber.c` (all platforms): the 64 slots; the guest stacks (256 KiB at `0x06000000`, the lowest
  16 KiB guest page of each reserved); the native stacks (8 MiB above one OS page of guard, the OS's
  own page from `GetSystemInfo` / `sysconf`, mapped and protected with `VirtualAlloc` /
  `VirtualProtect` or `mmap` / `mprotect`: the only `_WIN32` lines); creation, deletion, the root
  (the thread's own stack), the per-fiber `__stack_pointer` and call depth, `host_stack_ranges`,
  and the owner-thread check (a `_Thread_local` flag). Callback and posted-job fibers are ordinary
  fibers of it (`gen_host64_imports.py` is unchanged).
- `fiber_switch.h`: what an adapter provides. `fiber_switch_asm(&save, resume)` saves the ABI's
  callee-saved registers and the FP control register in one frame on the running stack;
  `fiber_frame(lo, hi, arg)` lays out a new fiber's first frame, which returns into a `fiber_start`
  stub that calls `fiber_entry(arg)` with a zero return address above it (stack walks end there).
- `fiber_win64.c` (Windows x64): rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15, MXCSR, and the TEB's
  StackBase, StackLimit and DeallocationStack (`gs:0x08`, `gs:0x10`, `gs:0x1478`, the fields
  Boost.Context's Windows switch swaps too) in a 264-byte frame; SEH unwind data for the switch and
  the stub. `fiber_sysv64.c` (Linux x64): rbx, rbp, r12-r15, MXCSR. `fiber_arm64.c` (Android,
  Linux, macOS): x19-x30, d8-d15, FPCR; ELF or Mach-O symbols.
- `build_host64.py`'s TARGETS name the adapter (`win64`, `sysv64`, `arm64`); `glue()` links
  `fiber.c` plus that one.

Windows uses the hand-written switch too, not `SwitchToFiber`. With it, Windows needs no code path
of its own for stacks, guards, the root or deletion (no `ConvertThreadToFiber`, no `CreateFiberEx`
stack whose size and guard the OS picks), and the switch does less: about 50 moves, no call into
kernelbase, no fiber-local storage or activation-context swap (the game uses neither). What the OS
fibers did that the game needs is the TEB's stack bounds: `__chkstk` probes down from StackLimit,
and the exception dispatcher, `RtlCaptureStackBackTrace` and the trap and fault reporters' walks
accept only frames inside them, so the switch carries them per fiber. The MXCSR save replaces
`FIBER_FLAG_FLOAT_SWITCH`. One difference: a native stack overflow on Windows now hits a no-access
page, not Windows' growable guard with its reserve, so it ends the process as an access violation
without the reporter's lines; the wasm call-depth limit (512) stops guest recursion long before 8
MiB.

The 16 KiB EE stack guard broke `selftest_host.c`'s check that `0x06001000` is committed (the
selftest failed on `desk/macos-linux`); it now checks the bytes either side of `HOST64_PAGE`.

`selftest_fiber.c` (`build_host64.py [--target T] --fiber-test`, the target's compiler and switch,
no module) checks 150 switches between three fibers and the root: ten integers and ten doubles live
across each switch, different in every fiber, so a missing register restore fails; the FP control
register, `__stack_pointer` and call depth; on Windows the TEB bounds (`NT_TIB`,
`GetCurrentThreadStackLimits`); a fiber made and deleted by a fiber; returning fibers; a 256 KiB
frame; a backtrace from a fiber; the 64 slots, reuse and stack ranges. Two children must fault: a
write to a slot's guest guard page (the reporter names `0x006000000`) and a write down the native
stack into its guard. Dropping the restore of xmm6, xmm15, rdi, r12, rbx or MXCSR (win64), r12, rbx
or MXCSR (sysv64), d8/d9, x19/x20 or FPCR (arm64) makes it fail.

Runs (2026-10-28):

- Windows x64: `host64_selftest.py --negative-controls` (selftest, 3 guard reads, the mirror, sp and
  depth negative controls), `--fiber-test`, `--fake --run` (module exit 0, 11 boundary negatives).
  `dhd64.exe` headless (`DHD_HEADLESS=1`, `DHD_FRAME_CLOCK=1`, race_compare.py's `NAV`): title,
  menus, the race's countdown at field 2954 and GO at 3214, then racing to race clock 30040 in
  240 s with no trap or fault.
- Linux x64 in WSL: `host64_selftest.py --linux --negative-controls`, `--fiber-test` (faults are
  SIGSEGV), the build (38.7 MB), a 25 s headless run with the same nav: GO at field 3214, no fault.
- macOS 27.2 on the M1: `--fiber-test` (faults arrive as SIGBUS), the build (32.2 MB), a 30 s
  headless run: GO at field 3214, no fault.
- Android arm64: `--fiber-test` builds, and so does the game (`libmain.so` 27.1 MB); no device was attached to
  run them.

### Runs (2026-10-27)

- macOS 27.2 on an M1 MacBook Air (8 GB), `build_host64.py --target macos-arm64` with 4 jobs:
  `dhd64` 32.2 MB, the -mac.zip 13.6 MB (`Downhill Revived.app`, `codesign --verify --deep
  --strict` valid, Gatekeeper rejects it as an unnotarised app, hence README's Open Anyway). The
  app's `dhd` run 32 s: `989snd: SDL audio output started (48000 Hz stereo PCM, coreaudio, ...)`,
  `gs: SDL window 896x672 (video cocoa)`, `gs: renderer dhd_render 1.0 wgpu 29.0.3 ... on Apple
  M1`, `gs: WebGPU presentation (wgpu, window kind 6)`. The movies play and the title screen
  draws correctly (DHD_SCREENSHOT; `screencapture` over SSH has no Screen Recording permission).
- The title screen is slow on the M1: 360 fields in 30 s (12 fields/s) at scale 2 and at scale 1.
  The GPU is 97-100% busy (`ioreg` IOAccelerator utilisation) and the game thread waits in
  `-[CAMetalLayer nextDrawable]` 80% of the time (`sample`). The cost is in the renderer's GPU
  work, not the pixel count; the movies run at full speed. (2026-10-28: this was the exact
  renderer, not the player's; see "The M1's speed" below.)
- Linux x64 in WSL (Ubuntu, `/mnt/<drive>`): `build_host64.py --target linux-x64` 38.7 MB. A 25 s
  `DHD_HEADLESS=1` run boots without a fault. A WSLg window: `gs: SDL window 896x672 (video x11)`,
  wgpu Vulkan on llvmpipe (no GPU in this WSL), SDL audio on PulseAudio, the title screen draws
  correctly (slowly, in software). `package_release.py --platform linux` (the launcher built in
  WSL with WebKitGTK 4.1): the -linux.zip 14.6 MB, `dhd` needing only libc, libm, libgcc_s and
  the loader; the unzipped launcher opens in WSLg.

### The M1's speed (2026-10-28, branch `desk/mac-speed`)

The 12 fields/s above were the **exact** renderer. That run started `dhd` directly with `DHD_ISO`
and no dhd.ini, and without a dhd.ini the game keeps the tools' defaults (`host_settings_load`):
`DHD_RENDER` unset, so the exact GS draws every field (raster.wgsl: one 8 x 8 workgroup per touched
tile, each pixel walking the tile's primitives in order with software binary64 depth and
compare-exchange stores). That explains the busy GPU, and why the scale changed nothing: the exact
renderer draws at the PS2's resolution and ignores DHD_SCALE. A player never gets it: the launcher
writes a dhd.ini before the first start (`ini::start`), which turns on the player defaults (native
renderer, scale 2, borderless at the monitor's size, widescreen, V-Sync). The game's settings screen
offers no renderer choice either.

Measured on the M1 Air (macOS 27.2, 8 GB, wgpu on Metal), one game at a time, fields/s from
`DHD_NATIVE_TRACE`'s field times in 300-field windows:

| run | renderer, picture | title / menus | race (from GO) |
|---|---|---|---|
| no dhd.ini (finding 099's run) | exact, 640 x 448 | 14.5 (DHD_BENCH 100-280: p50 85.9 ms, p99 118.7 ms) | not run |
| `DHD_RENDER=native DHD_SCALE=2`, 896 x 672 window | native, 2x | 59.9 | 59.8-60.0 |
| the launcher's dhd.ini (player defaults, `window = windowed` instead of borderless), 1440 x 900 pt Retina window | native, 2x, widescreen, V-Sync | 59.5-60.0 | 59.8-60.0 |

In the player's setup the GPU is 51-64% busy in the race (`ioreg` every 5 s), against 99-100% with
the exact renderer. Instruments can't record over SSH (`xctrace` fails with "Error connecting to
DTServiceHub", launched or attached), so the renderer measures its own passes: `DHD_GPU_TIMES=N`
(`native/pipelines.rs` `gputimes`, off unless set) asks for wgpu's TIMESTAMP_QUERY, writes a
timestamp at the start and end of every draw, surface, conversion, scanout and present pass, and
logs the mean every N presented frames. Player setup, 300-frame windows:

| scene | frame span (first pass start to last pass end) | draw passes a frame | conversions a frame |
|---|---|---|---|
| title | 10.6 ms | 2.3 | 1.0 |
| menus | 5.3-13.4 ms | 2.8-25.8 | 0-21.8 |
| race | 6.5-8.5 ms | 16.7-21.3 | 4.9-10.0 |

On Apple's tile-based GPU the timestamps come at pass boundaries and consecutive passes overlap (one
pass's vertex work runs beside the last one's fragment work), so the per-kind sums are larger than
the span (draw passes 40-48 ms "a frame" in the race against a 6.5-8.5 ms span) and only the span is
a frame's GPU time. Scanout is 0.3-2.9 ms and present 0.5-3.0 ms of overlapped time.

So nothing in the native renderer needed changing for 60 fields/s on the M1 Air: it has about half
its GPU time to spare at the player's settings. Public PR #6's 59.9 fields/s on an M1 Pro was the
native renderer too (its notes: "native renderer, 2x windowed and 3x fullscreen"). The exact
renderer stays as it is (it is the referee; no player path uses it); on this GPU it does about 69 ms
of work a field on the title.

Windows with the timer in (off): the GS replays of `gs-g+3600.gs` (exact, native, interpolated) and
`test_native_transfers.py` (plain, `--stress`, `--presented`) give 450 of 450 files byte-identical
to b10e2c747's. `bench_race.py --minimized --mode flat`, native at 2x, one run each on a shared PC:
menu 174.3 fields/s before, 175.4 after; race 216.2 before, 232.7 after (noise).

## Integration round 2 (branch `desk/integrate2`, 2026-10-28)

`desk/integrate` plus main (finding 100), `desk/macos-linux` and `desk/fibers`,
`desk/launcher-disc`, `desk/texpack`, `desk/settings` and `desk/settings-web`, `desk/glue-runner`,
`desk/tests64` and `fix/exact-oom`. The new findings were renumbered: 101 dhd_settings, 102 settings
in the browser, 103 GS glue and process runner (was 101), 104 tests on the shipped paths (was 101),
105 the exact renderer minimised (was 102). While merging:

- `build_host64.py` builds `texpack.c` on every target (the POSIX swap to `texpack_linux.c` went
  with that file); texpack's edits to the two deleted build scripts are in the table that way.
- host.c's dhd.ini template and save are gone (the crate's header and save), so texpack's new
  template words live in the crate's table comment; the launcher's online disc check reads `iso`
  through `dhd_settings` (launcher-disc still had the deleted `ini::read`).
- `package_release.py`: in the launcher, dhd_settings' constants follow the official address
  (`wss://dhd.skuza.appnamedhd...`), which the address check read as another server. A host that
  is the official one plus letters (no dot, no port) is the official one now.

Checks (2026-10-28): the core rebuilt (`build_wasm.py`: 132 host_api imports, the same set as the
branches' cores; sha256 `8914a50f...de50`). Windows x64: `build_host64.py` links;
`package_release.py --platform win` passes with this branch's launcher; `host64_selftest.py
--negative-controls`, `--fiber-test`, `test_runtime.py`, the launcher's and `dhd_settings`' `cargo
test` (11 + 1 ignored; corpus 8, name, header, web), `test_texpack.py`, `test_owned_process.py` (8,
the game too). Minimised runs: SDL audio (WASAPI) with 0 underruns on the title, menus and a minute
of racing, one of 80 ms at the race load; the race's countdown at field 2954; nettool lists the
official server's lobbies; PC SETTINGS on a dhd.ini with a byte order mark, CRLF, sections,
comments, an unknown key and no final line ending changed only `scale = 2 ; two` to `scale = 3`; the
exact renderer minimised 130 s in a race, private memory 824-833 MB (flat after 40 s). Linux x64
(WSL): the build (39.0 MB), 25 s headless to the countdown with no fault, `package_release.py
--platform linux` (15.0 MB). Android arm64: `libmain.so` 27.3 MB. macOS (M1): `--fiber-test`, the
build (32.3 MB), the launcher and `--platform mac` (14.1 MB, ad-hoc signed), 30 s headless to the
countdown with no fault. Browser: `port/web/build.py`, `check_web_layout.py --require-renderer`,
`check_settings.mjs` (8 corpus cases).

## Integration round 3 (branch `desk/integrate3`, 2026-10-28)

`desk/integrate2` plus `desk/discord` (Discord Rich Presence, its finding renumbered 102 -> 106 on
its tip first) and `desk/mac-speed` (`DHD_GPU_TIMES`, "The M1's speed" above). While merging:

- `port/render/Cargo.toml`: `native` enables `png` (texture packs) and `dhd_presence`; Cargo.lock
  holds both sides' packages (memchr and miniz_oxide) and passes `--locked`.
- `package_release.py`: both branches had fixed the official address running into dhd_settings'
  next constant; round 2's rule is kept.
- The core imports `host_service_presence` (132 -> 133 host_api imports). Every host provides it:
  the native hosts share `host.c` (Windows, Linux, macOS; Android's `dhd_presence_start` returns 0,
  no Discord there), the browser's `worker.js` and `run_headless.mjs` list it among their no-ops.
- `scan_retail.py`: the Linux package refused `dhd` for 73 bytes at PS2 0x281D47, zero fill and
  `0123456789abcdef`: serde_json's digits (dhd_presence) after zero padding, and the start of the
  game's C library digit string. A public table (`PUBLIC`, its exact extent), not game data.

Fixes:

- The error box runs on the main thread (`host_sdl.c` through `sdl_host_run`). On macOS a game with
  no disc image stayed up with no box until killed: the game thread asked SDL for the box, Cocoa's
  box waits for the main queue, and the main thread (SDL's event loop, no video yet: a sleep, no
  run loop) never ran it; SIGTERM only queued a quit SDL's loop handed to a game stuck in the box.
  On Linux (WSLg) the no-adapter box from the game thread, with the X11 video on the main thread,
  ignored Return and the game stayed up. Now: Windows, both boxes (no disc; no adapter, from
  `DHD_WGPU_BACKEND=vulkan DHD_WGPU_ADAPTER=fallback`) end the game 0.02 s after OK; Linux 0.10 s
  (no adapter) and 0.009 s (no disc) after Return, and at once with no display; macOS shows the box
  from `-[NSAlert runModal]` on the main thread with the game thread waiting in `sdl_host_run`
  (OK can't be pressed over SSH: System Events keystrokes aren't allowed).
- The launcher ends on SIGTERM and SIGINT on every Unix through Tauri's exit, with 143 and 130
  (`quit_signals` in main.rs: a handler writing to a pipe, a thread that ends the event loop); a
  signal the launcher was started with ignored stays ignored. Linux: 143 in 0.03-0.06 s, 130 in
  0.06 s; macOS: 143 in 0.01 s, 130 in 0.02 s. Round 2's Mac launcher also ended on SIGTERM here
  (0.08-0.11 s, started directly or through `open`), by the signal's default action: the reported
  hang wasn't reproduced; it may have been the stray game above.
- Input lag: just-in-time pacing never emptied the queue the start leaves before the presenter;
  a key reached the screen 90 ms after it was pressed on the title (Windows), 11 ms now. Finding
  052, "The queue before the presenter".

Checks (2026-10-28, compile and package, short runs; no benchmarks): the core (`build_wasm.py`, 133
host_api imports, sha256 `d646bd3b...01e6`; the fixes are host code). Windows x64: `build_host64.py`
(dhd64.exe 25.5 MB); `package_release.py --platform win` (22.2 MB); the launcher's `cargo test` (11
+ 1 ignored), `dhd_presence` (8) and `dhd_settings` (1, corpus 8, generated 1, web 1). A minimised
run to the race (DHD_NAV) with `DHD_DISCORD=1`: `discord: not running; the presence waits for it`;
an earlier run while Discord was up logged `presence set: In menus`, then `Racing: Alpine`. Linux
x64 (WSL): the build (39.3 MB), `--platform linux` (15.1 MB, system libraries only), 40 s headless
to the race with presence on. Android arm64: `libmain.so` 27.3 MB. macOS (M1): the build (32.5 MB),
`--platform mac` (14.1 MB, ad-hoc signed, `codesign --verify --deep --strict`), 30 s headless to the
race with presence on. Browser: `port/web/build.py` (133 imports match host_api.h),
`check_web_layout.py --require-renderer build/web`.

## From PR #13 (branch `fix/pr13-ports`, 2026-10-28)

Public PR #13 (by n0buholic: "Linux x86_64: SDL3 and Vulkan rendering, stable frame pacing and a
single AppImage", 9 commits on 0.6.3) can't be merged: most of the files it edits were replaced or
deleted on `desk/integrate2` (`build_host64_linux.py`, `gpu_null.c`, the second SDL thread,
`host_posix.c`'s wait, `texpack_linux.c`, the launcher's old `game.rs`/`update.rs`). A review read
the whole PR against integrate2 and sorted its parts. These are rebuilt here on the shared code,
crediting n0buholic in each commit:

- **RIDER SELECTED / shop-flight skip at high FPS** (every platform). The two skip gates in
  001EB1D8 (`D_0029DF70__c62` and `__c16`) count elapsed 60 Hz fields but advanced once per
  update, so at 240 FPS an early Cross was latched in the camera blend and reset at its end.
  `patches_fps.py` advances them by `port_fps_high() ? port_whole_step() : 1`, like the other
  field counters there. The PR's test is `port/tools/test_rider_skip.py`, on `owned_process`
  (finding 103), minimised: 60 and 240 FPS, Cross 10 fields or a second after RIDER SELECTED.
  Windows x64, fields from RIDER SELECTED to the next screen:

  | FPS | Cross after | Before (integrate2's 0.6.4) | After | Budget |
  |---:|---:|---:|---:|---:|
  | 60 | 10 | 41 | 41 | 60 |
  | 60 | 60 | 71 | 71 | 90 |
  | 240 | 10 | **822** (1124 in the review's run) | 68 | 240 |
  | 240 | 240 | 260 | 265 | 360 |

- **Launcher.** `update::latest` returns `Ok(None)` when the newest release has no build for this
  platform: no entry under latest.json's `platforms`, or no `DownhillRevived-*-<platform>.zip`.
  `app.js` then says "No update package is published for this platform yet." (the PR's wording)
  instead of the red "Couldn't check for updates". An unreachable source, a bad manifest and a zip
  without its digest stay errors. On game exit the launcher calls `show()` before `unminimize()`
  (on Wayland a minimized window can't be restored by request alone). On Linux only, when the
  player hasn't set them: `GDK_BACKEND=x11` on Wayland (GTK's "protocol error 71" with the
  undecorated window) and `WEBKIT_DISABLE_DMABUF_RENDERER=1` (a blank webview on some drivers).
  `tauri.conf.json` lists `icons/icon.png` first, because Tauri's first PNG is the Linux window
  icon. The PR's missing-platform test is `missing_platform_is_not_a_failed_check`
  (`release_from`, without the network).
- **The game window's app id and icon.** `make_window` sets `SDL_HINT_APP_ID` to the launcher's
  identifier (`app.skuza.downhillrevived`: Wayland's app_id, X11's WM_CLASS) on every platform.
  `build_host64.py` embeds `port/res/icon.png` (`dhd_icon.inc`), and `window_icon` loads it with
  `SDL_LoadPNG_IO` and sets it with `SDL_SetWindowIcon` on Linux and macOS. Windows keeps its
  resource icon (the two `SDL_HINT_WINDOWS_INTRESOURCE_ICON*` hints): SDL 3.4.18's
  `WIN_SetWindowIcon` makes one HICON at the surface's 512 pixels for both the small and the big
  icon, so Windows would scale it down for the title bar, where `dhd.rc` has every size from 16 to
  256. Android shows the APK's icon (SDL has no window icon there). The hints aren't redundant, so
  they stay.

Already covered on integrate2: the Vulkan surface from SDL's Wayland/X11 handles, BGRA surfaces
(`surface_format`), the Linux renderer target in rustgs, compositor fullscreen (`fill_monitor`),
window calls on the SDL thread (the main thread now), the pause-instruction wait (`host_wait_wall`),
the `$XDG_DATA_HOME/downhill-revived` data folder (`Place`), dhd.log without a console
(`host_console_init`), the platform's update asset, path remapping (scans of the Linux, -linux.zip
and -mac.zip binaries found no local path), `build_launcher.py` on Linux, the
`build_host64_linux.py` fixes (the script is gone), and the launcher's "the game isn't in this
folder" without a binary name.

Not taken, by design:
- The Linux-only `DHD_LATENCY=0` default. A per-OS default goes against one code path. The PR's
  numbers (Plasma Wayland/NVIDIA, JIT 59.785 against clock 59.942 fields/s) look like the JIT rule
  moving the grid when presents block, which a Fifo surface can do on macOS too. The rule should be
  one for every platform, measured on a real Wayland desktop first (open).
- Movies and loading cards at the chosen aspect: their 4:3 is deliberate (findings 055, 056, 072).
- Hiding the launcher while the game runs: it would drop off the taskbar, and it holds Close game.
- A separate AppImage manifest beside `latest.json`. The AppImage as *the* Linux package (one
  `platforms.linux` entry, `Place` gaining it as an install kind, the PR's `replace_appimage`) is
  planned as its own step, not done here.
- `-DSDL_X11_XSCRNSAVER=OFF` (only if a Fedora build actually fails) and `DHD_COFF_CLANG` (the core
  is built on Windows).
- The PR's early-boot SIGSEGV (guest base + 4) was not seen on our code: integrate2 replaced the
  second SDL thread, the fibers and the POSIX host it ran on.

Checks: the core (`build_wasm.py`, 132 host_api imports), Windows x64 (`build_host64.py`, then the 4
rider cases above), the launcher's `cargo test` (12 + 1 ignored), `build_launcher.py` on Windows. A
short run on SDL's offscreen driver reaches `window_icon`: the PNG decodes and `SDL_SetWindowIcon`
answers "not supported" (offscreen has no icons). The icon and app id on a real X11 or Wayland
desktop, the launcher on Linux and macOS's dock icon were not checked (no Linux desktop or Mac run
here).

## Linux on a real GPU: WSL2 with dzn (branch `fix/linux-dzn`, 2026-10-28)

WSL2 exposes the Windows GPU as `/dev/dxg` (Direct3D 12 through `libd3d12.so`); Mesa's dzn ("Dozen")
is Vulkan on top of it.

- The game found no adapter there: wgpu's Vulkan backend hides every driver reporting conformance
  0.0.0.0 (except MoltenVK) unless the instance has `ALLOW_UNDERLYING_NONCOMPLIANT_ADAPTER`, and
  `device.rs` started from empty instance flags (never `with_env()`). It sets that flag now; the
  high-performance choice still puts a discrete GPU before llvmpipe.
- `sdl_host_main` named the main thread "sdl", which on Linux renames the process: ps, top,
  pgrep/pkill and system monitors showed "sdl". The main thread keeps "dhd" now (Linux, Android).

Runs (player defaults from a header-only dhd.ini, except a 1280x720 window instead of borderless on
the monitor; V-Sync on, so 60 is the ceiling): the native renderer at 2x on the RTX 3070, SDL audio
on WSLg's PulseAudio with 0 underruns.

| Run | Window | fields/s | p99 ms | worst 1% ms |
|---|---|---|---|---|
| menus (fields 400-1000) | X11 | 59.74 | 21.7 | 29.4 |
| menus | Wayland | 59.22 | 24.0 | 37.3 |
| race (3600-4800, countdown 2954, GO 3214) | X11 | 59.49 | 25.0 | 30.5 |
| menus, exact renderer (`DHD_RENDER=exact`) | X11 | 18.27 | 80.6 | 86.1 |

The title and a race draw correctly (DHD_SCREENSHOT, X11 and Wayland). Without dual-source blending
the native renderer uses its second-draw path for alpha. The exact renderer's 18 fields/s (56 audio
underruns) is the tools' path, not the players'. The launcher from the -linux.zip (WebKitGTK 4.1)
opens on X11 and Wayland; PLAY starts the game with `DHD_LAUNCHER=1`, `DHD_SETTINGS`, `DHD_DATA_DIR`
and `DHD_MEMCARD_DIR` set and the launcher comes back when it ends. SIGTERM ends the launcher in
0.12 s (the default action, status 143) and leaves a running game running; SIGTERM ends the game
too. dzn's own limits, which real Linux drivers don't have: not conformant (its warning on every
start), no dual-source blending, WSLg's "COPY MODE" presentation.

## Integration round 4 (branch `desk/integrate4`, 2026-10-28)

`desk/integrate3` plus `fix/online-files-check` (finding 098: `dhd_disc`, the game files hash),
`fix/pr13-ports` (the section above) and `fix/linux-dzn` (the section above). main (4de73d87b) was
already in. While merging:

- `port/render/Cargo.toml`, `src/lib.rs`, `rustgs.py`: round 3's `dhd_presence` and the branch's
  `dhd_disc` are both workspace members, both in `native` and in rustgs's MEMBERS. Cargo.lock held
  both already (dhd_disc needs only ring); `--locked` passes and `rustgs.py tree` gives the pinned
  versions on all five targets; the launcher's lock passes `--locked`.
- `main.rs`: round 3's `quit_signals::note_start()` first, then PR #13's WebKitGTK defaults.
- The core no longer imports `host_path_info`: its one caller was the online menu's iso-check.txt
  key, which went into `dhd_disc`. It is deleted (host_api.h, host_sdl.c, the x64 bridge's span
  table, the browser's worker.js, run_headless.mjs and vfs.js's `info`), so the core has 132
  host_api imports (133 - `host_service_file_sha256_start/poll` - `host_path_info` +
  `host_service_disc_check_start/poll`), the same 132 on every host: the native hosts through the
  shared `host_sdl.c`/`host.c` (Windows, Linux, macOS, Android), the browser's worker.js and
  run_headless.mjs (`port/web/build.py`: "132 imports match host_api.h"; the disc check isn't
  provided there: the browser has no online races).
- `port/tools/online_race.py` passed the whole image's SHA-256 in `DHD_NET_ISO_SHA256` (0.6.x's
  value); it passes the game files hash now, dhd_disc's rule in Python, cached in iso-check.txt's
  line format. On the USA disc it gives `USA_FILES_SHA256` (`c8c17bca...3476`, 2338 files).

Checks (2026-10-28, compile and package, short; no benchmarks): the core (`build_wasm.py`, 132
host_api imports, sha256 `da45ac86...62ad2`). Windows x64: `build_host64.py` (dhd64.exe 25.5 MB);
`test_async_services.py` (the disc check and the lobby list: PASS); `cargo test -p dhd_disc` (6, 1,
and the USA image's reference test, 2), `dhd_presence` (8) and `dhd_settings` (1, corpus 8,
generated 1, web 1). Android arm64: `libmain.so` 27.4 MB. Browser: `port/web/build.py`,
`check_web_layout.py --require-renderer build/web`. Linux and macOS were built and packaged from
`release/0.7.0` (this branch plus the version and the experimental zip names): finding 093, "0.7.0".

## Open

- `host_memory_regions` reports nothing on macOS (Linux reads `/proc/self/maps`).
- One PNG decoder for texture packs on every platform: done on `desk/texpack` (finding 057,
  "One decoder"): one `texpack.c` over `host_dir_*`/`host_file_*` and the Rust library's PNG
  decoder on every target; WIC and `texpack_linux.c` are gone.
- The input lag on the Mac with the queue drained: the 45-73 ms there were measured before it; the
  probe (finding 052) ran on Windows only.
- The game while its error box is up ignores SIGTERM (SDL's handler queues a quit the main thread,
  in the box, reads only after OK); it ends once the box is dismissed.
