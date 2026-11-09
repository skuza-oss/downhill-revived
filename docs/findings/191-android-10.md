# 191: Android 10 (API 29) as the APK's minimum

Status: in progress

Branch `feat/android-10` from `main` ac62c78d (0.7.14), started 2026-11-09. This file is the lane's
plan while it runs and its finding when it merges.

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

## Decisions

## Surprises

## Result
