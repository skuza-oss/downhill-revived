# 233: Remove finished experiments and migration leftovers

Status: in progress

Branch `cleanup/finished-experiments` from `main` 584ce03f, started 2026-11-14.

## Goal and acceptance

Remove B01, B02, B04, A09 and A10 from the cleanup audit without changing default behavior.
Run `python tools/check.py quick -j 6`, `port -j 6`, and `docs`; compare boot and race
state traces with 584ce03f and run native renderer replay/picture checks. iOS builds remain
for the lead on a Mac. Matching sources and historical findings are out of scope.

## Progress

- B01: removed four migration stand-ins. Verified the real inherent display accessors,
  upload and surface fill/read methods exist; Rust method resolution already chose these.

- A09: removed the unread iOS release option and empty foreground forwarding trio;
  configuration still follows `args.debug`, and other lifecycle callbacks remain. iOS unbuilt.

- A10: corrected the Android direct-start notice, iOS import source and UIKit comments.

## Decisions

Keep the real renderer methods and ordinary native probe timing. No new implementation.
Source-reference audit only; no external code adapted.

## Surprises

None so far.

## Result

Validation pending; Mac/iOS compilation and lifecycle proof remain with the lead.
