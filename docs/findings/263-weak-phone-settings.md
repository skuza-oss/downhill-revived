# 263: weak phones start with Performance Mode, at 2x where it reaches 60

Status: current

Branch `feat/weak-phone-settings-263` from `main` e9bba855 (release 0.8.7), started 2026-11-16.
This file is the lane's plan while it runs and its finding when it merges.

Supersedes: part of 239 (the minimum class's resolution; the SoC identifier on Android 11 and older).

## Goal and acceptance

A new install on a phone with a weak graphics chip starts with the fast settings, picked from what
the game knows before the first race (the Vulkan adapter's name, the SoC identifier), with one note
to the player and a log line naming the rule. A player's written settings are never changed.
Unknown chips, desktop and iOS keep the usual defaults.

Acceptance: the settings crate's tests (names in, decision out; an existing dhd.ini unchanged),
`check.py quick`, `check.py port`, `check.py docs`, the Android library and APK build and
`test_build_apk.py`, boot and race state traces equal the base. On the Redmi 9: a first start with
its dhd.ini put aside picks Performance Mode at 2x and logs why; one timing run at 2x on lane
259's route.

Out of scope: new settings, new UI pages, a benchmark at the first start, any gameplay change.

## What existed (0.8.7)

- **Performance Mode** (REVIVED SETTINGS > Graphics, finding 183; the launcher's Settings >
  Graphics row of the same name): a preset over three switches, `shadows` (the riders' shadows and
  the sun's glow), `lens_flare`, `particles` (dhd.ini keys; `DHD_SHADOWS`, `DHD_LENS_FLARE`,
  `DHD_PARTICLES`). ON writes all three `0`, OFF all three `1`, a mix shows Custom. It does not
  touch the resolution (`scale`, default `2` on every platform).
- **Device rules already existed** (finding 239, shipped in 0.8.4): `port/render/settings/src/phone.rs`
  classed the Vulkan adapter name and `ro.soc.model` as minimum (Mali-G52 MC2, Adreno 610, a bare
  Mali-G52 with a known MT6769 / Helio G80/G85/G88 id, SM6225 without a GPU name) or below
  minimum (PowerVR GE8320/GE8322, Adreno 306/308/505/506). Both classes filled a missing `scale`
  with `1` and missing effects with Performance Mode on. The Android launcher (`LauncherActivity`
  -> `Native.phoneDefaults`) and a direct game start (`android_main.c`) call it; the explanation
  went to dhd.log only (`dhd-start-settings.txt`, read at the game's start) and to the Resolution
  row's hint.
- **It never matched the Redmi 9.** The phone's driver names its GPU `Mali-G52` (no core count:
  every lane 259 log says `gs: GPU Mali-G52 on Vulkan`), and Android 11 has no `ro.soc.model`
  (Android 12 added it; the phone's startup log says `unknown CPU`). A bare G52 with an empty SoC
  is ambiguous by design, so the phone the minimum spec is set on started with every effect on, at
  48.6 fields/s at 2x (finding 259). Finding 239 had not run on a weak phone.
- **iOS** never calls the Android helper: it starts with the usual defaults. Desktop the same.

## Decisions

- **The list is data**: `phone::RULES`, one row per rule with its adapter names, SoC ids, class,
  the resolution it fills (or none) and its source; `phone::rule()` returns the row, which the log
  line names. Exact names only; a row for "no adapter name" must name its SoCs (a test checks).
  No new chips were added: an unmeasured chip with no clear benchmark stays unchanged, and adding
  one would also fill missing settings on existing installs (see below).
- **The SoC on Android 11 and older is the board platform** (`ro.board.platform`) when
  `ro.soc.model` is empty, in the rules and in the startup log's CPU field. The Redmi 9's is
  `mt6768` (public device trees: `TARGET_BOARD_PLATFORM := mt6768` in the Redmi 9 `lancelot`
  BoardConfig.mk files on GitHub, and its stock firmware's `ro.board.platform=mt6768`). mt6768 is
  shared by the Helio P65, G70, G80, G85 and G88, which all carry a Mali-G52 MC2 (MediaTek's
  specifications), so `Mali-G52` with `mt6768` is the minimum class; a bare G52 elsewhere (Kirin
  810's MP6, Amlogic boxes) stays unchanged.
- **Mali-G52 MC2 phones keep 2x**: finding 259 measured the Redmi 9 at 2x with the three effects
  off at 61.1 fields/s in the race (race start 60.9, p99 22.1 ms, no frame over 50 ms), against
  48.6 with them on and 66.1 at 1x with them off. 2x with Performance Mode reaches 60, and the
  owner wants 2x on this phone (finding 174: 1x looks soft). Cost in looks: no riders' shadows,
  no sun glow or lens flare, no particles (snow, dust, sparks); the picture stays at 2x.
- **Adreno 610 keeps 1x** with Performance Mode, as finding 239 chose: not measured by us, and
  Notebookcheck's GPU comparison (Adreno 610 vs Mali-G52 MP2: about 85 % of the G52 MP2 over 22
  benchmarks, GFXBench Manhattan 3.1 offscreen 13.4 against 14.3 fps) puts it below the phone that
  just reaches 60 at 2x. Below-minimum chips (PowerVR GE8320/GE8322, Adreno 306/308/505/506) keep
  1x and Performance Mode (finding 239's sources: issues #95, #101, PLAYING.md's list).
- **Only missing settings are filled**, as in 239: a written key (aliases included) is never
  changed and any of the three effects keys protects the whole Performance Mode group. The game's
  SETTINGS saves each change as its own key, so a missing key is one the player never set. On a new
  install that is every key. An existing install on a listed chip has had its missing keys filled
  since 0.8.4, so the only installs this lane's change reaches for the first time are Mali-G52 /
  mt6768 phones whose effects keys were never written. Installs that got `scale = 1` from 239 keep
  it: it is a saved setting now.
- **The note**: the launcher's Play page shows one dismissable line ("Performance Mode is on for
  this phone. You can turn it off in Settings > Graphics.", in all eight languages) at the start
  that turned it on, beside the crash note (snapshot's `start_note`, said once like `last_exit`).
  The in-game menu calls the page SETTINGS on Android, the launcher Settings, so the line says
  Settings rather than REVIVED SETTINGS. No new page. The Resolution row's phone hint now says
  "Known slow Android phones start with Performance Mode, the slowest at 1x." The log line:
  `settings: this phone's GPU "<name>" (SoC "<id>") matched the rule "<rule>" (<source>): this
  install starts with Performance Mode on[, 1x]. Change in Settings > Graphics.`
- **Desktop and iOS**: the rule's code is shared (the settings crate), but only Android calls it,
  and the list names only phone chips (a test feeds desktop and Apple names: unchanged).

## Progress

- [x] 2026-11-16: read 183, 239, 257, 259, 174, 195; found that 239's rule misses the Redmi 9
- [x] 2026-11-16: the rule as data, the board-platform fallback, 2x for Mali-G52 MC2, the note
- [x] 2026-11-16: the rule (`c52f5db2`), the launcher's note (`6f51c59f`); checks below
- [ ] the phone proof on the Redmi 9 (owed: the phone was not attached)

## Surprises

- The rule this lane was asked to add had shipped in 0.8.4 (finding 239) and never fired on its
  target phone: the Redmi 9's driver drops the core count and its Android has no SoC model.

## Result

To be filled at the merge. So far:

- Settings crate tests pass (`rustgs.py test -- -p dhd_settings`; the phone tests: names in and
  the decision out, the Mi 11i's `Adreno (TM) 660`, desktop and Apple names unchanged, every rule
  with a source, an existing dhd.ini with written effects unchanged under every rule, the player's
  OFF never turned back on).
- `check.py quick`, `check.py port`, `check.py docs` OK; `lang_check` 0 errors; the launcher UI
  and handoff tests OK.
- The Android library (`build.py --target android-arm64`), the debug APK and
  `test_build_apk.py` pass.
- Boot (600 fields) and race (5000 fields) state traces equal the 0.8.7 release's `dhd.exe`.
- **Mi 11i** (not touched): its 0.8.7 release check logs `gs: GPU Adreno (TM) 660`, which matches
  no rule (a unit test feeds that name): nothing changes there.
- **Redmi 9: owed.** `adb devices` listed only the Mi 11i. Owed on it, with its dhd.ini put aside
  and restored (run-as, md5 `29bc64ce4be02af381b176f144f7eb79` as found): the startup log's CPU
  field shows `mt6768`; a first start logs the rule "Mali-G52 in a Helio P65/G70/G80/G85/G88" and
  writes the three effects `0` and no `scale`; the launcher shows the note once; one 2x timing
  run on lane 259's route with those settings (expected near 259's 61.1 fields/s in the race).
  If the phone's `ro.board.platform` is not `mt6768`, the rule does not match and nothing
  changes (the safe side); then add the value it reports.
