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
- [ ] The final matrix (`--label final`, v8) on three tracks; the 4x clips; `check.py port`.

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
