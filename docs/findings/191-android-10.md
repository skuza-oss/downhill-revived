# 191: Android 10 (API 29) as the APK's minimum

Status: current
Checked: 2026-11-10

Branch `feat/android-10` from `main` ac62c78d (0.7.14), started 2026-11-09. Merged into `main` at bd368803 on
2026-11-09; ships in 0.7.15.

## Goal and acceptance

Issue #65: phones that never got past Android 10 (a Black Shark 2 with a Snapdragon 855, for one)
can't install the APK, whose minimum is API 30 (Android 11, findings 091, 143). Goal: minSdk 29, and
Android 11 and later behave exactly as before.

- Java: every call above API 29 behind `Build.VERSION.SDK_INT`, with a reasonable Android 10
  behaviour; Android lint's NewApi check clean.
- Native: libmain.so and libSDL3.so built for API 29 (`--target=aarch64-linux-android29`,
  `ANDROID_PLATFORM=android-29`); anything newer loaded at run time, never linked.
- Signing: the release APK carries v2 and v3 signatures, so Android 10 (v2) and later both
  install it; `apksigner verify --verbose --min-sdk-version 29` passes.
- Storage (SAF), the Vulkan requirement, full screen and cutout: read and fixed where 10 differs.
- The players' text says Android 10.

Checks: `python tools/check.py port`, `python port/android/build_apk.py` (debug APK), lint NewApi
clean, apksigner as above. An arm64 Android 10 image can't run on this PC's x86 emulator, so a real
Android 10 device test is still needed (the lead).

Out of scope: online on phones (unchanged: P2P rooms), anything below API 29.

## Progress

- [x] 2026-11-09: finding reserved
- [x] 2026-11-09 Java (0fe1cea8): every call above API 29 behind `Build.VERSION.SDK_INT` (list
  below); the screen calls in one place, `Screen.java`. JVM tests (`test_disc_image.py`) pass with
  the new `CrashReport.logCrash` cases.
- [x] 2026-11-09 native (88d4fdfd): `ANDROID_API = 29` in `port/tools/sdl3.py` and
  `build_host64.py` (libSDL3.so's `ANDROID_PLATFORM`, libmain.so's
  `--target=aarch64-linux-android29`); guest RAM without `memfd_create` on Android 10
  (`port/host64/guest_memory_posix.c`). Built and linked (`--no-undefined`) with NDK r29; every
  dynamic import of libmain.so and libSDL3.so is in the NDK's API 29 stubs (`llvm-nm -D` against
  `sysroot/usr/lib/aarch64-linux-android/29/*.so`), except the PGO build's two weak
  `__llvm_profile_*` hooks, as before; libmain.so imports `ASharedMemory_create`, not
  `memfd_create`.
- [x] 2026-11-09 APK: minSdk 29; `enableV2Signing` and `enableV3Signing` both; `check_release`
  verifies with `apksigner verify --min-sdk-version <minSdk>` (v3), again over API 24-27 (v2),
  wants both, and the badging's `sdkVersion` equal to `app/build.gradle`'s minSdk
  (`build_apk.min_sdk()`); `test_build_apk.py` covers each.
  - Debug APK (`build_apk.py`): `sdkVersion:'29'`, `targetSdkVersion:'35'`, arm64-v8a only,
    zipalign `-P 16` OK, `apksigner verify --verbose --min-sdk-version 29`: Verifies (v2; the
    debug key is AGP's default v2 only).
  - Release config, signed with a throwaway key outside the checkout (Gradle `assembleRelease`
    in `build/android/project`): `apksigner verify --min-sdk-version 29` Verifies with v3, and
    `--min-sdk-version 24 --max-sdk-version 27` Verifies with v2. The same build without
    `enableV2Signing` fails the second verify (no v2 block). `check_release` on both: the first
    passes the signing checks, the second is refused for "no APK signature scheme v2" (both
    also, as expected, for libraries that are not a `--release` build).
  - Android lint (`gradlew lintDebug`): NewApi and InlinedApi clean (one NewApi first, below).
    Left as they were before the lane: SDL's own Java (26 MissingPermission BLUETOOTH_CONNECT
    in its BLE Steam Controller code, one UnspecifiedRegisterReceiverFlag, 31 ObsoleteSdkInt
    warnings), and UnusedAttribute warnings for `enableOnBackInvokedCallback` (33) and
    `preferMinimalPostProcessing` (30), which older Android ignores.
- [x] 2026-11-09 `python tools/check.py port -j 6`: OK (docs, tree, core, runtime tests, the
  Windows game, net tests, wgpu tests, wasm2c chunks). No state trace: nothing the game computes
  changed (desktop guest memory compiles as before; on Android only where the RAM's file comes
  from).
- [x] Players' text: README, `docs/PLAYING.md`, `port/release/ANDROID-NOTES.md` (the release
  notes' Android part), `docs/STATE.md` say Android 10.
- [ ] A real Android 10 phone (the lead): install over nothing and over 0.7.14 is not possible
  (0.7.14 refuses Android 10), so a fresh install; first start (picker, copy), a race, touch
  controls with the bars hidden and the picture under a cutout, tilt, a crash (the dialog says
  "It crashed." and Share log has the crash report), and an Android 11+ phone unchanged.

What was above API 29, and what Android 10 does instead:

| Where | Call (API) | Android 10 |
|---|---|---|
| `LastExit` | `ActivityManager.getHistoricalProcessExitReasons`, `ApplicationExitInfo` (30) | no system record: the end of the last run's dhd.log is searched for the game's own crash report (`crash: SIG...`, or guest memory's `host64 memory: access of uncommitted guest address`; `CrashReport.logCrash`); found, the dialog says "It crashed." once (its time is the log's last write) and Share log works as on 11; dhd-exit.txt says Android 10 keeps no record |
| `DhdActivity` | `WindowManager.getMaximumWindowMetrics` (30) | `Display.getRealSize` (the screen's shape for `DHD_MONITOR_ASPECT`) |
| `DhdActivity`, `SetupActivity` | `Window.getInsetsController`, `WindowInsets.Type` (30) | the system UI flags (immersive sticky), as SDL's own full screen does at every level |
| `DhdActivity.touchScreen` | `WindowInsets.getInsets(Type.systemGestures())` (30) | `WindowInsets.getSystemGestureInsets()` (29) |
| `DhdActivity` tilt | `Context.getDisplay()` (30) | `WindowManager.getDefaultDisplay()` |
| `DhdActivity` | (SDL sets `LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS` only from 30) | `SHORT_EDGES` set by `Screen.underCutout`, so the picture reaches under a cutout as on 11 |
| `guest_memory_posix.c` | `memfd_create` (bionic, 30) | looked up with `dlsym`; without it an ashmem region (`ASharedMemory_create`, 26) |

Already fine at 29 (read, nothing to change): `android_perf.c` loads APerformanceHint (33) with
dlsym; `backtrace_posix.h` builds `backtrace` for API < 33; `VibrationEffect.createPredefined` (29);
the picker and copy (`ACTION_OPEN_DOCUMENT`, `openFileDescriptor`, `DocumentsContract`, the app's
own files folder: no shared storage, so scoped storage changes nothing); `LogProvider`, the static
shortcut (25), `Process.getStartElapsedRealtime` (24); SDL 3.4.18's Java checks its own levels;
the vendored wgpu-hal already waits without a timeout on Android below 30 (its
`android_sdk_version`); the manifest's newer attributes (`preferMinimalPostProcessing`,
`enableOnBackInvokedCallback`, newer `configChanges` flags) are ignored by older Android; Vulkan
1.1 stays required (an Android 10 phone such as the Snapdragon 855's has it). The Rust library
links nothing from the NDK beyond libc, libm, libdl and liblog
(`native-static-libs-aarch64-linux-android.txt`).

## Decisions

- **Android 10's crash dialog from the log.** Without ApplicationExitInfo the app can't tell an
  ANR, a kill for memory or an abort from a normal end, so those ask nothing; a crash the game's
  handler reported (the usual native crash, which is what the dialog is mostly for) still gets the
  dialog and the share. Better than no dialog at all, and it never asks after a normal run.
- **Guest RAM on Android 10 is ashmem, not a raw `memfd_create` system call.** The app sandbox's
  seccomp filter on Android 10 is not documented to allow memfd_create, and a refused call there
  ends the process (SIGSYS), not an error. `ASharedMemory_create` is the NDK's own shared memory
  (API 26), shmem-backed like memfd and mapped the same way (MAP_SHARED at any offset). Android 11
  and later still find `memfd_create` in libc (looked up at run time), so they map as before.
- **v2 and v3 both.** Android 10 verifies v3 (from API 28), so v3 alone would install, but AGP
  then drops v2, and some installers and stores read v2 only. Both are asked for and the release
  check refuses an APK without either. The release key was not used here (it stays with the
  maintainer); the next `build_apk.py --release` runs the same checks with it.
- **One `Screen.java`** for the four screen calls, used by both activities, instead of two copies
  of each check.

## Surprises

- `apksigner verify` prints "Verified using v2 scheme: false" for an APK that has a valid v2
  signature when the range asked for starts at API 28 or later: it names only the scheme that
  range verifies with (v3). Finding 143's "with the flag the APK is v3 only" may have been the
  same report; here the bytes (the v2 block id 0x7109871a in the signing block) and a verify over
  API 24-27 show v2 is there only with `enableV2Signing = true`. `apksigner sign` with v2 and v3
  enabled behaves the same as AGP.
- Lint followed the early `SDK_INT` return in `LastExit.find` but not into `readTrace`, a separate
  method taking an `ApplicationExitInfo`: `@TargetApi(R)` on it, as it is only called from there.

- `port/android/test_build_apk.py` failed on main already: `check_release` reads `libmain.so`
  from the APK since finding 151, and the test handed it a file that doesn't exist. The test now
  writes a small zip with a release build's marker.

## Result

Merged (bd368803) and ships in 0.7.15: the APK installs from Android 10 (minSdk 29), signed v2
and v3; calls above API 29 behind `SDK_INT` checks; guest RAM through `ASharedMemory_create` on
Android 10 (`port/host64/guest_memory_posix.c`). Not run on a real Android 10 phone (Progress).
