# 151: Faster builds on every platform

Status: in progress

Branch `perf/build-speed` from `main` 6d232ea, started 2026-11-04. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Shorter builds of the PC port for Windows, Linux, Android, the Mac and the browser: cold, warm (no
change), after a one-line change in a game unit under `src/`, and after a one-line change in a
`port/hle` file. Measured before and after, per platform and per step (core, wasm2c, C, Rust,
link, packaging).

Nothing changes for players:

- the core module (`build/wasm/dhd_core.wasm`) has the same sha256 before and after for the same
  source;
- a Windows boot (600 fields) and race (5000 fields) state trace of the new build match the base
  build's (`port/tools/state_trace_run.py`);
- `python tools/check.py quick` and `port` pass;
- the shipped code's optimisation flags stay as they are.

Out of scope: anything on the phone (PGO profiles are made by a phone run; here only the build
side of using one), the matching build (`tools/`), the online server.

## Progress

- [ ] 2026-11-04: baseline per platform and step
- [ ] content-defined chunks of wasm2c's C, so an unchanged function hits the object cache
- [ ] one compile pool for the host's and the module's C
- [ ] the core cache's key without re-hashing unchanged files
- [ ] after-measurements, state trace, checks

## Decisions

## Surprises

## Result
