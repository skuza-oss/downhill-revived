# 254: online riders and reconnects on bad networks (late or lost snapshots, outages, drops)

Status: current

Branch `feat/online-feel-254` from local `main` 34d6273d (v0.8.5 plus lanes 247, 249, 250, 251,
252), started 2026-11-15. This file is the lane's plan while it runs and its finding when it
merges. Someone who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Players on ordinary bad connections (mobile data, weak Wi-Fi: round trips of 100-400 ms, jitter,
2-10 % loss, spells of 0.5-2 s with nothing, a short disconnect) should see the other riders move
believably and get back into the room or the race without drama. Finding 202 rebuilt the puppets
(the owner's complaint then: riders that teleport, riders under the ground, stops and jumps); this
lane measures what is left on bad networks and fixes the worst of it.

1. **Measure first**, with a repeatable script and numbers per network (good, mobile, bad,
   outages): per puppet rider, its drawn position against the sender's true position at that
   moment, jumps (steps beyond what its motion explains), time frozen, height against the ground,
   heading flips. A fault the test tools lack is added to the test tools, not to the game.
2. **Fix the worst, one change at a time**, each shown by the numbers (late or lost snapshots,
   events late or twice, what a rider does during an outage).
3. **Reconnects:** the room connection or the SFU link lost for 1 s, 5 s, 20 s, in the room and in
   a race: what each player sees, how long until riders move again, whether anyone is left out or
   stuck, whether the results stay equal. Fix defects; messages to players short and true.
4. Stop after about 5 agent hours or three ideas in a row without a measured gain.

Rules (root `AGENTS.md`): state sync only, race frames through Cloudflare for SFU rooms, no player
gets another's address, a rider's own race never changes because of the network (only how the
others are drawn), the original game's feel, additive protocol changes only.

Done: `check.py quick`, `port`, `docs` (`server` if touched); the boot and race state traces
(offline) equal the base; one SFU race and one P2P room race with equal results on a good network;
the script and its before -> after table here.

## Progress

- [x] 2026-11-15: lane plan (this file), e7458bf1.
- [x] The measuring tools, 96ed0a3f: the test link's held outages (`stall=`) and one-off cuts
      (`cut=`) in `port/hle/puppet_link.h` (their own random sequence, so the older faults' runs
      stay the same), the bench's `profiles` command and its new counts (teleports, longest step,
      longest freeze, heading error and flips), trace lines with the rider's orientation.
      93fd1e91, 78643372, 09f53ce3: `DHD_NET_TEST_CUT=S:MS` (or `room+S:MS`), a game with no
      network at all for MS ms from S s after its race's GO (or after joining the room): the room
      socket drops and no attempt gets through, SFU and P2P race frames are lost both ways.
      `local_server.py --env-file` for a private SFU room on the local server.
- [x] Before: the profiles on the base model, tracks 0 and 1, computer riders and a player's.
- [x] Fixes, one at a time (below): one kept, f98e8575; two dropped.
- [x] Reconnects: 1 s, 5 s, 20 s in a race (the test games and the real ONLINE menus), 5 s and
      20 s in the room. Two fixes: quick rejoins while nothing reaches the server (below), and CONNECTION LOST for a game back
      too late for its seat.
- [x] Checks.

## How to measure

    python port/tools/puppet_bench.py record --track 0          # once per track: the truths
    python port/tools/puppet_bench.py record --track 1
    python port/tools/puppet_bench.py --label mine profiles --tracks 0,1   # build/puppet-bench/mine/profiles.md

Each profile is a `DHD_PUPPET_LINK` (`port/hle/puppet_link.h`); a run is the same deterministic
race (seed 81257) with every AI slot a puppet of its recorded rider (`ai`), or with the scripted
human rider replayed as a puppet (`player`, `DHD_PUPPET_MAP=0:1`: no AI controller, the owner's
pad only):

| profile | link | what it is |
|---|---|---|
| good | `delay=25,jitter=5,loss=0.5` | a wired connection |
| mobile | `delay=60,jitter=30,loss=2,reorder=2` | ordinary mobile data |
| bad | `delay=120,jitter=50,loss=8,reorder=5` | weak Wi-Fi, a busy cell |
| burst | `delay=60,jitter=20,loss=2,burst=6:500-2000` | about every 6 s nothing for 0.5-2 s (lost) |
| burst-held | `...,stall=6:500-2000` | the same, held and delivered at once after (the SFU's reliable channel, TCP TURN) |
| cut5 | `...,cut=30:5000` | 5 s without anything from 30 s |

Columns (units: riders move about 95 a second): error p50/p95/p99 against the truth at that
moment; teleports a minute (a drawn step over 3 units beyond the truth's); the longest step; frozen
share and the longest freeze while the truth moves; heading p95 (degrees) and flips a minute
(heading jumps over 8 degrees in a step). `profiles.json` beside the table has every field.

Reconnects: `DHD_NET_TEST_CUT` on the guest of a two-game race, with the room on the local server:

    python port/tools/local_server.py --env-file online/server/.dev.vars -- \
      python port/tools/online_race.py --hidden --capture "" --a DHD_NET_TEST_FINISH=60 \
        --b DHD_NET_TEST_FINISH=62 --b DHD_NET_TEST_CUT=20:5000       # the test games (DHD_NET_CREATE/JOIN)
    DHD_SERVER_ENV_FILE=online/server/.dev.vars python port/tools/race_menu_check.py OUT --scenario finished \
      --sfu --host-env DHD_NET_TEST_FINISH=60 --guest-env DHD_NET_TEST_FINISH=62 \
      --guest-env DHD_NET_TEST_CUT=20:20000                           # the real ONLINE menus

## Decisions

- Good, mobile and bad networks were already fine (error p95 at most 3.4 units, no teleports, no
  freezes): finding 202's model holds there and this lane leaves it alone. Everything worth fixing
  was in outages.
- The outage guard eases a puppet back to its owner's last speed only while it is on the ground,
  not crashing, and less than SYNC_COAST (3 s) since the last snapshot; its direction is the
  puppet's own (it never turns a rider), its speed the owner's. It is the guard of finding 202
  (`SYNC_GUARD`) carried on past `SYNC_GUARD_AGE`: the owner's pad still rides the puppet, the
  guard only stops it standing or falling behind on a bump while nothing comes. The review
  (14b782ea): the rule is `sync_outage_target` in `puppet_sync.h`, tested in
  `test_puppet_sync.c`. An owner that was under 30 units a second at its last snapshot (the start
  grid, standing after a wreck, braking to a stop) or a rider crashing here or at its owner's,
  waiting for its respawn or finished is never pushed on, and the easing ends; before, an owner at
  speed 0 with the easing still on from braking gave 0/0, NaNs in the puppet's velocity.
- Rejoins (`port/render/net/src/lib.rs` retry_delay, 12055856 after the review; it was 1, 2, 4, 8,
  8 s, five attempts). The server allows an address 30 connects a minute (`online/server`
  RL_CONNECT), shared by everyone behind it, and keeps a seat 20 s after it last heard from a
  member (REJOIN_GRACE_MS). So the schedule tells apart what reaches the server:

  | after | wait (each with 0.8-1.2x jitter) |
  |---|---|
  | the drop | 1 s |
  | an attempt the server never answered (no network here: connecting or TLS failed) | 2 s |
  | an attempt the server answered and that still failed (an HTTP answer, a socket closed at once), the 1st, 2nd | 4 s, 12 s |
  | a refusal by the rate limit (4011) | 20-30 s |

  The attempts end when the next would start more than 30 s after the drop (ws.h
  WS_ATTEMPTS_OVER). No network here: tries at about 1, 3, 5 ... 29 s, none of them reaching the
  server, and the game is back within about 2 s of its network's return. The server answering and
  failing (its trouble, not the player's): tries at about 1, 5 and 17 s, then no more: at most 3
  connects per player in a 60 s outage, 18 for 6 players behind one address, of the 30 allowed
  (the tests in lib.rs `retry_tests`; the first schedule of this lane, every 2 s for 30 s, made 15
  and failed them). The jitter keeps a room dropped by a server update from retrying in step.
- A game back in the room after its seat went says CONNECTION LOST for its two seconds, not THE
  RACE ENDED: the race went on for the others. A new `NetLobby` field, `seat_lost`, cleared at each
  start; no protocol change. A welcome without the race also comes when the race ended normally
  while the game was away with its seat kept (the review), so `client.c` read_room decides from
  the room's snapshot after the welcome (0f552033): the seat went when the race still runs without
  this game or the race's results list this game's member (its id in the race) as left; otherwise
  the race's own words (RACE OVER, TIME UP). `test_session.c` has both.
- No server change: the 20 s seat is the server's and stays.

## Surprises

- What the puppets did in an outage, from the per-step traces (the bench's `play` of one profile, its
  `DHD_PUPPET_TRACE` read step by step): a player's puppet keeps its owner's last stick and veers 40-80 degrees
  over a long gap, sometimes topples, and is then placed 40-70 units away; computer and player
  puppets both lose speed on bumps and stand or fall, because finding 202's guard only acted
  within 0.4 s of a snapshot.
- Dropped, idea 1: let go of the steering after 0.2 s without a snapshot. Track 0 burst player
  p99 28.7 -> 9.8, but burst-held player 20.5 -> 30.1: holding the stick is right when the owner
  had just started a turn, and the outage guard (kept) won the burst case without it.
- Dropped, idea 3: after an outage place a rider only when it is over 30 units off
  (`SYNC_PLACE_QUIET`), not 12, catching up smaller errors by riding. Fewer teleports (track 1
  burst player 4.5 -> 1.5 a minute), but longer steps where it did place (track 0 burst ai 45 ->
  106 units, track 1 burst player 40 -> 56) and p99 slightly worse everywhere (track 1 burst ai
  16.1 -> 17.6).
- cut5, player, track 1: the owner crashed just before the cut, and its puppet stands frozen at
  the crash (4.2 s) until the respawn snapshot comes; error p95 116 units is that one stretch. A
  puppet cannot know where the respawn puts the rider; left as it is.
- The SFU (reliable, ordered STATE, finding 173) resumed 2.8 s after the network came back on the
  5 s cut, in the same session: the held frames came at once ("member 1 sent over 240 snapshot
  messages in a second; the excess is dropped") and every puppet was placed "far" (120-360
  units) in one step. That is the burst-held case; on the real network the wait depends on
  SCTP's retransmission timer.
- A hit sent during a held outage arrives late but once (`sfu.rs` takes a pending event once it is
  written); events carry no time, so a late hit still lands late. Left as it is.
- The test games (`online_race.py`, DHD_NET_CREATE/JOIN, not the ONLINE menu) of a player whose
  seat went wait for the results 120 s; the menu's games go back to the room 2 s after the
  rejoin. Only the tests do that.

## Result

Kept: the outage guard (f98e8575, 14b782ea), quick rejoins (f8cd61ab, 12055856), CONNECTION LOST
for a game back too late for its seat (f6486ff4, db59be7e, 0f552033), and the test tools. No protocol or server
change: nothing to deploy.

**Puppets**, before (local `main` 34d6273d's model, the tools alone) -> after (this branch), the
same truths (seed 81257, tracks 0 and 1), `puppet_bench.py profiles`; one value where nothing
changed:

| network | rider | track | error p95 | error p99 | teleports min | step max | frozen pct | stuck s | flips min |
|---|---|---|---|---|---|---|---|---|---|
| good | ai | 0 | 0.51 -> 0.52 | 1.1 -> 1.2 | 0.0 | 1.0 | 0.00 | 0.00 | 1.7 |
| good | ai | 1 | 0.64 | 1.4 | 0.0 | 1.3 | 0.03 | 0.05 | 0.0 |
| good | player | 0 | 0.59 | 0.9 | 0.0 | 0.3 | 0.00 | 0.00 | 0.0 |
| good | player | 1 | 0.64 | 1.0 | 0.0 | 0.7 | 0.00 | 0.00 | 0.0 |
| mobile | ai | 0 | 1.26 | 2.4 | 0.0 | 1.9 | 0.00 | 0.00 | 4.4 |
| mobile | ai | 1 | 1.56 | 3.1 | 0.0 | 1.4 | 0.05 | 0.08 | 0.5 |
| mobile | player | 0 | 0.92 | 1.4 | 0.0 | 0.4 | 0.00 | 0.00 | 0.0 |
| mobile | player | 1 | 1.00 -> 1.01 | 1.6 | 0.0 | 0.8 | 0.00 | 0.00 | 3.0 |
| bad | ai | 0 | 2.71 | 5.2 | 0.0 | 2.1 | 0.00 | 0.00 | 6.9 |
| bad | ai | 1 | 3.28 | 5.3 | 0.0 | 1.4 | 0.07 | 0.17 | 4.7 |
| bad | player | 0 | 1.91 | 3.2 | 0.0 | 0.6 | 0.00 | 0.00 | 0.0 |
| bad | player | 1 | 2.08 | 3.5 | 0.0 | 0.9 | 0.00 | 0.00 | 10.5 |
| burst | ai | 0 | 3.24 -> 3.25 | 16.4 -> 13.2 | 1.5 -> 1.3 | 78.6 -> 45.2 | 0.20 -> 0.00 | 0.72 -> 0.00 | 8.1 -> 8.2 |
| burst | ai | 1 | 5.69 -> 5.46 | 16.6 -> 16.1 | 2.7 -> 2.5 | 35.5 | 0.13 -> 0.11 | 0.32 | 2.7 |
| burst | player | 0 | 3.38 -> 3.18 | 28.7 -> 9.8 | 4.3 | 46.4 -> 41.3 | 1.49 | 0.50 | 10.1 -> 8.7 |
| burst | player | 1 | 6.64 -> 7.95 | 35.0 -> 34.1 | 3.0 -> 4.5 | 42.0 -> 39.9 | 0.00 | 0.00 | 3.0 -> 9.0 |
| burst-held | ai | 0 | 4.64 -> 5.14 | 20.5 -> 20.7 | 0.4 -> 0.5 | 56.6 -> 61.6 | 0.06 -> 0.05 | 0.27 | 7.1 -> 8.1 |
| burst-held | ai | 1 | 4.95 -> 4.84 | 15.9 -> 15.5 | 0.5 | 39.6 -> 23.6 | 0.05 | 0.08 | 4.9 -> 4.7 |
| burst-held | player | 0 | 3.08 -> 3.86 | 20.5 -> 20.8 | 0.0 | 1.8 | 0.04 -> 0.00 | 0.02 -> 0.00 | 17.2 |
| burst-held | player | 1 | 1.74 -> 1.69 | 13.8 -> 14.7 | 0.0 | 1.5 -> 1.6 | 0.00 | 0.00 | 13.8 |
| cut5 | ai | 0 | 4.93 -> 4.96 | 89.9 -> 78.5 | 0.8 | 219.0 -> 174.8 | 0.07 -> 0.00 | 0.43 -> 0.00 | 3.8 -> 4.5 |
| cut5 | ai | 1 | 6.46 -> 6.42 | 94.8 -> 88.3 | 0.9 | 167.5 -> 134.9 | 0.05 -> 0.04 | 0.08 | 2.5 -> 2.4 |
| cut5 | player | 0 | 3.37 -> 2.06 | 33.6 -> 31.6 | 0.0 | 1.3 | 0.00 | 0.00 | 0.0 |
| cut5 | player | 1 | 115.86 -> 111.86 | 213.9 | 1.5 | 194.5 | 10.41 | 4.20 | 1.5 |

On good, mobile and bad networks nothing changed (the guard does not act while snapshots come).
In outages the worst moments shrank: the longest step (a teleport) 219 -> 175 and 168 -> 135 units
on the 5 s cut, 79 -> 45 on bursts; error p99 of a player's rider in bursts on track 0 28.7 -> 9.8;
the freezes of computer riders in bursts and cuts went (stuck 0.72 s and 0.43 s -> 0). Not
everything gained: a player's rider in bursts on track 1 has more small teleports (3.0 -> 4.5 a
minute) and heading flips (3 -> 9), and held bursts on track 0 have a slightly higher p95 (4.6 ->
5.1): the guard keeps a rider going at its owner's speed where it would have slowed, so the
correction when the snapshots come is sometimes a little different. The cut5 player row on
track 1 is the crash before the cut (Surprises): the same before and after. The review's change to the outage
rule (14b782ea) moved the burst, held-burst and cut rows by at most 0.4 units at p95 and 0.3 at
p99 (largest step 61.6 -> 63.3 in held bursts on track 0); the table is the build before it.

**Reconnects**, on the local server (a private SFU room, `--env-file`), two games on this PC, the
guest without network (`DHD_NET_TEST_CUT`) from 20 s after GO, or 5 s after joining the room;
before = local `main`'s rejoin schedule, after = this branch:

| case | before | after |
|---|---|---|
| race, 1 s | back in the race 1 s after the cut; the host placed the guest's rider once (123 units); results equal | the same |
| race, 5 s | attempts at 1 and 3 s fail, back 7.7 s after the cut (2.7 s after the network); results equal | back at 5.0 s (as the network returns); results equal |
| race, 20 s | attempts at 1, 3, 7, 15, 23 s; the room let the seat go at 20 s; the menus said THE RACE ENDED (untrue); the test games waited 120 s for results | back 1 s after the network, the seat still gone (the room's 20 s); CONNECTION LOST for 2 s, then the room; the host lists the guest DNF (left) and its rider rode on as the host's AI |
| room, 5 s | (not run) | back in the room as the network returns; the next race with both, results equal |
| room, 20 s | (not run) | back in the room 1 s after the network, listed in the host's room again (a new SFU session); the run's scripted menu presses went on during the outage and left it not ready, so no second race (the script, not the game) |

In every race case the other riders moved on through the cut on both games (computer riders on
their AI, a player's rider on its owner's last pad for 3 s, then let go). The SFU race frames
came back in the same session, 2.8 s after the network on the 5 s cut and about 1 s after it on
the 1 s cut (the rejoin schedule does not change that); the held frames then came at once, more than 240 a second were dropped, and every
puppet was placed in one step (90-360 units). Nobody was stuck; equal results wherever the seat
held.

Checks: `check.py port` (quick included) and `docs` OK; boot (600 rows) and race (5000 rows)
state traces equal to the base build; an SFU room race and a P2P room race on a good network
through the real menus, both games the same results (`race_menu_check.py --scenario finished`,
with and without `--sfu`).

Left, and unproven on real networks:
- Every number here is the test link or the test switch on one PC; real Wi-Fi and mobile outages
  were not tried. How long the SFU's reliable channel takes to resend after a real outage
  (2.8 s here) depends on SCTP's timers and the path.
- The flood after a held outage (the receiver drops what is over 240 a second) and the one-step
  placements after it: a held outage could ease in instead; not tried.
- The P2P transport's reconnects: the test switch covers it now (09f53ce3), no P2P cut was run.
- The browser (`port/web/www/net.js`) never rejoins a dropped room connection; unchanged.
- A puppet whose owner crashed just before an outage stands until the respawn snapshot comes.
- Events carry no time: a hit held by an outage lands late (once).
