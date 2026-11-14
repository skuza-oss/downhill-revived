# 235: Shared launcher rules

Status: current
Checked: 2026-11-14

Branch `cleanup/shared-launcher-rules` from `main` 584ce03f, started 2026-11-14.

## Goal and acceptance

Three rules each written once instead of per platform (cleanup audit A04, A06, the phone-scale
half of A07): which links the launcher opens, the phones' update lookup, the phones' largest
resolution scale. No gameplay change. Done when `check.py quick`, `port` and `docs` pass, the
desktop launcher builds and its tests pass, Android's native library, Java and unit tests pass,
and the iOS host tests that run on a PC pass. The iOS app, phones and the Simulator are the
lead's proof after the merge. Out of scope: the disc check (lane 234), the minimum OS versions.

## Progress

- [x] A06: `port/host/launcher_link.h` (`launcher_link`) decides `open_link` for the desktop
  (`src-tauri/launcher_link.c`, C ABI), Android (`Native.launcherLink`, JNI) and iOS
  (`launcher_calls.c`); one fixture set, `port/host/test_launcher_link.c` (test_runtime host tests).
- [x] A04: Android's direct-start notice (`android_update.c`) calls `phone_update_find_source`
  (`phone.c`), which also names the winning source for its log line; daily stamp, `auto_update`,
  SDL thread and JNI notice stay in `android_update.c`. Header comment rewritten.
  Test: `port/host/test_phone_update.c`.
- [x] A07 (scale): `NATIVE_PHONE_SCALE_MAX` lives in `port/render/include/dhd_limits.h`; the
  renderer clamp (`gpu_rust_shared.inc`) and both phone snapshots (`scale_max`) read it.
  `LauncherBridge.PHONE_SCALE_MAX` and `LAUNCHER_PHONE_SCALE_MAX` are gone. Still 6.

## Decisions

- Links: the strictest rule that opens every link the page sends. The page sends the aliases
  `discord` and `kofi` (`ui/app.js` footer), the repository home (Help), the releases page (notes
  fallback), release pages from validated update results, and the disc problem's More link
  (`blob/main/docs/findings/100-other-releases.md`, desktop `disc.rs`). So the rule is
  the two aliases plus `https://github.com/skuza-oss/downhill-revived` followed by end, `/`, `?`
  or `#`. Differences resolved:
  - all three took any URL under `https://discord.gg/`, `https://ko-fi.com/skuza*`,
    `https://github.com/skuza-oss/` and `https://downhill-revived.pages.dev/`; none of these is
    sent by the page except through the aliases, so they are refused now;
  - the desktop refused nothing inside an allowed prefix; iOS refused space, quotes, `<>` and
    `\`; Android refused nothing outside the repository and rewrote a bad repository link to
    releases/latest (`UpdatePage.safe`). Now all refuse whitespace, controls, quotes, `<>`, `\`
    and dot segments (`..`, `%2e`, WHATWG URL: percent-encoded dots are dot segments), and
    Android refuses a bad link instead of opening another page.
- The update's URL rules (`net/update_check.c`, `update.rs`, `UpdatePage.safe` for the release
  page) stay separate: they are a narrower contract.
- Android's notice and the launcher kept slightly different failure log prefixes (`update:` /
  `launcher:`); `phone_update_find_source` takes the prefix so both logs read as before.

## Surprises

The iOS build copies the host sources; shared headers are found through the existing port-root
header search path (`host/...`, `render/include/...`), the same way the PC fixture compiles them.

## Result

Filled in at the merge.

Checked:
- 0.8.3 gate on 128a743a: `check.py port -j 6`, `check.py docs` and `check.py server` OK; the seven state traces
  (`boot`, `race`, `race2`..`race6`) equal `main-524948ba-ref-v3`.
- The phone launcher's update line showed "Up to date, version 0.8.2" (the shared update lookup); the iOS Simulator's
  launcher answered its snapshot. The link rule's buttons were not tapped in the gate.
