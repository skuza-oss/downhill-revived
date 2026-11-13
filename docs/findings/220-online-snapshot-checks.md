# 220: another game's snapshots checked before they are used

Status: current
Checked: 2026-11-13

Branch `fix/online-snapshot-checks` from `main` c5f839a2, started 2026-11-12. This file is the
lane's plan while it runs and its finding when it merges. Someone who has only this file and the
git log must be able to pick the lane up. Merged into `main` on 2026-11-12 (82a18a8a) and checked
there the same day ("Checked on the merged main" below). The two cases that check left open (a
game's own pause, late HOLDs) were fixed on `fix/online-pause-clock` from `main` 63a24aa9 ("Round
2" below).

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
- [x] protocol doc (0fb9cdce); `check.py port` OK on 0fb9cdce; state traces, bench and live race
  (below)
- [x] round 2 (branch `fix/online-pause-clock`): the time this race clock stood still in the
  stamps (d8811708); a HOLD's stop on the wall clock, against the newest snapshot by stamps
  (ce68d5f1); `race_menu_check.py --scenario alone-guest` and its refusal check; docs

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

## Lane 202's bench, before and after

`port/tools/puppet_bench.py matrix --tracks 0,1,2 --fps 60 --only
perfect,80ms,150ms,bad,send-hitch,send-behind`, the base build (c5f839a2) as `before` and this
branch (0fb9cdce) as `after`, the same truths and seeds, hidden desktop, deterministic. The bench
replays recorded riders through `feed` (not the receive path), so what can change it is item 6 (the
ground push) and item 4's HOLD rules (the hitch scenarios). Per run, before > after:

| run | error p95 | error p99 | sunk % | deep % | depth p99 | pops a min | jolts a min | catch-ups |
|---|---|---|---|---|---|---|---|---|
| perfect t0 | 0.29>0.29 | 0.72>0.72 | 0.29>0.29 | 0.04>0.04 | 0.28>0.28 | 1.1>1.1 | 0.9>0.9 | 0>0 |
| perfect t1 | 0.30>0.30 | 0.73>0.73 | 0.13>0.13 | 0.07>0.07 | 0.38>0.38 | 3.1>3.1 | 1.1>1.1 | 0>0 |
| perfect t2 | 0.25>0.26 | 0.47>0.48 | 0.14>0.14 | 0.03>0.03 | 0.19>0.20 | 0.0>0.1 | 0.2>0.5 | 0>0 |
| 80 ms t0 | 1.37>1.43 | 2.61>2.72 | 0.62>0.56 | 0.06>0.06 | 0.31>0.30 | 11.4>16.2 | 1.9>3.1 | 0>0 |
| 80 ms t1 | 1.65>1.64 | 3.23>3.16 | 0.36>0.42 | 0.08>0.07 | 0.38>0.47 | 36.2>36.6 | 1.5>2.2 | 0>0 |
| 80 ms t2 | 1.47>1.44 | 2.33>2.27 | 0.55>0.46 | 0.06>0.05 | 0.25>0.23 | 12.1>13.3 | 2.0>3.1 | 0>1 |
| 150 ms t0 | 3.04>3.08 | 5.72>6.08 | 0.80>0.74 | 0.04>0.05 | 0.30>0.32 | 94.0>100.2 | 2.9>5.3 | 4>13 |
| 150 ms t1 | 3.70>3.58 | 5.91>5.68 | 0.73>0.69 | 0.05>0.05 | 0.34>0.33 | 141.6>142.7 | 3.8>5.1 | 0>0 |
| 150 ms t2 | 3.19>3.39 | 5.21>5.79 | 0.66>0.52 | 0.04>0.03 | 0.25>0.23 | 110.3>127.3 | 4.7>5.4 | 10>8 |
| bad t0 | 3.99>4.15 | 7.90>8.31 | 1.05>1.08 | 0.09>0.12 | 0.34>0.35 | 134.8>147.5 | 5.4>7.0 | 18>20 |
| bad t1 | 4.54>4.52 | 7.35>7.31 | 0.74>0.86 | 0.03>0.03 | 0.32>0.37 | 214.2>204.6 | 5.3>4.4 | 9>7 |
| bad t2 | 4.27>4.04 | 9.18>8.20 | 1.13>0.88 | 0.20>0.20 | 0.35>0.31 | 185.9>179.3 | 10.6>10.9 | 30>25 |
| send-hitch t0 | 2.19>2.18 | 17.05>17.31 | 0.90>0.80 | 0.11>0.09 | 0.37>0.35 | 63.2>60.3 | 11.3>10.3 | 23>23 |
| send-hitch t1 | 2.13>2.14 | 14.61>14.60 | 0.36>0.43 | 0.08>0.08 | 0.33>0.42 | 76.2>71.3 | 12.3>11.4 | 9>12 |
| send-hitch t2 | 1.86>1.87 | 16.74>16.43 | 0.68>0.62 | 0.14>0.13 | 0.28>0.28 | 54.2>52.4 | 10.4>10.4 | 13>12 |
| send-behind t0 | 12.24>11.97 | 37.68>37.54 | 0.88>0.77 | 0.19>0.16 | 0.36>0.35 | 111.6>108.9 | 21.9>19.5 | 20>20 |
| send-behind t1 | 14.52>14.28 | 36.10>36.10 | 0.76>0.72 | 0.31>0.29 | 0.97>0.89 | 169.4>164.2 | 28.8>27.1 | 7>8 |
| send-behind t2 | 13.57>13.56 | 34.79>34.83 | 0.63>0.75 | 0.19>0.20 | 0.26>0.30 | 78.3>81.3 | 17.2>16.6 | 13>10 |
| sum of the 18 | 74.55>74.13 | 208.3>208.3 | 11.40>10.85 | 1.80>1.74 | 6.24>6.35 | 1498>1510 | 142.1>144.0 | 156>159 |

The errors are the same (sums within 0.6 %). Sinking is less: sunk steps 5 % fewer over all runs,
fewer in 10 of the 15 lossy runs. The cost is jolts on the plain 80 and 150 ms links: 1-2 more a
minute (16.8 to 24.0 over those six runs), and pops up to 15 % more on some of them; the stall and
catch-up scenarios have fewer jolts and pops. The jolts are the fix itself: at a landing the
correction's own into-ground speed (up to 30 units a second) now ends in that step instead of
pushing the wheels in for a few more, and the bench counts a drawn speed change over 36 units a
second that the truth does not make as a jolt. Kept, because a rider pushed into the ground is
the visible fault the lane was asked to remove and the review's arithmetic is right; the totals
are otherwise even. If the jolts show, ending that speed over two or three steps instead of one
would trade a little sinking back (not tried).

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
- Round 2: the time a race clock stood still is measured, not announced. Every way a race clock
  can stop (the game's own pause, a wait that rebases the clock, the end of a race, the touch hint)
  shows as wall time the clock neither stepped nor owes, so one measurement covers them all and no
  pause path needs a hook. It is measured on this game's own wall clock (no clock-sync error), at
  the moment the clock has counted to (`port_clock_wall_seconds`), so a long frame still running is
  not mistaken for a stop (it is owed time at the next step).
- Round 2: it goes into `hle_puppet_clock_excess`, the one place where the server clock and this
  race clock meet, not into the GO moment: the GO is also sent to the others (EVENT GO, for the log's
  skew line) and must stay the real one.
- Round 2: a stop is counted whole once it is past the normal lag (`SYNC_CLOCK_SLACK`), not less
  the slack: the first version subtracted it and left every later snapshot 40-50 ms "younger"
  than before the pause (age p50 105 ms before, 64 after, in the first live run).
- Round 2: the HOLD rule keeps the stamp check's 1 s and adds 0.1 s (`PUPPET_HOLD_SLACK`) for what
  this race clock may lag unseen (the slack the owed time leaves and the slack before a stop is
  counted). The margin was never the problem: both refusals found were this game's own clock
  standing still (below), so a wider margin alone would only have moved the edge.

## Surprises

- `func_0019F210` indexes two arrays with the checkpoints passed, a byte on the wire: a record could
  make it write past the rider's +0x79B0 array into the next fields. The review did not name it.
- The orientation rows are not of unit length: down to 0.911 in a recorded race (still orthogonal
  and right-handed).
- `feed`'s reset for a replayed file ran online too (Decisions).
- Round 2: neither late-HOLD refusal was clock-sync error. In the many-player room the host's
  race clock stood still for at least 1.1 s two seconds after its own finish (from race tick 5825
  to 5862, 0.6 s of race, its `netstats:` lines' wall times show about 2 s), while the bots' HOLDs
  counted on; before that its race clock had kept to the wall clock within 15 ms all race. In the lead's plain two-game race (no test
  link) both games stopped stepping at about race second 74.7, after both finishes; each one's
  network thread then sent HOLD (its game gave no bundle: a real stop) and each receiver, its own
  clock stopped too, saw a stop of 1.07 s against a stamp 0.01 s old. With the test link the
  bundles wait in `DHD_NET_LINK`'s queue, which only the race's steps release, so those HOLDs were
  never looked at: that is why the link "decided".
- Round 2: with a receiver's own stop in the mapping, the HOLD's stamp lands earlier than the
  snapshot it names did when it arrived; by race times the HOLD then looked older than the newest
  snapshot ("over", not taken). It is matched with the newest snapshot by their two server stamps
  now (`Puppet.newest_stamp`).

## Open

- **The browser has no HOLD sender** (the review's note, checked): its race frames go out only from
  the game's worker (`net.js` `host_service_net_send_frame`, called by the game), and nothing on the
  page sends HOLD. When a browser game stalls (a long frame, a background tab), the others ride its
  riders on their last inputs for up to `SYNC_COAST` (3 s; its computer riders on their own AI),
  then brake them to a stop; when its snapshots come again they catch up by riding, glide while its
  clock catches up, or are put in place when far off. The page's star ticks every 10 ms on its own
  (`p2p.js`) and could send HOLD from the last bundle's stamp and the time since it; not done here.
- **A sender's own pose after a crash at the finish line** (gate, 2026-11-13): the host's rider had
  crashed on the line (wheels about 2 units below the ground); 0.3 s after its finish its spin ran
  down, the orientation rows shrank to 0 over four steps, then one step had a row of length 3.5e12
  and a spin of 2.4e24, and the next was normal again. The guest refused that record (`a record
  refused (orientation)`), which is the check doing its work: nothing of it was used. Seen in 1 of
  95 own-rider recordings (both 0.8.0 gate runs and this cycle's verifications); whether the PS2 does
  the same in that crash is not known.
- **A HOLD more than `PUPPET_HOLD_MATCH` (0.5 s) after the newest snapshot, after a finish** (gate,
  2026-11-13): `a HOLD refused (stamp 78.916, newest snapshot 78.148, quiet 0.116 s)`, race second
  78.6, after both finishes (70 / 72 s): 0.77 s past the newest snapshot the host had taken of the
  finished guest. Refused, the rider coasts and then brakes (`SYNC_COAST`). The window is to be
  widened in the next release.

## Gate, 2026-11-13

The 0.8.0 gate's second run (main d5896f18) logged those two refusal lines, one each in two of four
two-game P2P races with no test link on one PC, both after the riders' finishes, results equal on
both games; every other log of the run (A2's 40, A3's 12) had none. The owner waived both for 0.8.0
(2026-11-13); both stay open above.

## Checks

- `python tools/check.py port -j 6` on 0fb9cdce: OK (check_docs, check_tree, build_wasm,
  test_runtime with `test_puppet_record.c` in the guest and under the address sanitizer on the
  host, `test_rider_states.py`, build_game, scan_retail, net_tests, test_gs_wgpu, stack checks).
  `rustgs.py test -- -p dhd_net -p dhd_star`: 38 + 12 passed (10 ignored: they need a local
  server).
- The fuzz: 200000 random and mutated bundles (bytes flipped, words overwritten with NaN, infinity,
  huge and random values, cut, grown, all random, or unchanged) through `state_bundle_check`,
  `puppet_record_read` and `puppet_record_take`: 99681 passed the bundle check, 60265 records were
  taken (every one sane), 39416 refused (the mirror unchanged each time); no sanitizer report.
- Each new test was seen to fail on the old code: the landing test of `test_puppet_sync.c` against
  the old `puppet_sync.h` ("never into the ground"), the SFU backlog test against the old line (240
  taken of 300).
- State traces against `main-524948ba-ref-v3` (same `state_trace_codeptrs.txt` and
  `state_trace_mask.txt`): `boot` all 600 rows match, `race` all 5000 rows match (PASS).
- The live race: `local_server.py -- online_race.py --hidden --lag --link
  delay=80,jitter=15,loss=1,seed=41 --a DHD_NET_CREATE=p2p --a DHD_NET_TEST_HITCH=20:800`, two
  games on this PC in a P2P room (a direct link), eight host computer riders, the guest attacking,
  75 s. Records refused: 0 on both games; HOLDs refused: 0; no State-list line. The host's 0.8 s
  stop at tick 1200: the guest held all nine host riders at tick 1222 and let all nine ride on at
  ticks 1258-1261 (2.3-11.4 units from their spots), as in finding 202's runs. `puppet_lag.py`: the
  guest shows the host's rider at error p50/p95/p99 0.16/0.99/1.90, its computer riders at
  0.35/2.35/15.32 (the stop's catch-up in the tail; finding 202's 80 ms races without a stop:
  0.38-0.42/2.35-2.45/4.3-4.9), sunk 0.6 %, 8 respawn placements and nothing else placed; four hits
  applied.

## Checked on the merged main (2026-11-12)

On `main` 3073462c (c5f839a2, then the merges of lanes 217, 221, this one and 219), by the lead's
verifier. Merged at 82a18a8a. The status stays `in progress` for one case below (a game's own pause
when alone).

- `python tools/check.py port -j 6` and `check.py server`: OK. The seven state-trace recipes
  equal the format-2 references made from 524948ba: 7 of 7 PASS.
- Two games on this PC (hidden desktop, a local server per race, computer riders on, P2P unless
  said, the 80 ms test link `delay=80,jitter=15,loss=1`): `online_race.py --lag` both finish (70 /
  72 s) and the finish window (host 60 s, guest DNF); all nine `race_menu_check.py` scenarios and
  `--lag` for `finished` and `timeup`: PASS; `mosh_round_check.py`: PASS; one race in a plain SFU
  room (`DHD_NET_CREATE=1`; the local SFU connected via UDP at the first try): no flood-limiter line
  (`member N sent over ... snapshot messages in a second`), snapshot age p50/p95 21-37 / 33-48 ms,
  the guest's view of the host's computer riders at error p95 2.10, results by time on both.
- Refusal lines (`a record refused`, `records refused in the race`, `a HOLD refused`, the State
  list's line) in every game log of those runs and of the PC <-> Mac runs below: none, except in
  `race_menu_check.py --scenario alone` (below) and the many-player room (next).
- The many-player room (`p2p_crowd.py --bots 7 --sfu-bots 1`: this PC's game as the hub, seven
  nettool bots, one of them over the room's SFU): all eight raced, setup to the countdown 41.1 s,
  every bot's result arrived, no freeze. The host logged seven refused HOLDs, one per bot, at tick
  5862 (race second 97.7): `a HOLD refused (stamp 74.97, newest snapshot 74.97, quiet 23.79-23.82
  s)`. The bots stop sending at their finish (75 s) and their client sends HOLD from its network
  thread 23 s later; the rule allows a quiet of the stamp's age here plus 1 s, 22.73 + 1.0 = 23.73 s,
  so this game's race clock stood about 1.1 s behind the server's then (a hidden-desktop game's
  hitch adds such a lag). Finished riders, so nothing shows; a real game keeps sending after its
  finish, so only nettool bots do this. Not zero, though.
- A host stop (`DHD_NET_TEST_HITCH=25:800`, 80 ms link): the guest held all nine host riders at
  tick 1523 and let all nine ride on at tick 1560 (3.7-8.1 units past them); none left held.
- PC <-> Mac (the Mac's own build of 3073462c), P2P rooms on a local server, host finishes at
  20 s, guest at 40 s, the host's first race alone; scored as finding 216's check was (same
  wrappers). Error p95 in units, ranges over the shared races:

| run | shared races | other player (host / guest view) | guest's computer riders | after the end: host sees guest / guest sees host / guest sees AI | drawn behind p95 | placed (not respawns) | snapshot age p50 / p95 | guest's finish in the host's log | jolts a minute | sunk % |
|---|---|---|---|---|---|---|---|---|---|---|
| PC hosts | 4 | 0.34-1.11 / 0.27-0.86 | 0.48-1.96 | 0.34-0.97 / 0.01-0.38 / 0.29-1.28 | 0-6 ms, 34 once | 0 | 11-30 / 18-40 ms | 4 of 4 | 0-7.2; one race 16.7 / 24.9 / 13.6 | 0.3-1.3 |
| Mac hosts | 4 | 0.32-0.61 / 0.28-1.14 | 0.41-1.64 | 0.38-0.71 / 0.02-0.54 / 0.27-1.57 | 0-7 ms, 18 once | 0 | 11-34 / 17-41 ms | 4 of 4 | 0-9.0 | 0.2-2.7; 9.1 once |
| PC hosts, `--force-turn` | 2 | 0.33-0.54 / 0.32-0.40 | 0.54-0.57 | 0.43-0.73 / 0.02-0.09 / 0.54-0.82 | 1-5 ms | 0 | 21-32 / 35-45 ms | 2 of 2 | 2.3-7.9 | 0.2-1.5 |
| Mac hosts, `--force-turn` | 2 | 0.41-1.07 / 0.27-0.41 | 0.45-0.65 | 0.54 / 0.00-0.01 / 0.48 | 1-4 ms | 0 | 23-35 / 36-50 ms | 2 of 2 | 0-8.0 | 0.2-2.7 |

  Jolts and sunk are each race's three views (the host's of the guest, the guest's of the host, the
  guest's of the computer riders). On c5f839a2 the same runs gave 0-11 jolts a minute with one race
  at 31 (13 and 19 in its other views) and sunk at most 3.0 %. About the same: the one race with
  more jolts here had a burst of catch-ups of the computer riders at race second 5.8 (14, 12-16
  units, while two other test games ran on this PC), as c5f839a2's first two-game race had; the
  9.1 % is the guest drawing the finished host parked at the finish 0.52 units into the ground
  (none deep; mid-race 0.0 %). `--force-turn`: the guest raced over the room's SFU in both runs.
  Results equal on both machines in every race; no `freeze:` line.
- One placement far from the owner (two-game hitch race): a host computer rider crashed again one
  tick after a respawn placement; the guest drew it frozen 274 units back for 0.37 s until the next
  respawn placement. The class of finding 202's "a reset placed 312 units off for 0.2 s"; the only
  one in these runs.

**Was open (fixed in round 2, below): a game's own pause left it refusing the others' records.** In `race_menu_check.py
--scenario alone` (the guest finishes at 12 s, the host, alone, opens the game's own pause at 30 s
for 5 s) the host logged `puppet: tick 1802 slot 1: a record refused (stamp ahead)`. The pause
stops this game's race clock while the others' stamps (the server's clock) go on, so after it every
record stands about as far in the future as the pause lasted, beyond `PUPPET_AHEAD_MAX` (1 s), and
is refused for the rest of the race (`hle_puppet_clock_excess` counts only the pacing lag, not a
pause). There it is harmless: the guest's rider is parked at the finish. With the roles swapped
(the host finishes at 12 s, the guest pauses alone 5 s while the host's computer riders race on; a
scratch variant of the tool), the guest refused every later record of the host and its computer
riders (`2462 records refused` for the host's rider, `1232` for each computer rider, in both
races), so on its screen the computer riders were never corrected again until the race ended. The
c5f839a2 build in the same run took them as about 5.1 s in the future (snapshot age -5125 ms) and
was no better: `puppet_lag.py` scores the computer riders after the pause as badly on both builds
(it compares by race time, which the pause shifts by 5 s, so neither number is the drawn error). So the case is older than this
lane, which turns it from "taken as future" into "refused"; the fix would add the time this game's
race clock stood still (its own pause) to the excess taken off the stamps, or measure the stamp
against the server clock's race moment instead of this game's race time.

## Round 2: a game's own pause, and late HOLDs (2026-11-12)

Branch `fix/online-pause-clock` from `main` 63a24aa9. No wire change, nothing new shared.

**1. After this game's own pause the others' records came from the future.** Online the race
menu does not pause (finding 215) unless no other human still races; then the game's own pause
stops this race clock while the server clock goes on. `receive_record` placed a record at
`(stamp - go) / 1000 - hle_puppet_clock_excess()`, and the excess (`sync_clock_excess`) only knew
the owed time, which a pause is not (the pause's menu frames take the clock's steps; nothing is
owed afterwards and nothing is caught up, finding 215). So after a 5 s pause every record stood
5 s ahead of `RACE_TIME`: taken as the future before this finding, refused after it, and this
game's own stamps stood 5 s in the past.

The rule now (`port/hle/puppet_sync.h` `sync_clock_stood`, used by `hle_puppet_clock_excess`):
the wall time since GO that the race clock neither stepped nor owes,
`wall - base - race - owed`, read on this game's own wall clock at the moment the clock has
counted to (`fps.c` `port_clock_wall_seconds`; `base` set at GO, `hle_puppet_go`), is time the race
clock stood still. A trail more than `SYNC_CLOCK_SLACK` past what already stood still starts a stop,
which then counts whole while it lasts; it is never given back. The excess is the owed part plus
that, so the others' snapshots, their HOLDs and this game's own stamps all follow: a pose another
game made "now" lands at this game's "now", and this game's poses are stamped with the server
moment they happened. Logged once a stop is over: `puppet: tick T: this race clock stood still S s
since GO`.

Every user of the server-to-race mapping, and what it needed:

| user | needed it? |
|---|---|
| `receive_record`: another game's snapshot onto this race clock | yes: through the excess |
| `receive_bundle`: a HOLD's stamp | yes: through the excess; and its match with the newest snapshot by stamps (item 2) |
| `online.c` `send_snapshots`: this game's own stamps | yes: through the excess (a paused game's snapshots now carry their server moment, not the pause in the past) |
| this game's HOLD (`port/net/client.c` `hold_check`) | nothing to do: it repeats the last bundle's stamp, which carries it |
| GO (`on.go_server`, EVENT GO, `go_skew`) | no: GO is a moment before any stop; its log line compares two GOs |
| the finish window, its HUD count and TIME UP (`lobby.finish_ends_at` against the server clock) | no: server time against server time; it ends on time whatever this game's pause (its race then closes from the pause through `leave_race`: read, not run) |
| the end-of-race waits (`end_held_at`, `RESULTS_WAIT`, `first_finish`) | no: this game's wall clock |
| hit events | no: no stamp; applied when they arrive |
| the Mosh round's two minutes | no: each game's own race clock by design (finding 204); a game paused alone ends its round that much later in wall time, and the others, finished, have sent their final scores |
| the spectate camera | no mapping of its own: it watches the puppets, which now land right |
| `test_fps.c`, the bench (`bench_behind`, `Delivery`) | no: their own simulated lag, not the online path |

What the others see of a paused game: when it stops, its network thread sends HOLD and they brake
its riders to a stop where they are (`alone`: the guest held the host's rider and its four computer
riders at tick 1823). Its first snapshots after the pause are stamped with their real server
moment, newer than the HOLD, so they release it (ticks 2135-2137, the riders 0.5-8.2 units from
their spots), and its riders are drawn at the present as riders that stood still for the pause.
Before, those snapshots were stamped the pause in the past and the others drew it that far back
for the rest of the race.

Cases, from the code (unit tests in `test_puppet_record.c` where marked):
- a pause before any record arrived (test): counted the same; the first record lands at the
  present. A pause before GO (the countdown) is no case: the mapping starts at GO.
- two pauses (test): both counted, each once (8 s for 5 s and 3 s).
- a pause that ends the moment a snapshot arrives (test): the frame that steps the race again maps
  it within a step of its age. Records arriving during the pause land at the paused moment; they
  are taken, and the first ones after the pause place the riders that moved meanwhile.
- a pause over the finish window's end: the window runs on the server clock, so the room closes
  the race on time; the stamps play no part.
- a hitch (test): owed time, caught up, not a stop; nothing is counted as stood still.
- a phone sent to the background: not this path. `present.c` `app_background_wait` holds the game
  thread inside a field; the next step finds the wall time owed (`clock_interval`), and online it is
  kept and caught up (finding 139), so it is in the owed part of the excess, as before. Read, not
  run.
- the browser's tab hidden: the game runs in a worker paced by a MessageChannel with
  `DHD_FPS_WALL=1`; a slower or stalled worker owes the time and catches it up, as on x64, so it is
  the owed part too (a clock rebased meanwhile would be counted as a stop). Read, not run; the
  browser still sends no HOLD (Open, above).

**2. HOLDs refused near a race's end.** The rule takes a HOLD whose stop (`quiet`, the sender's
wall clock) is no longer than its stamp's age on this race clock plus 1 s. Both refusals the checks
found were this game's own race clock standing still (Surprises): a stop measured against a clock
that stopped too. With item 1 the age is the wall clock's again. On top of the stamp check's 1 s
the rule now allows `PUPPET_HOLD_SLACK` (`puppet_record.h`, 0.1 s: the lag `sync_clock_excess`
leaves and the same again before `sync_clock_stood` knows a stop for one), and whether a HOLD is
about the newest snapshot is decided by the two server stamps (`Puppet.newest_stamp`), so a stop of
this race clock between them cannot make it look "over". Still refused: a HOLD a day ahead, a stop
far longer than its stamp's age, one more than 0.5 s after the newest snapshot (tests).

Can a real game send a HOLD long after its last snapshot? Yes: its network thread sends one every
0.1 s from 60 ms after its last bundle, up to 30 s, whenever its game gives none while it races: a
stall, its own pause when alone (5 s in the `alone` scenario), and the end of a race, when both
games stop stepping a few seconds after the finishes (the lead's run). Each is a real stop, so the
others take it and hold its riders where they are; for finished riders it changes nothing
visible. Taking it is right: refused, the riders would coast for 3 s and then brake anyway.

Checks (this branch, hidden desktop, a local server per run, P2P rooms unless said):
- `test_puppet_record.c`: the pause arithmetic (the cases above), the HOLD's edges (the crowd
  case), a HOLD during this game's own stop. Each new assertion fails on the code before (without
  the stood-still time a 5 s pause's records are refused; without the slack the crowd case is
  refused; by race times the HOLD is "over").
- `race_menu_check.py --link delay=80`, all ten scenarios (`alone-guest` new): PASS; each now
  fails on any refusal line in either game: none. In `alone-guest` the guest stood still 5.22 s
  (its log). The host's computer riders on the guest, live puppet error (`puppets off by`, units,
  p50 / p95) and snapshot age (ms, p50):

| | error | age |
|---|---|---|
| before the pause (0-30 s) | 0.37 / 1.15 | 116 |
| the 5 s after it (each computer rider placed once, 356-416 units off: 5 s of riding) | 0.44 / 3.86 | 124 |
| 35-45 s, 45-55 s, 55 s to the end | 0.44 / 2.55, 0.60 / 2.28, 0.44 / 1.75 | 125 |
| races without a pause after 35 s (`timeup`, `resume` guests) | 0.21-0.29 / 1.75-1.88 | 116 |

  Before this branch the same run refused 1232 records a computer rider (none corrected after the
  pause), and c5f839a2 took them as 5.1 s in the future. Ages after the pause are 8-9 ms over
  before: the stop is counted up to the frame the race steps again. `alone` (the host pauses): no
  refusal (before: from tick 1801 on); the guest held all five of the host's riders and released
  them at the host's first snapshots after the pause.
- The lead's plain two-game race (`online_race.py --hidden`, no test link, finishes at 70 / 72 s):
  both games again stopped stepping at about race second 74.7 and sent HOLDs; the host held the
  guest's rider, the guest all nine of the host's; HOLDs refused 0, records refused 0 (before: 1
  and 9 HOLDs refused).
- `p2p_crowd.py --bots 7 --sfu-bots 1`: eight racers, the countdown after 42.1 s, every bot's race
  and result; the host held the seven bots at their finish and raced to its own (1:35.03) and on;
  HOLDs refused 0 (before: 7), records refused 0.
- `race_menu_check.py --scenario alone-guest --sfu --link delay=80`, a plain SFU room (the local
  SFU connected over UDP): PASS, no refusal; the guest stood still 5.22 s; error p50 / p95 before
  0.36 / 1.70, the 5 s after 1.02 / 2.46, after that 0.70 / 2.16; age 122 ms before, 132 after.
- State traces against `main-524948ba-ref-v3`: `boot` all 600 rows and `race` all 5000 rows
  match (every new line runs only online).
- `python tools/check.py port -j 6` on 1470b78b (the last code commit): OK.

## Result

Every field of another game's record is checked before any of it is used, and a refused record
leaves no trace; no stamp can freeze a rider or stale out honest snapshots; the SFU limiter reads
the bundle's layout from the game's own header; a landing puppet is not pushed into the ground.
Offline play is unchanged (state traces equal). Merge, and the review's findings 1, 2, 3, 5, 6 and
8 are closed. Round 2: the time a race clock stood still (a game's own pause when alone, a race's
end) is in the mapping between the server clock and the race clock, so after a pause the others'
snapshots land at this game's present and its own carry their server moment; a HOLD's stop is
measured against the wall clock and matched with the newest snapshot by stamps. No refused record
or HOLD in any check of this round.
