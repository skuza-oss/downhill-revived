# 233: Remove finished experiments and migration leftovers

Status: current
Checked: 2026-11-14

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

- B04: removed folded-Z requests, delayed mapping, deferred picture/present submission
  and submit prepoll/statistic. Kept the default separate probe encoder, asynchronous map,
  poll, newest-completed-copy selection and the C take-before-start publication order.

- B02: removed the offline whole-memory hash diagnostic, exclusive network XXH64 API,
  fixtures and build wiring. Repository-wide references verified before removal. Kept
  state_trace.c's comparator hashes, GS texture/CLUT hashes (including xxhash.h), SHA-256
  for disc/executable identity and Rust hashes: these have independent live consumers.
  `host_stack_ranges_copy` stays (the browser hosts and the host64 fake module still call it).
  The credits' line on `port/net/hash.c` went with it.

## Decisions

Keep the real renderer methods and ordinary native probe timing. No new implementation.
Source-reference audit only; no external code adapted.

## Surprises

- `native_replay` does not compile on the base 584ce03f either: its `COMMAND_HEAD` (168) no longer
  matches `DhdrCommand` since the native VU merge grew the struct. Not touched here (abi.rs is
  unchanged by this lane); it needs its own fix before the replay can serve as a check again.
- The GS golden for this PC's current driver (`10de-2484-32.0.16.2114-dx12`) has no `meta.json`,
  so `gs_golden.py` refuses; no golden exists for this driver yet.
- Both fixed by finding 242 (the command head from the struct; `gs_golden.py adopt`).

## Result

On Windows: `check.py port` OK (it runs the quick and docs tiers too); state traces equal to the
base 584ce03f for `boot` (600 rows), `race` (5000), `race` with `DHD_EXACT_PROBE=0` (the native
Z readback answers the game, 5000) and with `DHD_EXACT_PROBE=2` (5000); `native_vu_pictures.py`
capture of both builds, `compare --same`: every field identical. `test_gs_wgpu` (in the port
tier) passed. Not run: the GS golden and `native_replay` (both above). The iOS edits (A09, the
disc_copy.h and sdl3.py comments) are unbuilt: a Mac build and a simulator background/foreground
cycle remain with the lead.

Checked:
- 0.8.3 gate on 128a743a: `check.py port -j 6`, `check.py docs` and `check.py server` OK; the seven state traces
  (`boot`, `race`, `race2`..`race6`) equal `main-524948ba-ref-v3`.
- The gate's builds and runs (Windows, Android, Mac, iOS Simulator and the unsigned IPA) built and ran with the
  stand-ins and the old hash log gone; PC <-> Mac races in SFU and P2P rooms ended with equal results.
