# 091: Android arm64 build (task AND1)

Status: partly superseded by 143 (the known gaps: touch, online, release key; the first speeds; since 155 also -march)
Checked: 2026-11-04

The design (arm64 host, Rust renderer on Vulkan, SDL3, Gradle without Android Studio) is
current. Changed since: the build is `build_host64.py --target android-arm64` (finding 099;
`build_host64_android.py` is gone) and `port/android/build_apk.py`; races went from 24-25 to
about 115 fields/s at 1x on the Mi 11i (findings 107, 112); the known gaps below are closed or
decided: touch controls (finding 141), online not offered on phones (finding 143, by decision;
the network client is Rust `dhd_net` with TLS, not `ws_posix.c`), a release key and the APK as
a release asset (finding 143), the native renderer crash (AND3 below), the surface format
(finding 145). Low-end phones: findings 126, 127, 146-148.

Branch `rr/AND1` (from `e7bd97212`, main `4755df0af` merged in). **It runs on the Mi 11i (M2012K11G,
Android 13): title, menus and a race draw correctly, at about 24-25 fields/s in a race** (see
"Device results").

## Toolchain (all off C:)

| Part | Version | Where |
| --- | --- | --- |
| SDL3 | 3.4.18, the same pinned tarball as Windows; built as `libSDL3.so` with the NDK's CMake toolchain file | `build/port/sdl3/3.4.18-android-arm64/` |

CMake is the existing `C:\Program Files\CMake`; Ninja is the pip package. No Android Studio. C: had
20.6 GB free before and after.

## Build

From a checkout with the core already built (`python port/tools/build_wasm.py`), Git Bash:

export PATH=~/scoop/apps/llvm/current/bin:$PATH DHD_VUTEXT=$PWD/assets/vutext.bin python
port/tools/build_host64_android.py build/wasm/dhd_core.wasm --jobs 2   # libmain.so + libSDL3.so
python port/android/build_apk.py [--offline]                                   # the APK

`build_apk.py --wasm build/wasm/dhd_core.wasm` does both steps. Output:

- `build/android/lib/arm64-v8a/libmain.so` (28.5 MB, stripped) and `libSDL3.so` (1.7 MB);
- `build/android/libmain.debug.so` (37.9 MB, with symbols, for `llvm-addr2line`);
- `build/android/downhill-revived-<version>-debug.apk` (30.4 MB; versionCode = major*10000 +
  minor*100 + patch from `port/include/version.h`). zipalign `-P 16` and apksigner verify it.

The Android libraries take about 5 minutes on 2 jobs with a warm objcache; Gradle takes 15 s.

## Design

### Native library (then `port/tools/build_host64_android.py`; now `build_host64.py --target android-arm64`)

Modelled on `build_host64_linux.py`: the same wasm2c output (`prepare_units(exact_f32=True)`, the
mirror-address adapter, `--num-outputs 8`; since AND2 guard pages and a plain call-depth counter, as
dhd64.exe), the same `vu1_aot.generate`, the objcache, and the Linux host unit list with the SDL
window (`sdl_host.c`), SDL sound, the Rust renderer front end (`gpu_rust.c`), `font_sharp.c` and
`port/host/android_main.c` instead of `gpu_null`.

- NDK clang `--target=aarch64-linux-android30 -O3 -march=armv8.2-a -mtune=cortex-a78
  -fomit-frame-pointer -fPIC -ffp-model=strict -ffp-contract=off` (no FMA contraction: the x64
  build doesn't fuse either; -O2 without tuning before AND2).
- `-shared`, a version script that exports only `SDL_main`, `--gc-sections --no-undefined
  --build-id -z max-page-size=16384` (16 KB pages for Android 15+), against `libSDL3.so`,
  `-landroid`, the libraries rustc lists and `-lm`. NEEDED: libSDL3, libandroid, libdl, liblog,
  libm, libc.
- `launcher.c` is compiled with `-Dmain=dhd_launcher_main`; `android_main.c` calls it.

### arm64 host

- **Fibers** (`port/host64/fiber_posix.c`): the switch saves the AAPCS64 callee-saved set, x19-x30
  and d8-d15, in a 160-byte frame. A new fiber's first frame has the entry in x19 and
  `fiber_trampoline` in x30 (the earlier draft put the trampoline one slot too high, so the first
  switch would have returned to 0).
- **FP mode**: `fp_mode.c` already had FPCR: the EE thread runs RMode=3 (round to zero) with FZ,
  the x64 MXCSR 0xFFC0 semantics; IEEE mode is FPCR 0. `graphics_queue.c`/`gs_queue.inc` use
  `fp_control()`/`fp_set_control(FP_IEEE(...))`, which are MXCSR on x64 and FPCR on arm64. The
  Rust renderer's guard (`guard.rs`) reads and writes FPCR on aarch64 and clears RMode, FZ, FZ16,
  DN, AH/FIZ/NEP and the trap enables (`ieee_fpcr`).
- **`host_fp_csr`/`host_fp_set_csr`** (AND2 fix): the callers (`vu0_interp.c`, `ps2asm.c`) speak
  MXCSR: they set `(control & ~0x603F) | 0xFFC0` (round to zero + FTZ/DAZ) and restore. `host_posix.c`
  used to write that word straight into FPCR on aarch64, which left RMode/FZ alone, so VU1 on the vu
  thread (FPCR IEEE there) rounded to nearest with denormals; the game thread (already RZ+FZ) was
  unaffected: the AND1 build's VU1 output differed from x64
  (state trace: vu0, gs_stream and gs_regs from field 29; ram/cpu still identical). It now
  translates RC (bits 13-14) to RMode (22-23, nearest/down/up/zero = 0/2/1/3) and FTZ|DAZ
  (0x8040) to FZ (bit 24), both ways, and skips the `msr` when nothing changes.
- **NaN bit patterns**: arm64 and x86 produce different default NaNs (arm64 positive quiet NaN,
  x86 negative). wasm2c's float code follows the host. The AND2 state trace (5000 race fields,
  every column identical to x64) shows no difference on that route.
- **SSE**: `vu0_interp.c` uses SIMDe's SSE2 (NEON underneath) on aarch64.
- **x64-only `#if`s**: `host.c`, `vif.c` and `keyboard.c` tests of `_WIN64 || __x86_64__` (the
  64-bit layout) now include `__aarch64__`. `host.c`'s crash report reads `pc`/`sp` from the
  aarch64 `mcontext`.
- **Memory ordering**: the presenter's `shown_due/shown_at/shown_late` record relied on x86 store
  order; arm64 gets release fences on the writer and acquire fences on the reader.
- **Guard pages** (AND2): `WASM_RT_MEMCHECK_GUARD_PAGES`, as on Windows: `guest_memory_posix.c`
  already reserved 8 GiB, so an out-of-bounds access faults into its reporter.
- **Signals**: `guest_memory_posix.c`'s fault handler prints the report, then on Android restores
  the previous handler and returns, so debuggerd still writes its tombstone.
- **Backtraces**: bionic's `backtrace()` needs API 33; `port/host/backtrace_posix.h` builds it from
  `_Unwind_Backtrace` + `dladdr` (library + offset) below that.
- **Threads/locks** (`port/gs/native_thread.h`): one SDL3 implementation on every platform
  (`SDL_Mutex`/`SDL_Condition`, detached `SDL_Thread`, `SDL_DelayNS` then a spin for the frame
  wait); the queues, movie decoder, texture packs and net client use SDL3 and C11 atomics directly.
  `win32_compat.h` is gone (2026-10-27).

### Renderer (Rust, Vulkan)

- `rustgs.py` builds `port/render` for `aarch64-linux-android` (staticlib `libdhd_render.a`, dep
  file `libdhd_render.d`); `check_rust_lib.py` reads ELF archive members as well as COFF and checks
  the native libraries against `port/render/native-static-libs-aarch64-linux-android.txt`
  (`-ldl -llog -lunwind -lm -lc`).
- `dhdr_surface_create` accepts `DHDR_WINDOW_ANDROID` (an `ANativeWindow*`) on Android; wgpu gets an
  `AndroidNdkWindowHandle` and picks Vulkan (`DHDR_DEV_VULKAN` from `gpu_rust.c`; DXGI adapter
  selection is Windows-only).
- `gs_gpu_surface_reset` drops and recreates the surface under the native lock, for the
  background/foreground cycle.

### SDL and the app (`port/host/android_main.c`, `port/gs/present.c`, `port/android/`)

- SDL's own Java (`org.libsdl.app`, copied from the 3.4.18 source by build_apk.py) runs the
  activity; `app.downhillrevived.DhdActivity` extends it, loads `SDL3` and `main`, and passes the
  screen aspect as `DHD_MONITOR_ASPECT`.
- `SDL_main` makes the internal storage dir (`getFilesDir()`) the game's folder: `dhd.ini`, the
  saves and `dhd.log` live there (no storage permission). stdout/stderr go to logcat (tag
  `DownhillRevived`) and `files/dhd.log`.
- **The disc**: on first start, a message box, then SDL's file picker (`SDL_ShowOpenFileDialog`,
  no filter: Android's MIME types don't know `.iso`). The chosen content URI is copied through
  `SDL_IOFromFile` into `files/SCUS-97177.iso` (via a `.part` file, 1 MB blocks, progress in the
  log). Before the core exists only the ISO's SYSTEM.CNF is checked (`host_disc_check`: the full
  check, `game_data_read`, runs in the core); `dhd.ini` gets `iso = <path>` (not `DHD_ISO`, which
  skips the check) and `auto_update = off` (the updater is a Windows zip flow), so the core's
  `host_disc_ready` checks the game data; a bad copy gets an error box and is deleted, and the next
  start asks again. Later starts use the copy. Needs the image's size (2.5 GB) free in internal
  storage. Any `DHD_*` line in dhd.ini sets that variable (host.c), which is how the device tests
  below drove the game.
- Landscape fullscreen (`sensorLandscape`, SDL's orientation hint, `SDL_WINDOW_FULLSCREEN`); Back is
  trapped (SDL sends it as a key) so it doesn't kill the activity.
- **Background**: an SDL event watch (`lifecycle_watch`, present.c) sees `WILL_ENTER_BACKGROUND`
  synchronously, before SDL blocks its event thread (block on pause: the queued copies only arrive
  after the resume), drops the renderer's surface at once and sets a flag. The game thread's next
  vsync waits until `DID_ENTER_FOREGROUND`, waits for the new `ANativeWindow`, recreates the
  surface and marks a resize. A present that still races Android destroying the window gets a
  validation error from wgpu; on Android `surface.rs` turns that into a failed present (the C side
  stops presenting until the surface reset) instead of the fatal error handler.
- **SDL's event thread** polls, then sleeps 4 ms, on Android: SDL's Android
  `SDL_WaitEventTimeout` wakes itself with each event it queues and spun a whole core.
- **The log**: stdout/stderr go through a pipe to logcat and `files/dhd.log`;
  `android_log_drain` (host_exit, atexit, the fault handler) lets the last lines through before
  the process ends.
- **Memory card**: mc.c names its folders with backslashes (`saves\card1`); on Android the POSIX
  host's path calls turn them into slashes, so the saves are real folders under `files/saves`.
- **Gamepads**: SDL's Android joystick/HIDAPI (Bluetooth and USB; the manifest declares both as
  optional). The keyboard path also works with a hardware keyboard.
- **Movies**: left as the player defaults set them: `skip_movies = 1` skips the startup movies;
  OPTIONS > VIDEO CLIPS still plays through `movie.c` (untested on arm64).
- **Manifest**: Vulkan 1.1 hardware level required, touchscreen optional, `isGame`, VIBRATE and
  INTERNET; minSdk 30, target/compile 35; `useLegacyPackaging false` (the libraries load from the
  APK uncompressed).

## Status

Built and verified offline: the APK's badging, alignment and signature; `libmain.so` links with
`--no-undefined` and exports only `SDL_main`. The Windows exe (`package_release.py`) and the browser
build (`port/web/build.py --renderer rust`) were rebuilt after the shared changes.

### Device results (Mi 11i, M2012K11G, Android 13, MIUI 14; 2026-10-26)

Run over adb only (nobody touched the phone). The ISO was pushed with `adb push` and copied into the
app's files with `run-as`, so the file picker path is not tested.

- **Starts and draws**: the libraries load, wgpu picks the Adreno 660 over Vulkan
  (`gs: renderer dhd_render 1.0 wgpu 29.0.3 ... on Adreno (TM) 660`), the native renderer at
  scale 2, dual-source blending, presented full screen at 2400x1080 in landscape, widescreen 20:9
  with the HUD in shape. Sound through AAudio (48 kHz).
- **Title and menus**: drawn correctly; a keyboard key (adb `input keyevent --longpress
  KEYCODE_SPACE`) opens the main menu. A tap shorter than one field is not seen (keys are level,
  read once a field), which a real press isn't.
- **A race** (bench_race.py's race scene from dhd.ini: DHD_NAV, DHD_AUTOPAD_FRAMES, DHD_RACE_PAD,
  seed 81257, paced): the menus are navigated, the race loads and is ridden to the bench window;
  the picture is right (riders, particles, HUD, upgrades). No crash, clean exit at the window's end.
- **Speed** (fields 6000-7000, paced to 60): **24.5 fields/s**, p50 43 ms, p99 76 ms. CPU ms per
  field: game 26.5, vu 22.9, gs 18.3, native 9.1 (busy cores 1.98); sound had 134 underruns at that
  rate. The menus run about 34 fields/s. The threads were on the big cores (4-7), but the phone held
  them low: the X1 capped at 2.15 of 2.84 GHz and sampled at 0.84 GHz, the A78s about 1.9 GHz
  (MIUI's governor/thermal limits; phone settings were not changed). Before the SDL pump fix the
  `sdl` thread used a whole core as well.
- **Home and back**: the game waits in the background and draws again in the foreground (after the
  two fixes above; before them a SIGABRT in the render thread, then a black screen).
- **Memory**: 272 MB PSS (100 MB graphics).

### Known gaps

- **Speed**: see "AND2: Android speed" below.
- **No touch controls** (out of scope): a gamepad or keyboard is needed. Gamepads go through SDL
  (Bluetooth/USB) but none was connected: untested.
- **Online** won't connect: the server is `wss://` and `ws_posix.c` is plain sockets (no TLS).
- **Texture packs** are off (as on Linux).
- The file picker and the ISO copy, saving to the memory card, the Back button and OPTIONS > VIDEO
  CLIPS are untested (they need a person or a gamepad).
- No progress screen while the ISO copies (only the log).
- A debug build only; a release key and Play packaging are later work.

## Device run

Git Bash needs `MSYS_NO_PATHCONV=1` or it rewrites the device paths.

adb install -r build/android/downhill-revived-0.4.0-debug.apk adb logcat -c adb shell am start -n
app.downhillrevived/.DhdActivity adb logcat -s DownhillRevived SDL DEBUG AndroidRuntime libc adb
shell run-as app.downhillrevived cat files/dhd.log > dhd.log adb exec-out screencap -p > screen.png

Skipping the picker (tests): `adb push <iso> /data/local/tmp/SCUS-97177.iso`, then `adb shell run-as
app.downhillrevived sh -c "mkdir -p files && cp /data/local/tmp/SCUS-97177.iso files/"` and delete
the /data/local/tmp copy. A test dhd.ini goes in the same way (`files/dhd.ini`); the race scene's
lines come from `bench_race.SCENES['race']` plus `DHD_BENCH = 6000-7000`, `DHD_BENCH_EXIT = 1`,
`DHD_MEMCARD = 0`. Profiling: `adb shell simpleperf record --app app.downhillrevived --duration 4 -e
cpu-clock -o /data/local/tmp/perf.data`, then `simpleperf report -i /data/local/tmp/perf.data --sort
comm,dso`.

A crash prints `libmain.so+0x...` lines (our handler) and a tombstone (`DEBUG` tag); symbolise with
the NDK's `llvm-addr2line -f -C -e build/android/libmain.debug.so 0xOFFSET`.

Still to check with hands on the phone: the file picker and the copy on a fresh install, a Bluetooth
or USB gamepad in menus and a race, the Back button, a save to the memory card surviving a restart.

## AND2: Android speed

Branch `rr/AND2` (2026-10-26). Goal: from 24.5 race fields/s toward 60 on the Mi 11i (Snapdragon
888: 1x X1 + 3x A78 + 4x A55; MIUI 14). Every number below is the race bench window (fields
6000-7000, paced to 60, `bench_race.py`'s race scene from dhd.ini) on the phone over adb. CPU ms are
per field and per thread. Runs were alternated A/B; the run-to-run noise is about +-1 field/s. The
tools (`phone_bench.py`, `phone_trace.py`, A/B and simpleperf scripts) stayed outside the repo.

### Changes and their effect

| step | race fields/s | game / vu / gs / native ms |
|---|---|---|
| AND1 APK (same session, same phone) | 26.4 | 24.3 / 21.5 / 17.0 / 8.8 |
| main + the vif.c build fix (VU1R) | 27.1 | 23.7 / 19.4 / 15.9 / 8.3 |
| guard pages, plain depth counter, -O3, -mtune=cortex-a78 | 27.8-28.6 (4 runs, mean 28.3) | 21.1-21.4 / 18.4 / 14-15 / - |
| + MXCSR->FPCR fix, `isb` pause | 27.5-29.7 (7 runs, mean 28.2) | 23.4 / 17.6 / 14.0 / 8.5 |
| + DHD_SPIN_US=3000 (the Android default) | 28.6-30.4 (5 runs) | 28-29 / 21-22 / 17-18 / 9.2 |
| + main 0c29546e4 (UI1, SP1 compiled VU0): final | 29.0-31.1 (7 runs, mean 29.6) | 27.9 / 20.9 / 17.0 / 9.1 |

The game ms with the spin include its spinning.

- **Guard pages and the call-depth counter** (`build_host64_android.py`):
  - `WASM_RT_MEMCHECK_GUARD_PAGES=1` with `WASM_RT_SKIP_SIGNAL_RECOVERY=1` replaces the software
    bounds checks. `guest_memory_posix.c` already reserves the 8 GiB.
  - `WASM_RT_THREAD_LOCAL=` makes wasm2c's call-depth counter a plain global. The build copies
    `wasm-rt.h` into its work folder and adds an `#ifndef` around WABT's definition. The core
    runs on one thread at a time, as on dhd64.exe.
  - Before, `__tls_get_addr`'s TLSDESC resolver alone took 1.6% of all samples (about 5% of the
    game thread).
  - Together with -O3, `-mtune=cortex-a78` and `-fomit-frame-pointer`: game thread -2.5 ms, about
    +4% fields/s.
- **MXCSR->FPCR** (`host_posix.c`, see "arm64 host"): this is an exactness fix, not a speed change.
  Before it, the state trace's vu0, gs_stream and gs_regs columns differed from x64 from field 29.
- **`isb` as the spin pause** (`graphics_queue.c`): `yield` is a no-op on Cortex cores, so the
  4096-step spin lasted about 2 us. The workers then slept and were woken far more often than on
  x86. `fp_set_control` skips an unchanged FPCR (an `msr fpcr` + `isb` costs dozens of cycles).
- **Producer-side spin** (`graphics_queue.c` `idle_wait`, `DHD_SPIN_US`): before sleeping, the game
  thread's waits on the vu/gs queues spin for up to that many microseconds.
  - The default is 3000 on Android and 0 elsewhere.
  - Final APK A/B, 3 pairs: 0 gives 28.2, 27.3, 28.1; 3000 gives 29.3, 29.5, 29.5 (+5%). p99 drops
    from 57-60 ms to 50 ms.
  - Cost: about 0.3 more busy cores (2.0 -> 2.3), so more power and heat.
  - DHD_TIMELINE showed why: the EE waited 8.9 ms a field for VU and 2.5 ms for GS. MIUI drops a
    sleeping big core's clock, and the wake-up costs more than the wait.
- **Merging main** (SP1 compiled VU0, VU1R, SP9/SP10): with the spin on in both, final against s5
  gave 29.0, 29.2 against 29.2, 28.6. No measurable change on the phone.

### Tried and not kept

- **ADPF** (`port/host/android_perf.c`): `APerformanceHint` is loaded with dlsym and reports each
  field's longest thread CPU time against `DHD_PERF_TARGET_MS` (13). On this phone (Android 13,
  MIUI 14), `APerformanceHint_createSession` returns NULL ("no performance hint session"). The
  code stays in for phones that support it; `DHD_PERF_HINT=0` turns it off.
- **Affinity** (`DHD_AFFINITY=game:7,vu:4-6,...`, re-applied every 10 fields): MIUI's
  GamePerfEngine resets thread affinity, so pinning has to be re-applied. Pinning game to the X1
  gave no gain, and vu/gs got slower on three A78s. Off by default.
- **Clocks** (simpleperf stat): the A78s ran at about 1.75-1.9 GHz and the X1 at 1.9-2.18 GHz
  (2.84 GHz is its maximum). The game thread was on the X1 only about 12% of the time, with an IPC
  of 2.38 there against 2.05 on the A78s. Sustained-performance mode needs the Java side and was
  not tried; phone settings were not changed.
- **PGO** (`--pgo-generate`, `--pgo-use PROFDATA`): a profile from one instrumented race gave 28.4
  and 28.4 against 27.7 (game 23.9 against 25.5 ms).
  - The instrumented APK writes `files/dhd-<pid>.profraw` at `host_exit`; merge it with the NDK's
    `llvm-profdata`.
  - It stays a build option, not a default: the profile must come from the same core build.
- **rr/SP8** (the renderer's staging belt), cherry-picked on top: 27.92 and 27.93 fields/s, native
  thread 9.09 and 9.34 ms. Without it: 27.46 and 27.59, native 9.15 and 9.01 ms. Within noise, so
  it was dropped from AND2.

### Profile (simpleperf, final branch)

- **game**: `ps2_vu0_run` 11% (before SP1), `lookup` 3.5%, `host64_memory_address_slow` 1.7%;
  the rest is the recompiled game.
- **vu**: `native_geometry_loop` 15%, `native_clip_run` 12%, kernel 8.5%.
- **gs**: `native_draw` 26%, `gs_write` 17.5%, `gs_feed` 17%.
- **native** (the Rust/wgpu render thread): about 59% kernel (the Adreno driver and ioctl).

### Exactness

The x64 trace comes from `state_trace_run.py race` with `DHD_WIDESCREEN=0 DHD_ASPECT=4:3
DHD_SCALE=2`, using the branch's dhd.exe. The phone runs the same recipe from dhd.ini, with the mask
and code-pointer files pushed.

- **Final APK, `DHD_HEADLESS=1`, 5000 race fields**: all 13 columns identical to x64
  (`trace_columns.py` PASS, `state_diff.py` all 5000 rows match). s5 gave the same result before
  the merge.
- **Not headless**: only gs_regs differs, because PC headless skips `gs_priv_write`. This is an
  artefact of the recipe, not of the core.
- **The AND1 build** (before the FPCR fix): vu0, gs_stream and gs_regs differ from field 29;
  ram/cpu stay identical.

### Native renderer crash (exists since AND1, not fixed)

Fixed in AND3 (below): the cause was not a pipeline.

SIGSEGV at address 0x68 in the "native" thread. The fault is in `vulkan.adreno.so`
`vkCmdBindPipeline`, reached from wgpu's `encode_render_pass` -> `command_encoder_finish` ->
`Native::picture`. Some crashes are in `wgpu_core::Queue::submit` instead.

- **When it happens**: in the trace recipe, always at field 1551, right after the race loads. In
  paced benches, about 1 run in 4 earlier in the session; none in the last 12 final-APK runs.
- **What it isn't**:
  - DHD_WGPU_DUAL_SOURCE=0, DHD_SCALE=1 and dropping the Constant blend factors don't fix it.
  - DHD_RENDER=exact doesn't crash.
  - logcat shows no AdrenoVK "link failed".
- **Public reports**: the same frame in the Adreno driver comes from binding a null or failed
  pipeline:
  - DKR-R issue 70 (github.com/ThatGuyMcd/DKR-R/issues/70);
  - DKR-R PR 1 (github.com/dmillerzx/DKR-R/pull/1);
  - Defold issue 13164 (github.com/defold/defold/issues/13164).
- **Suspect**: one of the native pipelines first created at the race load (keys 001CBDD, 001D89D
  and 001C89D were the last ones created).
- **Next step**: check `create_render_pipeline`'s error scope on Android and fall back when it
  fails.

### Why it is not 60 yet, and next ideas

The field costs about 34 ms. The game thread is busy for about 25 ms of it and waits on VU for about
9 ms, all at the 1.8-1.9 GHz MIUI allows.

1. **The renderer crash above**: it comes before more speed.
2. **Pipelining**: let the EE start field N+1 while VU1/GS finish field N, so the vu wait leaves the
   critical path. Worth up to the 9 ms that the EE waits for VU now.
3. **The vu thread's NEON quality**: `native_geometry_loop` and `native_clip_run` go through SIMDe
   SSE2. Native NEON intrinsics, or checking that clang -O3 turns SIMDe's code into plain NEON,
   could help.
4. **Game-thread hot paths**: the `lookup` cache and `host64_memory_address_slow` (SP2/SP3/SP14's
   work applies on arm64 as well).
5. **PGO as a release step**: a profile per core build, from a headless race on the phone.
6. **Clocks**: sustained-performance mode (`Window.setSustainedPerformanceMode`) or the Game Mode
   API on phones where ADPF exists. On MIUI the governor is the limit.

## AND3: the native renderer crash, NEON on the vu thread

Branch `rr/AND3` (2026-10-26), same phone and tools as AND2. The tools stayed outside the repo:
`phone_bench.py`, `phone_trace.py`, an A/B script, and a repro script that runs the trace recipe to
field 1800 and reports where each run ended.

### The crash: cause and fix

**Symptom.** In the trace recipe, about 60% of runs died right after field 1557. Base `main` crashed
3 of 4 runs and AND2's final APK 2 of 4. The failures were:

- SIGSEGV in the "native" render thread inside `vulkan.adreno.so`, either in `vkCmdBindPipeline`
  (`ldr w9,[x8,#0x68]` with x8 = pipeline->[0x128]->[0x108] = NULL) or in
  `vkEndCommandBuffer`;
- now and then a SIGSEGV in the game thread;
- or a hang with `dhdr_native_present: get_current_texture: validation error`.

Paced benches crashed less often, but the cause was the same.

**Where it starts.** Field 1557 is the first lens-flare Z probe of the race. In native mode, the
exact renderer answers only that probe: `native_stage_pages` -> exact `stage_pages`/`fetch_pages` ->
`dhdr_exact_submit` / `read_begin`, all on the GS worker. A temporary submit log showed that the
crash follows the exact renderer's first submit within one picture (about 30 ms). That submit held 5
writes (4.2 MB), 4 copies and 2 `main` dispatches. The native pipelines that AND2 suspected (first
bound at picture 1530) were bound without trouble before then.

**Bisection.** Each row is a trace run to field 1800, using environment switches in throwaway
builds:

| variant | runs that died |
|---|---|
| base | ~60% (3/4, 2/4, 2/2, ...) |
| Vulkan validation layer (VVL 1.4.363, packed with `build_apk.py --extra-lib`) | crashed; no messages |
| context lock around every exact op and the native picture | 3/5 |
| exact renderer off (`staged` zeroed) | 0/6 |
| replay and watch on, no exact `stage_pages`/read | 0/5 |
| no replay, `stage_pages` + fetch | 4/5 |
| readback without map/poll, map without copy, spin-polling instead of Wait | all still crashed |
| exact `batch` dispatches dropped | 1/5 (at submit 372, not the first) |
| exact writes only, so no exact command buffer is ever submitted | 0/5 |
| a separate wgpu device for the exact renderer | 4/6 |

- **What the table shows.** The trigger is exact compute command buffers submitted from the GS
  worker while the render thread records the native passes.
- **What it is not:**
  - Not a pipeline: the native pipelines bound fine before the probe.
  - Not the readback.
  - Not a CPU race in wgpu: the lock did not help.
  - Not state shared through the device: a second VkDevice crashed as well, and more often.
- **The driver's process-wide state.** The crash depends on two threads recording and
  submitting command buffers. In exact mode one thread does all the Vulkan work and never
  crashed.
- **Robustness.** The driver offers `VK_EXT_robustness2`, so wgpu leaves shader buffer accesses
  unchecked. raster.wgsl masks every VRAM address to 4 MB, so an out-of-bounds write is unlikely.

**Fix** (commit d4eb129b3):

- **Rust:** `port/render/src/exact/encoder.rs` gets `dhdr_exact_set_submitter`, Android only.
  wgpu 29 records a command buffer's Vulkan commands at `finish()`, so `Encoder::submit` (finish
  plus `queue.submit`) is where all of the exact renderer's Vulkan recording happens. With a
  submitter registered, that closure runs through it.
- **C:** `port/gs/native_core.inc` (Android, not `NATIVE_INLINE`) registers the render thread
  when it starts. The GS worker posts the call under `queue_lock` and waits. The render thread
  runs it at the top of its wait loop (`NATIVE_WAIT`), between pictures and outside
  `context_lock`, so no exact submit overlaps a native picture.

**Evidence:**

- Trace recipe to field 1800 with the fix: 12 of 12 runs reached 1800, the first 6 with all 380
  exact submits logged. The base rate was about 60% failures.
- The final merged APK: 4 more trace runs, all 4 reached 1800 (16 of 16 in total).
- 10 paced race benches (the 8 A/B runs and 2 on the final APK at 26.08 and 26.08 fields/s) had
  no crash; each logs `gs: exact submits run on the render thread`.

**Cost.** An exact submit now waits for the render thread to finish its current picture. That
happens about 1.5 times a field during a race. The A/B below already includes it. The fix-only APK
ran at 23-26 fields/s, against AND2's 29-31 earlier in the day. The phone got hotter during this
session: in each pair, the later runs were slower, at the same rate for A and B. So that difference
is not measured as the fix's cost. A cleaner check needs a cool phone and base against fix.

### NEON on the vu thread (commit 1cd4889e0)

`native_geometry_loop` and `native_clip_run` ran through SIMDe's SSE. Three things made that code
slow on arm64:

- **`PS2_OPAQUE_M128`** had a volatile stack round trip on aarch64, a 16-byte store and reload
  per VU operand. It is now `__asm__("" : "+w"(v))`, the same empty asm as on x86, with the
  value kept in its NEON register.
- **Float compares.** With `-ffp-model=strict`, clang splits SIMDe's `vceqq/vcgtq/vcgeq_f32`
  into four scalar `fcmp`/`csetm`. In `vu0_interp.c` on aarch64, `_mm_cmp{eq,lt,gt,le,ge}_ps` are
  now inline-asm FCMEQ/FCMGT/FCMGE. Under FPCR.FZ they read a denormal as zero, as CMPPS does
  under DAZ.
- **Conversion and MAC flags.**
  - `_mm_cvttps_epi32` is FCVTZS, with INT32_MIN wherever CVTTPS gives its integer indefinite:
    bits >= 0x4f000000 as signed, or a negative NaN. The test works on the integer bits.
  - `fmac_flags4` builds the zero and sign MAC bits from lane masks (`vaddvq` of weights 8/4/2/1)
    instead of movemask plus a table.
  - `vu1_native_geometry.h`'s `native_clip` does the three lanes' signed compares at once (bound
    from lane 0, weights 1/4/16 and 2/8/32).

The x86 code is the `#else` side, unchanged.

**A/B** (race bench window, paced, alternated, before the main merge; A = fix only, B = fix + NEON;
phone warming through the session):

| pair | A fields/s | B fields/s | A vu ms | B vu ms | A game ms | B game ms |
|---|---|---|---|---|---|---|
| 1 | 26.34 | 27.86 | 24.9 | 16.4 | 32.0 | 27.3 |
| 2 | 24.49 | 25.85 | 28.3 | 19.6 | 35.4 | 30.7 |
| 3 | 24.21 | 26.11 | 28.3 | 19.7 | 35.6 | 30.4 |
| 4 | 23.00 | 25.43 | 30.4 | 20.8 | 38.0 | 32.0 |
| mean | 24.51 | 26.31 (+7.3%) | 27.96 | 19.14 (-32%) | 35.3 | 30.1 |

- **Other threads:** gs went from 21.0 to 18.1 ms. Native stayed at 13.7 against 13.4 ms. Busy
  cores fell from 2.46 to 2.22.
- **Why the game thread drops too:** the game ms include the producer spin (DHD_SPIN_US), and the
  game thread now waits less for VU.

### Exactness

- **x64 reference:** this branch's merged exe (`state_trace_run.py race`, `DHD_WIDESCREEN=0
  DHD_ASPECT=4:3 DHD_SCALE=2`, 5000 fields).
- **The final merged APK** (fix + NEON + main 962b6fc24), `DHD_HEADLESS=1`, 5000 race fields:
  all 13 columns identical to x64 (`trace_columns.py` PASS, `state_diff.py` all 5000 rows match).
- **Against AND2's x64 trace:** this x64 trace differs in ram/cpu from field 25. That comes from
  the main merge (SP13/SP14 core changes), not from AND3: every AND3 change is Android-only or
  aarch64-only.
- **Builds:** APK, x64 (`package_release.py`), browser (`port/web/build.py --renderer rust`,
  `check_web_layout.py build/web` OK) all built from the merged tree.

### Idea 2 (design only): EE runs field N+1 while VU1/GS finish field N

**Today.** At each vsync, `kernel.c` calls `host_service_graphics_wait()` before
`host_service_gs_vblank()`, the CSR read, the VBLANK handlers and pad/sound. The game thread
therefore waits for the vu ring and the GS queue to drain completely, every field: about 9 ms for VU
in AND2's timeline, still several ms after NEON. `DHD_GRAPHICS_ORDERED` (used by the browser)
already drops the second wait and queues `gs_vsync` after the handlers' packets. The first wait
stays.

**Design:**

1. **The vblank enters the stream.** At vsync, instead of waiting, the EE appends a "vblank N"
   marker packet to the graphics ring. The GS worker runs `gs_vblank` (FIELD toggle, native
   hand-off of field N to the render thread) when it reaches the marker. Presentation then
   follows from the GS worker, not the EE.
2. **CSR on the EE side.** The VBLANK handler and `vsync_flag_addr` need `GS_CSR`. Its FIELD and
   VSINT bits depend only on the vsync count and the game's CSR writes, so the EE computes it
   from a shadow kept in `kernel.c`. Any other CSR read, and every GS privileged write, still goes
   through `ps2_graphics_sync`, as now.
3. **Bounded depth.** At vsync N the EE waits only until field N-1's marker has retired: one
   field in flight, at most one extra field of latency. A ring-space wait already exists.
4. **Every existing sync point stays:**
   - VU1 memory/register access (`ps2asm.h` ranges, `VI_FBRST`);
   - DMA channel status reads (`dma.c`);
   - `sceGsSyncPath`;
   - `sceGsExecStoreImage` and its deferred guarded reads;
   - the state-trace hook (`state_trace.c` waits, so traces are taken at the same points).

**Risks:**

- **Exactness.**
  - The game's observable state must not change. A read that today happens after the drain would
    see the GS/VU1 state of field N-1 instead of N if any read path lacks a sync. Audit every
    `host_service_gs_*` getter called from EE code.
  - The trace and lockstep paths must keep the full wait (an env switch, as with
    `DHD_SCREENSHOT`).
- **Online and pad timing** (online is state sync, not lockstep). Input sampling and the
  race-clock pad scripts key on vsync. The pad and
  sound tick stay on the EE at vsync, so game timing is unchanged, but the picture shown lags by
  up to one field.
- **The Z probe.** It already syncs (guarded pages, `graphics_settle`). With the AND3 fix, an exact
  submit now waits for the render thread, which may be busy with field N-1's picture. The probe
  can get slower, though it stays correct.
- **Pacing.** `host_pacing_*` and the present timing assume the EE presents. Moving the hand-off
  to the GS worker changes where the frame time is measured. Frame-time stats and
  `bench_race.py`'s window need the new point.
- **Gain bound:**
  - It saves up to the VU/GS drain time per field (now less than 9 ms after NEON). On the phone
    the game thread is then the limit (about 25-30 ms with spin).
  - The expected gain is the part of the drain that is not covered by the spin. That means
    +10-20% fields/s at best, at the cost of the extra frame of latency.
