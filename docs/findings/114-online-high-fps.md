# 114: online races at the player's frame rate

Status: current
Checked: 2026-11-04

Branch `feat/online-high-fps` (from `release/0.7.1`), 2026-10-28. Online races used to be forced to
60 (`port/hle/fps.c`: `return pads_online() ? 60 : requested;`) as a safety default. Online is state
sync (findings 041, 044, 047): each PC runs its own one-player race and nothing has to stay in step
between PCs, so each PC can run at its own rate (60-240) as long as everything that crosses the wire
or times the online flow counts real time, not frames.

## What counted ticks or frames

The race clock `D_0043EEC8` (`race->timer`, func_0019EE98) is not a frame count: the game adds
`func_00238F60()` to it, which above 60 is fps.c's whole-field accumulator (finding 067). So it
counts 1/60 s fields at any rate: 60 a second at 144 as at 60. What did depend on the rate:

| Where | What | Problem above 60 |
|---|---|---|
| puppet.c `take`, the wire | snapshot stamped with the integer race clock | the stamp is up to a field (16.7 ms) early: the sender's steps between fields all carry the same tick |
| puppet.c `wanted`, `feed` | display moment `RACE_CLOCK + offset - delay`, offset samples | the local clock moves once a field, so a puppet on a 144 PC moved in 60 Hz jumps (held 2-3 frames) |
| `DHD_PUPPET_DELAY` | 6 ticks | 100 ms only because ticks are fields; now set in ms |
| field vs wall time | a field is 1001/60000 s at 60 (the NTSC grid), 1/60 s above | a 60 and a 144 race clock drift apart by 0.1% (0.4 s over a 400 s race); the offset tracked it, but in ticks the two clocks are not the same unit |
| online.c `rematch_step` | `++rematch_fields > 1800` and the 12-field press cycle | `pads_begin_field` runs every frame (each frame is a VBLANK), so at 144 the 30 s give-up came after 12.5 s and presses lasted 28 ms instead of 67 ms |
| online.c GO stamp | `server_ms - clock * (1000 / 59.94)` | a field is 1000/60 ms above 60 |
| kernel.c `vsync` | `if (pads_online()) port_fps_wall_mode(wall_steps)` every VBLANK | turned the wall-clock steps off online: with nominal steps a PC that cannot hold its rate runs in slow motion, and the shared pacing's catch-up (one extra VBLANK) only gives one more nominal step, not a field |
| fps.c `clock_interval` | at most 30 fields (0.5 s) of a stall repaid | online a longer stall would leave this PC's race clock behind the others for good (finding 047 fixed that at 60 with extra fields) |

Not changed, as they already count race ticks or wall time: the send rate (`SNAPSHOT_TICKS` = 3 race
ticks: 20 a second at any rate), the countdown start (server clock), finish times and the results
(race ticks of game time, the same unit at every rate), `DHD_NET_TEST_FINISH`, the puppet's forced
finish (`finish + 60` ticks), the results deadline (90 s wall), the race-end countdown
(`func_001B45F8`, 4*60: its timers `001B4688/4718/4900` are rate-aware since finding 067), the flyby
skip and menu presses (`DHD_NAV`, already checked at 120/144 in finding 067), `check_left` (every 15
frames: 10 a second at 144 instead of 4; it only reads the lobby copy).

## What changed

- `fps.c`: `port_fps()` returns the selected rate online too (log `fps: requested N real
  steps/s`). New `port_field_seconds(counter)`: a whole-field counter in seconds, the part of a
  field the high-rate clock is past it included (`phase`); 1001/60000 s a field at 60, 1/60 s
  above. `port_fps_behind()`: game time still owed to wall time, for the online log's "ms late".
  In an online race (race screen, phase 1-4) a stall is repaid in full instead of 0.5 s.
- `kernel.c`: the per-VBLANK wall reset is gone; `kernel_pace_slip` (lobby and ride waits) and
  `kernel_pace_shared` (the agreed start, a rematch) reset the wall steps instead, so a wait is
  no stall to repay. Above 60 `pace()` treats the shared clock as unshared: the frames are paced
  as offline and the wall-clock steps catch up (at most one field a step: 84 fields a second of
  catch-up at 144). At 60 the shared pacing and its extra fields are as before.
- `puppet.c`: wire version 3. A snapshot carries the sender's race time in ms
  (`port_field_seconds(RACE_CLOCK)`, rounded); the ring, the offset (remote minus local race
  time, seconds), the display moment, the 0.2 s extrapolation, the new-race reset (1 s back) and
  the jump reset (0.5 s) are in seconds on the local race time with its fraction, so a puppet
  moves every frame at any rate. `DHD_PUPPET_DELAY` is in ms (default 100). Hit EVENT payloads
  keep online.c's EVENT version 2 (the receiver drops any other). The online log's lag is in ms.
  `DHD_PUPPET_TRACE=file` (tests): each step, each puppet as shown (local race time, the remote
  race time shown, slot, position). `DHD_PUPPET_RECORD` writes the new Snapshot (with `time`)
  every step; `DHD_PUPPET_PLAY` feeds the first snapshot of each RATE-th tick.
- `online.c`: GO stamp from `port_field_seconds`; `rematch_fields` counts whole fields
  (`port_whole_step` above 60), and no press is chosen before the first field; the log line shows
  the rate.
- `nettool --replay` reads the ms stamps and reports its finish in ticks.
- Text: the launcher's frame-rate hint ("Game steps a second, offline and online."), the settings
  table comment, fps.h, docs/online-protocol.md. The server only relays (unchanged); rooms refuse
  other builds by exe hash, so the new payload never meets an old one.

Offline at 60 nothing changes: `port_fps()` is the same value offline, `port_field_seconds`,
`port_fps_behind` and `online_race()` are only reached from online code or above 60, `pace()`'s
`shared` equals `pace_shared` at 60 (and is 0 offline), the removed VBLANK line ran only online,
`kernel_pace_slip/shared` are only called by online.c, and every puppet path is gated on `pp.online`
or a `DHD_PUPPET_*` test variable. Offline above 60 is also unchanged (the stall cap applies
whenever not online). No game RAM layout or retail code changed.

## Checks

- `python port/tools/test_fps.py` (production fps.c in the core): PASS at 60, 61, 75, 90, 100,
  120, 144, 165, 240, with a new case: a 2 s stall, offline repays 0.5 s, an online race all 2 s
  (144: 4.0417 / 5.5417 s of game time over 3.5 s, as wanted), and a wait reset charges nothing.
- One online race through a local relay (`online/server`, `wrangler dev` on port 8791), two
  games minimised, `port/tools/online_race.py --minimized` (new switch), scripted pads,
  `DHD_NET_TEST_FINISH=40`: host A at `DHD_FPS=60`, guest B at `DHD_FPS=144`, native renderer.
  - Both finished and both have the same results: 1. Guest 0:40.01, 2. Host 0:40.03 (and the
    eight AI, not finished). GOs 6.1 ms apart on the server clock.
  - Snapshots: B sent 20.0 a second, A received 20.0; A sent 180 a second (its rider and the
    eight AI, 9 x 20), B received 179.8. A gave 2-4 extra fields in the whole race, B was never
    late after the start.
  - Puppet steps: A showed B on 2461 steps (60 a second), B showed A on 5914 (141 a second).
  - Clock: the remote race time shown against the local race time has slope 1.00004 (A) and
    0.99989 (B): no speed or time skew. Lag (shown time behind local time: the 100 ms delay, the
    trip and the GO skew) steady through the race: A 112-125 ms (median 119.6), B 87-98 ms
    (median 93.0).
  - Position of the puppet against the sender's own recorded path at the shown moment
    (`DHD_PUPPET_RECORD` every step on the sender): A shows B median 0.010, p95 0.048, max 0.57
    game units; B shows A median 0.024, p95 0.060, max 0.87. The few steps over 0.2 (8 of 2461,
    45 of 5914) are single moments when a hit or crash changed the velocity between two
    snapshots. Path length over the same span: puppet 1849.5 vs owner 1858.1 on A, 2900.5 vs
    2981.7 on B (B's span includes A's crash at tick 1651, during which the puppet's own crash
    runs and is not traced).
  144.cmd"). Both players need this exe.

## The next race after the results (issue #15)

- Repro: `port/tools/online_race.py --next again|new` (new): the host's test pad
  (`DHD_NET_HOST_NEXT`, online.c `host_next_pad`) picks TRY AGAIN or NEW LEVEL on the results
  menu, and the run waits for both games' next countdown. With `DHD_NET_TEST_FINISH=40`.
- Before the fix (60 fps, TRY AGAIN): both games reached the next race, but the host waited
  61.5 s and the guest 75 s on "waiting for 1 player to load". `start_together` sent EVENT_LOADED
  once. The guest loaded first, before the host's `rematch_reset`, which clears `on.loaded`; the
  host never saw it, started at its 60 s timeout, and its START came after the guest's 75 s
  limit. To a player that is the friend not coming along.
- Fix: LOADED, and once announced START (same `go_at`), are sent again every 0.5 s until the
  countdown. The online lines that matter here now carry wall times (choice, accept, reset,
  loaded), and the host says when it starts without someone.
- After the fix: NEW LEVEL to track 6, both at 60 and both at 144: the guest followed, the
  countdown came 1.5-1.9 s after loading on both PCs.
- The level list moves with RIGHT (cursor 0 -> 1 is ALPINE -> track 6); DOWN does nothing there.

### Reopened report: track 16 to track 2

The two 0.7.1 logs end just after rematch initialization, before either reports loading.
The host controller disconnected and reconnected during the first race. Both machines built the
new grid, including slots 8 and 9. This does not identify where execution stopped afterwards.
`kernel_pace_shared(0)` runs at reset; the high-rate wall clock caps accumulated debt outside
phases 1-4. The AI count is recomputed at race initialization, and the online rider count reads
the resulting setup. Neither observation establishes a fault.

The Windows log thread writes each pipe chunk immediately; stderr is unbuffered. Forced
termination can still lose bytes in the pipe or copier, and stdout could remain buffered. The
launcher log now makes stdout unbuffered as well. After a rematch, online logs menu and phase
changes once each until the countdown begins, so a later report can narrow the interval.

Local relay tests (minimized clients, forced finish at 40 s): an idle-input 60/120 run went
from track 16 to track 6 and reached both second countdowns in about 21 s after reset; this used
eight then six riders, not the reported transition. A scripted 60/60 run from track 0 to 6
also reached both countdowns in about 20 s. Both ran the pre-instrumentation game module.

An instrumented 60/120 run with idle player pads and `DHD_NET_TEST_NEXT_TRACK=2` went from
track 16 with eight riders to track 2 with ten. Both logged menu 9 phase 5 immediately after
reset, phase 1 about 25 s later, countdown about 27 s after reset, and second-race clock lines.
The test override changes the destination at restart acceptance after the host's scripted menu
choice, so it does not test selecting JUNGLE on the level list. It is retained for repeatable
tests. The module passed strict link and offset checks and the native build passed. These runs
do not reproduce the reported failure; there is no confirmed gameplay fix or root cause yet.
The next useful evidence is the new transition log from an affected machine, especially whether
it reaches phase 5 and then phase 1, and how long it remains in each.

The same 16 -> 2 run with the players' other settings also reached both countdowns in about 27 s:
the guest with movies on, the host with Unlock All off and the guest with it on, and both together.

## Crash 2 s after the finish at 240 (0.7.3 test)

A 240 fps guest (Mac host at 60) crossed the line and, 2.3 s later, before the results, stopped
with `host64 trap: Call stack exhausted (EE fiber 1, wasm depth 0)`: the stack cycled through
00204F38 -> 00204CE0 -> 00205D10, the evaluation of an animation's blend chain, one level per
blend, about 125 deep (wasm2c's limit is 512 calls). The chain was not cyclic: it was a
creature's (func_00125E38, type 6, ids 0x1018-0x1022). Once the player has finished and stopped,
those creatures walk to a point 3 units ahead of the rider; one that arrives from ahead ends
between the two thresholds (more than 12.2 units from the rider, within 10 of the point) and
switches mode 2 and back to 1 at every pick, each switch a 250 ms blend on top of the last
(func_00126B20). Retail picks once a field: about 30 deep. Above 60 every update picked: 120 deep
at 240. Offline the 8 s to the results usually hid it; online the race waits for the others.
The picks now run on whole fields (`port/patches_fps.py`), so the chain is as deep as in retail.

Repro (local, two games, real server; debug build only, not committed): the guest finishes as a
creature comes in range and is held still, with that creature's 150 lowered to 40 so it always
stops in the switching band. Without the fix at 240: the same trap, 101 deep after about 1 s.
With it at 240: 32 deep, no trap, results on both. At 60: 34 deep.

## Open (as written; since finding 139 every rate runs one delta-time clock, the race at 30-144
steps a second finishes with no trend against 60, and a 60 PC's clock follows wall time too)

- High-rate physics equivalence is still open (finding 067: a full-pedal race finished in
  392.5 / 432.7 / 402.4 s at 60 / 120 / 144 in round 52). Online, each PC's finish time is its own
  race's game time, so a rate whose handling differs gives its player a different time for the
  same riding. That, not the sync, was the reason for the 60 default; whether mixed rates are
  fair enough to ship is a release decision.
- A 60 PC's race clock runs at 59.94 fields a wall second, a 144 PC's at 60: the same game time
  takes 0.1% longer in wall time at 60. The puppets follow each other's real position (the
  offset tracks it); the finish order is by game time.
- The launcher's hint text changes only with the next launcher build.
