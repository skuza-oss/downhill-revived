# 239: Slow phones start at suitable graphics settings

Status: current
Checked: 2026-11-14

Branch `fix/slow-phone-defaults` from `main` b1d5e515, started 2026-11-14.

## Goal and acceptance

Issues #101 and #95: known minimum and below-minimum Android chips start at 1x
with Performance Mode, only for settings never written by the player. Unknown
hardware, desktop and iOS keep their defaults. No phone use or PC speed benchmarks.
Gate: quick, port, docs, Android native/Java builds and tests, base-equal boot trace.

## Progress

- [x] Read the issue comments and #101's attached log; traced settings and pacing.
- [x] Implement conservative classes, missing-key defaults and tests.
- [x] Document phone checks for the lead; no phone accessed.
- [x] Finish quick, port and docs gates; Android builds and tests; base-equal traces.

## Decisions

Use GPU families and SoC identifiers, never phone models. Keep written keys,
including environment aliases, and persist automatic choices as ordinary settings.

The table is `port/render/settings/src/phone.rs`: minimum = Mali-G52 MC2,
Adreno 610; a bare G52 needs a known MT6769 / Helio G80/G85/G88 SoC. With no GPU
name, known MT6769 and SM6225 / Snapdragon 680/685 identifiers suffice. Below =
PowerVR GE8320/GE8322 and Adreno 306/308/505/506. Other names, ambiguous G52 core
counts, and multiple adapters stay unchanged. No numeric GPU ranking.

Android queries Vulkan adapter names without a device or surface, before the launcher
shows settings; reads `ro.soc.model`, as the startup log already does. Missing settings
are saved in one settings transaction. Any written effects key protects the whole
Performance Mode group; resolution is protected independently. Subsequent starts
keep the saved choices. The direct game start calls the same helper. A pending text
file carries the one-time explanation into dhd.log after its initialization. The
existing Resolution hint explains the defaults in all eight languages. iOS never
calls the Android helper; its shared settings table and defaults are unchanged.

Research: issues #101/#95 and #101's attached log, findings 146-149, 156, 174,
195/197 (especially the Redmi 9's pixel cost), Android's
[Build reference](https://developer.android.com/reference/android/os/Build), and the
[Vulkan presentation specification](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html).
No external code copied. RAM and display size/refresh are diagnostic information,
not reliable chip classes; #101 has 7.6 GB RAM despite its minimum-class GPU.

## Surprises

No existing below-minimum dialog was found; PLAYING.md contains the advice.
The below-minimum automatic-choice log repeats that advice and points to Help.

The first host build found one unmatched import after the initial check commands
overlapped generated-file writes. Stopped the duplicate checks. The prescribed
`DHD_MODULE_CACHE=0` rebuild passed strict linking (147 host imports, none unknown);
replaced this run's incomplete cache entry with that verified output. No core
source change or relaxed import check was needed.

#101 uses `fps=60 pacing=just-in-time vsync=on present_mode=mailbox display=90.00 Hz`.
`present/surface.rs::frame_latency` requests latency 2 (three buffers), avoiding the
two-buffer half-refresh trap. Mailbox takes the newest pending image at a refresh.
60 on fixed 90 Hz necessarily alternates one/two refresh holds, not perfectly even
16.7 ms presentation; it is not capped to 45. No pacing change warranted by this log.

## Result

Implemented on this lane's branch.
Settings tests pass (38 tests, including class selection, restart persistence, aliases,
custom effects and preservation of player edits). `check.py quick`, `check.py port`
and `check.py docs` pass. Android native library and Java/APK compile successfully;
`test_build_apk.py` and `test_disc_image.py` pass (UpdatePage, CrashReport, DiscImage/JNI).
All eight language files pass `tools/lang_check.py`. The launcher UI and handoff tests pass.
Boot (600 fields) and race (5,000 fields) match the released v0.8.3 base exactly.
Both comparisons were repeated on the port gate's final rebuilt executable and pass.
No PC timing benchmark, Mac work, phone access, push or external post was performed.

Lead phone checks (no phone used by this lane): preserve the player's ini first.
On Mi 11i, test a fresh ini: 2x and Performance Mode off remain, with no automatic
choice line. On Redmi 9, test fresh and iso-only ini: launcher Graphics shows 1x
and Performance Mode on before PLAY; first game log explains both and their location.
Relaunch: no repeated choice line. Choose 2x and Performance Mode off, restart both
launcher and game: choices stay. Also test an old ini with explicit scale/effects
(including DHD_* aliases), and scale-only/effect-only ini: written choices stay,
only the untouched setting/group gets its default. Confirm the translated hint and
run a race, share the log. Restore each saved ini after testing.

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- Mi 11i (not a known weak GPU): the owner's settings stayed as they were. The weak-GPU defaults were not run
  on real hardware.
