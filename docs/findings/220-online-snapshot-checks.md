# 220: another game's snapshots checked before they are used

Status: in progress

Branch `fix/online-snapshot-checks` from `main` c5f839a2, started 2026-11-12. This file is the
lane's plan while it runs and its finding when it merges. Someone who has only this file and the
git log must be able to pick the lane up.

## Goal and acceptance

A rider record in a STATE bundle (`port/hle/state_bundle.h`, `port/hle/puppet.c`
`receive_record`) comes from another player's game. The owner check stops a game from touching
riders that are not its own, but nothing stopped a broken or modified game from sending bad values
for its own riders (a host owns every computer rider). Public rooms exist, so a receiver must hold
against any bytes. The outside review of 0.8.0's online code found six places (its findings 1, 2,
3, 5, 6, 8); lane 221 fixes its 4, 7 and 9. Receiver-side checks only: the wire format and the
bundle version (8) stay.

1. Racing-line node indexes only within the loaded track's nodes.
2. The State offset only one of the States a rider has.
3. Every number in a record finite and sane, checked before anything (the pad mirror) changes.
4. Stamps from the future: a snapshot or HOLD stamped far ahead neither freezes a rider nor makes
   honest snapshots stale.
5. The SFU flood limiter reads the bundle version from one shared definition.
6. The ground push is not undone by the slide's acceleration limit.

Done: each with a unit test; `python tools/check.py quick` and `check.py port` green; a boot and a
race state trace equal to the reference build; lane 202's replay bench not worse; one live
two-game race through a P2P room on a local server with no refused records.

## Progress

- [ ] 2026-11-12: finding reserved

## Decisions

## Surprises

## Result
