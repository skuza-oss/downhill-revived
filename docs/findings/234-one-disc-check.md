# 234: One boot-disc check

Status: current
Checked: 2026-11-14

Branch `cleanup/one-disc-check` from `main` 584ce03f, started 2026-11-14.

## Goal and acceptance

Use the callback-based C disc checker everywhere; preserve its whole-volume acceptance rule,
platform messages and Android import policy. No gameplay or copy-engine changes.
Gate: check.py quick, port and docs; launcher build/tests; Android native/Java build and tests;
iOS host disc/copy tests; boot trace equal to base. Device proof belongs to the lead after merge.

## Progress

- [x] Read the three parsers and callers; keep port/host/disc_check.c.
- [x] Replace launcher and Java walks with reader adapters and share synthetic fixtures.
- [x] Run local gates and document remaining platform proof.

## Decisions

C is already callback-based and used by game boot and iOS. Compile it into Tauri with cc;
Java passes a seekable FileChannel through JNI. Keep diagnostics in each platform adapter.
After an Android copy, a cut image gives the copy's own reason first (`completeness`: "the copy
stopped early, try again"), so the shared check never hides why an import failed.
Research: cc build-script/FFI documentation, https://docs.rs/cc/latest/cc/ . No copied code.

## Surprises

The installed Android Studio runtime has javac but no JNI headers. JVM tests accept
`DHD_JNI_HEADERS` for a separate JDK include tree. The production JNI adapter is compiled
and exercised by the host JVM, including positional reads preserving the provider copy cursor.
Fresh native host compilation misses the object cache; other lanes also share the build slots.

## Result

Implementation complete; quick, port and docs tiers pass, boot trace equal to base (600 rows). Desktop build and 30 enabled tests pass
(one existing ignored test). Android native/Java debug APK builds; packaging tests and JVM/JNI tests pass; the configured
real USA disc passes too. Shared C corpus and iOS disc-copy host tests pass.

Player-visible changes: desktop and Android boot pre-checks refuse cut-off images and zero
volume sizes, as game boot already does. Both now inherit C's handling of malformed CNF/root
metadata. Desktop's European/Japanese release table and messages survive. Android import
completeness/copy rules and their messages are unchanged; a seekable source that is itself
cut off is now refused before the copy ("incomplete") instead of after it.

Lead proof after merge:
- Windows: `python tools/check.py port -j 6`, `python port/tools/build_launcher.py`;
  launch the resulting desktop launcher, choose the real USA disc and PLAY. Choose a cut-off
  copy and other releases: refusal before PLAY, with existing named-release text.
- Android: `python port/android/build_apk.py --wasm build/wasm/dhd_core.wasm --jobs 6`;
  install the generated debug APK using the device workflow. Import a real USA disc from a
  seekable document provider and boot; refuse a cut-off dump/another release. Also import
  from a streaming provider, cancel/retry, and verify no incomplete copy becomes current.
- iOS Simulator (lead on Mac): `python3.12 port/ios/build_ios.py --simulator --wasm core_*.c`
  with the merged build's wasm2c parts. Open through the device workflow, import USA and boot;
  refuse cut-off/other-release files, cancel/retry an import. No iOS source behavior changed.

No device or Mac touched; no push/post/comment.

Checked:
- 0.8.3 gate on 128a743a: `check.py port -j 6`, `check.py docs` and `check.py server` OK; the seven state traces
  (`boot`, `race`, `race2`..`race6`) equal `main-524948ba-ref-v3`.
- The Mi 11i's launcher opened with the owner's disc accepted and PLAY started the game; the iOS Simulator's
  launcher snapshot reported no disc problem; every PC game of the gate booted from the same check.
