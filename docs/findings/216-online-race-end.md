# 216: an online race's last seconds, the finish on every screen, a reset as a respawn

Status: current

Branch `fix/online-race-end` from `main` 1140b23f, started 2026-11-12. This file is the lane's plan
while it runs and its finding when it merges. Merged into `main` on 2026-11-12 (c5f839a2) and checked
there the same day ("Checked on the merged main" below).

## Goal and acceptance

The lead's PC to Mac check of 18f5e70c (finding 202, "PC <-> Mac") found three things:

1. In the about 2.8 s between the room ending a race and the race screen closing, the other
   riders drift 13-72 units (p95) from their owners: both games go on sending, neither receives.
   Finding 202 round 4's window in `port/net/client.c` (`race_frames_until`) never sees a frame:
   the transport drops them first.
2. In 3 of 4 Mac-hosted races the host's race screen never showed the PC guest's finish.
3. An owner's own reset to the track (a 364.8-unit jump, standing) was placed as `far`, not as a
   respawn.

Done: the drift reproduced on this PC in a P2P room made from the menus (two hidden games, a local
server, the 80 ms test link) with the live scoring extended to the seconds after the end (a column
of `puppet_lag.py`), and gone after the fix in at least four races (host and guest views, one ended
by the finish window, one where everyone finishes); the finish shown on the other screen in each; a
reset counted as a respawn; Rust and C unit tests for the rule; `check.py port` and `check.py docs`
green. Out of scope: the SFU from this PC (failing for an unknown reason), host stops' steps.

## Progress

- [x] 2026-11-12: lane started, finding reserved (87a229fe)
- [x] the live column: `puppet_lag.py` `end_report` (every rider from the room's end of the race to
  the race screen's close; printed by every `--lag` run) and `race_menu_check.py --lag` (a room
  made from the menus, traced and recorded, the race both play split out and scored) (a59edb3d).
  On the verifier's own PC to Mac recordings it gives their numbers again (Mac host race 1: the
  host draws the guest 38.05 / 55.96 / 57.51 units p50 / p95 / p99 after the end).
- [x] the drift and the lost finish reproduced on this PC (below), the cause found in five places,
  one rule (4820e9f5)
- [x] the reset as a respawn (2c8c3b9c); the race-closed log line names its race tick (e3b65ec4)
- [x] after numbers in five races (below); Rust (`rustgs.py test -- -p dhd_star -p dhd_net`), C
  (`port/net/build_tests.py`, `test_runtime.py`'s puppet sync) and browser (`check_net.mjs`) tests
- [x] `check.py port` OK on e3b65ec4 (the code's last commit); `check.py docs` OK on the last
- [x] merged into `main` (c5f839a2) and checked there by the lead's verifier: `check.py port` and
  `server`, the seven state traces, two-game races and the race menu's scenarios on this PC, PC <->
  Mac both ways with the after-the-end column (below)

## Where the frames stopped (every place) and the one rule

At the room's `ended` (and the finish window's `left` for every racer just before it):

1. `port/render/net/src/lib.rs` (the send thread): purged this game's queued STATE and EVENT
   frames at `ended`. Now only a `start` purges (a last race's frames must not go out with the next
   race's marker).
2. `port/render/net/src/sfu.rs` (the SFU association): `ended` set `race_open = false`, which
   refused `send`, dropped every received frame of the race, cleared the queued ones and reset the
   release (so nothing was written either); `left` removed the leaver from the slot map, so its
   frames were dropped. Now a frame of the latest race travels both ways while the race marker is
   that race's (until the next `start`); `race_open` keeps its other jobs (the setup, the release,
   the silence report, the TURN renewal between races); once the race is over every racer of its
   grid is stamped (`racers`), a finish-window leaver too.
3. `port/render/star` (`dhd_star`, the P2P star, direct and bridged members alike): routes without
   a race dropped the race at once (every frame then had the wrong marker, the hub forwarded
   nothing), and the finish window's `left` cut those racers. Between races the room also retries
   the SFU members' links, so a fallback racer's frames had no way out. Now an ended race keeps its
   routes (`Race.over`): its slots, every member's link as it was during the race (`race_link`),
   every racer carried; a running race still drops a racer who left (BACK, a dropped game). The
   next race's routes replace it. `race_start` and `sfu_expect` say nothing for an ended race.
4. `port/net/client.c`: a room snapshot with this member out of the race cleared the frames
   already received (the relay queue). Now only `client_back` clears them.
5. The browser's `port/web/www/net.js` (its own client): `ended` dropped frames both ways.

The server closes nothing at the end: the SFU subscriptions stay across races (finding 177) and a
P2P room's stale-channel rule applies only inside a race's group (`room.ts` `staleChannels`).

**The rule**, in one place: the game's client sends and takes an ended race's frames until it
leaves the race screen (`client_back`, i.e. online.c's `back_to_room`), at most
`RACE_FRAMES_AFTER_MS` (10 s; a race outside the room's menus never says so); net.js does the same.
The transports carry the latest race's frames until the next start and have no end rule of their
own. The old 3 s window is gone (its constant now 10 s and a backstop only).

## The finish not shown

Not a separate loss: the finish is an EVENT race frame (online.c `finish_step` sends it when the
rider crosses; `read_finish` logs "X finished in" and later makes the puppet cross), sent in the same
step as the room's `finish`. When the last racer finishes, the room ends the race at once: the
other game's routes (or `ended`) could arrive before the finisher's EVENT on its peer link, and the
star (marker gone) or the SFU (`race_open` false) dropped it. On this PC the host never logged the
guest's finish in the guest-last race, and the guest never logged the host's in the host-last race
(base runs below); with the fix both games log both finishes in every race. Nothing else was
needed.

## The reset counted as `far`

The verifier's case (live/p2p-80-1, guest's view of the host at tick 4109): the owner rode at 76
units a second, then stood 364.8 units away (no crash bit); the snapshot of the jump and the next
one arrived in the same step (the 80 ms link bunches them), so `owner_jumped` compared the newest
snapshot only with its predecessor, both after the jump, and the rider was placed as `far` (the
log's "speed 1" is the second snapshot's). The rule (`puppet_sync.h` `sync_owner_jumped`) now looks
at every pair from the last snapshot used to the one used now, whatever crash flags the owner's game
set; a respawn is placed at once on the owner's pose and logged as one. `test_puppet_sync.c` has
the live case. None of this lane's live races had such a reset (no `far` placement in any of them).

## Measured

Two games on the hidden desktop, a local server per race, a P2P room from the menus,
`race_menu_check.py OUT --lag --link delay=80` (computer riders on), base 87a229fe's game (built from
1140b23f) against 4820e9f5 + 2c8c3b9c + e3b65ec4. Columns: from the room's end of the race to the
race screen's close (about 2 s), error p50 / p95 / p99 in units; A the host, B the guest.

| race | build | A draws the guest | B draws the host | B draws the host's AI riders | finishes each logged |
|---|---|---|---|---|---|
| guest finishes last (20 / 25 s) | base | 12.59 / 54.85 / 58.50 | 0.04 / 0.04 / 0.04 (standing) | 3.33 / 46.60 / 83.47 | host never the guest's |
| | fix | 0.43 / 1.54 / 1.65 | 0.03 / 0.08 / 0.08 | 0.35 / 1.69 / 2.25 | both |
| | fix, again | 0.42 / 1.09 / 1.19 | 0.92 / 1.80 / 1.81 | 0.42 / 1.54 / 1.91 | both |
| host finishes last (30 / 20 s) | base | 0.01 (standing) | 0.73 / 5.18 / 6.30 (63 steps) | 5.91 / 20.44 / 22.81 | guest never the host's |
| | fix | 0.35 / 0.39 / 0.39 | 0.37 / 1.72 / 2.13 | 0.42 / 2.34 / 3.08 | both |
| finish window, guest DNF | base | 0.50 / 1.03 / 1.11 (52 of 120 steps scored) | 0.00 / 0.14 / 0.14 | 0.27 / 0.56 / 0.67 (52 steps) | |
| | fix | 0.17 / 0.81 / 0.87 (121 steps) | 0.00 | 0.07 / 0.09 (4 steps, the rest finished) | |
| finish window, host DNF | base | 0.35 / 0.42 / 0.43 | 0.60 / 1.14 (12 steps) | 0.13 / 1.68 / 3.57 | |
| | fix | 0.28 / 0.28 / 0.28 | 0.20 / 0.24 (14 steps) | 0.54 / 0.93 / 0.96 | |

During the same races the usual error (the whole race, `puppet_lag --riders`) is p95 1.0-1.8 for
the other player and 1.7-3.2 for the AI riders: after the fix the last seconds are within it. The
finish-window races drifted little in the base too (one rider standing finished, the DNF rider
riding on straight; fewer steps scored there): the races that end at a finish are where it shows. The finished guest of the host-DNF race stops tracing 0.2 s after the end in both
builds (its race screen is already the watching one), so its column is short there.

## Decisions

- **The rule lives in the game's client, not in a transport timer.** The transports only know
  which race a frame belongs to (its marker); the game knows when its race screen closes. One
  backstop (10 s) for races that never say so, instead of the 3 s that was shorter than the
  screen (2 s plus the screen's leaving) in the verifier's runs.
- **A running race still drops a racer who left; an ended one carries everyone.** A racer out by
  BACK or a dropped game sends nothing more, and its rider is the host's AI on the others (the game
  also checks the owner); the finish window's racers leave at the moment the race ends and still
  ride on their screens, so once a race is over its whole grid is carried.
- **An ended race keeps the star's links as they were.** The room retries its SFU members' direct
  links between races; their last frames go the way they went during the race.
- **No wire or server change.** Nothing new on the wire, so an older game in the room only keeps
  dropping its own last seconds.

## Surprises

- In the base, the guest of the host-last race also never logged the host's finish: the lost
  finish is not a Mac or internet effect, it happens between two games on one PC.
- The guest's jolts of the host's rider varied 0-19.5 a minute between runs of either build; they
  all lie inside the race (none after the end) and follow where the scripted rider crashed (the
  placements are the old after-crash respawns in both builds).

## Not checked

- An SFU room (public or private) once the local server's SFU works for one from this PC: the SFU
  association's side is covered by the session test (`sfu.rs` `the_association_outlives_races...`)
  and, live, by a P2P room's SFU fallback (the PC <-> Mac `--force-turn` runs below).
- The browser (net.js) by its unit test only.
- A live owner's reset: none in the lane's runs or the merged main's (every placement there was a
  respawn, none `far`); the unit test has the verifier's case.
- Edge cases accepted: a TURN renewal due in the 2 s after a race (the association is replaced,
  those seconds go blind, rare: the credentials' age); a new hub in those seconds.

## Checked on the merged main (2026-11-12)

On `main` c5f839a2 (afd16cbb, then the merges of lanes 214, 211 and this one), by the lead's
verifier. Other workers' builds were running on this PC during the two-game races (seen once, at
the first race menu scenario); not watched during the PC <-> Mac runs.

- `python tools/check.py port -j 6`: OK. `python tools/check.py server`: OK.
- State traces: all seven recipes (`boot`, `race`, `race2` to `race6`) equal the format-2
  references made from 524948ba (the same `state_trace_codeptrs.txt` and `state_trace_mask.txt`):
  7 of 7 PASS.
- `race_menu_check.py`, all nine scenarios (`resume`, `room`, `end`, `leave`, `closed`, `alone`,
  `timeup`, `timeup-host`, `finished`, 80 ms link): PASS. `mosh_round_check.py` (default): PASS.

### Two games on this PC (P2P rooms, a local server per race, computer riders on)

Error p50 / p95 / p99 in units, A the host, B the guest. `online_race.py --hidden --lag` with the
80 ms test link (`delay=80,jitter=15,loss=1`); `race_menu_check.py --lag` with the same link, a room
made from the menus. `online_race.py` stops both games about 2 s after the race ends, before they
leave the race screen, so its after-the-end column has no "race closed" line to start from (the
trace's last 2 s); `race_menu_check.py`'s games go back to the room and log the race tick.

| race | A draws the guest | B draws the host | B draws the host's AI riders | after the end: A draws the guest / B the host / B the AI riders | finishes |
|---|---|---|---|---|---|
| `online_race`, both finish (70 / 72 s) | 0.19 / 1.69 / 2.64 | 0.22 / 2.52 / 5.16 | 0.37 / 2.44 / 5.28 | 1.31 / 1.86 / 1.91; 2.50 / 2.89 / 2.97; 0.45 / 2.13 / 3.39 | both on both |
| `online_race`, finish window (host 60 s, guest DNF) | 0.15 / 1.20 / 2.32 | 0.01 / 0.82 / 1.62 | 0.36 / 2.15 / 3.80 | 0.71 / 1.13 / 1.28; nothing to compare on B | the host's on both |
| `race_menu_check finished` (20 / 25 s) | 0.50 / 1.79 / 3.05 | 0.14 / 1.51 / 2.61 | 0.45 / 2.02 / 3.45 | 0.20 / 0.67 / 0.81; 0.05 / 0.05 / 0.05; 0.32 / 1.48 / 3.39 | both on both |
| `race_menu_check timeup` (host 20 s, guest DNF) | 0.21 / 1.49 / 2.50 | 0.08 / 0.75 / 1.57 | 0.31 / 1.81 / 3.23 | 0.76 / 1.79 / 1.89; 0.00 / 0.01 / 0.01; 0.27 / 0.38 / 0.39 | the host's on both |

Results equal on both games in every race; hits applied both ways (`hit kind 5`, `hit kind 0`); no
hold, no freeze line; every placement a respawn. The `timeup` race closed with "RACE OVER" on the
host and "TIME UP - DNF" on the guest. In the first `online_race` race the guest caught all nine
host riders up by riding just after GO (62 catch-ups between race ticks 253 and 272, 13-22 units,
snapshot age 133-150 ms); none of the other races here or on the Mac had a catch-up in their first
5 s (other workers' builds were running then).

### PC <-> Mac

The Mac (M1, 8 GB) ran its own build of c5f839a2 from this PC's wasm2c core; `cross_race.py`
through the verifier's wrappers (the PC's game on the hidden desktop, both games tracing and
recording), P2P rooms on a local server, direct inside the local network unless said. Host
finishes at 20 s, guest at 40 s; the host's first race is alone. Each race split per seed and
scored with `puppet_lag.riders_report` (which prints `end_report`, started from the "race closed"
line of each game). Error p95 in units, ranges over the run's races.

| run | shared races | other player as drawn (host / guest view) | computer riders on the guest | after the end: host draws the guest / guest the host / guest the AI riders | drawn behind p95 | placed (not respawns) | snapshot age p50 / p95 | guest's finish in the host's log |
|---|---|---|---|---|---|---|---|---|
| PC hosts | 4 | 0.30-1.48 / 0.18-1.27 | 0.53-1.40 | 0.23-1.14 / 0.05-0.58 / 0.40-0.76 | 0-7 ms | 0 | 11-29 / 16-41 ms | 4 of 4 |
| Mac hosts | 4 | 0.31-0.41 / 0.28-0.60 | 0.44-1.46 | 0.31-0.48 / 0.02-0.42 / 0.34-1.96 | 1-5 ms, 26 once | 0 | 11-32 / 18-43 ms | 4 of 4 |
| PC hosts, `--force-turn` | 2 | 0.38-0.58 / 0.33-0.90 | 0.58-1.25 | 0.46-0.55 / 0.02-0.08 / 0.43-0.45 | 1-3 ms | 0 | 25-34 / 38-54 ms | 2 of 2 |
| Mac hosts, `--force-turn` | 2 | 0.35-0.50 / 0.24-0.26 | 0.43-1.35 | 0.38-0.43 / 0.05-0.07 / 0.41-1.77 | 1-7 ms | 0 | 23-35 / 35-49 ms | 2 of 2 |

The room's results were the same on both machines in every race, and both games logged both
finishes in every shared race (before: the Mac host missed the PC guest's in 3 of 4). After the end
the riders now stay within about 2 units at p95 (before: 13-72): the other player at most 1.14,
inside the spread of the races' own p95 (0.18-1.48), though above its own race's p95 in some
races where that was small (0.71 against 0.30 once). In two Mac-hosted races the guest's view of
the host's computer riders sat a little higher after the end than in the race itself (p50 1.3,
p95 1.77 and 1.96 against the race's p95 1.35 and 1.46). Sunk at most 3.0 % of steps for the other
player, 0.2-0.6 % for computer riders; no catch-up; one hold (the PC host, its second shared
race), the race that also had more jolts (31 a minute on the host's view of the guest, p99 4.7);
the rest 0-11.

**`--force-turn`:** the local server's SFU connected this time (both ways, via UDP): the guest's
direct links were off and it raced over the room's SFU, bridged by the hub, in both runs.

## Result

Merged and checked (above). An ended race's frames travel until each game leaves the race
screen, on the SFU, the P2P star and the browser's client, with one rule in the game's client
(`client_back`, backstop `RACE_FRAMES_AFTER_MS`); every finish shows on every screen; an owner's
reset is a respawn. On the merged main, PC <-> Mac both ways, the other riders after the end are
within about 2 units at p95 (were 13-72) and the host's log has the guest's finish in every race.
Open: an SFU room (public or private), the browser live.
