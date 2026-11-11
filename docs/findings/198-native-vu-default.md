# 198: Native VU is the default

Status: in progress

Branch `feat/native-vu-default` from `main` 652a734b (release 0.7.15), started 2026-11-11. This
file is the lane's plan while it runs and its finding when it merges. Someone who has only this
file and the git log must be able to pick the lane up.

## Goal and acceptance

The owner's decision (2026-11-11): Native VU (finding 194: the VU1 programs' batch loops run as GPU
shaders; dhd.ini `native_vu`, override `DHD_NATIVE_VU1`) becomes the default on every platform
except the browser. The emulated VU path stays only as the automatic fallback (a pipeline the
driver refuses, `dhdr_native_meshes`) and as the browser's path; a later lane deletes it once
players' logs show no fallbacks. This lane measures first, then flips the default.

- Measure on the Redmi 9 (Helio G80, Mali-G52 MC2), the low-end phone: finding 156's route at 2x
  (and 1x), Native VU off against on, interleaved, one debuggable APK of `main` 652a734b. dhd.log
  with it on: no fallback line, no crash. This also answers finding 172's open question (was the
  Redmi's 8 % at 2x heat?).
- `native_vu` on by default: the settings crate, REVIVED SETTINGS, the launcher; hidden in the
  browser as before. Players who already have a dhd.ini get the new default unless they chose off.
- The label loses "experimental"; the switch stays while the emulated path exists.
- Acceptance: `python tools/check.py port`; boot and race state traces equal to `main`'s; pictures
  (`port/tools/native_vu_pictures.py`): the new default identical to `main` with the option on, the
  fallback (`DHD_TEST_NO_MESHES`) identical to `main` with the option off; the settings tests.
- Out of scope: deleting the emulated path, the near-bound batches still run on the VU, the
  browser's Native VU (lane 199).

## Progress

- [x] 2026-11-11: finding reserved
- [ ] the Redmi 9: 2x and 1x, off against on
- [ ] the default: settings crate, REVIVED SETTINGS, launcher, browser left out
- [ ] existing dhd.ini files
- [ ] docs: AGENTS.md, STATE.md, finding 194, PLAYING.md, the release texts
- [ ] `check.py port`, state traces, pictures, settings tests
- [ ] the integrated GPU and the Mac, off against on

## Decisions

## Surprises

## Result
