# 253: Native VU finished: the near batches clipped on the VU thread

Status: in progress

Branch `perf/native-vu-finish-253` from `main` 7987ff50 (release 0.8.5 plus lanes 247 and 249),
started 2026-11-15. This file is the lane's plan while it runs and its finding when it merges.
Someone who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Finding 249's work list: on finding 156's route, 1.78 million of 16.4 million batches (11 %) of
the translated VU1 loops still run on the emulated VU, nearly all because a vertex of the batch is
below the near bound and the loop's own clipper has to cut its triangles (a GPU's clipping draws
something else there, finding 164).

Target: those batches run natively with the clipper's own result (the triangles the VU program
kicks), so that in a race no batch of a translated loop runs on the emulated VU, or only a short
list with a reason each. Then a survey beyond the race (menus, rider select, replays, split
screen, other tracks, the poster maker) of what still reaches the emulated path. Deleting the
emulated VU1 path is not this lane.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` 7987ff50's; pictures against Native VU off no worse than the base's
(`native_vu_pictures.py`, near geometry included); the Android unit tests; the throttled Mi 11i
(`t612`, 4x) no slower than the base with Native VU on.

Out of scope: the deletion of the emulated path, gameplay, the picture's content.

## Progress

- [x] 2026-11-15: finding reserved

## Decisions

## Surprises

## Result
