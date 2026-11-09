# 194: Native VU (experimental), a Graphics setting on every platform

Status: in progress

Branch `feat/native-vu-option` from `main` cdee456, started 2026-11-10. This file is the lane's
plan while it runs and its finding when it merges. Someone who has only this file and the git log
must be able to pick the lane up.

## Goal and acceptance

The owner's decision (2026-11-10): ship the native VU1 path (VU1 microprograms run as GPU shaders
instead of being emulated; the native lane's five findings on the local branch `native/vu-gs`) as an
experimental option on every platform, **off by default**.

- Bring the native work onto this branch from today's `main`: merge `native/vu-gs` (26 commits on
  `main` 9ecb8e1), keeping what its last finding kept and leaving out what it dropped. `native/vu-gs`
  itself is not rewritten.
- One setting: **Native VU (experimental)** in REVIVED SETTINGS > Graphics, next to the Performance
  group; a dhd.ini key, default off, in the settings crate, the game's page and the launcher's
  settings, the way finding 183 added its settings. `DHD_NATIVE_VU1` stays as its override (its
  per-program values for testing).
- Safety: a native pipeline or shader a driver refuses (PowerVR, Mali, Intel, WebGPU limits) sends
  that draw path back to the emulated one for the session, with one dhd.log line saying why; the
  game never closes over it. Finding 188's Intel dual-source rule holds in the native path.
- Platforms: Windows fully; macOS on the owner's Mac (build, a harness run with the option on and
  off); Linux build; Android release-signed APK, the phone run with the option on and off; the
  browser built, the option working or hidden there with the reason.

Acceptance:

- Option off: byte-for-byte today's `main`: boot and race state traces equal to `main`'s, the
  picture unchanged.
- Option on: the game's state unchanged (state traces), pictures within the native lane's tolerance
  (per field MAE <= 0.1, at most 0.1 % of pixels off by more than 8) against the emulated path.
- Timing: Windows RTX at 8x, off against on; the Mi 11i on the t612 profile at 2x, off against on
  (the recipes of findings 146-149 and the native lane's).
- `python tools/check.py port` passes; the release APK builds.

## Progress

- [x] 2026-11-10: finding reserved

## Decisions

- **Both paths are kept while native is experimental** (the owner, 2026-11-10): the emulated VU1
  path stays beside the native one, an explicit exception to the single-implementation rule (root
  AGENTS.md, decision 099), as for the P2P experiment (180). The plan: once native is proven on
  every platform (pictures, state, speed, drivers), it becomes the default and the emulated draw
  path for the translated loops is deleted.

## Surprises

## Result
