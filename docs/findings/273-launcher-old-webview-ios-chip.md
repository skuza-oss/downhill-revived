# 273: the phone launcher on old WebViews, and a clear message on a too-old iPad/iPhone

Status: current

Branch `fix/launcher-old-webview` from `main` bbe4acc5 (0.8.10), started 2026-11-21. This file is
the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

- Issue #114 (Android 10, Snapdragon 665): the launcher shows its page with the desktop's window
  buttons and nothing reacts to a tap: the page's script did not start. Find the constructs that
  need a newer Chromium than the player's WebView, lower the page's minimum, and show a plain
  "update Android System WebView" message (with the Play Store button) when the WebView is still
  too old. The WebView's version goes in the logs.
- Issue #113 (iPad Pro, A10X, iPadOS 17): the app dies when the core's code runs. Check the chip
  before any code built for the A12 floor (finding 267) can run and show one plain page instead.

Done: `check.py quick` and `check.py docs`, the launcher's tests, the debug APK, the iOS app build
and `ipa_audit.py`. No game state changes (no traces).

## Progress

- [x] 2026-11-21: the page runs on Chromium 80; `test_launcher_ui.js` refuses newer syntax, APIs
      and CSS; `ui/boot.js` (ES5) shows the WebView message or a failed start's error; the
      `webview` link alias; Android logs the WebView's version (a9ad9107)
- [x] 2026-11-21: the iOS chip check (`port/ios/app/chip.c`, its rule tested in `test_runtime.py`'s
      `ios-chip`) and its page (56146dd7); PLAYING.md's minimums (5542bee1)
- [x] 2026-11-21 checks on the PC: `check.py quick` OK (`ios chip: PASS`, `launcher links: PASS`),
      `check.py docs` OK, the launcher's three node tests, `test_build_apk.py`, the debug APK
      (0.8.10, 34.0 MB, `assets/launcher/boot.js` in it); the floor test fails on 0.8.10's app.js
      (`replaceChildren`) and style.css (`inset`)
- [ ] owed: the iOS app build, `ipa_audit.py`, the Simulator's forced page
      (`SIMCTL_CHILD_DHD_IOS_CHIP_TOO_OLD=1`) and a disassembly of the three initializers (the Mac
      was offline for the whole lane); a real A10 run; a start on an old WebView (no such emulator
      image on the PC: only API 36 and 37; none downloaded)

## What the page needed (before this lane)

Read from `port/launcher/ui` (every script, `style.css`) against the first Chromium with each:

| Construct | Where | First Chromium | Effect on an older WebView |
|---|---|---|---|
| `?.`, `??` | app.js, controls.js, stick.js, bridge.js | 80 | below 80: every script but i18n.js fails to parse |
| `Element.replaceChildren` | i18n.js's `applyStatic` (the start's first drawing), 22 places in app.js | 86 | below 86: `start()` throws right after the snapshot, before the platform's layout is set |
| `Array.prototype.at` | app.js, the patch notes' headlines | 92 | the home page's news throw |
| canvas `roundRect` | stick.js (desktop Controls tab) | 99 (Safari 16) | the stick picture throws |
| CSS `inset` | `.page` | 87 | pages shrink to their content |
| CSS `translate:` | the select's arrow | 104 | arrow misplaced |
| CSS `:has()` | the pad's Tabs hint | 105 | rule dropped |
| CSS `:focus-visible` in one list with `.pad :focus` | focus ring | 86 | the whole rule dropped |
| unprefixed `appearance` | selects | 84 | native select look |

So the page's real minimum was Chromium 86 to start at all (92 for the news, 99 for the desktop's
stick picture). The rest it uses is older than 80: optional catch binding (66), `Array.flat` (69),
`Object.fromEntries` (73), regex lookbehind (62), `Intl.PluralRules` (63), `Promise.finally` (63).

**#114's cause, as far as the code shows it**: the symptom (the HTML and CSS drawn, the desktop's
title bar and window buttons, no tap reacts) is the page before `start()` has set anything; with
any Android System WebView below 86 the start throws there (`replaceChildren`), below 80 nothing
parses. Android 10 shipped with WebView 74-77, and a phone whose Play Store updates are off can
still carry one of those. The player's version is not known
yet: the next report's Share log says it.

## Decisions

- **Floor Chromium 80, not lower.** 80 is where `?.` and `??` parse; they are in about 40 lines
  and read well. Below that would mean rewriting all of them for WebViews that the Play Store updates
  for free; the safety net tells those players to update instead.
- **No transpiler, no parser dependency.** The test is a denylist of the constructs newer than 80
  a page like this could plausibly use (each with its first Chromium), run over the scripts with
  comments stripped, plus CSS rules; boot.js gets an ES5 check (no arrows, let/const, template
  strings, spread, async, `?.`). It is not a full parser, but it catches every construct this lane
  found and the common next ones.
- **Flex `gap` stays (Chromium 84).** On 80-83 the page works but items sit without their
  spacing. Rewriting every flex container's spacing as margins for those four versions was not
  worth it; the floor is where the page *runs*.
- **The safety net is a script file, not inline**: the content security policy has no inline
  script. `boot.js` loads in `<head>`, before the body is drawn: it adds `html.touch` on Android,
  iPhone and iPad (app.js sets it again from the caps), so a phone never shows the desktop's layout
  while loading; the title bar and Patch notes are hidden in the HTML until the caps show them.
- **The Play Store button goes through the bridge** (`open_link` `webview` on the port the app
  posts; `launcher_link.h` resolves it to the WebView's `play.google.com` page, which Android
  opens in the Play Store): no JavaScript interface, the Java side stays without UI.
- **iOS shows the native message view, not the web page**: the launcher's web page answers its
  calls through `launcher_calls.c` and the shared C and Rust it calls, built for the A12. The
  plain `message_view` main.m already has needs none of that.
- **What the chip check asks** (`chip.c`): the features LLVM's apple-a12 adds to the A10 that a
  compiler uses by itself or the code asks for: LSE (every atomic operation), FP16, RCPC (acquire
  loads), JSCVT, FCMA, by `hw.optional.arm.FEAT_*` (older names as fallback). Only a 0 counts as
  missing; a name the system does not know is never a refusal. iPadOS 17 runs on exactly one class
  below the floor: the A10 / A10X (iPad 6, 7, the 2017 iPad Pros), which report 0 for all five.

## What runs before the iOS check, and why it is safe on an A10

The check is the first line of `main()`. The app's own sources (`App/`, `Host/`, `Render/` in the
Xcode project: main.m, chip.c, launcher.m, launcher_calls.c, run_record.c, metal_bridge.m) are
compiled by Xcode with no `-mcpu` (build_ios.py's project.yml sets none): clang's default for
arm64 iOS, the apple-a7 (ARMv8.0). Only `libdhd_ios.a` (build_host64.py, `-mcpu=apple-a12`) and its
Rust part carry the A12 floor; the Rust part is built without `target-cpu` (rustgs.py), so it is
rustc's default for `aarch64-apple-ios`, also the apple-a7.

Before `main()`: dyld and Apple's libraries (built for every device the OS supports), then the
static initializers of the app's image: from `libdhd_ios.a` `guest_memory.c`'s `reserve_early`
(the guest memory's early reservation: `mmap`, `mach_vm_region`, plain integer C), `vif.c`'s
`publish_vu1` (one pointer store), `gpu_rust.c`'s `renderer_line` (`fprintf` and the Rust
`dhdr_version`). None of the A12-only instructions a compiler picks by itself can be in them: LSE
and LDAPR come only from atomics, FP16 only from half-precision types, FCMA/JSCVT only from
intrinsics, and these functions use none (grep of guest_memory.c, guest_memory_os.h,
guest_place.c and vif.c: no `atomic`, `_Float16`, `__fp16`, `_Complex`; `renderer_line` is one
`fprintf`). The
player's own report agrees: on 0.8.10 the A10X opened the launcher's page, which runs all of
them, and died only at the disc check (`host_disc_check`, from the A12 library). A disassembly of
those three functions in the built app is owed with the Mac build.

After the check, a too-old chip reaches only main.m's `message_view`: no `ios_run_record_start`,
no launcher (its calls), no lifecycle hooks (`ios_lifecycle_*` reach the renderer), and a document
opened in the app is refused.

## Surprises

- The page's floor was set by `replaceChildren`, not by the syntax: on WebView 80-85 everything
  parsed and the start still threw at its first drawing.
- The Mac was not reachable on the network for the whole lane (its `.local` name unresolved, no
  ssh at its old addresses), so the iOS part is unbuilt here.

## Result

Filled in at the merge.
