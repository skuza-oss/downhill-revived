# 242: Two test tools fixed, one Share-log builder, one phone disc-import rule set

Status: in progress

Branch `cleanup/shared-phone-rules` from `main` 7906a93f, started 2026-11-14.

## Goal and acceptance

Cleanup audit A03 and A08, plus the two broken tools finding 233 noted. (1) `native_replay` builds and
runs again, its command head taken from one definition; `gs_golden.py` usable on this PC's current
driver. (2) The Share log file assembled once in shared C for Android (JNI) and iOS. (3) The phones'
disc-import rules (room, completeness, the copy's check, constants, superseded picks) once in shared C;
the document adapters stay per platform. No player-visible change except where stated here.
Gate: `check.py quick`, `port`, `docs`; Android native + Java build and tests; the iOS Simulator app
and `port/ios/tests`. No run on the owner's phone (the lead checks it after the merge).

## Progress

- [x] Part 1: the two test tools (and `replay_gs.py` in a worktree)
- [ ] Part 2: one Share-log bundle (A08)
- [ ] Part 3: one disc-import rule set (A03)

## Decisions

- **native_replay**: `COMMAND_HEAD` is `offset_of!(DhdrCommand, texture)` (abi.rs pins the layout at
  compile time), so it follows the struct. The trace version goes to 2 (head 184 bytes). Version 1
  (168-byte head, before finding 168's mesh constants) is refused: only the retired C recorder wrote
  it and no such trace is kept, so a v1 reader would have nothing to read (NTRACE.md, "Versions").
  Its `--selftest` (31 cases, synthetic traces, no GPU, no window, 2 s warm) is in `check.py port`;
  a fresh worktree builds the tool once (about 3 minutes).
- **gs_golden**: a golden's directory is keyed on, and compare pins, the adapter's driver (and WARP
  for test-gpu). Measured on this PC: driver 32.0.16.1742 -> 32.0.16.2114 and WARP 10.0.26100.9278 ->
  .9818 left `test-gpu`, `test-gpu-ee` and `native-transfers-stress` byte-identical. The pins stay
  (the tool's rule: no check relaxed); new `gs_golden.py adopt --from OLD GROUP[=SOURCE]...` makes
  the new driver's golden only from groups a current run proves identical to the old golden, hard
  links (no extra disk), a meta.json naming both drivers and the groups left out, written last.
  Any differing group refuses the whole adopt. Selftest cases cover differ, other device, same
  driver, success, left-out group and existing golden. Not in `check.py` (needs a GPU run and a
  local golden); `docs/TESTING.md` says when to run it.
- **replay_gs.py**: refused every output in a worktree (`build/` is a junction, the prefix was
  resolved and the root was not); both sides are resolved now.

## Surprises

- The other golden groups are stale for reasons other than the driver: the replay goldens' captures
  (`gs-g+0.gs`, `gs-g+3600.gs` of a scratch folder) are gone and the meta has no capture hash, and the
  two capture sets kept beside the goldens give other pictures (`replay` 1020 / 222186 pixels,
  `replay-native` 1768); `native-transfers` and `-presented` gained scenes since (finding 176, 196:
  38 pictures, golden 33, renumbered). These need a recording from the oracle; they are left out of
  this PC's adopted golden (`10de-2484-32.0.16.2114-dx12`: test-gpu, test-gpu-ee,
  native-transfers-stress, 71 items).

## Result

Filled in at the merge.
