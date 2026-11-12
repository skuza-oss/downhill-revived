# 219: three checks that passed when they should fail

Status: in progress

Branch `fix/check-tools-false-pass` from `main` c5f839a2, started 2026-11-12. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

An outside review of the 0.8.0 candidate (findings 3, 4 and 5 of its game/host report) found three
checks that can say OK when the thing they guard is broken. This lane makes them stricter and
proves each with a test that shows the old false pass now fails. Tools and their tests only: no
game code, no patches.

1. `port/tools/stack_escapes.py --check` passes when a unit's `-gstabs` compile fails (the unit is
   a `WARN` and drops out of the analysis). A failed compile must fail `--check`, except the units
   listed (with a reason each) in a reviewed file; a listed unit that now compiles fails too.
2. A reviewed `fixed` overrun passes when its unit has any port patch (`check_fixed` only tests that
   the unit is a key of `patches.PATCHES`). Each `fixed` entry must be tied to proof that the fix is
   in the patched source; a test removes each fix in turn and the check fails for all nine entries.
3. `port/tools/options_walk.py` prints `ok` for a row whose picture did not change. A row that
   should open a page needs positive evidence (changed picture, a stable second capture, back to the
   list afterwards); only named in-place rows may keep the picture, with their own side-effect
   check; a dropped key is retried once and says so. A test switch withholds the Cross press and
   the walk must fail.

Acceptance: `python tools/check.py port` and `check.py docs` green; `docs/TESTING.md` updated.

## Progress

- [x] 2026-11-12: finding reserved
- [ ] 1 stack scan compile failures
- [ ] 2 fixed entries tied to the patched source
- [ ] 3 OPTIONS walk evidence
- [ ] docs/TESTING.md, final checks

## Decisions

## Surprises

## Result
