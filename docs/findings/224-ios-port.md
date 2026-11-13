# 224: The native iOS port on main

Status: current
Checked: 2026-11-13

Branch `feat/ios-port` from `main` b9da043f (v0.8.0), started 2026-11-13. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decisions (2026-11-13): the native iOS port by Gustavo Ribeiro (GitHub
`ribeirogustav`, his fork `downhill-revived-ios`, pull requests #72, #73, #74) comes into main,
and iOS becomes a platform we build and release as an unsigned, experimental IPA, like the Mac,
Linux and Android builds. Later the same day: his SwiftUI launcher and his touch controls do not
come in; iOS uses the shared in-game touch controls and the game's own settings, as Android does,
and the shared mobile launcher (a separate lane) once it exists. So this lane is the iOS platform
itself: his shared-code changes reviewed one by one, his host, Metal bridge, build and packaging,
our wiring of the shared touch controls, a minimal first-start disc screen, the docs and the
proofs.

Done when: the shared changes are on the branch in his name, each kept, changed or left out with
the reason below; `port/ios` holds the platform parts in his name and our commits on top; the
iOS Simulator app and the unsigned device IPA build on the Mac from the branch; the Simulator app
starts and its logs show how far the game gets; iOS offers what Android offers online (P2P rooms);
the docs say how iOS is built, installed and what is experimental; on Windows `check.py port` and
`check.py docs` pass and the seven state traces equal the reference; the Mac desktop build still
builds and starts.

Out of scope: the on-screen run (a second worker, with the device tools), a real iPhone (an Apple
ID the lane does not have), the shared mobile launcher and the touch layout work (other lanes).

## Progress

- [x] 2026-11-13: finding reserved (9e5daa39)
- [x] his shared-code changes reviewed and committed in his name (0156c9ae, 1201e8d4, 8174c690
  and the renderer, host and build-tool commits after them)
- [x] the owner's second decision: the branch rebuilt without his launcher and touch controls
  (nothing of them is in the branch's diff against main)
- [x] `port/ios` platform parts in his name; Simulator target; P2P only; the shared touch
  controls wired; the first-start disc screen; `package_release.py --platform ios`; docs
- [x] the Simulator build, the device IPA and the release IPA, the Simulator run by its logs
  ("What was run")
- [x] Windows: `check.py port` green, the seven state traces equal the reference; the macOS
  desktop build builds and races; the Android debug APK builds (Result)

## His changes, one by one

His fork's `main` (6a58c732, "iOS v0.7.16") is upstream 0.7.16 plus his changes, squashed;
`git diff v0.7.16 6a58c732` is the picture. Each shared change was reviewed as an outside pull
request: what it does on every other platform, and whether it is right.

| Change | Verdict | Why |
|---|---|---|
| `fps.c` `port_fps_update_display`, called from `kernel.c` at each VBLANK; `fps.h`; `test_fps.c` "display" case (PR #73) | kept | Runs only with `DHD_IOS_DISPLAY_AUTO`, which only the iOS app sets; elsewhere it returns at once and the clock is unchanged. Moves the selected rate between 60 and 120 when ProMotion, Low Power Mode or heat change the display's rate, carrying the fractional phases. `test_fps.py` now runs the case. |
| wgpu-hal Metal `setAllowsNextDrawableTimeout` (PR #74) | kept, documented in `vendor/wgpu-hal/PATCH.md` | `cfg!(target_os = "ios")`: macOS keeps upstream's setting. On iOS an inactive app's `nextDrawable` waits at most about a second (`SurfaceError::Timeout`, skipped like a missed picture) instead of for ever. |
| `widescreen.c` live aspect, `widescreen_sprites.h` VU offsets, `vif.c` `vif1_update_widescreen_sprites`, `hle.h` `host_widescreen_frame_aspect`, `host.c` `host_frame_video`, `kernel.c` (browser) | kept | The init's word writes became `apply_aspect`, which writes the same words as before when the switch is off (the 4-ULP tolerance only matters for a word already rewritten); the per-field HUD code returns at the same test. The rest runs only with `DHD_IOS_DISPLAY_AUTO` or under `TARGET_OS_IPHONE`. |
| `patches.py`: the inventory (`func_00139BF8`) builds its camera matrices after its projection | changed: iOS only | Game code on every platform; it can change game state. Now `port_ws_live_aspect()` picks his order on iOS and the game's own order everywhere else. |
| renderer (`gpu_rust*.c/.inc`, `native_core.inc`, `gpu.h`) and `present.c`: the app's Metal layer, the surface dropped while inactive, the field's output aspect | kept, with changes | Behind `TARGET_OS_IPHONE`; the new `output_aspect` is carried on all platforms and read on iOS only. Conflicts with 0.8.0's staged exit resolved to `host_end` (finding 210). His change dropped `DHD_SCALE_MAX_REASON=phone` on Android: restored. His weak reference to `ios_game_is_active` on every Apple target failed the macOS link (ld64 refuses an undefined weak symbol in a static link): iOS-only now. Not taken: the hand-over of his launcher's render scale and present filter and his frame-time panel hook (the game's own settings do this, as elsewhere). |
| host: Documents as the executable's folder, the display from the bridge, no SDL message box, `SDL_SetMainReady`, the game threads' QoS class, guest memory in the bounds-check layout, the `shm_open` fallback, the log drained before a trap or memory failure | kept, with changes | QoS and `SDL_SetMainReady` are iOS-only now (he set the QoS on macOS too and `SDL_MAIN_HANDLED` everywhere, unmeasured there). The log drain runs on every platform: dhd.log keeps its last lines on a trap. |
| `host.c` player 1's pad from his touch controls (`touch_pad_merge`) | not taken | The shared touch controls feed the pad on iOS as everywhere (`touch_host_merge`). |
| `gamepad.c`: a cross-axial dead zone for physical pads on iOS | not taken | A handling change other platforms do not have (keep the original feel); the owner decides. |
| build: `build_host64.py --target ios-arm64`, `sdl3.ios_build`, `rustgs.py`/`check_rust_lib.py` for `aarch64-apple-ios`, `prepare_wasm2c.py`'s `WASM_RT_SKIP_BOUNDS_CHECKS` | kept, with changes | The SDK is asked of `xcrun` only for the iOS target (he asked at import on every platform); `/usr/bin/clang` only for iOS; WABT and SIMDe from the macOS target's places. |
| build: the Nintendo Switch target and its host parts, a vendored SIMDe (243,000 lines), his `build_wasm.py` changes, the quoting change in `sdl3.posix_build` | not taken | No decision for Switch; the core is built on Windows and handed to the Mac (as the macOS release does); the quoting would rebuild the Mac's and Linux's SDL for nothing. |
| `port/ios`: host (`ios_main`), Metal bridge, `build_ios.py`, `package_unsigned.py`, `project/`, tests | kept, with changes | See "The iOS platform" below. |
| `port/ios`: the SwiftUI launcher (8 languages), the touch controls and their editor, the controller-bindings view, the frame-time statistics, the external display scene | not taken | The owner's decision (shared touch controls, settings and launcher). His ideas are written down below for the lanes that take them. |
| `tools/check_tree.py` (a 1 MiB exception for his icon), the fork's README, cover art, previews, release notes, `scripts/verify_release.py`, `ATUALIZACOES_*.md`, his findings (207-217 in his fork, 180-183 on the PR branches) | not taken | The icon was re-encoded to fit the rule (256 colours, 1.8 MB to 0.5 MB); presentation material stays in the fork; what his findings establish is folded in here. |

## The iOS platform

- **The app.** `port/ios/app/main.m` (ours, Objective-C, UIKit): at the first start a screen
  asks for the disc image (a document picker), copies it into the app with its progress, checks
  it (`ios_main.m`: ISO 9660, `SYSTEM.CNF` names SCUS_971.77) and starts the game; later starts go
  straight to the game. A disc copied into the app's Files folder is found too. It is a stopgap
  for the shared mobile launcher. The game thread is an `NSThread` with 8 MiB of stack (his ran it
  on a GCD worker): `dhd_main` runs SDL's event loop on it and the game on SDL's threads.
- **Exit and lifecycle.** EXIT GAME, a lost device and the end of `dhd_main` end the process with
  `host_end` (finding 210), as on the desktop: the app does not start a second core in one process
  (the guest memory is reserved once). When the app goes inactive the bridge lets go of every
  finger, the game thread drops the surface at `gs_pump_messages` and waits there, so the game is
  paused; it makes the surface again when the app is active. iOS may kill a suspended app at any
  time: dhd.ini is written at each change (`dhd_settings_save`) and the memory card at each write
  (`mc.c` flushes), so a kill loses nothing written; a kill in the middle of a save can leave that
  save incomplete, as a power cut does on a PC.
- **Threads and clocks (finding 217's class).** The named threads are SDL's, plus the app's game
  thread, which names itself `sdl` in `sdl_host_main` and lives until the process ends (it never
  returns while the game runs; when it does, `host_end` follows on it). No thread that ends is
  asked for its clock.
- **Online.** `port/net/net.c`'s `NET_OFFERED`: iOS offers P2P rooms only, as Android (Create A
  P2P Race, Join With A Code, the address warning), and refuses SFU rooms whatever asks. His fork
  offered SFU rooms; his launcher's Online page (a display of the server and a Join field that did
  nothing) is not taken: online is the game's ONLINE menu with the shared touch panel.
- **The shared touch controls.** `touch_host.c` has an iOS part: touch is on (as on Android;
  `DHD_TOUCH=android`, the core's word for a phone's touchscreen, so SETTINGS behaves as on a
  phone); the Metal view passes UIKit's touches as SDL's finger events, plus the mouse SDL makes
  of the first finger for the menus (finding 092), so both reach `touch_host_event` and
  `present.c` on the SDL thread as Android's do. The screen in mm comes from the view's size in
  points at 158 points per inch on an iPhone and 132 on an iPad (UIKit gives no physical size; a
  few per cent off makes the buttons a few per cent larger or smaller); the home indicator is the
  bottom strip; the sensor housing in landscape is the cutout band on its side (its safe-area
  inset), as Android's. The overlay's vibration goes to the Taptic Engine. No tilt steering yet
  (CoreMotion is not wired: a later lane).
- **Typing.** While a game field takes typing (a rider name, a room code) the system keyboard shows
  and its characters go to `text_input_character` (thread-safe); the game's own pad picker works
  too.
- **Display.** The output is the screen's own size (his launcher capped it at 720 pixels by
  default); dhd.ini's `fps` (120: ProMotion) and `widescreen` (off: 4:3) set the bridge's rate and
  shape at the start; landscape only, the home indicator hidden, edge swipes deferred (the first
  swipe goes to the game).
- **Memory.** No 8 GiB reservation without the extended-virtual-addressing entitlement, which needs
  a provisioning profile: the iOS builds use wasm2c's bounds-check layout (2 GiB and a 64 KiB
  window) with the access checks themselves off (`WASM_RT_SKIP_BOUNDS_CHECKS`), as his port did.
  An access past 2 GiB faults outside the reservation instead of trapping: the core makes none in
  normal play, but a bug there would show as a crash, not a trap message.
- **Packaging.** `build_ios.py --unsigned` builds the device app without signing and
  `package_unsigned.py` makes `DownhillRevived-<version>-unsigned.ipa`; its privacy check named one
  builder's own Team ID, Apple ID and device UDID: now generic patterns plus `DHD_IPA_PRIVATE`.
  `package_release.py --platform ios` makes the release IPA and its checks.

## His touch controls and launcher next to ours

| | His (iOS fork) | Ours (shared overlay, findings 141, 214, 218; the game's SETTINGS) |
|---|---|---|
| Layouts | one fixed set of controls: stick, ✕ ○ □ △, L1 R1 L2 R2, START, SELECT, L3 R3 (hidden), Menu | two presets, Classic pad (the full pad) and Thumb zones (PEDAL, flick attacks, the air layout) |
| Steering | the stick (fixed or floating) | stick (floating or fixed, range in mm), tilt (Android), D-pad |
| Per-button size | 25 % to 600 % for each control, in steps of 1 % | 60 % to 160 % per control in the editor (5 % steps), plus Touch Size 70-105 % or Auto for all |
| Placement | drag any control anywhere; swap a slot's action; hide any but the essential ones | drag in the in-game editor (Ground and Air tabs), rules kept: on screen, 3 mm gaps, nothing in a flick lane or a strip |
| Orientations | portrait and landscape layouts, saved separately | landscape |
| iPad / tablet | its own defaults (other places, a 1.25 base scale) | sizes in mm from the screen's real size, the same layout rules |
| Languages | the launcher in 8 languages (en, pt-BR, es, fr, de, it, ja, zh-Hans) | English (the game's own text) |
| Game moments | START is always on screen: it skips the flight, pauses; menus by stick and ✕ | SKIP for the flight, movies and demo; the online menu's touch panel; READY by touch; CODES page; Back; labels and hints |
| Settings | the launcher's own pages (graphics profile, output resolution cap, frame rate, filters, controller bindings, haptics) | the game's SETTINGS pages, on every platform |

What iOS loses with his controls left out, and what it keeps: every game moment that needs touch
works on iOS through the shared overlay as on Android (SKIP, the online room's READY and race menu,
the pause menu, CODES); only tilt steering is missing (no CoreMotion yet).

### For the later lanes: how his code does the three ideas the owner picked

**Per-button size and place** (`launcher/TouchLayout.swift`, `TouchLayoutStore.swift`,
`ControlsCustomizationView.swift`, `TouchControlSurface.swift` in his fork):
- Model: `TouchLayout { controls: [TouchControlPlacement], coordinateSpace, baseScale }`;
  `TouchControlPlacement { slot, x, y (0..1 of the screen), scale (0.25..6.0, default 1),
  visible, action }`. Slots: stick, cross, circle, square, triangle, l1, r1, l2, r2, start,
  select, l3, r3, menu; `action` lets a slot send another button (swaps keep each action once).
  Essential actions (✕, △, START, R1), the stick and the menu button cannot be hidden.
- Size: `size(fit, baseScale)` = base size in points (stick 130, shoulders 66x38, START/SELECT/
  Menu 76x34, face buttons 50) x scale x baseScale x fit, where `fit` = min(1, view width / 780,
  view height / 300) in landscape (390x280 in portrait). A 600 % button may be larger than the
  view; its centre is clamped so a part stays on screen (`limitedCenter`).
- Storage: one JSON file in Documents, `touch_layouts_v2.json`:
  `{version: 2, portrait, landscape, ipadPortrait, ipadLandscape}`, each a `TouchLayout`; read
  with fallbacks (an older file without the iPad keys keeps an edited phone layout, else takes
  the iPad defaults), sanitised on load and save (clamped values, duplicate actions reset, the
  essential ones made visible); a save is refused while an essential control is missing.
- Editor: tap to select, drag to move, a size slider with "Reset to 100%", an action picker, a
  Visible switch, undo/redo per orientation (a gesture is one step), Save; touches go to the
  editor, not the game, while it is open.
- Ours today: `touch_edits` in dhd.ini (`DHD_TOUCH_EDITS`), one `TouchEdit {dx, dy, size}` per
  control and preset (`touch.c` `edit_of`), edited in the game's own editor (`touch_open_editor`):
  drag, − / + (0.6 to 1.6), Reset, Done, with the layout rules above; sizes are in mm, so a
  control is the same physical size on every screen.

**iPad and tablet defaults** (`TouchLayout.ipadDefaults`): separate default places for iPad
(landscape: stick at 0.16/0.75, ✕ 0.83/0.82, ○ 0.91/0.71, □ 0.75/0.71, △ 0.83/0.60, L1 0.10/0.37,
L2 0.10/0.54, R1 0.91/0.35, R2 0.91/0.49, SELECT 0.34/0.09, START 0.66/0.09, Menu 0.50/0.09) and a
`baseScale` of 1.25, chosen by `UIDevice.userInterfaceIdiom == .pad`; stored apart from the
phone's so one device's edits never move the other's.

**Languages** (`launcher/*.lproj/Localizable.strings`, `L10n.swift`): 224 strings of the
launcher (its pages, the disc import's messages, graphics and controls settings, errors) in
English, Brazilian Portuguese, Spanish, French, German, Italian, Japanese and Simplified Chinese,
looked up with `Bundle.main.localizedString` (`L10n.text`, `L10n.format` for numbers); the key is
the English text. The C side's disc messages use `NSLocalizedString` too. None of the game's own
text is translated.

## What was run

On the M1 MacBook Air (8 GB, Xcode 26.5, the iOS 26.5 Simulator), from this branch and the core
parts the Windows build of the branch made (`wasm2c dhd_core.wasm -n core --num-outputs=8`,
checksums checked on the Mac):

- **The Simulator app**: `build_ios.py --simulator --rebuild-core --wasm core_*.c`: SDL3 for the
  Simulator 33 s, the Rust library cold 155 s, the game's C with 4 jobs, then Xcode. It builds.
- **The Simulator run**, logs only (no screenshots, no input): a simulator device made for the lane
  ("DHD Lane 224", iPhone 17 Pro Max, iOS 26.5), the app installed with `xcrun simctl install`,
  the disc cloned into the app's `Documents/SCUS-97177.iso` (`cp -c`), started with
  `xcrun simctl launch` and game switches as `SIMCTL_CHILD_DHD_*`:
  - 60 s, `DHD_SKIP_MOVIES=1`: the game starts on the disc in Documents, the native renderer on the
    Simulator's Metal GPU ("Apple iOS simulator GPU", WebGPU, dual-source blending, Fifo), the iOS
    Metal surface 2868x1320, resolution scale 2 (up to 4x allowed), Native VU on (its loops drawn
    by the native renderer), SDL audio out (Core Audio, 48 kHz), the memory card in `saves/card1`,
    the shared touch overlay laid out for a 153.6 x 70.7 mm screen with the home indicator's 3.2 mm
    strip and the sensor housing's 9.9 mm band on the left; the menus at 54 fps while pipelines
    were still built (first 30 s).
  - 240 s, the macOS smoke test's menu route (`DHD_NAV`, `DHD_RACE_SYNC=1`, no memory card):
    through the menus (the live aspect went to 16:9 on the way and back to 2.17:1 after:
    `widescreen: aspect 1.7777` then `2.1727`) to a race, GO at field 3210, then about three
    minutes of racing: every 30 s line 58.1 to 59.9 fps of 60, low 1 %
    16 to 58 fps, the game thread 3-5 ms a field. "touch: the overlay takes the touches" in the
    race. No fault, trap or device loss.
  - Without the launch's own capture of stderr the log is `Documents/dhd.log` (with stderr sent to
    a file, the tools' rule keeps it there, as on the desktop).
- **The device app and the unsigned IPA**: `build_ios.py --unsigned --rebuild-core --wasm core_*.c`
  (the Rust library for `aarch64-apple-ios` cold, the game's C, Xcode without signing): no
  warning; `package_unsigned.py`'s check passed; the IPA is 13.6 MB. `package_release.py
  --platform ios --ipa ... --elf SCUS_971.77` passed every check (game data, the version, servers
  and paths, the retail-bytes scan with the core library exempt, unsigned) and wrote
  `DownhillRevived-0.8.0-ios-experimental.ipa` (14.2 MB), latest.json's `ios` entry and
  SHA256SUMS. Not installed on a device (no Apple ID here).
- The first Simulator build warned that ring's C objects in the Rust library were built for the
  SDK's iOS 26.5, not the app's 17.0 (the `cc` crate's default): rustgs now sets
  `IPHONEOS_DEPLOYMENT_TARGET` for the iOS targets and keys its cache on it.

Not checked here (no screen): what the overlay draws and where, taps on it, the disc screen and its
document picker, the keyboard for a room code, a P2P room, backgrounding and coming back, a real
iPhone or iPad.

## Decisions

- Authorship: his commits carry `ribeirogustav <ribeirogustav@users.noreply.github.com>`, the
  address most of his commits carry (one carries his Mac's local address, which is left out).
- The app icon is his (the app needs one); it sits in `port/ios/project/` because the repository
  ignores every `assets/` folder (game data's name), which kept it out of the first commit.
- No external display: his TV scene belonged to his launcher's SwiftUI scenes; the bridge keeps
  one phone view. A later lane can bring it back on the shared launcher.
- `DHD_TOUCH=android` on iOS rather than a new value: the core's settings pages only ask "a phone?";
  a new word would need core changes for no gain.

## Surprises

- His fork's findings are numbered 207-217 on its main and 180-183 on the pull requests' branches;
  both collide with ours. Their content is folded in here.
- `.gitignore`'s `assets/` rule ignores `port/ios/assets` silently: `git add port/ios` left the
  icon out without a word.
- The branch's macOS build rebuilt the Rust library cold (its sources changed) and the link then
  warned that ring's C objects were built for macOS 26.5, the Mac's SDK, not the game's floor
  11.0: the `cc` crate's default without `MACOSX_DEPLOYMENT_TARGET`. 0.8.0's Mac build log has no
  such warning (its library was cached from an earlier build). rustgs now sets the deployment
  target for the Mac as for iOS and keys its cache on it.
- On Windows a test that makes a symbolic link in a temporary folder on the build drive fails with
  "The file or directory is corrupted and unreadable" (WinError 1392); the test is skipped on
  Windows (the IPA is made on a Mac).

## Result

iOS builds from this branch: the Simulator app and the device app, its unsigned IPA and the
checked release IPA (`DownhillRevived-<version>-ios-experimental.ipa`), on a Mac from the Windows
build's core parts. In the Simulator the app starts the game on the disc in its Documents, draws
with the native renderer on Metal, lays out the shared touch controls and races through the
game's menus at 55-60 fps by its log.

Checks:
- Windows: `python tools/check.py port -j 6` OK on the branch's last code commit; the seven state
  traces (`boot`, `race`, `race2` to `race6`) against `main-524948ba-ref-v3`: 7 of 7 PASS (600,
  5000 and 5 x 14000 rows equal). The shared changes do not change the game on other platforms.
- macOS: `build_host64.py --release --target macos-arm64` from the branch builds without warnings
  and its smoke run (the 0.8.0 release's route) reaches GO after 11 s and races for 60 s.
- Android: `port/android/build_apk.py --wasm build/wasm/dhd_core.wasm` builds the debug APK (not
  installed: no phone in this lane).
- `port/ios/tests`: 23 tests, 17 run on Windows (6 need a compiler named `cc` or a Mac); a step
  of `check.py quick` now.
- The repository's push guard (tree, added lines, commit messages): clean.

Left for later lanes or the owner: the on-screen run (what the overlay shows, taps, the disc
screen and its picker, the keyboard, a P2P room, backgrounding); a real iPhone or iPad (an Apple
ID to sideload); tilt steering (CoreMotion); an update notice like Android's; the external
display; the shared mobile launcher, per-button size and place, tablet defaults and languages
(notes above); his cross-axial dead zone for physical pads (the owner's call).
