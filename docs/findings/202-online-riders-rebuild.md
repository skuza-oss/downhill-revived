# 202: online riders that look right: a bench of today's puppets, then a rebuild

Status: in progress

Branch `feat/online-riders-rebuild` from `main` 6e2f7d23, started 2026-11-11. This file is the
lane's plan while it runs and its finding when it merges. Someone who has only this file and the
git log must be able to pick the lane up.

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

- **A sender whose game stops (a hitch).** v8 on track 0: a 0.8 s stop, error p50/p95/p99 0.45 /
  2.55 / 14.4 units (today 0.97 / 3.5 / 8.4; another run of the same build gave p99 16.4: the
  bench's tails vary a little from run to run, the AI controller's state not being part of the
  replay); 3 s behind, 0.50 / 7.8 / 114 (today 1.0 / 6.2 / 23).
  The tail is the stop itself: the sender's rider stands still on its own screen while the
  receiver hears nothing, and the receiver cannot tell a stopped game from lost packets. The
  puppet rides on for up to 1 s (`SYNC_COAST`), then brakes; when the snapshots return it is put
  back (a re-sync). Today's model froze after 0.25 s, so its error was smaller there, but it froze
  in every loss too (outages: today p99 27.8, v8 7.9). Braking earlier traded the common case for
  the rare one (v9, Decisions). What would fix it: the sender's network thread (it keeps running
  while the game thread hitches) sending "no game frame since T" in place of STATE, so receivers
  hold the rider at once; a protocol addition, not made here. The catch-up afterwards is followed
  (the sender's clock rate, Decisions).
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

### Left for the lead

- Boot and race state traces against 6e2f7d23 (`state_trace_run.py boot` / `race`): every change
  is behind `pp.online` or the bench switches (`DHD_PUPPET_PLAY`, `DHD_PUPPET_RECORD`,
  `DHD_PUPPET_TRACE`, `DHD_NET_LINK`) except the snapshot stamp, which only online races send.
  Not run in this lane (the game lock was full of other lanes' state traces).
- A PC <-> Mac race both ways over the internet (`cross_race.py`), and with `--force-turn`.
- A race with a phone in a P2P room (Android: nine puppets now run physics, as ten riders do
  offline; watch the phone's frame rate and the `online:` lines' "off by" figures).
- A 6+ player room (`p2p_crowd.py`, and an SFU room with bots).
- For the owner when they play: other riders' wheels on the ground at landings, dips and rises;
  no jumping back and forth; a rider that crashes or respawns appears at its respawn at once
  (that jump is real on its owner's screen too). A player whose game hitches will ride on for up
  to a second on the others' screens, then be put back.
- Merge: finding 169's item 4 (the straight-line prediction) becomes partly superseded by this
  finding; 044's "Interpolation" section already points at 169.
