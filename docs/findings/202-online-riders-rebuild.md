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
- [ ] The full before/after matrix on three tracks, the interpolation-buffer comparison, clock
      cases, clips, the live two-game race under `DHD_NET_LINK`, docs, checks.

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
- Sources: Glenn Fiedler, "Networked Physics" (gafferongames.com, 2014-2015: state
  synchronisation, the receiver simulates and applies the sender's state with smoothing); Jared
  Cone, "It IS Rocket Science! The Physics of Rocket League Detailed" (GDC 2018: other cars
  simulated with their last inputs between server states). No code taken.

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

## Result
