# 244: remove the dead code the C audit proved

Status: current
Checked: 2026-11-14

Branch `cleanup/dead-code-c` from local main 65f94597, started 2026-11-14. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Remove seven small, proven dead items (audit items L01-L04, L06-L08), one commit each, no
redesign. Not in scope: L05 (the browser address fold) and the audit's "suspected" list.
Gates: `check.py quick`, `port`, `server`, `docs`; the browser build; boot and race state
traces equal to the base build.

## Progress

- [x] L01 `net_kick`, `client_kick`, `net_local_slot` (the server's kick handling stays)
- [x] L02 `raster_module` in `port/render/src/exact/pipelines.rs`
- [x] L03 snapshot macro override scaffolding and `native_snapshot_free`
- [x] L04 ignored `--renderer` argument (`port/web/build.py`, `port/tools/web_gs_replay.mjs`)
- [x] L06 `isValidCode` in `online/server/src/protocol.ts`
- [x] L07 `online/server/AGENTS.md` address sentence scoped to SFU rooms
- [x] L08 `put8`, `dvalue`, `web_q_vu_ready`

## Decisions

Each item is searched again on this branch before it is deleted; an item something now uses is
skipped and noted here.

## Surprises

## Result

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- `check.py server` covers the removed `isValidCode`; no server deploy needed.
