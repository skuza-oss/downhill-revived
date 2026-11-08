# 155: The Android build for every arm64 core (ARMv8.0)

Status: current

Branch `main` 738ad0b, 2026-11-05. Partly supersedes 091 (its `-march=armv8.2-a`).

## Goal and acceptance

Phones with a Snapdragon 680 (Redmi Note 11 4G, Xiaomi 2201117TY; issues #45 and #51) close at
once when the game starts. #51's shared log (finding 152) has the reason: `SIGILL`,
`ILL_ILLOPC`, in libmain.so under `SDL_main`, before the game's own start, on 0.7.10 and on a test
build alike. The Snapdragon 680's Kryo 265 cores are Cortex-A73 and A53: ARMv8.0. The Android
target was built with `-march=armv8.2-a` (finding 091: "every arm64 phone since 2018", which
budget chips disprove), so the compiler used ARMv8.1 atomics (LSE: `ldaddal`, `swpal`, `casal`)
those cores do not have.

Acceptance: the release libmain.so has no instruction beyond ARMv8.0 outside code that checks for
it at run time; the game still starts on the Mi 11i; its speed is measured against 0.7.10.

## Progress

- [x] 2026-11-05: `build_host64.py` android-arm64: `-march=armv8-a -moutline-atomics`
  (`-mtune=cortex-a78` kept). With outline atomics each atomic goes through a small helper that
  uses LSE when the core has it (chosen once at start) and a load/store-exclusive loop otherwise.
- [x] 2026-11-05: the release APK's libmain.so disassembled (NDK 29 llvm-objdump): 0.7.10 has 59
  LSE atomics in ordinary code; this build has 4, all inside the outline helpers behind their
  run-time test. No ARMv8.2 FP16, RDM or dot-product instructions in either. `crc32*` (47) is in
  both: optional in ARMv8.0 and present on the Cortex-A53 and A73, from code that checks for it.
- [x] 2026-11-05 the Mi 11i, throttled to the T612 profile (`phone_throttle.py --apply t612`),
  2x, uncapped, fields per second, three rounds in alternating order (the lead's bench scripts, the
  0.7.10 release APK against main 085117f's, both PGO with the same profile): race start
  0.7.10 66.3 / 67.9 / 68.6 (mean 67.6), this build 68.2 / 68.5 / 68.5 (68.4); a later window
  86.2 / 86.7 / 85.6 (86.1) against 85.9 / 85.9 / 86.7 (86.2). No loss: within the runs' spread.
- [ ] A Snapdragon 680 player confirms the game starts (issues #45, #51).

## Decisions

- **ARMv8.0 with outline atomics, not two builds.** One APK for every phone (AGENTS.md: one code
  path); the atomics keep LSE on newer cores through the helper. Rust's `aarch64-linux-android`
  target is ARMv8.0 already, and SDL3 is built with the NDK's defaults.

## Surprises

## Result

The Android build targets ARMv8.0, so phones with older arm64 cores (Snapdragon 680 and other
Cortex-A53/A73 chips) no longer close at start with an illegal instruction. On the Mi 11i the
speed is unchanged (race start 67.6 -> 68.4, a later window 86.1 -> 86.2 fields per second,
within the spread).
