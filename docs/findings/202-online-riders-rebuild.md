# 202: online riders that look right: a bench of today's puppets, then a rebuild

Status: current
Checked: 2026-11-13
Supersedes: part of 169 (item 4, the straight-line prediction)

Branch `feat/online-riders-rebuild` from `main` 6e2f7d23, started 2026-11-11. This file is the
lane's plan while it runs and its finding when it merges. Someone who has only this file and the
git log must be able to pick the lane up. Merged into `main` on 2026-11-11 (ad0135bd) and checked
there on 2026-11-12 ("Checked on the merged main" at the end).

## Goal and acceptance

The owner's complaint: in online races other riders teleport back and forward and half of them
sink a little into the ground. Today a remote rider (a puppet, findings 044, 169) skips its input
and physics and is drawn at its newest snapshot carried forward in a straight line (at most
250 ms), with corrections fading in 80 ms and anything over 8 units shown at once.

1. **A bench first.** Real rides (AI riders and scripted riding on at least three tracks: one with
   big jumps, one twisty) recorded at full rate as the truth, replayed onto puppets through an
   injected link (delay, jitter, loss, bursts, reordering, 60/30 snapshots a second, the receiver
   at 30/60/144 fps, clock cases), deterministic by a seed. Numbers per scenario: distance from
   the truth, below ground (how far and how often), jumps and twitches, time frozen, how far
   behind. The same injection as a test switch on the live receive path for real two-game races.
   Gate: if the bench does not show sinking and teleporting at realistic settings, stop and report.
2. **The rebuild**, chosen on the bench against today's predictor and a pure interpolation
   buffer: clearly better at 80 and 150 ms with steering and jumps (no visible sinking, no
   teleports outside respawns, no back-and-forth), graceful on a bad link; the old predictor
   deleted; the clock cases fixed.

Done: bench + how-to in `docs/TESTING.md`; new model in, old deleted; unit tests;
`python tools/check.py port` (and `server` if the protocol changed) and `docs` green; a
before/after table here; a two-game online race through a local server under the injected link
with equal results and hits landing. Out of scope: the room server, the SFU/TURN transport and
the P2P star (unless a bug there is part of the problem).

## Progress

- [x] 2026-11-11: lane plan (this file), 3d7b82dc.
- [x] The bench, 2e7d4c83 (`port/tools/puppet_bench.py`, `port/hle/puppet_link.h`, recording and
      replay in `port/hle/puppet.c`). How it measures: below.
- [x] Gate passed: today's model sinks and jumps on the bench at every realistic delay (below).
- [x] The new model, f4ba28c1 (`port/hle/puppet_sync.h`; the old predictor deleted).
- [x] The clock cases: stamps follow a game whose race clock falls behind, 621a40d5; the
      tools for live races and clips, 91345805.
- [x] Refinements on the bench (v6-v8): corrections carried to now and eased, the sender's
      clock rate followed, a rider let go after 1 s of silence, 44583044; clips at 4x, 0d44bcf1.
- [x] The interpolation-buffer comparison (a throwaway build, never committed): rejected.
- [x] Live two-game races through a local server under `DHD_NET_LINK` (below).
- [x] The final matrix (`--label final`, v8): track 0 every scenario, tracks 1-2 the main ones;
      the 4x clips; `check.py port`.
- [x] Round 2 (the lead's review, "Round 2" below): the start, AI riders measured live, HOLD for a
      stopped sender, what a puppet may do on its own; local main merged (ec966d03).
- [x] Round 3 ("Round 3" below): what decides a computer rider's line (the decompiled AI
      controller), the owner's turn scale in the record, placement only on real discontinuities,
      the hitch's hand-over, short stops ridden through; local main merged again (6c1650df).
      Not reached: the 150 ms computer riders' error and jolts, the host stops' 1.5-unit steps.
- [x] Round 4 ("Round 4" below): computer riders take their owners' racing line (bench 150 ms p95
      4.35 -> 2.75, live 3.6-3.7 in two of three races), host stops eased and timed by their
      sender, live-view clips; the verifier's three (online_race.py --bots, a reset drawn 312
      units off, the race's last seconds) and the phone's stuck rider; local main merged (6e8579d0).
      Not reached: host stops' 1.5-unit steps, one 150 ms race in three.
- [x] Round 4's closing A/B (the lead): 70a288fa against the tree without it, interleaved, in P2P
      rooms (the SFU failed every handshake): kept ("70a288fa settled" below).
- [x] Merged into `main` (ad0135bd); the lead's checks there on 2026-11-12: `check.py port` and
      `server`, the seven state traces, live races on one PC (80 and 150 ms, the arena's round, a
      room of six racers) and PC <-> Mac both ways with and without TURN ("Checked on the merged
      main").
- [x] Round 4 merged into `main` (573a5b37, with lanes 215 and 210: 18f5e70c) and checked there on
      2026-11-12 ("Round 4 checked on the merged main" at the end): the checks pass; the race's
      last seconds are not fixed in rooms made from the menus (race frames stop at the transport).

### The bench (how it measures)

- **Truth:** a deterministic race (`DHD_FRAME_CLOCK=1`, seed 81257) on the chooser's rows 0, 1, 2:
  Italy FR, Italy MX (big jumps; six riders), Italy TD (technical, twisty). This PC's rider pedals
  and steers (`state_trace_run.RACE_PAD`); the AI riders race as they do, steering, braking,
  jumping, crashing and respawning. Every rider is recorded each race step
  (`DHD_PUPPET_RECORD`, `DHD_PUPPET_FROM=all`), 75 race seconds.
- **Replay:** the same race again; every AI slot is a puppet of its own recorded rider, sent as
  online (AI riders 30 a second, a player's rider 60) through `DHD_PUPPET_LINK`. The race is the
  same in every run, so the truth is exact; the scored part is the puppets as drawn after each
  whole step (`DHD_PUPPET_TRACE`). `DHD_PUPPET_MAP=0:1` replays the scripted human instead.
- **Ground:** each wheel's clearance from the game's own world query (`func_00214BA0`, as the
  wheels' probes in `func_00131998` cast it): a segment 4 units above to 4 below the wheel's centre
  along world z (up), the first surface hit, the distance along its normal less the wheel's radius
  (1.25). It matches the game's own contact points exactly for a real rider (checked on the
  grid). The truth's own wheels press in a little: median -0.01, p1 -0.33 units. "Sunk" counts a
  drawn wheel more than 0.15 units deeper than the truth's own at that moment, "deep" 0.5.
- **Jumps:** "pops" are steps whose drawn motion differs from the truth's over the same step by
  more than 36 units a second (0.6 units in a 1/60 s step); "jolts" are steps where the drawn
  speed itself jumps by that much while the truth's does not (a snap or a hold: the visible
  teleport); "back" is motion backwards along the truth's. Steps within 0.5 s of a crash or
  respawn are left out (no model shows those the same way).
- **Scenarios** (`puppet_bench.SCENARIOS`): perfect, 30/80/150/250 ms with jitter and loss, 80 ms
  at 60 a second, a bad link (150 ms, 60 ms jitter, 5 % loss, 5 % reordering, an outage of
  200-500 ms every ~4 s), outages alone, the shared clock 40 ms off either way, the sender's and
  the receiver's game stopping 0.8 s; the receiver at 60, and at 30 and 144 for 80 ms, 150 ms and
  the bad link. A game runs at about 7x real time on the hidden desktop; a run is about a minute.

### Today's model on the bench (before; v0.7.16's puppet code plus the bench switches)

Track 0 (Italy FR), receiver at 60, nine AI puppets (from the 30-a-second AI snapshots):

| link | error p50 / p95 / p99 (units) | sunk / deep (% of steps) | depth p99 | jolts / pops a minute |
|---|---|---|---|---|
| perfect (0 ms) | 0.24 / 0.73 / 1.38 | 13 / 5 | 1.65 | |
| 30 ms | 0.48 / 1.59 / 2.43 | 23 / 12 | 2.92 | |
| 80 ms | 0.95 / 3.13 / 4.59 | 33 / 22 | 3.89 | 25 / 80 |
| 150 ms | 2.08 / 5.85 / 8.00 | 42 / 31 | 4.22 | 426 / 678 |
| bad | 3.15 / 13.1 / 34.7 | 41 / 30 | 4.14 | 909 / 1877 |

All three tracks show the same: 23-55 % of steps with a wheel sunk, 12-40 % deep, at every delay
from 30 ms. The scripted human (60 a second) at 150 ms: 23 % sunk, 91 pops a minute. This is the
owner's complaint: the straight-line carry leaves the ground at every landing, dip and rise (it
grows with the time carried), and every snapshot moves the prediction, which the 80 ms fade shows
as a jolt.

### Live races (two games, local server, real SFU, `DHD_NET_LINK` on both)

`python port/tools/local_server.py -- python port/tools/online_race.py --hidden --lag --link
"delay=80,jitter=20,loss=2,reorder=2,seed=21" --a DHD_NET_TEST_FINISH=70 --b DHD_NET_TEST_FINISH=72`
(this PC, both games on the hidden desktop at 60, eight host AI riders; v6, then the same with a
second seed and `--a DHD_PUPPET_FROM=all` so the host also recorded its AI riders):

- Results equal on both games in both races (1:10.03 / 1:12.03; 0:40.03 / 0:42.03).
- Hits: the guest's punch landed and was applied by the host's game (`hit kind 0 ... event from
  player 1`).
- Snapshot age on arrival 122-161 ms (the link's 80 ms plus jitter plus the SFU).
- Each player's rider as the other game draws it (`puppet_lag.py`): race 1 p50/p95/p99 0.29 /
  1.60 / 2.74 and 0.20 / 0.72 / 1.43 units, race 2 0.24 / 1.09 / 2.39 and 0.09 / 1.09 / 2.97
  units; drawn at the present (lag p50 0 ms). Finding 169's prediction measured 0.18-0.25 /
  1.0-1.5 on a link with a quarter of this age.
- The guest's AI puppets against the host's AI riders: 0.71 / 3.11 / 4.96 units, wheels below
  -0.15 in 3.5 % of steps (the real riders' own: 3.3 %).
- Re-syncs: after crashes (respawns), the host's own stuck-rider recovery (Surprises), and once
  three AI riders 0.6 s after GO.

## Decisions

- **Gate:** passed on the bench; the rebuild went ahead.
- **Model: the puppet rides on this game's physics, steered by its owner's snapshots**
  (`puppet_sync.h`). Its input is the owner's pad (newest snapshot), then the game's own physics
  runs for it, so its wheels follow this game's ground. Each snapshot is compared with where the
  puppet itself was at that snapshot's moment (a history of its own poses, about 2 s at 240
  steps a second), less the corrections applied since; the difference (position, velocity,
  orientation) is made up over about 0.1 s (`SYNC_TAU_*`), faster for larger differences
  (`SYNC_FAST`), never sliding faster than 30 units a second (`SYNC_SLIDE`, below the jump
  measure), and the part that would push the rider into the ground under touching wheels is left
  to the wheels. Over 12 units (`SYNC_RESYNC`; a respawn, a long loss) or over 3 just after a crash
  the rider is put where the snapshot says, carried to now in a straight line (at most 0.25 s),
  and its history restarts from that straight path (so the snapshots still on their way don't
  read as their age). The physics is the predictor: through a landing or a turn the puppet does
  what a rider does there. No wire change: the snapshot already carries all of this.
- **Computer riders' puppets run this game's AI controller.** AI riders steer in their own States
  from the AI controller (+0x78D4), not from the pad (their pad words change two or three times a
  second, measured), so a puppet with only the pad drifted sideways (p50 1.43 units at 150 ms
  against 0.84 with the controller). The controller now runs for them (the game's input pass),
  then the owner's pad replaces what it pressed. A player's rider steers from the pad: its puppet
  runs no AI.
- **Body writes keep the previous position in step:** the bookkeeping pass saves the position the
  physics integrates from before the puppet is steered, so a correction moves both, or it is
  undone the next step (first build: a 3-unit offset that never faded).
- **Carried to now, eased, fast for large errors (44583044):** a snapshot's velocity difference
  has been moving the puppet away since its moment, so it is added to the position still to
  correct (`SYNC_PROJECT`, at most 0.3 s): 150 ms error p95 6.1 -> 4.7, p99 9.4 -> 7.2. The slide
  limit grows with the error (`SYNC_SLIDE_GAIN`) and a correction's speed changes at most 600
  units a second each second (`SYNC_SLIDE_ACCEL`): without the easing the faster slide brought
  jolts back (150 ms: 59 a minute; eased: 3).
- **The sender's clock rate:** a snapshot carries the sender's race time beside its stamp; two of
  them show how fast its race clock runs on the shared clock (2 while it catches up after a hitch,
  under 1 below 30 fps), and the owner's velocity is taken at that rate. A receiver 3 s behind:
  p95 5.3 -> 2.2.
- **Stamps follow a game whose race clock falls behind (621a40d5):** online the race clock keeps
  the time it could not step and catches up later (finding 139). The stamp now adds what the
  sender's clock trails beyond 50 ms (normal pacing lag: 6-14 ms) and a receiver takes its own off
  when it places a snapshot (`sync_clock_excess`). In normal play both are zero; no wire change.
  Before it, a receiver 3 s behind drew riders up to 123 units off (p99) and, on the new model,
  snapshots seconds "old" ran past the puppet's history into re-sync loops.
- **After 1 s without a snapshot** (`SYNC_COAST`) a player's puppet lets go of the pad and slows
  to a stop, a computer rider's rides on its AI. Braking from 0.3 s was tried (v9): the stalled
  sender's p99 14.4 instead of 16.4, but the bad link's p99 16.1 instead of 11.7 and outages' 10.2
  instead of 7.9. A receiver cannot tell a stalled game from lost packets; lost packets are far
  more common, so 1 s.
- **Rejected: a pure interpolation buffer** (finding 169's adaptive delay: the measured age plus an
  interval plus twice the jitter; a throwaway build on the bench commit). It draws real poses, so
  it sinks less than today (80 ms: 6 % against 33 %), but in the past: 150-370 ms behind, so 15
  units off at 80 ms, 23 at 150 ms, 28 on the bad link, and it still sinks 4-23 % (the delay
  carries on through the gaps). Hits and closeness would be judged on riders that far back.
- **Rejected: sending the AI's power (+0x4214)**: no change on the bench (Surprises).
- **A rider whose player left after finishing** (run by no PC) stays at its last snapshot without
  physics, as before; **a rider the host's AI took over** runs the AI controller in its puppet.
- **No protocol change:** the STATE bundle is unchanged (version 6, protocol 4); only the stamp's
  meaning grew for a game that falls behind (`docs/online-protocol.md`). The release need not
  raise `MIN_VERSION`; a 0.7.16 game in the same room shows the riders the old way.
- Sources: Glenn Fiedler, "Networked Physics" (gafferongames.com, 2014-2015: state
  synchronisation, the receiver simulates and applies the sender's state with smoothing); Jared
  Cone, "It IS Rocket Science! The Physics of Rocket League Detailed" (GDC 2018: other cars
  simulated with their last inputs between server states). No code taken.

### What is not fixed, and what would fix it

- **A sender whose game stops (a hitch).** Round 1 rode on for up to 1 s and put the rider back
  (0.8 s stop: p99 14.4; 3 s: 114, today 8.4 and 23). Round 2 added HOLD: the stopped game's
  network thread says so and the others hold its riders, then glide after it while it catches up
  ("Round 2"). Left: the 0.8 s stop's tail is still a little over today's on the bench.
- **The shared clock 40 ms off** draws a rider about 4 units off either way (v8: 3.9 p50, today 4.0):
  a snapshot placed 40 ms wrong is a pose 40 ms of riding (95 units a second) off, whatever draws
  it. Not fixed: it is the clock's error, not the drawing's. Live the offset is a few ms (race 1:
  the GOs 6.3 ms apart on the shared clock, the puppets 0.2-0.3 units off at p50), since finding
  169/175's rule (only round trips near the quickest move the offset). What would fix the rest: each
  pair of players exchanging how old they find each other's snapshots; half the difference is
  their clocks' offset (independent of the server).
- **150 ms: closer, not only smoother.** v5 drew riders on the ground and without jumps but no
  closer at the tail (p95 6.1, p99 9.3 against today's 5.85 / 8.0). Carrying the velocity
  difference to now (v6) made them closer: v8 1.07 / 4.71 / 7.2 (track 0). For a player's rider
  (from the pad, 60 a second) v6 measured 0.28 / 1.39 / 2.43 against today's 0.61 / 3.46 / 5.72.
  What is left is what nobody can know 150 ms early: a turn, a brake or a bump begun since the last
  snapshot; the puppet keeps its owner's last inputs, which is the best guess.

## Surprises

- The world's up axis is +z (a wheel's contact lies its radius below its centre in z); riders'
  y does not go down a mountain.
- `func_0012C9E0`, in the human branch of the input pass, is a toggle handler (flags flipped by
  pad actions, the power cheat at +0x4214 among them), not steering: a puppet must not run it.
- The AI writes one rider field directly, +0x4214 (power, eased toward its own target); sending it
  changed nothing on the bench (150 ms: p50 2.19 with it, 2.16 without), so it does not travel.
- First new build: 1,268 re-syncs of one rider in a run at 150 ms: after a re-sync the history was
  empty, so the next snapshots (150 ms old) were compared with the present and read as 14-20
  units off: a loop. Fixed by restarting the history from the re-sync's straight carry.
- The bench is kind to AI puppets: the replayed race is the same world, so this game's AI
  controller steers exactly as the recorded one did. Online the guest's AI controller has its own
  state (its catch-up against the guest's own rider); in a live race the guest's AI puppets were
  0.71 / 3.1 / 5.0 units (p50/p95/p99) from the host's AI riders at a snapshot age of ~130 ms,
  wheels below -0.15 in 3.5 % of steps (real riders: 3.3 %). Once, three AI riders were put back
  13-18 units 0.6 s after GO (another run: none).
- A live re-sync of 249 units was the host's own stuck-rider recovery (fps.c: stuck pedalling for
  2 s) moving its rider on its own screen: the jump is real there too.

## Result

Branch `feat/online-riders-rebuild`: the bench (2e7d4c83, 91345805, 0d44bcf1), the new model
(f4ba28c1, 44583044), the clock rule (621a40d5), docs. The model as merged is "v8" in the notes
above (the builds v1-v9 were steps on the bench, each in this file's Decisions).

### Before and after on the bench (same truth, same seeds)

Track 0 (Italy FR), nine AI puppets (30 snapshots a second) unless the link says otherwise;
"before" is v0.7.16's puppet code with the bench switches, "after" this branch. `->` reads
before -> after.

| link | fps | error p50 / p95 / p99 (units) | sunk % | deep % | jolts a minute | back a minute |
|---|---|---|---|---|---|---|
| perfect | 60 | 0.24 / 0.73 / 1.4 -> **0.07 / 0.41 / 0.7** | 13.1 -> **0.3** | 5.0 -> **0.02** | 6 -> **0.2** | 0.0 -> **0.0** |
| 30ms | 60 | 0.48 / 1.59 / 2.4 -> **0.18 / 0.93 / 1.4** | 22.9 -> **0.4** | 12.1 -> **0.03** | 11 -> **0.5** | 0.0 -> **0.1** |
| 80ms | 60 | 0.95 / 3.13 / 4.6 -> **0.44 / 2.16 / 3.0** | 33.5 -> **0.7** | 22.5 -> **0.02** | 24 -> **0.7** | 0.1 -> **0.2** |
| 80ms | 30 | 1.19 / 3.88 / 5.6 -> **0.57 / 2.46 / 3.6** | 35.7 -> **1.0** | 25.6 -> **0.04** | 29 -> **5.1** | 0.3 -> **0.6** |
| 80ms | 144 | 0.82 / 2.71 / 4.0 -> **0.45 / 2.13 / 3.0** | 31.5 -> **0.8** | 20.4 -> **0.02** | 23 -> **1.5** | 0.4 -> **0.0** |
| 150ms | 60 | 2.08 / 5.85 / 8.0 -> **1.10 / 4.81 / 7.3** | 41.6 -> **1.5** | 31.2 -> **0.03** | 426 -> **3.1** | 19.4 -> **0.4** |
| 150ms | 30 | 3.58 / 7.41 / 9.0 -> **1.31 / 5.16 / 7.7** | 43.3 -> **1.6** | 32.0 -> **0.15** | 920 -> **21.6** | 3.0 -> **1.2** |
| 150ms | 144 | 1.69 / 5.12 / 7.2 -> **1.07 / 4.68 / 7.0** | 41.1 -> **1.7** | 30.5 -> **0.05** | 303 -> **3.1** | 7.6 -> **0.2** |
| 250ms | 60 | 9.74 / 15.78 / 19.1 -> **2.78 / 10.15 / 15.5** | 43.6 -> **2.5** | 31.4 -> **0.20** | 1076 -> **19.0** | 1.1 -> **4.4** |
| 80ms-60hz | 60 | 0.86 / 2.85 / 4.2 -> **0.38 / 1.86 / 2.7** | 32.4 -> **0.6** | 21.2 -> **0.03** | 20 -> **1.0** | 0.1 -> **0.2** |
| bad | 60 | 3.15 / 13.09 / 34.7 -> **1.43 / 6.57 / 11.7** | 40.5 -> **2.2** | 30.1 -> **0.14** | 909 -> **10.0** | 17.5 -> **1.0** |
| bad | 30 | 4.61 / 14.76 / 36.3 -> **1.70 / 7.14 / 12.9** | 41.6 -> **2.3** | 30.4 -> **0.25** | 1096 -> **38.1** | 1.6 -> **2.1** |
| bad | 144 | 2.48 / 12.23 / 34.0 -> **1.38 / 6.61 / 11.9** | 40.0 -> **2.4** | 29.6 -> **0.07** | 847 -> **8.3** | 20.6 -> **0.7** |
| bursts | 60 | 1.02 / 10.41 / 27.8 -> **0.52 / 3.25 / 7.9** | 32.7 -> **1.0** | 22.1 -> **0.03** | 86 -> **6.0** | 4.1 -> **1.1** |
| skew+40 | 60 | 4.00 / 5.17 / 6.3 -> **3.89 / 4.99 / 6.1** | 28.6 -> **0.9** | 16.7 -> **0.03** | 16 -> **1.8** | 0.0 -> **0.2** |
| skew-40 | 60 | 4.04 / 5.88 / 7.6 -> **3.89 / 5.33 / 6.6** | 35.8 -> **1.1** | 26.1 -> **0.04** | 38 -> **2.7** | 0.6 -> **1.2** |
| send-hitch | 60 | 0.97 / 3.45 / 8.4 -> **0.45 / 2.55 / 14.4** | 33.4 -> **0.9** | 22.7 -> **0.03** | 46 -> **4.7** | 0.2 -> **1.3** |
| recv-hitch | 60 | 0.93 / 3.06 / 4.6 -> **0.44 / 2.20 / 3.6** | 33.5 -> **0.8** | 22.3 -> **0.03** | 29 -> **3.7** | 0.7 -> **3.1** |
| send-behind | 60 | 1.02 / 6.22 / 23.1 -> **0.50 / 7.77 / 114.4** | 33.0 -> **0.9** | 22.6 -> **0.03** | 115 -> **7.4** | 0.4 -> **1.2** |
| recv-behind | 60 | 0.95 / 7.21 / 122.7 -> **0.48 / 2.21 / 3.4** | 31.0 -> **0.7** | 20.5 -> **0.03** | 110 -> **1.6** | 2.3 -> **0.6** |

The scripted human rider (60 snapshots a second, the case of every player's own rider), track 0:

| link | error p50 / p95 / p99 | sunk % | jolts a minute |
|---|---|---|---|
| 80 ms | 0.38 / 1.98 / 3.3 -> **0.28 / 0.75 / 1.3** | 18.8 -> **1.0** | 14 -> **4.4** |
| 150 ms | 0.61 / 3.46 / 5.7 -> **0.28 / 1.58 / 2.9** | 22.8 -> **1.0** | 43 -> **0** |

Tracks 1 (Italy MX, big jumps, five AI puppets) and 2 (Italy TD, twisty, seven), at 60:

| track | link | error p50 / p95 / p99 (units) | sunk % | deep % | jolts a minute |
|---|---|---|---|---|---|
| MX | perfect | 0.27 / 1.02 / 1.5 -> **0.06 / 0.38 / 0.7** | 21.2 -> **0.1** | 11.9 -> **0.06** | 23 -> **1.3** |
| MX | 80ms | 1.30 / 4.19 / 5.8 -> **0.33 / 2.11 / 3.6** | 34.4 -> **0.5** | 28.8 -> **0.08** | 65 -> **2.0** |
| MX | 150ms | 2.79 / 7.25 / 9.7 -> **0.95 / 4.93 / 6.7** | 42.0 -> **1.2** | 32.2 -> **0.08** | 458 -> **3.3** |
| MX | bad | 3.65 / 11.11 / 29.5 -> **1.22 / 6.34 / 9.9** | 41.4 -> **1.7** | 32.4 -> **0.09** | 895 -> **8.0** |
| TD | 80ms | 1.15 / 4.32 / 6.4 -> **0.57 / 2.52 / 3.8** | 40.8 -> **0.5** | 25.1 -> **0.03** | 36 -> **1.7** |
| TD | 150ms | 2.53 / 7.80 / 11.2 -> **1.55 / 6.09 / 9.0** | 53.5 -> **1.4** | 39.0 -> **0.07** | 436 -> **7.1** |
| TD | bad | 3.63 / 11.63 / 27.6 -> **2.10 / 8.37 / 12.7** | 52.2 -> **1.8** | 38.0 -> **0.21** | 932 -> **14.3** |

The interpolation buffer (rejected, Decisions), track 0 at 60: 80 ms 14.9 / 18.6 / 21.5 units,
6.1 % sunk, 154 ms behind; 150 ms 23.4 / 29.5 / 34.8, 12.5 % sunk, 246 ms behind; bad 27.5 /
39.7 / 61.8, 10.4 % sunk.

What the numbers say for the owner's complaint: the wheels in the ground (a third to almost half
of all steps today at any delay, a quarter to a third more than half a unit deep) are gone (0.3-2.5 % of
steps more than 0.15 units deeper than the real rider there, deep 0.02-0.25 %); the teleports
and back-and-forth (jolts 6-1,100 a minute today, backwards motion up to 20 a
minute) are 0.2-10 a minute at 60 (up to 38 at 30 fps on the bad link, where corrections are
coarser per frame); and the riders are closer to where they really are at every delay. Not better:
a sender whose game stops (tail), and a clock that is off (unchanged): see "What is not fixed".

### Clips

`F:\Temp\lane-202\clips\before\` and `after\` (not in the repository), with a README: eight
scenes each, 4x internal resolution, 1920x840, the rider as it rode beside the same rider as
another player sees it. Watch first: `mx-jumps-150ms.mp4` (before, then after) and
`td-twisty-bad-link.mp4`.

### Checks

- `python tools/check.py port -j 6` on 0d44bcf1: OK (runtime tests with `puppet-sync`,
  `puppet-link`; the game; net tests; GS tests). `check.py docs` OK on the last commit.
- No server or protocol change, so `check.py server` was not needed.
- Live: two two-game races through a local server under `DHD_NET_LINK` (above): equal results,
  the hit landed and was applied by its victim's PC.
- Round 2 (after the merge of local main, ec966d03 and later): `check.py port`, `check.py server`
  (one run failed one timing test by a millisecond under load, `session.test.ts` auto timer
  14989 against 14990 ms, the rerun passed; this lane changes nothing on the server) and
  `check.py docs` OK; `net_tests` with the new HOLD test; `mosh_round_check.py --link
  "delay=80,jitter=15,loss=1"` PASS (the arena round's table equal on both games).

### Left for the lead

- Done on the merged main (the end of this file): the state traces (every change is behind
  `pp.online` or the bench switches `DHD_PUPPET_PLAY`, `DHD_PUPPET_RECORD`, `DHD_PUPPET_TRACE`,
  `DHD_NET_LINK`, `DHD_NET_TEST_HITCH`, except the snapshot stamp, online only, and the rider-pair
  patch in `func_0011A100`, which asks `hle_puppet_both_remote`, 0 offline: all seven recipes
  equal); PC <-> Mac both ways, with and without `--force-turn`; an SFU room of six racers.
- Still open: a race with a phone in a P2P room (Android: nine puppets now run physics, as ten
  riders do offline; watch the phone's frame rate and the `online:` lines' "off by" figures); a
  P2P room of six (`p2p_crowd.py`).
- For the owner when they play: other riders' wheels on the ground at landings, dips and rises;
  no jumping back and forth; a rider that crashes or respawns appears at its respawn at once
  (that jump is real on its owner's screen too). A player whose game hitches stops on the others'
  screens and then rides on fast while it catches up. Computer riders on a guest are the weakest
  part ("Round 2": their line is this game's AI's).
- Merge: finding 169's item 4 (the straight-line prediction) becomes partly superseded by this
  finding; 044's "Interpolation" section already points at 169.
- Round 2's open items:
  - The hand-over from the catch-up glide back to the steering put the whole pack 23-40 units
    forward once in a live hitch race (`DHD_NET_TEST_HITCH=20:800`): the error measured right then
    is far larger than the riders' real distance. Start there with the re-sync log lines (they say
    the snapshot's age, the rate and both speeds).
  - Computer riders on a guest follow this game's AI line: their snapshots can't steer the AI
    controller's target. The fix is that target in the record; its state was not found.
  - The early re-sync count did not reach zero in every live start (see the counts).
  - The bench's stopped-sender rows got worse with the catch-up glide (jolts); live it removed the
    pack jumps during the catch-up. Worth a second look with the hand-over fixed.
- Clips: the "after" clips are from v17; the later changes only act at the start (first 1.5 s)
  and while a sender catches up (none in the clip scenes), so they show the final model.
- Release: no wire format or protocol change needs a `MIN_VERSION` raise (HOLD is a version older
  games drop; the record's power is in the wheels' bytes, same size and version).

## Round 2 (the lead's review): the start, AI riders live, a stopped sender

Commits 862d4a1f (HOLD and the record's power), 8afb11bf (grid start, hits, hold, power,
diagnostics), 407864ee (every rider scored live, re-syncs by phase), 1c7b34fe, 46331648 (speed
guard, no puppet-to-puppet collisions, no jumps on stale snapshots), ead61d1d (a late first
snapshot), ec966d03 (the merge of local main: lanes 199, 203, 204), 97af3a94 (the catch-up
glide), bbcec624 (`mosh_round_check.py --link`) and the start's first 1.5 s (the last commit).

### The re-syncs just after GO

Measured with short live races (finish at 12 s, both games on the hidden desktop, local server,
the real SFU, `DHD_NET_LINK` 80 ms with 15 ms jitter and 1 % loss on both games, eight host AI
riders), each re-sync logged with what it was measured against (age, history or present, both
speeds, the sender's rate), counted by `puppet_lag.py --riders` (first 5 s after GO, later, after
crashes).

Causes found:

1. **Every start, silently: the first snapshot moved the rider.** On a guest a computer rider's
   slot was not a puppet until its first snapshot (GO plus about 0.13-0.17 s), so this game's own
   AI started it, at its own pace (15-24 units a second where the host's had 0-4); the first
   snapshot then put the puppet back. Not logged as a re-sync (it was the first sync), but a jump
   at the gate: in ten starts 48 of 100 first snapshots moved a rider more than 1 unit, up to 6.6.
   Fix: from GO a puppet stands on the grid (no input, no physics) until its first snapshot, its
   history the grid pose, so the first snapshot is a small correction (`grid_step`). After: no
   first-snapshot jump. A slot with no snapshot 2 s after GO (a racer whose connection never came
   up) is this game's AI rider as before.
2. **A landing taken differently here.** The puppet's own physics took one from 150 to 50 units a
   second in a step on a landing the host's rider rode through; 0.13 s later it was 25 units
   behind. Fix: a puppet under 60 % of its owner's recent speed (snapshot under 0.4 s old, no
   crash) takes its owner's velocity at once (`SYNC_GUARD`).
3. **Puppets bumping each other here.** Two riders another game runs collided here as well as on
   their owner's game. Fix: they don't collide here (`hle_puppet_both_remote`, a patch in
   `func_0011A100`'s rider pairs); this game's rider still bumps into everyone.
4. **An SFU stall at the start.** The SFU delivered almost nothing for about a second; with the
   silence brake at 1 s the puppets let go and braked (50-80 units behind), and the backlog's first
   snapshot (781 ms old) made a jump to a pose carried only 0.25 s, a second jump following with
   the fresh ones. Fixes: the brake waits 3 s (a stopped game says so with HOLD now); a snapshot
   older than 0.4 s never makes a jump.
5. **A host catching up after its own hitch** (its clock rate 1.4-1.7). Following such a sender
   kinematically was tried and dropped (worse on the bench); the rate is taken before anything
   else in the step and scales its rider's velocity.
6. **Different power?** The record now carries the rider's power (`+0x4214`, an AI's catch-up),
   in place of the wheels' spin (the puppet's physics spins them). In the measured starts the host's
   AI riders had power 1.0 throughout, so this was not the cause there; it stays (it costs nothing
   and removes a difference whenever an AI eases its power).

Counts (re-syncs of riders not after a crash, first 5 s after GO, both games of each start):

| build | starts at 80 ms | games with any | re-syncs | first snapshots moving a rider > 1 unit |
|---|---|---|---|---|
| before (round 1's model, diagnostics) | 10 | 1 of 20 | 1 | 48 of 100 (largest 6.6) |
| grid start only (v10) | 10 | 1 of 20 | 1 | none |
| this round's model before the merge (v17) | 10 | 2 of 20 | 3 | none |
| v17 on the bad link | 2 | 1 of 4 | 2 | none |

| final, merged (97af3a94) | 10 | 1 of 20 | 5 | none |
| final on the bad link | 2 | 2 of 4 | 5 | none |
| the start's 1.5 s without jumps (f5719524) | 5 | 0 of 10 | 0 | none |
| f5719524 on the bad link | 2 | 1 of 4 | 2 | none |

The one 80 ms start left with early re-syncs (final build): at 0.6 s four AI puppets nearly
standing (5 units a second against their owners' 31-34, the start's push-off) were jumped 13-15
units; the last commit makes the start's first 1.5 s a quick start instead (no jump under 36
units: the speed guard gives the owner's speed, the slide closes the gap); its five starts had
none. On the bad link the rest are two AI riders at 3.7 s, at their owners' speeds (141/139,
129/130) 12-20 units off on snapshots 203 ms old: the AI line (below) through an outage.

**Not met:** the lead's acceptance (no re-sync in the first 5 s unless a rider respawned) is not
reached. What is left, from the logs:

- an AI rider whose line here leaves its owner's (two in one start, 18-27 units at equal speeds):
  this game's AI controller steers by its own racing-line state (Surprises: not one rider field;
  rider +0x4000-0x40BF holds no float that follows an AI rider's turning, best correlation 0.23);
- this game's rider shoving the other player's puppet (the scripted test rides both humans on one
  line, punching for 40 s: 116 against 67 units a second, 17 units);
- a first snapshot late after an outage at the start (fixed after v17: the first snapshot after
  the grid slides unless over 36 units).

What would remove the first: the AI controller's steering target (its path point, `+0x78D4` +0x7C,
and the line it chose) travelling in the record so a puppet's controller steers to the same point.
Finding where that state lives is a reverse-engineering task of its own.

### Computer riders on a guest, live

`puppet_lag.py --riders` over whole races (70 s, both games on the hidden desktop, local server,
real SFU, eight host AI riders, the link on both games), the guest's puppets of the host's AI
riders against the host's own recording of them:

| link | build | error p50 / p95 / p99 (units) | sunk / deep % | jolts a minute | re-syncs (first 5 s / later / after crashes) |
|---|---|---|---|---|---|
| 80 ms | v17 | 0.73 / 3.85 / 10.9 | 2.1 / 0.36 | 9.4 | 0 / 18 / 4 |
| 80 ms | final | 0.70 / 3.26 / 5.32 | 1.5 / 0.09 | 1.6 | 0 / 0 / 3 |
| 150 ms | v17 | 1.55 / 6.35 / 10.3 | 2.3 / 0.13 | 10.5 | 1 / 13 / 7 |
| 150 ms | final | 1.50 / 6.67 / 11.8 | 1.9 / 0.15 | 22.9 | 0 / 24 / 8 |
| bad | v17 | 2.26 / 8.77 / 13.8 | 2.8 / 0.28 | 20.5 | 2 / 36 / 12 |
| bad | final | 1.97 / 7.97 / 11.9 | 2.4 / 0.07 | 10.9 | 1 / 21 / 4 |

The other player's rider in the final races: guest's view 0.13 / 1.21 / 3.5 (80 ms), 0.47 / 3.86 /
8.3 (150 ms), 0.24 / 2.64 / 5.2 (bad); host's view 0.18 / 1.97 / 3.0, 0.31 / 2.70 / 4.7, 0.33 /
2.76 / 4.8. Results equal on both games in every race. Run-to-run spread is large on this PC
(three games at once, other lanes' state traces): the 150 ms race's re-syncs come in bunches with
the senders' catch-ups. Live the AI
riders are clearly worse than on the bench (150 ms bench: 1.06 / 4.8 / 7.4): the bench replays the
same world, so this game's AI controller does what the recorded one did; live the guest's own
controller runs its own state. The error is mostly sideways (live, interpolated: along the
rider's way p5/p95 -0.97 / 0.81, sideways p95 2.76, p99 4.42): the line, not the speed.

Removing it: see "Not met" above (the controller's target in the record). Not done: the target's
state was not found in this round; the power (the AI's catch-up) was, and travels now, but the
host's AI riders had power 1.0 in the measured races.

### A stopped sender: HOLD

A game whose own thread stops (a hitch, a stall) now says so: its network thread, which runs on,
sends HOLD (11 bytes on the STATE channel, `state_bundle.h`) every 100 ms once no bundle has gone
out for three of its usual gaps (at least 60 ms). The receivers stop that sender's riders where
they are; once newer snapshots come and the rider, carried to now, has reached the held spot, the
puppet takes that pose and speed (a unit or two from where it stands) and, while the sender then
catches up at up to twice the shared clock, glides toward its snapshots carried to now (1/e in
50 ms) until its rate settles. A game that does not know HOLD drops it (its bundle check wants
version 6). The record carries the rider's power in place of the wheels' spin (same size).

Bench, track 0 at 60 (sender stopped 0.8 s / 3 s at race second 20):

| | error p50 / p95 / p99 | jolts a minute |
|---|---|---|
| 0.8 s: today's model | 0.97 / 3.45 / 8.4 | 46 |
| 0.8 s: round 1 (rides on, put back) | 0.45 / 2.55 / 14.4 | 5 |
| 0.8 s: HOLD | 0.45 / 2.60 / 10.5 | 9 |
| 3 s: today's model | 1.02 / 6.22 / 23.1 | 115 |
| 3 s: round 1 | 0.50 / 7.77 / 114 | 7 |
| 3 s: HOLD | 0.51 / 7.01 / 15.0 | 12 |

The 3 s stop is much better than today at the tail; the 0.8 s stop is not quite (p99 10.5 against
8.4): the rider stops about (detection 60-100 ms + the link) of riding ahead of the frozen one,
where today's model stopped 250 ms ahead but only 0.25 s after its last snapshot. Live (the host's
`DHD_NET_TEST_HITCH=20:800`, 80 ms link): the guest held all nine host riders 0.2 s after the stop
and let them ride on 0.75 s later, each 3.5-7 units short of its rider; before the catch-up glide
they were then put 13-23 units forward within 4 ticks (the whole pack). With the catch-up glide
(on whenever a sender's rate rises past 1.25, announced or not; after a hold alone it ended at
once, the rate not having risen at the release) the riders rode on 1.3-2.1 units from theirs and
were not put anywhere during the catch-up; but when the rate fell back, 0.9 s later, the whole
pack was put 23-40 units forward at once (final build, `z-hitch` run). Not understood yet: the
hand-over from the glide to the steering measures far off; left for the lead (see "Left").
On the bench the glide costs jolts (final: 0.8 s stop p99 11.1, 14 jolts a minute; 3 s 15.4, 46),
where the glide's targets step with each 30-a-second snapshot.
Holds also fired, correctly, for short stalls of the games under this PC's load (the other lanes'
games, builds).

What the release must do: nothing for the wire (no protocol change, no `MIN_VERSION` raise; rooms
never mix releases). Tried and dropped: following a sender kinematically whenever its rate rose
(without a hold: worse on the bench, 0.8 s p99 11.1 and 13 jolts a minute, 3 s 15.4 and 45).

### Point B: what a puppet may do on its own

Before this round a rider whose snapshots had not begun (the first ~0.15 s after GO, or a slot
never heard from) was not a puppet: this game's own AI rode it, its hits were applied here and
went out as this game's EVENTs (lane 204's bots dealt hits nobody controlled), and it could start
crashes. Now:

- Hits and crash starts go by whether another game runs the rider (`remote`), not by whether its
  snapshots have begun: such a rider never deals or takes a hit here and never starts a crash by
  itself; a hit by this game's rider on it is an EVENT for its owner (and lane 204's
  `hle_online_hit_dealt` stays at that point).
- Pickups were already only for riders this game runs (`hle_online_pickup_allowed`).
- Before its first snapshot it stands on the grid (no input, no physics, so no pickup, no score);
  while its owner is silent it rides on its owner's last pad (an AI rider on this game's AI
  controller), still `remote`.
- Scoring: the arena's points come from `hle_online_hit_dealt`, called only when this game's own
  rider deals the hit.

## Round 3 (the lead's review): computer riders on a guest, no put-in-place, the hitch's hand-over

Commits e0f288e9 (the owner's turn scale; placement only on real discontinuities; the tools'
counts), 6c1650df (the merge of local main: lanes 205, 206; `online_race.py` takes its pads from
`online_riders.rider_env`), d1e6f0e1 (short stops ridden through).

### What decides a computer rider's line

Read in the decompiled source (an Explore agent's reading, checked against the files it names;
`src/unsorted/<address>.c`). The AI controller is a 0x450-byte object at rider +0x78D4
(`func_0010A730` initialises the nine), run each tick by `func_0012C8B0` (case `+0x7A5C == 3`)
through `func_0010BE38` -> `func_0010C4C0` (race mode) -> `func_0010CAF0` -> `func_0010F068` ->
`func_0010F0D8`.

- **The steering is the pad.** `func_0010F0D8` writes the virtual pad's (+0xFE0) +0x1B0 / +0x1B4
  (rider +0x1190 / +0x1194, pad words 7 and 8), 0 or 1: bang-bang, pressed when its heading error
  `e = x27C - 2 * rider[+0x4308]` passes 3 degrees (0.05236), held while its sign stays. `x27C` is
  the angle from the bike's forward axis to the target point `x30` (`func_0010F8E8`), or a look-ahead
  along the node polyline (`func_0010FF38` -> `func_001101B8`: `speed * 0.3 + 5` ahead, plus air
  and curve terms) or the landing normal in the air (`func_00111160`). Recorded: word 7 turns the
  rider toward its side axis (rotation row 0) at about 0.6 radians a second on average, word 8 the
  other way; the words change 2-3 times a second.
- **The turn scale.** The same function sets the controller's +0x2B0 to `min(|e| * 9/pi, 1)`
  (dropping at once, rising 0.07 a tick; 0.2 cap for rider id 0xA0, at least 0.5 with x440 bit
  50), and `func_00137950` (the steering's dynamics) multiplies an AI rider's turning acceleration
  by it. Recorded: median 0.08, p90 0.2-0.7.
- **The target.** `func_0010BD10`: `x30 = *x74 + rider matrix * x220`, `x220` easing toward `x230`
  (rate 0.03). `x74` is the racing-line node (graph `D_0029EA20`, 0x20 bytes a node: position,
  width +0x10, flags +0x14, link count +0x16, links +0x18), advanced by `func_0010B2B8`; at a fork
  the next node `x78` is chosen by `func_00118650` (a random link, `rand(0, count - 1)`, up to 7
  retries for flag bit 8 links, allowed from 5th place or when behind schedule), toward a rival's
  node (`func_00118BB8`, a coin flip, personality x3A0 bit 2) or preferring flag bit 12 links (x3A0
  bit 0, a coin flip). The sideways offset `x230` is drawn at each node (`func_0010B998`):
  `±func_00222B68(s/2, s)`, positive for odd places and negative for even, `s` from the node's width
  and the place (1st 1, 2nd-3rd max(2, w), 4th-5th max(4, w), lower max(7, 2w); 0.5 with x3A0 bit
  11; a fixed grid table where the node's flag bit 5 is set; the rival's line with x440 bit 53).
- **Why two games differ.** Every draw (`func_00222BD8`, `func_00222B68`, `func_00110BF8`: libc
  `rand`, `func_002559D8`; `func_00222B08`: the generator at `D_00663140`) is game-wide, shared
  with attacks (`func_00112360`), air tricks (`func_001109B0`, `func_00110BF8`, `func_00110DA0`,
  `func_00110F88`, `func_001140A0`) and personality timers (`func_001162D0`, `func_00116368`); the
  places (+0x798C), the nearest rider (`x8`, `func_001128F0`), the schedule (`x408`,
  `func_00111B18`, the skill `x2B8`), and the frame step (`func_00238F60` into x3B4, x414, x430,
  x410, x418, x42C) all differ between games, and some decisions run only on the rider's turn in
  a rotation (`func_0010FB00`, `func_00112360`, `func_0010EC80` when `+0x797C == D_002AF2C0.x501B4`).
  Seeding cannot make two games draw alike.

Not decompiled or not traced: `func_0010C5D8` (finished riders), `func_002559D8`, `func_00243198`;
+0x4308 (probably the current steer or lean), what x214/x218 are, `func_00119150` (per-node
schedule times), `func_0010ED50`, `func_0010E9B0`, `func_0010D0C0`, `func_001FAC50`, the writer of
x2AC.

**What a puppet does with it.** A computer rider's puppet already replayed its owner's pad
(words 7 and 8 included; this game's controller runs for it and its words are replaced), but its
turning was scaled by *this game's* controller's +0x2B0, computed from this game's line (its own
node choices and offset): several times too much or too little. The record now carries the
owner's +0x2B0 (the 12 reserved bytes' first float; same size and version; 0 from an older
sender, which leaves the guest's own) and the guest sets it after its controller ran
(`hle_puppet_input`). Bench 150 ms (track 0, a new recording with the field): error p95 4.79 ->
4.41, sideways p95 4.18 -> 3.75. Live it is the larger difference (the bench's guest controller
nearly agrees with the recorded one: same world).

Tried and dropped:
- **A follower** (route b): the puppet's words 7/8 written as an analog value from its heading to
  its owner's pose carried 0.3 s ahead, the owner's turn rate as feed-forward, the turn-rate
  difference as damping, at full turn scale; on the ground only (in the air the words make air
  spins). Bench 150 ms p95 4.81 (sideways 4.14) against 4.41 with the owner's words and scale: the
  owner's words are the better input; tuning was not pursued.
- **The turn-rate difference carried** over the snapshot's age into the heading correction (as the
  velocity difference is): bad link 1572 jolts a minute. Reverted.
- **Syncing the line state** (route a: x74/x78/x230/x220 and the mode flags in the record): not
  built. With the turn scale the owner's words already steer the owner's line; what is left is
  their age (the words arrive 80-150 ms late, a bang-bang pulse with them).

### Placed only where nothing else looks right

`steer()` places a rider at once only for:
- its first snapshot;
- a respawn (the first snapshot after a crash, more than 3 units off);
- more than 50 units off (`SYNC_PLACE_FAR`: over half a second of riding; nothing closes that
  unseen);
- more than 12 units off after a second without snapshots (`SYNC_PLACE_SILENCE`: it was a guess
  that long);

and never on a snapshot older than 0.4 s. Anything else is caught up by riding: the correction
adds or takes at most 90 units a second (`SYNC_SLIDE_MOST`, 1.5 units a step at 60) on top of
the rider's own riding, logged as "catches up". `puppet_lag.py --riders` counts placements by
kind (first 5 s, later, respawns) and the catch-ups; the bench's table has a `catchups` column.

### A stopped sender: the hand-over and short stops

The pack jump at the catch-up's end (round 2) was a measurement, not a position: the guest's
puppets were 4-14 units from their riders against the host's wall-time path, but a debug line per
snapshot (`DHD_PUPPET_DEBUG=slot`) showed the first snapshot after the catch-up arriving 343 ms
late (no snapshot for a third of a second, this PC under load), measured 13.4 units against the
glided history, and the 12-unit jump rule placed every rider. Fixes:
- the placement rule above (13-40 units: a catch-up by riding);
- the catch-up glide moves a rider at most 1.5 units a step beyond its riding;
- the release from a hold moves a rider only under 1.5 units (it moved up to 12);
- **short stops are ridden through**: a HOLD stops a sender's riders only once its last snapshot
  is 0.25 s old (`SYNC_HOLD_AFTER`). A loaded race (round 3's first runs, a build running beside)
  held a sender's riders 50 times, each a dead stop and a burst after; after a short stall the
  sender's catch-up brings its riders back where riding on put them.

### Live, round 3 (d1e6f0e1 on the merged tree)

Whole races (70 s), two games on the hidden desktop, local server, the real SFU, eight host
computer riders, the link on both games (80 ms: `delay=80,jitter=15,loss=1`; 150 ms:
`delay=150,jitter=30,loss=2,reorder=2`; bad: `delay=150,jitter=60,loss=5,reorder=5,burst=4:200-500`),
three seeds each, nothing else of this lane running on the PC (other lanes' state traces were).
"Placed" excludes respawns and first snapshots; catch-ups are errors over 12 units caught up by
riding.

| race | AI riders on the guest: error p50 / p95 / p99 | sunk / deep % | jolts a min | placed (first 5 s / later) | respawns | catch-ups | guest sees host player p50 / p95 / p99 | host sees guest p50 / p95 / p99 |
|---|---|---|---|---|---|---|---|---|
| 80 ms #1 | 0.57 / 2.88 / 4.9 | 0.8 / 0.03 | 1.0 | 0 / 0 | 3 | 1 | 0.13 / 1.24 / 3.2 | 0.17 / 1.07 / 2.3 |
| 80 ms #2 | 0.56 / 2.77 / 5.1 | 1.0 / 0.05 | 2.2 | 0 / 0 | 5 | 1 | 0.10 / 0.80 / 2.6 | 0.17 / 1.60 / 5.4 |
| 80 ms #3 | 0.61 / 2.98 / 4.8 | 1.2 / 0.06 | 3.0 | 0 / 0 | 15 | 1 | 0.17 / 1.49 / 2.9 | 0.17 / 1.75 / 3.3 |
| 150 ms #1 | 1.22 / 6.06 / 13.0 | 1.5 / 0.04 | 6.3 | 0 / 2 | 5 | 48 | 0.35 / 4.18 / 15.5 | 0.23 / 1.95 / 4.6 |
| 150 ms #2 | 1.18 / 5.72 / 9.3 | 1.6 / 0.10 | 6.2 | 0 / 0 | 7 | 14 | 0.31 / 3.76 / 7.1 | 0.35 / 1.92 / 3.4 |
| 150 ms #3 | 1.44 / 7.83 / 22.3 | 1.6 / 0.05 | 13.5 | 0 / 1 | 10 | 103 | 0.28 / 4.01 / 8.6 | 0.35 / 2.87 / 6.8 |
| bad #1 | 1.40 / 7.78 / 14.4 | 1.5 / 0.07 | 8.3 | 0 / 0 | 8 | 60 | 0.26 / 2.68 / 5.5 | 0.34 / 3.64 / 6.8 |
| bad #2 | 1.52 / 8.89 / 18.3 | 2.4 / 0.12 | 9.4 | 0 / 6 | 9 | 84 | 0.43 / 4.09 / 7.8 | 0.43 / 5.37 / 9.0 |
| bad #3 | 1.63 / 9.08 / 17.5 | 2.3 / 0.11 | 8.0 | 0 / 0 | 8 | 79 | 0.34 / 3.02 / 6.2 | 0.41 / 2.99 / 6.3 |

Against the acceptance (150 ms: at most 1 placement a race, jolts under 5 a minute, p95 under 4;
bad: at most 3 placements; sunk no worse):
- **Placements: met at 80 ms and 150 ms (0, 0, 1 and 2), bad 0, 6, 0.** The 2 at 150 ms #1 and the
  6 of bad #2 are link outages: snapshots arriving 0.75 s old in a burst after about 0.6 s of
  silence, riding on through it having put riders 58-75 units off (over `SYNC_PLACE_FAR`).
- **Jolts: not met at 150 ms** (6.3, 6.2, 13.5; round 2 22.9); 80 ms 1.0-3.0.
- **Error p95: not met at 150 ms** (5.7-7.8; round 2 6.67): what is left is the steering's age.
  The owner's words reach the guest 150 ms late and the AI steers in pulses, so each turn starts
  that much late (bench sideways p95 3.75 of 4.41).
- Sunk: 1.5-1.6 % at 150 ms (round 2 1.9), 0.8-1.2 % at 80 ms, 1.5-2.4 % on the bad link.

The run-to-run spread is large (150 ms #3: 103 catch-ups, 13.5 jolts): the catch-ups come in
bursts at link outages, the races differ in where the SFU stalls. Races run beside a build on
this PC (round 3's first runs) had 176-220 catch-ups and 57-109 jolts a minute: on a loaded PC
the games stall and the puppets' corrections pile up.

### Host stops, live

Host test stops (`DHD_NET_TEST_HITCH=20:MS`, 80 ms link), three each; the guest's puppets of
the host's nine riders from the stop to 3 s after it ends. "Largest step change" is a rider's
largest |p[k] - 2 p[k-1] + p[k-2]| (units; it counts velocity changes too), not counting the
step where a held rider is released, and not respawns.

| stop | holds | placed (not respawns) | catch-ups | largest step change, three races |
|---|---|---|---|---|
| 0.3 s | 9, 9, 9 | 0, 0, 0 | 26, 29, 27 | 3.6, 3.1, 4.1 |
| 0.8 s | 9, 9, 9 | 0, 0, 0 | 20, 16, 17 | 4.1 (two races: respawns, 41 and 120) |
| 3 s | 27, 9, 9 | 0, 0, 0 | 0, 0, 0 | 5.6, 4.4, 4.3 |

**No placements; not met: steps of 3-6 units beyond 1.5.** The largest are where the catch-up
glide starts (velocity to the sender's rate, up to twice) and where it ends. Even a 0.3 s stop
is held: the HOLD threshold is the last snapshot's age, which the link's delay adds to. The pack
jump of round 2 is gone (round 2: 9-34 units in one step).

### Checks

- On d1e6f0e1 (the merged tree): `check.py port` OK (runtime tests, the game, net tests, GS tests),
  `check.py server` OK, `check.py docs` OK on the last commit.
- `mosh_round_check.py` default scenario (`hit`, lane 206) under `--link
  "delay=80,jitter=15,loss=1,seed=41"`: PASS (the guest scored 50 on the host's rider; the same
  finals on both games).
- Every live race above ended with equal results on both games.
- Not run: state traces, PC <-> Mac, a phone in a P2P room, a 6+ player room (see "Left for the
  lead").

### Left (round 3)

- **150 ms computer riders:** p95 5.7-7.8 and 6-13 jolts a minute against 4 and 5. What is
  left is the owner's words' age. A puppet's controller steering by the owner's line *now* would
  remove it: the owner's line state (x74, x78, x230/x220 and the mode bits above, about 24 bytes)
  in the record, written into this game's controller before it runs each tick, its own steering
  bits kept (they are then timely) and only the turn scale and pedal taken from the owner. The
  random node choice at a fork arrives a snapshot late and must be overwritten when it does.
  This is the next step for whoever continues; the map above is what it needs.
- **Host stops:** step changes of 3-6 units where the glide starts and ends. The glide should ease
  its velocity to the sender's rate and back (now it is set at once), and a stop under the HOLD
  threshold plus the link's delay should not hold at all (the threshold should count from the
  HOLD's own stamp, not the receive time).
- **The bench's stopped sender:** send-hitch 0.8 s p99 10.5 against today's model's 8.4 (r3b,
  before the short-stop rule): the error while the puppet is held is the difference between
  holding and the sender's frozen screen, which today's model, freezing 0.25 s after the last
  snapshot, matches by accident. Not re-measured after d1e6f0e1.
- **Clips:** not re-rendered this round, and the two live-view clips (a guest's view of the host's
  computer riders at 150 ms, v0.7.16's model and now) not made. The clip tool films bench
  replays of a deterministic race; a live view needs the guest's recording (`B/own.bin` has every
  rider as the guest drew it) played exactly, next to the host's (`A/own.bin`), in a game on the
  same track: a `--live RUN` mode for `puppet_bench.py clip`.

## Checked on the merged main (2026-11-12)

On `main` ad0135bd (c8257cd5 and this lane), by the lead's verifier. Another lane built its core
and game on this PC during the 150 ms race and the six-racer room (live numbers there are from a
loaded PC).

- `python tools/check.py port -j 6`: OK. `python tools/check.py server`: OK.
- State traces: all seven recipes (`boot`, `race`, `race2` to `race6`) equal the format-2
  references of 0.7.16's game code (`state_trace_codeptrs.txt` the same as theirs): 7 of 7 PASS.
- Two games on the hidden desktop, a local server, the real SFU, eight host computer riders,
  `online_race.py --hidden --lag` with the link on both games. Results equal on both in every race.

  | race | the guest's computer riders: error p50 / p95 / p99 | sunk / deep % | jolts a min | placed (not respawns) | catch-ups | guest sees host | host sees guest | hit applied |
  |---|---|---|---|---|---|---|---|---|
  | 80 ms #1 | 0.61 / 3.01 / 5.1 | 0.8 / 0.06 | 2.0 | 0 | 1 | 0.13 / 1.09 / 2.1 | 0.15 / 1.60 / 3.6 | yes |
  | 80 ms #2 | 0.60 / 2.75 / 5.3 | 0.7 / 0.02 | 2.6 | 0 | 15 | 0.20 / 1.47 / 3.5 | 0.25 / 1.85 / 4.2 | yes |
  | 150 ms | 1.27 / 6.47 / 11.8 | 1.7 / 0.21 | 15.2 | 4 | 48 | 0.28 / 1.70 / 5.9 | 0.37 / 1.92 / 3.2 | no hit landed |

  The 150 ms race's placements: 62.8 units 4.7 s after GO (the guest's game had stalled, its
  rider was held, then it crashed), and three where a rider was reset to the track on its own
  screen (speed 0, 222-269 units; the port's recovery, no crash flag). On one of those the guest
  first showed the host's rider at a spot about 310 units from where it really was and put it
  right 0.2 s later (`puppet: tick 3836` and `3849`; the host's own record has one jump only).
  The first two tries at 150 ms never raced together: the host's first Realtime session's
  handshake timed out, its TURN fallback was asked for, and the room took it out of the race
  ("Realtime connection failed: out of the race"); the test link holds only STATE in `puppet.c`,
  so this is the transport, not this lane.
- `mosh_round_check.py` (default, `hit`): PASS (the guest scored 50 on the host's rider; the same
  table and finals on both games).
- A room of six racers: two hidden games and four nettool bots (`--echo-ai`), four computer riders,
  track 0. All six connected (the SFU's channels ready for 6 racers at the start), the race started
  for all, the ten results the same on both games, no freeze or fault line, every puppet off the
  grid, no storm of placements (3 and 2, all of bots, whose riders echo an AI rider's pose; 29 and
  31 catch-ups). `online_race.py --bots` fails on main (a TypeError: the bot loop reuses `log`,
  which 91345805 made the game log's name); this room ran a copy with that variable renamed.
- PC <-> Mac (M1, 8 GB, its game built from ad0135bd with this PC's wasm2c core) with
  `cross_race.py`'s pads, the PC's game on the hidden desktop, both games recording
  (`DHD_PUPPET_TRACE`, `DHD_PUPPET_RECORD`, `DHD_PUPPET_FROM=all`), races split and scored by
  `puppet_lag.py` while both rode (the host finishes at 20 s, the guest at 40 s). Every shared race
  ended with the same room results on both (host 0:20.03, guest 0:40.03); no freeze line.

  | direction | shared races | other player as drawn: p95 (p99) | drawn behind: p50 / p95 | computer riders on the guest: p95 (p99) | placed (not respawns) | snapshot age p50 / p95 |
  |---|---|---|---|---|---|---|
  | PC hosts | 4 | 0.18-1.32 (at most 1.8) | 0 / 1-103 ms | 0.61-1.77 (at most 2.1) | 1 | 15-42 / 21-52 ms |
  | Mac hosts | 4 | 0.24-3.78, 13.3 once (at most 45) | 0 / 1-488 ms | 0.59-2.21, 6.7 once (31) | 5 | 12-41 / 26-68 ms |
  | PC hosts, TURN | 3 | 0.21-1.39 (at most 1.8) | 0 / 1-253 ms | 0.64-1.60 (20 once) | 4 | 16-33 / 40-98 ms |
  | Mac hosts, TURN | 3 | 0.32-0.69 (at most 1.6) | 0 / 1-58 ms | 0.67-1.36 (30 once) | 4 | 19-34 / 30-56 ms |

  Sunk 0-4.6 % of steps (9.1 once, in the hitched race below; a finished rider standing still,
  90 %). Every large placement but five is a rider reset to the track on its owner's screen
  (speed 0, 52-459 units, matched in the owner's record). The five: in the Mac-hosted race 3 the host's rider 115 units 2.2 s after GO
  and a computer rider (60 units) after half a second without snapshots, while this PC's game had
  frames of 482 and 730 ms; over TURN two computer riders (70 units) after a 0.76 s gap in the
  Mac host's snapshots (its game ran smoothly: the relay), and one more (60 units) after 0.2 s
  without snapshots in the PC-hosted TURN run. The worst tails are there too, and the
  Mac's view of this PC's rider had up to 41 jolts a minute in that run (this PC's frames up to 730 ms).
  In the 2 s between the last finish and the race's close the riders drift 20-330 units from their
  records (the race is over for both then).

### Open (after the merge)

- **Computer riders at 150 ms** (p95 6.5 live here): the owner's line state (x74, x78, x230/x220
  and the mode bits, "Left (round 3)") in the snapshot, so a guest's controller steers the owner's
  line now.
- **The hitch's glide steps** (3-6 units where the catch-up glide starts and ends; "Host stops").
- **The live-view clips** (`puppet_bench.py clip --live`, "Left (round 3)").
- **A phone in a P2P room**: done once as a guest (below); a P2P room of six is still open.

### A phone as the guest of a P2P room (2026-11-12, after the merge)

On `main` dbddf61e (with finding 209), by finding 209's verifier: a release-signed APK on the Mi 11i
(Android 13) joined by code a P2P room on the official server that a Windows game of the same main
created on the hidden desktop (scripted rider, pedal only). Computer riders on, Mt. Midoule, Italy
FR; the link direct inside the local network. The phone ran nine puppets (`puppets: 1 2 3 4 5 6 7
8 9`): the host's rider and its eight computer riders on this game's physics.

- Both games showed the same results (the PC's rider 9th in 6:22.36, the phone's 10th in 7:12.76,
  the eight computer riders' times equal), the countdown at the room's moment on both (+0.2 and
  +0.3 ms), no freeze, hold or fault line.
- The phone's pictures at the start and after GO: the riders' wheels on the ground, none
  underground, none left on the grid, the pack riding off. The phone's rider was last, so later
  pictures have the others only far ahead; the screenshot tool takes 1-2 s a picture, too slow
  for pairs half a second apart.
- `perf: race` on the phone over 15 windows of 30 s: game thread 11.99 ms cpu a field, VU 9.95,
  59.9 fps, 9.1 % of frames over 20 ms (5.1-12.3 %). The day before on the same phone, offline:
  game 10.95, VU 6.15, 59.8 fps, 0.1 % over; online as the host with one remote rider: game 11.13,
  VU 9.17, 9.4 % over. So nine physics puppets cost the game thread about 0.9 ms a field over the
  one-puppet host and change little else: the share of slow frames is the online path's, not the
  puppets'. The window with the track load had one frame of 4977 ms (the load, then the wait for
  the PC to load).
- `net:` lines: the host's rider's snapshot age p50 16-32 ms, p95 27-39, p99 31-63, no gap over
  100 ms; the `online:` lines put every puppet within about 0.5 units of its snapshot at
  5-13 ms age.
- `puppet:` placements: 14, of them 10 respawns (4.7-77 units, all computer riders) and 4 "far"
  (124-200 units), all four the PC's scripted rider, which rides pedal only and is reset to the
  track often; 65 crash events (kinds 1, 2 and 10).
- A rider reset to the track (no crash) was once first shown about 310 units off for 0.2 s.
- A finished rider's puppet and the computer riders drift in the last 2 s before the race closes.
- `online_race.py --bots` (the `log` name clash above).

## Round 4 (the lead's review): the owner's racing line, host stops eased, live clips

Commits bd2f84e4 (the racing line in the record, eased holds and catch-ups, HOLD timed by its
sender, the exact replay and `clip --live`), then the merge of local main and the finding.

### Computer riders take their owners' racing line

What the record carries (44 bytes, every record; zero and `0xFFFF` nodes for a player's rider): the
racing-line node the owner's AI controller heads for (`+0x74`) and the one it has chosen next
(`+0x78`), as indexes into the node array `D_0029EA20` (0x20 bytes a node, the same static array
in every game; at most 2117 fit before `D_002AF2C0`); its target point (`+0x30`: the node's
position plus the sideways offset) and the next (`+0x50`); the offset (`+0x220`, x and y) and the
one it eases toward (`+0x230`). Bundle payload version 6 -> 8. A ten-rider tick still fits: a
normal tick's records grow from about 110 to about 155 bytes (two bundles of at most 990); the
full pad refresh (730 bytes a record, once a second) was already one bundle a rider and stays so,
and the riders' refreshes are staggered (`hle_puppet_start`: three ticks apart).

What the guest does (`take_line`, before its controller runs, once per newer snapshot):
- same node: it takes the next node, both target points and the offsets (and the directions and
  lengths `+0xF0`/`+0x270`, `+0x100`/`+0x274` between the targets);
- a node behind (its chosen next is the owner's node): the game's own advance, `func_0010B2B8(ai,
  NULL)`, which does the bookkeeping (the previous target, the distances, the next node's
  default), then as above;
- a node ahead (it reached the owner's next node first): nothing; the next snapshot brings the
  owner's choice after it;
- anything else (another fork, a re-snap): `func_0010B2B8(ai, node)` jumps there (its random offset
  and next node are then replaced by the owner's).

Its controller then steers the owner's line from where the puppet is, at once. On the ground the
puppet keeps its own controller's steering words and turn scale; in the air the owner's words
stay (there they are tricks, air spins, which this game's controller would choose with its own
random draws). The owner's pedal and every other word stay the owner's.

What the write must not break:
- **The controller's own bookkeeping when the node jumps:** the jump goes through the game's own
  advance. Only fields the advance itself sets are written after it.
- **A puppet handed to this game's AI** (its player left, or the host migrated and this game now
  runs the computer riders): its controller holds valid nodes from the same array and targets on
  the line. It goes on from there like any AI rider; nothing points outside the array (indexes are
  checked against it).
- **Older games:** rooms refuse mixed builds (`hello.version`), and the bundle version changes, so
  an older game would drop the bundles rather than misread them. The release needs no
  `MIN_VERSION` raise for this.

Bench (track 0, a new recording `truth5`, 888-byte records), line off against on:

| link | line off: error p50 / p95 / p99 | line on | jolts a minute off / on |
|---|---|---|---|
| 80 ms | 0.41 / 1.89 / 3.0 | 0.26 / 1.30 / 2.5 | 1.0 / 1.0 |
| 150 ms | 0.90 / 4.35 / 6.8 | 0.53 / 2.75 / 4.5 | 2.3 / 0.9 |
| bad | 1.15 / 6.16 / 12.0 | 0.65 / 3.65 / 6.3 | 3.2 / 1.5 |

The experiment that decided it: the puppets' own controllers' steering words on the bench (where
this game's controller already follows nearly the same line) took 150 ms from p95 4.44 to 3.08
and the bad link from 6.36 to 3.85. The owner's words were good, only late.

### Host stops

- **HOLD timed by its sender:** HOLD carries how long its sender's game has given no bundle when it
  leaves (u16 ms; 13 bytes now). A receiver holds a stop only from 0.25 s by that, so the link's
  delay no longer counts toward it (round 3 held even a 0.3 s stop for about 0.15 s).
- **Eased:** a held rider brakes to a stop along its way. Its speed when released, the catch-up
  glide's speed toward its sender's rate, and the speed guard all change at most 900 units a
  second each second (0.25 units of a step's travel at 60). The glide's position now goes through
  the usual slide, with its speed and acceleration limits.

The record also carries nothing new for the hold: HOLD itself grew by 2 bytes (13 now).

### The verifier's three (lead, after the merge of round 3)

- **`online_race.py --bots` crashed with a TypeError:** in the bots' loop a bot's open log file was
  named `log`, the name that held the games' log file name, and the next `da / log` failed. Renamed
  (`game_log`, `bot_out`). `online_race.py --dry-run` walks every path the tool takes (both games'
  settings, each bot's command line, every log it would watch) and starts nothing. `build_tests.py`
  runs it with and without bots, so `check.py port` (`net_tests`) would have caught it.
- **A reset rider drawn 312 units off for 0.2 s:** the host's player rider was reset to the track
  (a 260-unit jump, standing, crash flags 27 then 11: not the crashing bit). The guest placed its
  puppet at the owner's new pose, and in the same step switched it to its owner's State. That
  State's Enter is the game's own reset to the track, which put the rider on *this* game's idea of
  the nearest spot, 312 units from where the owner's reset had put it. The next placement (0.2 s
  later, the State already the same) corrected it. Now a State switch on a puppet keeps the pose
  the snapshots gave it (logged "kept in place at a State switch"). An owner's own jump between two
  snapshots, further than its speed explains, is logged and counted as a respawn. The live scoring
  counts placements that put a rider more than 20 units from its owner's real pose 0.05-0.3 s later
  (`puppet_lag.misplaced`).
- **After the last finish:** a finished rider's owner goes on sending its snapshots (and the host
  its computer riders') while its race screen runs: the owner's rider rolls out as its game runs it,
  and the puppet does what the snapshots say. But the network client stopped passing race frames the
  moment the room ended the race (`"ended"`, or this member out of the race), while both games show
  the race about 2 s longer (`online.c`: `leave_at = now + 2`). In that time every puppet rode on
  blind: in the verifier's PC to Mac runs the host's count of sent computer-rider snapshots froze at
  the last result (5409) with its race clock still running. The client now lets race frames pass
  for 3 s after the race ends (`client.h` `race_frames_until`; 0 once the game is back), so the
  last seconds are drawn from snapshots. Not measured live over the internet; the session test
  checks the frames pass and stop at `client_back`.


- **The phone's stuck rider** (the lead's race with a phone hosting): the PC's scripted rider stood
  stuck while pedalling for 2 s on its own screen (its game's collision recovery then put it back),
  and the phone's puppet of it rode on until 105-158 units ahead, placed as "far". Now the speed
  guard eases a puppet much faster than its owner down as well as a slow one up, and the recovery's
  jump is a respawn (the owner's own jump between two snapshots). Not measured with the phone.

### What the release must do

Nothing on the server. The bundle payload version is 8 (6 before; rooms already refuse mixed builds
by `hello.version`), the record is 44 bytes longer (the racing line), HOLD is 13 bytes (its stop's
length). No `MIN_VERSION` raise is needed for these: a mixed room cannot form. The recordings of
the bench tools are 888 bytes a record (`puppet_lag.SNAP_SIZE`); older recordings need re-recording.

### Live, round 4

Whole races (70 s, two games on the hidden desktop, local server, the real SFU, eight host computer
riders, the links of round 3), on bd2f84e4 (r4d: the racing line, the eased stops); three more on
8a9d41c3 (r4e: plus the reset and race-end fixes), the same seeds as race 1 and 2. No other online
runs on the PC except where noted: another worker's phone check held a game slot during the r4e
races, and one r4e race at 150 ms (seed 73) is left out (the two games never saw each other: no GO
skew, each won its own race).

| race | AI riders on the guest: error p50 / p95 / p99 | sunk / deep % | jolts a min | placed (first 5 s / later) | respawns | misplaced | catch-ups | guest sees host player p50 / p95 / p99 | host sees guest p50 / p95 / p99 |
|---|---|---|---|---|---|---|---|---|---|
| 80 ms #1 | 0.37 / 2.08 / 4.6 | 0.7 / 0.04 | 1.7 | 0 / 1 | 5 | 0 | 4 | 0.20 / 1.20 / 2.4 | 0.16 / 1.05 / 1.7 |
| 80 ms #2 | 0.44 / 2.34 / 4.3 | 0.9 / 0.06 | 2.2 | 0 / 2 | 7 | 0 | 1 | 0.21 / 1.31 / 3.7 | 0.13 / 1.27 / 4.7 |
| 80 ms #3 | 0.36 / 2.29 / 4.4 | 0.9 / 0.08 | 1.8 | 0 / 1 | 6 | 0 | 1 | 0.13 / 0.73 / 1.5 | 0.23 / 1.91 / 4.6 |
| 150 ms #1 | 0.68 / 3.59 / 7.2 | 0.7 / 0.01 | 2.9 | 0 / 0 | 6 | 0 | 22 | 0.35 / 2.91 / 10.1 | 0.24 / 2.05 / 4.3 |
| 150 ms #2 | 0.70 / 3.71 / 8.4 | 1.0 / 0.11 | 1.7 | 0 / 0 | 11 | 0 | 10 | 0.54 / 3.89 / 6.6 | 0.28 / 2.15 / 5.1 |
| 150 ms #3 | 0.71 / 4.47 / 10.3 | 0.9 / 0.06 | 5.9 | 0 / 0 | 10 | 0 | 30 | 0.41 / 3.12 / 6.1 | 0.41 / 2.88 / 8.1 |
| bad #1 | 0.85 / 4.88 / 9.7 | 0.8 / 0.03 | 4.8 | 0 / 2 | 9 | 1 | 17 | 0.23 / 1.96 / 4.4 | 0.33 / 4.26 / 10.3 |
| bad #2 | 1.05 / 5.83 / 13.9 | 1.5 / 0.16 | 12.1 | 0 / 0 | 12 | 0 | 55 | 0.32 / 2.06 / 5.3 | 0.32 / 2.30 / 6.3 |
| bad #3 | 0.92 / 5.50 / 10.4 | 1.1 / 0.06 | 4.0 | 0 / 0 | 7 | 0 | 30 | 0.32 / 3.83 / 6.6 | 0.32 / 2.55 / 4.8 |
| 150 ms, r4e #1 | 0.86 / 5.26 / 14.6 | 0.9 / 0.04 | 15.9 | 0 / 1 | 8 | 0 | 71 | 0.28 / 2.85 / 10.8 | 0.26 / 2.43 / 7.2 |
| 150 ms, r4e #2 | 0.77 / 4.43 / 8.6 | 0.9 / 0.03 | 7.5 | 0 / 0 | 9 | 0 | 29 | 0.36 / 3.12 / 5.2 | 0.28 / 1.86 / 5.2 |
| bad, r4e #1 | 0.86 / 5.24 / 12.1 | 1.3 / 0.09 | 8.1 | 0 / 0 | 1 | 0 | 43 | 0.33 / 4.31 / 9.0 | 0.31 / 4.29 / 8.1 |

Against the targets (150 ms: p95 under 4, jolts under 5 a minute, at most 1 placement a race; bad: at
most 3 placements; 80 ms no worse than round 3):
- **150 ms, r4d: met in races 1 and 2** (p95 3.59, 3.71; jolts 2.9, 1.7; no placements), race 3 just
  over (4.47, 5.9). **r4e: not met** (5.26 and 4.43; 15.9 and 7.5 jolts): race 1 had two SFU stalls
  (71 catch-ups bunched at 9 s and 46-47 s, snapshots 200-225 ms old), not the model (no State rule
  fired, no game late). Round 3: p95 5.7-7.8, jolts 6-13.5.
- **Placements: met.** 150 ms 0, 0, 0 (r4d) and 1, 0 (r4e); bad 2, 0, 0 and 0. The 80 ms ones (1, 2, 1)
  are an owner's reset (the host's player standing 69 units off, then 176-351 units: the game putting
  a rider back on the track without the crash bit), which r4e counts as respawns. None put a rider
  more than 20 units from its owner's pose (the new `misplaced` count).
- **80 ms: better than round 3** (p95 2.1-2.3 against 2.8-3.0; jolts 1.7-2.2 against 1.0-3.0).
- **Sunk: better** (0.7-1.0 % at 150 ms against 1.5-1.6 %).

What the remaining error is made of (r4d, the guest's view of the computer riders, steps over 30
units a second; a scratch script beside the runs, not in the repository):

| link | along p50 / p95 | sideways | height | turns p95 | straights p95 |
|---|---|---|---|---|---|
| 150 ms (three races) | 0.20 / 1.65-2.16 | 0.37-0.40 / 2.12-2.41 | 0.21 / 1.66-2.29 | 3.47-3.86 | 3.25-4.51 |
| bad (three) | 0.27-0.34 / 2.41-3.33 | 0.46-0.52 / 3.02-3.73 | 0.28-0.37 / 2.41-3.56 | 4.91-6.31 | 4.73-6.30 |
| 80 ms (three) | 0.10-0.13 / 0.90-1.08 | 0.20-0.25 / 1.19-1.64 | 0.11-0.13 / 0.99-1.05 | 1.84-2.20 | 2.15-2.63 |

The line is no longer the larger part: along, sideways and height are about equal, and turns no worse
than straights. What is left is the 150 ms itself (a jump's flight, a landing, a bump begun since the
last snapshot) and the link's stalls (the catch-ups come in bunches at the SFU's stalls).

### Host stops, live

Host test stops at race second 20 (`DHD_NET_TEST_HITCH`, 80 ms link), the guest's view of the nine
host riders from the stop to 3 s after it ends; "largest step change" is |p[k] - 2 p[k-1] + p[k-2]|
(units), every step but crashes (since round 4 the release step counts too).

| build | stop | holds | placed (not respawns) | catch-ups | largest step change |
|---|---|---|---|---|---|
| r4e | 0.3 s (two races) | 0, 9 | 0, 0 | 39, 33 | 3.49, 3.15 |
| r4e | 0.8 s (three) | 9, 9, 9 | 0, 0, 0 | 23, 26, 23 | 3.79, 3.68, 4.00 |
| r4e | 3 s (three) | 9, 9, 9 | 0, 0, 0 | 0, 0, 0 | 3.72, 3.98, 3.83 |
| r4f | 3 s (one) | 9 | 2 (far: 58, 50) | 28 | 54 (a placement) |

**Not met: steps of 3-4 units** (target 1.5). Where they are (rider 7, r4e 0.3 s): the glide
eases its speed up to the sender's catch-up rate (2.3 to 4.5 units a step, changes of 0.2-0.8 a
step), then at the glide's end the rider's own physics speed takes over in one step (4.5 to 1.0:
3.49). The game's physics keeps its own speed state, which a body velocity set each step only
overrides for that step. 70a288fa eases the speed back down after the glide and lets the speed
guard work both ways (for an owner stuck while pedalling: the phone race). Its one valid stop race in
round 4 (3 s, over the SFU) had two far placements during the catch-up (50 and 58 units, the puppet
faster than its owner); the other stop races of that build were lost to an SFU outage on this PC
(every attempt, both games, "SFU connection failed (DTLS error: timeout: handshake)" over the TURN
relay).

**70a288fa settled (the lead's closing round).** Build A = the head without 70a288fa's puppet
changes, build B = the head (86c3f4a1), built from the same tree, raced in turn (A, B, A, B, ...)
so the PC's load hit both alike. The SFU still failed every handshake from this PC (six attempts
dropped, all "DTLS error: timeout: handshake"), so the races ran in P2P rooms (`DHD_NET_CREATE=p2p`:
the host the hub, the guest direct inside the local network) with the same 80 ms test link on
both games; none of those was dropped. Other lanes ran state traces, a touch-control and an
OPTIONS walk on this PC meanwhile (races took about 5 minutes each). Host stops at race second 20,
the guest's view from the stop to 3 s after it; steps within three ticks of a respawn's placement
not counted:

| | placements outside respawns | largest step change | catch-ups |
|---|---|---|---|
| A, 0.8 s stop (three races) | 1 (far: 53), 0, 0 | 33.1 (the placement), 4.07, 3.82 | 18, 22, 17 |
| B, 0.8 s stop | 0, 0, 0 | 3.98, 4.34, 3.52 | 33, 33, 24 |
| A, 3 s stop | 0, 0, 0 | 4.68, 3.77, 4.26 | 0, 0, 0 |
| B, 3 s stop | 0, 0, 0 | 4.16, 3.94, 4.09 | 0, 0, 0 |

Ordinary races at 150 ms, two each: A p95 4.58 and 4.24, 10.8 and 6.6 jolts a minute; B 4.48 and
4.65, 15.2 and 6.7; no placements outside respawns in any (B's first race: 72 catch-ups in bunches).
B is at least as good on placements and no worse on steps, so 70a288fa stays, both parts. The two far
placements of round 4 did not recur in three 3 s stops; they came over the SFU, which could not be
tested again. Both builds still step 3.5-4.7 units at the stops (target 1.5).

The bench's stopped sender (truth5): r4d 0.8 s p99 17.4, 3 s p99 38.7; the final code (r4f) 16.7
and 35.5 (v0.7.16's model 8.4 and 23.1). The final code elsewhere: 150 ms 0.54 / 2.68 / 4.4 (2.3 jolts
a minute), bad 0.66 / 3.60 / 6.5 (3.3). During the stop the bench's truth is the sender's frozen rider; a receiver learns of the stop
a link delay plus the HOLD threshold (0.25 s by its sender's clock) late, so its riders have ridden
0.3-0.4 s past it (35-42 units at that moment), and then lag their sender's twice-speed catch-up by
9-26 units while their speed eases up. Holding sooner is what round 3's loaded races argued
against (a dead stop and a burst at every short stall); this trade is deliberate, and the bench
rows say what it costs at a long stop.

### Clips

`clips/live/` beside the other clips (not in the repository): live recordings replayed exactly
(`puppet_bench.py clip --live RUN`), the host's own rider on the left and the guest's drawing of it
on the right, 4x, 6 s. Watch first: `150ms-v0716.mp4` then `150ms-now.mp4` (computer rider 7 at
58 s; the v0.7.16 model had 8 steps of over 4 units there). Also `bad-v0716.mp4` and
`bad-now.mp4` (rider 7 at 30 s), and `hoststop-0.8s-now.mp4` (rider 3 from 19.5 s, the host
stopping at 20 s). The v0.7.16 and round-4 races are different races on the same track, the same
link seed: the riders' paths differ, the drawing is what to compare.

### Checks (round 4)

- On the head (the merge of local main b8c4cf88 and 70a288fa): `check.py port` OK (with the online
  tools' dry runs in `net_tests`), `check.py server` OK (one run beside the clips timed out a guard
  test at 5 s; the rerun on a quiet PC passed; this lane does not touch the server), `check.py
  docs` OK.
- `mosh_round_check.py` default scenario under `--link "delay=80,jitter=15,loss=1,seed=41"` on
  r4f: PASS.
- Unit tests: `test_state_bundle.c` (HOLD's 13 bytes), `test_puppet_sync.c`, `build_tests.py`
  (`test_session`: bundle version, HOLD's stop length, race frames for 3 s after the end).
- Not run: state traces (the change acts only online and in the bench switches), PC <-> Mac.

### Left (round 4)

- **Host stops' steps** (3-4 units): the rider's own physics speed state, which the glide's set
  velocity does not change. Write the speed where the physics keeps it (the bike's forward speed,
  `func_00137950`'s inputs; not traced) or keep easing the set velocity until the physics' own speed
  agrees.
- **70a288fa over the SFU:** its far placements (round 4, one 3 s stop over the SFU) did not
  recur in P2P rooms; a host stop over the SFU (PC to Mac) should show whether they do there. One
  thing to watch: since 70a288fa the guard's easing ends only when the puppet's velocity is within
  5 % of its owner's snapshot velocity (a vector, 100-200 ms old on a curve), where it ended on the
  speed alone before; the A/B showed no harm, but an easing that never ends would pull a rider along
  a stale velocity.
- **150 ms: race-to-race spread.** One race in three is over the line on this PC's SFU path. The
  internet links the verifier measured (PC to Mac, snapshot age p95 21-98 ms) are kinder than the
  150 ms test link.
- **After the last finish:** measured on the merged main (below): not fixed in rooms made from the
  menus. The race frames stop at the transport when the room ends the race, before the 3 s window
  in `client.c` can let them through.

## Round 4 checked on the merged main (2026-11-12)

On `main` 18f5e70c (49f7367a, then the merges of this round, of finding 215 and of finding 210), by
the lead's verifier. No other worker's build was running on this PC when the live runs started or
after they ended (not watched in between).

- `python tools/check.py port -j 6`: OK. `python tools/check.py server`: OK.
- State traces: all seven recipes (`boot`, `race`, `race2` to `race6`) equal the format-2
  references made from 524948ba (the same `state_trace_codeptrs.txt` and `state_trace_mask.txt`):
  7 of 7 PASS.
- **The SFU from this PC still fails.** Four tries of an ordinary two-game race through a local
  server (two at about 07:25, two at about 09:20): every session gave up direct UDP after 2 s, then failed over the
  TURN relay as well (`SFU connection failed (DTLS error: timeout: handshake)`), and the room took
  the racer out (`Realtime connection failed: out of the race`). In a PC to Mac SFU room the Mac,
  on the same network, failed the same way. Once it connected: in a P2P room hosted by the Mac
  with the PC's direct links off, the PC's session came up over UDP and carried two races
  (below). Everything else ran in P2P rooms (`DHD_NET_CREATE=p2p`, `cross_race.py --backend p2p`;
  the guest direct inside the local network), whose race sync is the SFU's. In runs where a
  session was failing, the race load stood 6-8 s on both games (`freeze:` at menu 9, waiting for
  the other's riders).

### Two games on this PC (P2P rooms)

`online_race.py --hidden --lag`, eight host computer riders, the test link on both games,
finishes at 70 / 72 s. Results equal on both games in every race.

| race | the guest's computer riders: error p50 / p95 / p99 | sunk / deep % | jolts a min | placed (not respawns) | respawns | misplaced | catch-ups | guest sees host p50 / p95 / p99 | host sees guest p50 / p95 / p99 | hit applied |
|---|---|---|---|---|---|---|---|---|---|---|
| 80 ms #1 | 0.38 / 2.45 / 4.9 | 0.7 / 0.03 | 3.1 | 1 (an owner's reset) | 7 | 0 | 2 | 0.12 / 1.65 / 2.2 | 0.13 / 1.69 / 5.7 | none landed |
| 80 ms #2 | 0.42 / 2.35 / 4.3 | 0.8 / 0.02 | 4.1 | 0 | 6 | 0 | 4 | 0.14 / 1.38 / 3.4 | 0.15 / 1.32 / 2.1 | none landed |
| 150 ms #1 | 0.73 / 3.95 / 7.2 | 1.0 / 0.09 | 3.8 | 0 | 5 | 0 | 13 | 0.29 / 3.33 / 6.8 | 0.23 / 2.28 / 5.2 | yes |
| 150 ms #2 | 0.80 / 4.94 / 9.5 | 1.2 / 0.17 | 5.1 | 0 | 12 | 0 | 20 | 0.28 / 2.41 / 6.0 | 0.28 / 2.39 / 4.5 | yes |

Against round 4's own table (SFU): 80 ms the same (p95 2.35-2.45 against 2.08-2.34); 150 ms within
its spread (3.95 and 4.94 against 3.59-4.47 for r4d and 4.43-5.26 for r4e). No hit landed at
80 ms (the scripted riders do not attack; a hit there is a computer rider's); at 150 ms both
games applied hits (`hit kind 5`, `hit kind 0`, event and victim). The one placement: the host's
rider was put back on the track by its own game (its record jumps 364.8 units at 68.35 s, standing)
and the guest placed it there 0.16 s later as `far`: drawn right (misplaced 0), but the rule that
counts an owner's own jump as a respawn missed it.

Host stops (`DHD_NET_TEST_HITCH=20:MS` on the host, 80 ms link), the guest's view of the nine host
riders from the stop to 3 s after it:

| stop | holds | placed (not respawns) | catch-ups | largest step change |
|---|---|---|---|---|
| 0.8 s | 9, 9 | 0, 0 | 26, 20 | 4.15, 5.26 |
| 3 s | 9, 9 | 0, 0 | 0, 0 | 3.70, 4.00 |

The guest holds every host rider 0.38 s after the stop begins (the link's 80 ms and the 0.25 s
threshold), releases them as the snapshots come back and catches up by riding. The host itself,
after its own 3 s stop, placed the guest's rider once in each race (201 and 134 units, 0.3 s after
it went on): its view was 3 s old, a placement after silence.

- `mosh_round_check.py` (default, `hit`): PASS (the guest scored 50 on the host's rider; the same
  table and finals on both games).
- A P2P room of six racers (two hidden games, four nettool bots, computer riders, 80 ms link) with
  the host's race menu open at race second 30 for 5 s: all six joined and readied, the race
  started for all, the ten results the same on both games, no freeze line. The host's race clock
  ran on under the menu (tick 1800 to 2114 in 5.2 s); the guest held nobody and caught nobody up
  in that time (snapshot age 101-116 ms throughout; two respawn placements, a bot's and a computer
  rider's). The bots stop sending at their finish (60 s), and the games then hold their riders.

### PC <-> Mac

The Mac (M1, 8 GB) ran its own build of 18f5e70c from this PC's wasm2c core; `cross_race.py`
through scratch wrappers (the PC's game on the hidden desktop, both games recording), P2P rooms
direct inside the local network unless said. Host finishes at 20 s, guest at 40 s. The room's
results were the same on both machines in every race.

| run | shared races | other player as drawn, p95 | drawn behind p50 / p95 | computer riders on the guest, p95 | placed (not respawns) | snapshot age p50 / p95 |
|---|---|---|---|---|---|---|
| PC hosts | 3 | 0.28-1.78 | 0 / 1-4 ms | 0.42-1.76 | 0 | 12-30 / 18-41 ms |
| Mac hosts | 4 | 0.21-1.55 | 0 / 0-4 ms, 119 once | 0.43-1.79 | 0 | 9-32 / 18-41 ms |
| Mac hosts, the PC over the SFU | 2 | 0.13-0.61 | 0 / 1-4 ms | 0.30-0.51 | 0 | PC 25-26 / 43-189 ms |
| PC hosts, `--force-turn` | 0 | the Mac's session failed (DTLS), out of the race | | | | |

Sunk at most 2.1 % of steps (computer riders 0.1-0.7 %); no hold or catch-up in the ordinary races; 17
catch-ups in one SFU race. In 3 of 4 Mac-hosted shared races the Mac host's own race screen never
showed the PC guest's finish (`PCGUEST finished` missing on the Mac; the room's results the same on
both).

- **Host stops** (0.8 s at race second 30, so the host's alone first race, which ends at about
  22 s, never reaches it): PC host, the Mac held 5 riders, no placement, largest step change 3.58;
  Mac host, the PC held 5, no placement, 11 catch-ups, 3.51. The Mac host placed the PC's rider
  once right after its own stop (61.7 units, its view stale). A first PC-hosted run lost its
  direct link (the Mac's direct sends failed `HostUnreachable`), raced over the failing SFU and
  was run again.
- **The race menu** (the host's, at race second 10 for 5 s): on each host it opened over the
  running race (ticks 600 to 913) and the other machine held nothing, caught nothing up and
  placed nothing; in the host's alone first race START was the game's own pause.
- **After the last finish (not fixed).** In the 2.8 s after the guest's finish the host draws the
  guest's rider off by p95 13-72 units and the guest draws the host's computer riders off by p95
  11-64 (the host's finished rider, standing, stays right). Both games go on sending, but neither
  receives anything once the room ends the race: the PC host's count of received messages stood at
  2390 from race clock 2401 to 2522, the Mac's at 3607 from 2403 to 2523. The window
  `race_frames_until` in `port/net/client.c` never sees these frames: on `ended` the Rust transport
  drops them first (`port/render/net/src/lib.rs` purges the queued STATE and EVENT frames;
  `sfu.rs` sets `race_open = false` and drops late ones). In `online_race.py`'s rooms
  (`DHD_NET_CREATE`) frames did arrive in those seconds. The missing finish lines above are
  probably the same drop (a finish event purged when `ended` comes first): not proven.

### Open (after this check)

- **The race's last seconds:** done in finding 216: an ended race's frames travel on every
  transport until the game leaves the race screen, and the last finisher's finish shows on every
  screen. Measured PC <-> Mac again on the merged main (finding 216, "Checked on the merged
  main"): after the end p95 0.02-1.96 units, inside the race's own error, against 13-72 here.
- **An owner's reset counted as `far`:** fixed in finding 216 (`sync_owner_jumped` in
  `port/hle/puppet_sync.h` looks at every snapshot pair since the last one used; the unit test has
  this case). In its checks on the merged main every placement was a respawn, none `far`.
- **Host stops' steps:** 3.5-5.3 units here (target 1.5), as before.
- **The SFU from this PC:** only two races measured (one direction, P2P fallback); no host stop,
  no TURN race over the SFU yet. 70a288fa over the SFU stays unmeasured.
