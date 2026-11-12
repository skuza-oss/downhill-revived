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
bundle version (8) stay; nothing new is shared between games.

1. Racing-line node indexes only within the loaded track's nodes.
2. The State offset only one of the States a rider has.
3. Every number in a record finite and sane, checked before anything (the pad mirror) changes.
4. Stamps from the future: a snapshot or HOLD stamped far ahead neither freezes a rider nor makes
   honest snapshots stale.
5. The SFU flood limiter reads the bundle version from one shared definition.
6. The ground push is not undone by the slide's acceleration limit.

Done: each with a unit test; `python tools/check.py quick` and `check.py port` green; a boot and a
race state trace equal to the reference build (`main-524948ba-ref-v3`); lane 202's replay bench
not worse; one live two-game race through a P2P room on a local server with no refused records.

## Progress

- [x] 2026-11-12: finding reserved (64ec07b8)
- [x] a record's bytes read and written in one header, `port/hle/puppet_record.h` (2ffcf4ec; no
  change in behaviour)
- [x] 1, node indexes (ddae0da1)
- [x] 2, the State list (0c952faa)
- [x] 3, numbers (5782cca3; bounds widened to every recorded track in a5f76e9b)
- [x] 4, stamps and the hold's own release (c8f65e34)
- [x] 6, the ground push (6858440b)
- [x] 5, the SFU limiter's version (b99dc07c)
- [ ] bench after, state traces, live race, check tiers

## What each check is, and where its bound comes from

All in `port/hle/puppet_record.h` (pure functions; tests `port/hle/test_puppet_record.c`,
`port/tools/test_rider_states.py`), used by `puppet.c` `receive_record`, `hold` and `held_step`.
A refused record is dropped whole, counted per rider, and logged once per rider and race
(`puppet: tick T slot S: a record refused (WHY); more are only counted`, and at the race's end
`puppet: slot S: N records refused in the race`). A refused HOLD has its own line and count.

**1. Racing-line nodes.** The graph `D_0029EA20` holds 0x800 nodes of 0x20 bytes (0x10000 bytes;
`func_00117658` loads them and walks 0x800) and its own fields after them; its node count is at
+0x1004C, and the game's accessor `func_001177F8` refuses an index at or past it. `node_at`
accepted any index under 2117 (the room up to the rider array). Now `node_at`, `node_index` and the
receiver take the loaded count (clamped to 0x800): a record whose node or next node is neither
0xFFFF nor under it is refused whole. Refusing the whole record, not only its line part: an honest
game never sends such an index (its `node_index` names only loaded nodes, and both games load the
same track's file), so a record that does is from a broken sender and none of it is trusted.
The sending side was confirmed: `node_index` returns 0xFFFF for any pointer that is not a loaded
node, so a receiver on the same track refuses none of an honest game's records.

**2. The State.** `func_0012FE38` calls the current State's `Exit` and the new one's `Enter`
(virtual), so the offset must be a State object. The rider's constructor `func_0012B620` builds 18
State objects inside the rider and lists them in its own table at +0x6394..+0x63D8; the race's
setup `func_0012BF88` walks those 18 (calls each one's set-up) and starts each rider in +0x5FE0.
`PUPPET_STATES` is that table, each named (riding +0x4ED0, crashed +0x4EE0, the grid +0x5FE0, the
attack +0x5E90, punch +0x6248, stagger +0x6268, push-off +0x6298, the crash States +0x62E0 and
+0x6320, ...). Completeness: `test_rider_states.py` reads the constructor's table from
`src/unsorted/0012B620.c` and requires the list to equal it, and it collects every rider offset
named on each `func_0012FE38` call line in `src/` and in the ten lines before it (where the dynamic
callers pick their candidates: `func_0019C008`, `func_0017D1E0`, `func_001EB1D8`, ...): every one
in the States' span is on the list (all 18 are named by some switch), except +0x4EF0, the thrown
rider's body, which `func_0012E940` pushes just before its switch. The bench's recordings (every
rider of three tracks, 75 s each) show 12 different States, all on the list. And a debug check
runs everywhere: `take` (which builds every record sent and every recording) logs once per rider
and race when this game's own rider is in a State not on the list
(`puppet: tick T slot S is in State +0xX, not on puppet_record.h's list`). No such line appeared in
the bench's recordings or the live race below.

**3. Numbers.** Every float finite; then each bound, from the bench's recordings of tracks 0, 1
and 2 (`puppet_bench.py record`, every rider each race step, 75 s; 121366 records), widened:

| field | recorded | bound |
|---|---|---|
| position, target and next target, each axis | -11240 to 7200 | 200000 |
| speed | up to 263 a second | 5000 |
| angular velocity | up to 22 rad a second | 500 |
| orientation rows | length 0.911-1.000, orthogonal to 3e-7, determinant over lengths 1.0000 | length 1 +- 0.5, determinant over lengths at least 0.5 |
| power | 0.750-1.223 | +-10 (the input uses only 0.5-2) |
| turn scale | 0-1.04 | +-100 (used only 0-1) |
| sideways offset (and the one eased toward) | -21 to 20 | 1000 |
| wheel contact | 0-2 | 0-2 (the game's three values) |
| crash kind | 0-2 | 0-10 (`func_00155E78`'s callers pass 1-10) |
| checkpoints passed | 0-14 | 0-35: `func_0019F210` reads plane [passed + 1] of 36 at +0x220 and writes the rider's +0x79B0[passed]; past 35 it reads and writes outside both |
| attack | 0-4 | 0-4 (punch, kick, check, stick, bottle) |
| bottles | 0-3 | 0-3, or 255 (the cheat's endless -1 on the wire) |
| pad action words 0-24 | 0-1 | 0-255 |

The pad holds no floats: its words are integers the game compares (buttons 0 or 1 from
`func_0017EB00`, the analogue amounts 0-127 after `func_0017E1C0`'s square and clamp, 0-128 from
`func_0017DFB0`); the other words are timers, counters and flag bytes (up to 16777473 in the
recordings) that the game only compares, so they get no range. The pad update is applied to a copy
of the rider's mirror and the mirror changes only when the whole record passes
(`puppet_record_take`), so a refused record leaves no trace.

**4. Stamps.** A stamp is placed on this race clock through the shared server clock; its error
is the two games' clock offsets (each within half a trusted round trip, `port/net/client.c`
`clock_sample`: tens of ms, a few hundred on the worst links) plus what a race clock trails
beyond the pacing lag, which `sync_clock_excess` already takes off. A snapshot or HOLD stamped
more than 1 s (`PUPPET_AHEAD_MAX`) ahead of this game's race clock is refused. A HOLD is taken
only when it is about this game's newest snapshot of the rider: not more than 0.5 s after it
(`PUPPET_HOLD_MATCH`: bundles can be lost just before a stop) and with a stop (`quiet`) no longer
than its stamp is old here plus 1 s; a newer snapshot already here ends it, as before. And a held
rider is released once snapshots have been arriving again for `SYNC_COAST` (3 s) by this game's
clock, whatever their stamps (`PuppetHold.fed_at`, `puppet_hold_fed_long`), so no stamp can keep
it held.

**5. The SFU's limiter.** `sfu.rs` read a bundle's stamp only when byte 10 of the frame was 6;
bundles are 8 since finding 202, so every snapshot gave no stamp and the allowance for newer
snapshots over 240 a second (finding 175) was off. `port/render/net/src/bundle.rs` now takes the
version, the header's length and the stamp's place from `port/hle/state_bundle.h` when the crate is
built (`include_str!` and a const parser; a missing enumerator is a build error). Real bytes:
`test_state_bundle.c` makes a host's two bundles and a HOLD with the game's own code
(`puppet_record_write`, `pad_snapshot_pack`, `state_bundle_pack`, `state_hold_make`) and fails while
`port/hle/state_bundle_fixture.h` is not exactly that (it prints the new file); the net crate's
tests read the fixture. Looked for elsewhere: the native client (`port/net/client.c`) and its tests
use `STATE_BUNDLE_VERSION`; the star (`port/render/star`), the browser's `net.js` and the server
never read the version; a P2P test helper in `p2p.rs` wrote 6 (now the shared constant).

**6. The ground push.** `sync_step` took the into-ground part out of the wanted move, then eased
the correction's own speed (`slide`) toward that move with a limited acceleration, so the old
slide's into-ground part survived (-20 of -30 a step later at 60). Now, on the ground, the kept
slide loses its into-ground part too, and the move is the slide; that part is dropped, not owed,
like the move's own, so what is applied is exactly what is no longer owed.

## Decisions

- A record with a bad node is refused whole, not only its line part (1, above).
- The State list is a fixed table checked against the source, not read from the rider's own table
  at +0x6394 at run time: the brief asked for named entries, and the test ties the two together.
- The pad check is a range on the action words, not a float check: there are no float words.
- A snapshot a second older than the newest no longer clears an online rider's ring (`feed`):
  that reset is for a replayed file starting over; online it made a late packet put the rider
  where it said, and with stamps from the future it would have done so for every honest one.
- HOLD refusals are counted apart from record refusals: a HOLD more than 0.5 s after the newest
  snapshot can come from an honest game whose last bundles were lost; its riders then coast as on
  a quiet link.
- The bounds of 3 were first taken from track 0 alone (100000 units, row length 1 +- 0.25 with dot
  products); tracks 1 and 2 go to -11240 and have rows as short as 0.911, so they were widened and
  the dot products replaced by the determinant (a5f76e9b).

## Surprises

- `func_0019F210` indexes two arrays with the checkpoints passed, a byte on the wire: a record could
  make it write past the rider's +0x79B0 array into the next fields. The review did not name it.
- The orientation rows are not of unit length: down to 0.911 in a recorded race (still orthogonal
  and right-handed).
- `feed`'s reset for a replayed file ran online too (Decisions).

## Open

- **The browser has no HOLD sender** (the review's note, checked): its race frames go out only from
  the game's worker (`net.js` `host_service_net_send_frame`, called by the game), and nothing on the
  page sends HOLD. When a browser game stalls (a long frame, a background tab), the others ride its
  riders on their last inputs for up to `SYNC_COAST` (3 s; its computer riders on their own AI),
  then brake them to a stop; when its snapshots come again they catch up by riding, glide while its
  clock catches up, or are put in place when far off. The page's star ticks every 10 ms on its own
  (`p2p.js`) and could send HOLD from the last bundle's stamp and the time since it; not done here.

## Result

Filled in at the merge.
