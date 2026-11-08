# Building Downhill Revived

There are two builds:

- **The PC port**: `dhd.exe`, the 64-bit game for Windows, and the browser build. Built with
  clang, wasm2c and Rust.
- **The matching build**: the original PS2 executable rebuilt from `src/` with the game's
  original compiler, then compared with retail section by section. This is what proves the
  decompilation. You only need it to work on `src/`.

Both start from your own copy of the game executable. The repository contains none of it.

## 1. Prerequisites

For both builds:

- Windows 10 or 11, 64-bit. Commands below are for PowerShell; Git Bash works too.
- [Git](https://git-scm.com).
- [Python](https://www.python.org) 3.12 or newer, on PATH as `python`.
- [7-Zip](https://www.7-zip.org) (or any tool that can read an ISO).
- A dump of your own *Downhill Domination* USA disc (SCUS-97177) as an `.iso`.

For the PC port, also:

- [LLVM](https://github.com/llvm/llvm-project/releases) for Windows: `clang`, `clang-cl`,
  `lld-link`, `wasm-ld`, `llvm-rc`, `llvm-nm`, `llvm-objcopy` and `llvm-cxxfilt` on PATH, or a
  [scoop](https://scoop.sh) install (`scoop install llvm`). The project is built with LLVM 23.
- The wasi-sdk sysroot, WABT (`wasm2c`) and SIMDe (see
  [Tools outside the checkout](#tools-outside-the-checkout)).
- Rust ([rustup](https://rustup.rs)) for the renderer (`port/render`).
- Visual Studio (or its Build Tools) with the "Desktop development with C++" workload: the MSVC
  **x64** libraries and a Windows SDK.
- [CMake](https://cmake.org) 3.16 or newer, on PATH or in `C:\Program Files\CMake`, for SDL3
  (window, sound, controllers). `requirements.txt` brings the `ninja` package it builds with.
- A network connection once, for SDL3's source and the shader compiler (see
  [SDL3](#sdl3-window-sound-controllers)).

For the matching build, also:

- A few hundred MB of disk for the toolchain, which `tools/setup_toolchain.py` downloads.
- For four units built with Sony's older compiler (`-sce29` in `src/`): WSL with Ubuntu
  and its 32-bit C library (`sudo apt install libc6-i386`), and SCE ee-gcc 2.9-991111 from the
  [decomp.me compilers](https://github.com/decompme/compilers/releases/tag/compilers) release,
  unpacked into `tools/compilers/ee-gcc2.9-991111/` (archive `ee-gcc2.9-991111.tar.xz`, SHA-256
  `99cb5db603cd985b50fc6851157020ad69489f2227309ccf825bd43ac7bc19dd`).

## 2. Get the code and the executable

    git clone <repository URL> downhill-revived
    cd downhill-revived
    python -m pip install -r requirements.txt
    7z e "<your dump>.iso" SCUS_971.77 -oiso

Check that it is the right file:

    certutil -hashfile iso\SCUS_971.77 SHA1

It must print `4e25a222ad8a475b5d26c2e928e90b5f2b7075eb`. `config/splat.yaml` checks it too.

Then split it:

    python tools/split.py

This takes about 15 seconds and writes `asm/` and `assets/` (about 37 MB) from your executable with
[splat](https://github.com/ethteck/splat). Both folders are game-derived and gitignored, like
`iso/`. Never commit them.

## 3. Build the PC port

The PC game is the 64-bit Windows `dhd.exe` and the browser build. Both are made from one
portable game core, a wasm32 module (`build/wasm/dhd_core.wasm`). The 32-bit build is retired: its
last source is the tag `v0.3.1`.

    python port/tools/build_wasm.py             # the game core, build/wasm/dhd_core.wasm
    python port/tools/package_release.py        # dhd.exe from it, checked, zipped into build/release/
    python port/web/build.py                    # the browser build, into build/web/

`build_wasm.py` compiles every unit of `src/` (rewritten at build time by `port/tools/transform.py`
with the PC patches of `port/patches*.py`), the functions that are still assembly recompiled to C
(`port/tools/recomp.py`), the module-side runtime and HLE and the game data's fixups into one
module. Everything it needs is generated from the checkout, `asm/` and `assets/vutext.bin`.

`package_release.py` builds `dhd.exe` with `port/tools/build_host64.py` (wasm2c, the x64 host code
in `port/host64/` and `port/host/`, the Rust renderer in `port/render/` built by
`port/tools/rustgs.py`), checks it and writes the release (below). It builds only from committed
source. `python port/tools/build_host64.py build/wasm/dhd_core.wasm` builds the executable alone,
into `build/host64-exe/`.

`dhd.exe` holds no game data. The game's data sections (`.vutext` to `.sdata`) are read from
`SCUS_971.77` on the player's disc image at every start, checked by SHA-1
(`port/hle/game_data.c`); the executable keeps only the words the port changes in them
(pointers and code addresses, `port/tools/data.py`).

To run a build, copy `dhd.exe` (with `dxcompiler.dll`) to a folder of its own and start it with
the disc image named: `iso = <path>` in its `dhd.ini` or `DHD_ISO=<path>` in the environment. The
game has no disc dialog of its own; for players the launcher picks the disc (finding 093).

### Tools outside the checkout

Keep the checkout, `DHD_DEVCACHES` (below) and the Rust toolchain off the C: drive: the 64-bit
build and the Rust build refuse to write there, and `rustgs.py` needs `CARGO_HOME` and
`RUSTUP_HOME` set to folders on another drive. The tools below are looked for under
`DHD_DEVCACHES` (an environment variable; default `~/DevCaches`), with these folder names, or where
their own variable or option says:

| Tool | Folder under `DHD_DEVCACHES` | Or |
|---|---|---|
| [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) 34 | `wasi-sdk/wasi-sdk-34.0-x86_64-windows` | `WASI_SYSROOT`, `build_host64.py --wasi` |
| [WABT](https://github.com/WebAssembly/wabt) 1.0.42 | `wabt/wabt-1.0.42` | `WABT_DIR`, `build_host64.py --wabt` |
| [SIMDe](https://github.com/simd-everywhere/simde) source | `simde-src` | `SIMDE_DIR` |
| [Binaryen](https://github.com/WebAssembly/binaryen) 133 (browser build) | `binaryen/binaryen-version_133` | |
| DirectX Shader Compiler (downloaded and checked by `port/tools/dxc.py`) | `dxc` | `DHD_DXC_CACHE` |

The Rust toolchain comes from `rustup` (`port/render/rust-toolchain.toml` pins it); cargo's
output goes under `DHD_DEVCACHES/dhd-cargo`, compiled objects are cached in
`DHD_DEVCACHES/dhd-objcache` (`DHD_OBJCACHE_DIR`; `DHD_OBJCACHE=0` turns it off), the browser
build's linked wasm modules there too (each module's one compile-and-link clang call, keyed by the
command, its preprocessed sources and the wasi-sdk's files: the VU worker's module alone takes 20 to 60
minutes to compile), and finished core modules in
`DHD_DEVCACHES/dhd-modulecache`: `build_wasm.py` hashes everything the module is built from (the
checkout's files except docs, the online server and the launcher, renderer, browser, Android and
release folders; `asm/` and `assets/`; the tools' versions) and, when a module of that hash was
built before in any checkout, copies it back in seconds instead of building it again
(`DHD_MODULE_CACHE=0` turns this off; the intermediate files in `build/wasm` are then not
refreshed). Several checkouts building at once can take turns: with `DHD_BUILD_SLOTS=N`, at most N
`build_wasm.py` or `build_host64.py` runs build at the same time on the machine, the others wait
(`build_slots.py`). LLVM is found on PATH or in scoop's default
place (`~/scoop/apps/llvm/current/bin`). `build_host64.py` names the Visual Studio and Windows SDK
folders it uses (MSVC 14.51, SDK 10.0.26100.0) at its top.

### Build times

Measured per platform and step in finding 151. What keeps a rebuild short:

- `build_host64.py` prints each step's time as it ends (`step SDL3`, `Rust library`, `wasm2c`,
  `prepare C`, `chunks`, `C`, `link`) and a `steps:` line at the end; the object cache prints its
  hits and misses.
- The module's C from wasm2c, prepared and cut into chunks, is made again only when the module, the
  tools that make it, WABT or the target changed (`build/<target>/prepared.json`): a rebuild after a
  host, Rust or renderer change skips it (`module C as before`).
- A split module (`--num-outputs` above 1: Android's default 8, and the C the Mac and Linux builds
  take) is cut into content-defined chunks (`prepare_wasm2c.stable_units`): a chunk ends after a
  function whose name hashes to a cut, is named after its first function and declares only what it
  uses, so a changed function changes one chunk and the object cache has the rest. A change that
  moves data addresses (new data in the module) still changes every function that uses them.
  `python port/tools/test_wasm2c_chunks.py` checks it. The chunks are for development only: their
  smaller units inline less, and the game ran ~1.5 % slower on the throttled Mi 11i (finding 151).
  A release build passes `build_host64.py --release` (one unit or wasm2c's own parts, the shipped
  code as before); it writes `split.txt` beside the game (`one`, `wasm2c` or `chunks`; an APK
  carries it as `assets/dhd-split.txt`), and `package_release.py` and the APK's release check
  refuse `chunks` (`package_release.py` passes `--release` itself for Windows and Android).
- Windows builds the module as one unit (`core.c`, about 160 MB of C, one compiler thread, the
  longest step after any change of the core). `build.py -- --num-outputs 8` builds it in chunks
  instead, for a development loop: the same game, but other code (functions are no longer static
  to one unit, so fewer are inlined), so a speed measurement uses the default and a release
  `--num-outputs 8 --release`.
- The core cache's key remembers each input file's hash by its path, size, modification time and
  file id (`DHD_DEVCACHES/dhd-modulecache/file-hashes.json`), so only changed files are read.
- The browser build's wasm-opt passes keep their output in the object cache by tool, flags and input
  (`objcache.tool_output`).
- Android compiles with half the machine's threads by default (chunks are small; wasm2c's own parts
  needed 2). With `--pgo-use`, the profile is part of every object's key, so a second build with the
  same profile and core is all hits. A profile stays useful across core changes: clang applies a
  function's counts only while the function is unchanged (its control-flow hash), so the changed
  functions build as without PGO and the rest keep theirs. Record a new one when the hot code (EE,
  VU, GS) changed, and at least once per release (finding 146).

### Android (arm64 APK)

`python port/android/build_apk.py --wasm build/wasm/dhd_core.wasm` builds `libmain.so` with
`port/tools/build_host64.py --target android-arm64` and a debug-signed APK, `build/android/downhill-revived-<version>-debug.apk`.
It needs the Android SDK (`ANDROID_HOME`, platform android-35), an NDK (r29 tested), a JDK 17+
(`JAVA_HOME`), the Rust target `aarch64-linux-android`, and `GRADLE_USER_HOME` and
`ANDROID_USER_HOME` set (Gradle and the debug key). No Android Studio. Details and status: finding 091.

The release APK (finding 143): `python port/android/build_apk.py --release --wasm build/wasm/dhd_core.wasm`
builds the release build type (not debuggable, no R8), signed with the release key, which only
the environment names:

| Variable | What |
|---|---|
| `DHD_ANDROID_KEYSTORE` | the keystore file (outside the checkout) |
| `DHD_ANDROID_KEYSTORE_PASS` | its password |
| `DHD_ANDROID_KEY_ALIAS` | the key's alias |
| `DHD_ANDROID_KEY_PASS` | the key's password |

The key is kept by the maintainer, outside the repository; never commit a key or a password. Every
release must be signed with the same key: Android installs an update only over an APK of the same
signer, so a phone with a debug-signed build has to uninstall it first (which deletes the app's
files: the disc copy, dhd.ini and the saves). The APK is checked (`apksigner verify`, `aapt dump
badging`: not debuggable, the version, arm64-v8a only) and written as
`build/android/DownhillRevived-<version>-android-experimental.apk`. versionCode is
MAJOR * 10000 + MINOR * 100 + PATCH (0.7.7: 707), so it grows with every release.

### macOS (Apple silicon) and Linux (x64)

Each is built on its own machine with the same script, `build_host64.py --release --target
macos-arm64` or `--release --target linux-x64`, then packaged there with `package_release.py --platform mac|linux --game
build/host64-<macos|linux>/dhd64 --elf PATH/SCUS_971.77` (it builds the launcher with
`build_launcher.py`; the Mac zip's app is signed ad hoc).

- macOS: Xcode's command-line tools (Apple clang, ld), Python 3.12, CMake and Ninja, rustup
  (its toolchain file installs Rust 1.97.1), Node.js for the launcher. A Mac without WABT takes the
  C that `wasm2c dhd_core.wasm -n core --num-outputs=8` wrote on another machine, with `--wabt`
  naming a folder that holds WABT's `include/` and `share/`; `--simde` (or `SIMDE_DIR`) names SIMDe.
  `DHD_VUTEXT` names `vutext.bin` when the checkout has no `assets/`. An 8 GB Mac builds with 4
  jobs (the target's default). The first `cargo` run needs the network once
  (`CARGO_NET_OFFLINE=false cargo fetch --locked` in `port/render`). Set `DHD_OBJCACHE_DIR` there
  too: off Windows the object cache is off without it (a Mac rebuild with no change then compiles
  everything again). If every link fails with `ld: tapi error ... unknown architecture` (the
  command-line tools' newest SDK is newer than their `ld`), point the build at Xcode's SDK:
  `export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)`.
- Linux (or WSL): clang, lld, CMake, Ninja, and the X11, Wayland and PipeWire/PulseAudio/ALSA
  development packages SDL3 finds; WABT in `/opt/wabt`. The Rust library is built by `rustgs.py` on
  the machine, or in WSL without cargo by the Windows side through WSL's interop. The launcher needs
  WebKitGTK 4.1's development files.
- Linux release builds are made on Ubuntu 22.04 (a WSL distribution of its own works: LLVM 21
  from apt.llvm.org, rustup, Node.js 24, WABT built from source, a Python 3.12 or later), so the
  game and the launcher run on glibc 2.35 and later. `package_release.py --platform linux` refuses
  a binary that needs a newer symbol version (`LINUX_SYMBOLS`): one linked on a newer system binds
  the versions of that system's headers. The first `cargo` run needs the network once, as on macOS
  (`CARGO_NET_OFFLINE=false RUSTC_WRAPPER= cargo fetch --locked` in `port/render` and
  `port/launcher/src-tauri`).

Off Windows the object cache is used only when `DHD_OBJCACHE_DIR` names a folder; rustgs's target
dirs go to `DHD_CARGO_DIR`, else `~/.cache/dhd-cargo`.

### SDL3 (window, sound, controllers)

`port/tools/sdl3.py`, which `package_release.py` runs, provides SDL3 as a static library, so
`dhd.exe` stays one file:

- it downloads `SDL3-3.4.18.tar.gz` from SDL's GitHub release into `DHD_DEVCACHES/downloads/` and
  checks its SHA-256 (pinned in the script; any other file is refused), so later builds, in any
  checkout, need no network;
- it builds with CMake + Ninja + clang for `x86_64-pc-windows-msvc`, the static C runtime, only the
  window, audio, joystick, gamepad, HIDAPI and event parts, into `build/port/sdl3/3.4.18-x86_64/`
  (a few minutes), and keeps the library and its headers in `DHD_DEVCACHES/dhd-sdl3/` under the
  recipe and the compiler: another checkout copies them instead of building SDL again (finding 151).
  The macOS, Linux and Android builds of SDL are kept the same way.

To change the version, update `VERSION` and `SHA256` in `sdl3.py` together (the hash of the
release's `.tar.gz` as GitHub lists it). `python port/tools/sdl3.py` builds it alone.

### Package a release

    python port/tools/package_release.py

This writes `build/release/DownhillRevived-<version>-win.zip`: one folder with `dhd.exe`,
`dxcompiler.dll`, `README.txt`, `LICENSE.txt` and `THIRD-PARTY-NOTICES.txt` (from `port/release/`,
then the licences of the Rust crates linked in). There is no `dhd.ini` in it: the game writes one
the first time it saves a setting, so unpacking a new version over an old folder never replaces a
player's settings or saves. `dhd.exe` needs only Windows' own DLLs. The script refuses an
executable that holds an absolute path (your user or checkout folder) or a server address other
than the official server's, and any file that names another server. Every packaged file is
scanned for runs of 64 bytes or more of your `iso/SCUS_971.77` (`port/tools/scan_retail.py`);
any run outside the short list of string literals and tables that the decompiled source itself
holds stops the package. `--wasm PATH` packages another core; the docstring has the rest.

    python port/tools/package_release.py --platform android   # with the DHD_ANDROID_* variables set

builds the release APK (above) with `LICENSE.txt` and `THIRD-PARTY-NOTICES.txt` inside, checks it
and every file in it as the zips are checked, and puts it in `build/release/` as
`DownhillRevived-<version>-android-experimental.apk` with an `android` entry in `latest.json`.
Every run also writes `build/release/SHA256SUMS` (each release file and `latest.json`, as
`sha256sum` prints them); `--sums` writes it again after other machines' files are copied in.

### Tests

`python port/tools/test_runtime.py` builds and runs the runtime and HLE tests, among them
`test_keyboard` (keyboard layout and steering ramp) and `test_gamepad` (SDL starts and lists the
controllers; none needed). Test programs run where the game runs their code: the core's C as wasm32
guest programs through wasm2c, the host's on the 64-bit host (`port/tools/testprog.py`, finding 104).

The renderer's tests run the Rust renderer in x64: `python port/tools/test_gs_wgpu.py`
(test_gpu, test_depth, test_present) and `python port/tools/gs_golden.py compare KIND` (replay
goldens you record yourself; they hold pictures of the game, so they are not in the repository).
Per-field state traces (`port/hle/state_trace.c`, `port/tools/state_trace_run.py`,
`state_diff.py`) compare two builds' game state field by field.

## 4. Build the matching executable

    python tools/setup_toolchain.py
    python tools/configure.py
    python -m ninja

`setup_toolchain.py` downloads the pinned toolchain into `tools/compilers/` and checks every file
against `toolchain.lock.json`:

- SN Systems ProDG 3.01 (gcc 2.95.3 "SN BUILD 1.36" and the `ps2eeas` assembler), the game's
  own compiler, from a public mirror. Flags: `-O2 -G8 -fopt-stack`, C++ adds `-fno-exceptions`
  (`docs/findings/002-compiler-sn-prodg.md`).
- ps2dev binutils, to assemble the functions that are still assembly and to link.
- objdiff-cli, for progress reports.

`python -m ninja` rebuilds the executable from `asm/` and `src/`, then `tools/check_image.py`
compares every section with retail. The build must end with `MATCH`.

## 5. Decompile a function

    python tools/queue.py list
    python tools/try_func.py func_XXXXXXXX build-try/cand/func_XXXXXXXX.c
    python tools/promote.py func_XXXXXXXX build-try/cand/func_XXXXXXXX.c --commit

`try_func.py` builds the whole image with your candidate and compares the function word for
word. `promote.py` moves it into `src/` only if the whole image still matches.
`docs/WORKER.md` has the rules that make code match. `python tools/report.py` refreshes the
progress table in `README.md`.

## 6. The online server

`online/server/` is a Cloudflare Workers project. Its
[README](../online/server/README.md) covers tests, a local server (`wrangler dev`) and
deployment. The game's built-in default is the official server,
`wss://dhd.skuza.app` (`port/hle/host.c`, `OFFICIAL_SERVER`), used when
`dhd.ini`'s `server =` is empty and `DHD_NET_SERVER` is unset. Self-hosting is optional: a
`server = wss://...` line in `dhd.ini`, or the `DHD_NET_SERVER` environment variable, points the
game at another server. Don't commit any other deployed address; `package_release.py` refuses
a package that names one.

## Settings for developers

`dhd.ini` beside `dhd.exe` holds `key = value` lines. Each key stands for a `DHD_*` environment
variable (`port/hle/host.c`, `apply_setting`), and a variable set in the environment wins over
the file. A few useful ones:

| Key / variable | What |
|---|---|
| `iso` / `DHD_ISO` | the disc image |
| `scale` / `DHD_SCALE` | internal resolution, `1` to `8` or `window` |
| `widescreen` / `DHD_WIDESCREEN` | `1` the monitor's shape, `0` off, or a shape such as `21:9` |
| `DHD_ASPECT` | the picture's shape: `wide`, `4:3`, `16:9`, `stretch` or `pixels` |
| `window` / `DHD_WINDOW` | `windowed`, `borderless` or `fullscreen` |
| `skip_movies` / `DHD_SKIP_MOVIES` | `1` skips the movies (the test tools set it) |
| `vibration` / `DHD_VIBRATION` | `0` turns rumble off |
| `memory_card` / `DHD_MEMCARD` | `0` turns the memory card off |
| `texture_pack` / `DHD_TEXTURE_PACK` | a PCSX2-format texture pack folder |
| `textures` / `DHD_TEXTURES` | `off` draws the game's own textures |
| `texture_cache_mb` / `DHD_TEXTURE_CACHE_MB` | graphics memory for replacement textures |
| `auto_update` / `DHD_AUTO_UPDATE` | `ask`, `on` or `off` |
| `update_url` / `DHD_UPDATE_URL` | a `latest.json` or GitHub releases API address (empty: the official releases) |
| `server` / `DHD_NET_SERVER` | the online server address (empty: the official server) |
| `renderer` / `DHD_RENDER` | `native` (default for players) or `exact` (the reference) |
| `font` / `DHD_FONT` | `sharp` (the default) or `original`, the game's font as the PS2 draws it |
| `DHD_SDL` | `0` reads controllers without SDL (XInput and Sony pads only) |

A dhd.ini without a key gets the player defaults (`port/hle/host.c`, `player_defaults`):
native renderer, scale 2, borderless at the monitor's resolution, widescreen 1, movies skipped. With no dhd.ini at all (the test
tools set `DHD_SETTINGS=none`) none of them apply. Tools that drive the game (`DHD_NAV`,
`DHD_RACE_SYNC`, ...) never run the updater.

Online play is not a setting: the host decides whether it is offered (`host_service_net_offered`,
finding 143). The Windows, macOS and Linux builds offer it; the Android build and the browser
don't (no ONLINE row, and `server`, `DHD_NET_*` or anything else can't start a session there).
On Android `auto_update = off` also turns off the once-a-day update notice.

## Releases

`port/include/version.h` holds the version, the only place it is written. `package_release.py`
names the zip after it and writes `build/release/latest.json` beside it (version, the release
asset URL, the zip's SHA-256 and size) for the updater; publish both as release assets.

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
Android (finding 143): `package_release.py --platform android` with the release key's
`DHD_ANDROID_*` variables builds, checks and adds the `-android-experimental.apk` (latest.json's
`android` entry); every run writes `SHA256SUMS`. The release page gets `port/release/ANDROID-NOTES.md`.
Crash symbols (finding 152): `build/release/` also holds files that are kept, never published (not
in `SHA256SUMS`): the Windows link map `-win.map` (`crash_stack.py` reads dhd.log's crash stacks
with it) and `-android-libmain.debug.so`, the APK's `libmain.so` before stripping (copied only when
its GNU build ID is the APK's). Keep both with the release's other files; a player's
`libmain.so+0x...` frame (dhd.log's backtrace, or the tombstone a shared log carries) is named by
`llvm-symbolizer --obj=DownhillRevived-<version>-android-libmain.debug.so 0xOFFSET` or
`llvm-addr2line -fe ... 0xOFFSET` (the NDK's `toolchains/llvm/prebuilt/*/bin`). The macOS and Linux
binaries keep their symbol tables, so their backtraces name functions already. Publish exactly
the files `SHA256SUMS` lists, plus `SHA256SUMS`.
Since 0.7.0 the mac and linux zips are `-mac-experimental.zip` and `-linux-experimental.zip`
(finding 093, "0.7.0": built and checked on all three, published 2026-10-28; Linux is linked on
Ubuntu 22.04 and runs on glibc 2.35 and later, which `package_release.py` checks).

Before the version commit, run the doc-gardening checklist (`docs/RELEASE-CHECKLIST.md`).
