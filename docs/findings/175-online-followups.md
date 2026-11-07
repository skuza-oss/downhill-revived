# 175: online follow-ups after 0.7.12 (rejoin in a race, setup that never takes a race down, TURN check, clock, flood limit)

Status: in progress

Branch `fix/online-followups` from `main` 9298a71, started 2026-11-07. Local main merged in three
times for the lead's production hotfixes (b0812d6 SFU_CONNECT_MS at 1246a15; 0d6eee3 at 6fd2a5d;
637f1dc, the coalesced subscription pass, with hotfix 3 reverted, at ee9b074). 174 is taken by
`perf/mali-gpu`.

## Goal and acceptance

Six follow-ups from the PC↔Mac runs A-D (findings 169 and 173), and the lead's additions:

1. A player whose room WebSocket drops at any phase gets back into the room and the race; server
   and client tests; whether the abuse guard was involved in runs A and C. Lead: a racer whose
   setup fails must never take the others down (0.7.12 failed in rooms of 3-8), client side too;
   the start wait for a missing racer shorter or non-blocking; a 4+ racer setup measured; room
   SHCF93's late release (production) found and fixed; the subscription pass tested.
2. A PC↔Mac race over forced TURN (both ways, 3+ races each) and 2+ UDP races each way on the
   final build, plus a racer that never connects; result table.
3. The shared clock (finding 169's `clock_sample`) under a slow one-way path: a test switch, a race
   with it, symmetric ages, no puppet drawn ahead or behind; the base build for comparison.
4. A bug report draft for Cloudflare Realtime (outside the repository).
5. `tools/worktree_setup.py` leaves `online/server` usable off the system drive.
6. The SFU flood limit (`SenderRate`) keeps the newest STATE messages over the limit.

Done: `python tools/check.py port` and `python tools/check.py server` pass; boot and race state
traces equal 9298a71's.

## Progress

- [x] 2026-11-07: finding reserved (1a8f986).
- [x] Item 1, server: bf6f1f1; tests `online/server/test/race-rejoin.test.ts` (5).
- [x] Item 1, client: ddfe0fc; tests in `port/net/test_session.c`, `test_loopback.c`, Rust
      `a_rejoins_full_channel_list_reconciles`.
- [x] A racer that never connects: 8715952 (client `sfu_ready` fallback), 68cc24d (8 s), f7734cd
      (SFU_CONNECT_MS 15 → 10 s), 480e0ae (a racer's own session and TURN credentials off the
      room's SFU queue).
- [x] Room SHCF93: 8d7c005 (410 close = closed, cleanup off the alarm, reconcile per race, release
      asked again each tick), ee9b074 (the coalesced pass's repeat runs for the current race),
      2bf98fd (the release logged once); tests `online/server/test/release.test.ts` (6).
- [x] My batched subscription (90830c0) reverted (290b9c7): the lead's hotfix 3
      did the same, was slower in production and was reverted on main for hotfix 4.
- [x] Test switches: 29aa442 `DHD_NET_TEST_PONG_DELAY`, bd4a473 `DHD_NET_TEST_DROP`, c3d713d
      `DHD_NET_SFU_TURN=0`, all in TESTING.md.
- [x] Item 6: ac2c9b6. Item 5: 32980fa. Item 4: the draft (outside the repository).
- [x] Item 2 and 3: PC↔Mac runs T1, T2 (TURN), U1, U2 (UDP, forced and natural drops, a racer
      that never connects), K-lane, K-base (clock), M4 (4 racers); M5 (4 racers, final server).
- [ ] `check.py port`, `check.py server`, state traces.

## Item 1: a room connection lost in a race

### What happened in runs A and C

From the logs of finding 169's PC↔Mac runs, kept outside the repository (A-machost-r1/r2, A-pchost-try1, C-pchost, C-machost-try1) and the
code at 9298a71:

- Both games lost the room WebSocket at the same moment, during a race's countdown (`connection
  closed (1006)`: os error 10054 on the PC, 54 on the Mac). In C-machost-try1 the PC, in the room
  at that moment, logged `rejoining` and was `back in room` at once; the Mac, in its race, was not.
- **No rejoin was ever tried in a race.** `client_on_closed` treated any close while `racing` as
  final (`offline`, `NET_FAILED`, the "protocol 1" rule), and the Rust send thread dropped its SFU
  association whenever the socket was down and at every `welcome`. The "refused" in the logs is
  the room menu showing that same drop's own reset after the race closed, not a refused rejoin.
- On the server a 1006 in the member's race made it leave at once: both members went and the room
  ended with them.
- **The guard was not involved.** No connection was attempted after the drop, and the temporary
  Worker of runs A-D had no rate limiters, D1 or bans. With production's limit (30 connections a
  minute per address hash) two games on one address making 5 attempts each stay well under; the
  server test sends 6 failing tries plus both rejoins from one address and all get in, and the
  runs below had the limiters on. A `rate_limited` refusal during a rejoin is now retried.
- The drop itself points at the shared home uplink: in run U1 below the PC's room connection
  dropped by itself mid-race and the rejoin reached another Cloudflare location (ORN instead of
  MRS): a route change.

### What the fix does

Server (`room.ts`, protocol doc "Limits"):
- A hello listing `race_rejoin` marks the member. Its socket dropping (1006) in its race keeps it
  in the race for the 20 s grace while its game races on over the SFU. Not back in time: out as any
  racer who leaves.
- The rejoin's hello sends `raceStartAt`; the welcome says `race: {startAt, sfu}` (`ready`: keep
  the session; `setup`: start it again) and is followed by what it missed: `left` for racers gone
  since, its full channel list (`sfu_channels` with `full`), `sfu_ready` if released; or the
  `start` again when the hello names another race or none.
- `sfu_restart` (3 per racer per race) clears a session the rejoined game can't keep.
- Games without `race_rejoin` (0.7.12) keep the old rule (their SFU died with the socket).

Client:
- The association lives through the socket drop: race frames still go out, the SFU's signaling
  waits for the welcome, which keeps it, restarts it from the saved start (`sfu_restart`, new
  offer), or drops it.
- `client.c`: a drop in a race starts a rejoin; the state stays `NET_RACING` (the game sees
  nothing); a finish or BACK said during the gap is said again. A refusal other than
  `rate_limited`, or the end of the attempts, leaves it racing alone as before.
- A connection that ends within 5 s no longer resets the backoff, and the end of the attempts
  makes the state `NET_FAILED` (in the room it stayed `NET_RECONNECTING` forever before).

Across machines (runs below): U1's Mac lost its room connection 1.5 s before its first race's
start moment (`DHD_NET_TEST_DROP=-1.5`, during the SFU setup), rejoined, restarted its session
(UDP, 877 ms after the answer) and raced; U1's PC lost its connection by itself mid-race 3 and
rejoined as host, back in the race; U2's PC was cut mid-race (`=75`) and rejoined. Every one of
those races finished with the same results on both machines.

## A racer whose setup fails, and the start wait

- **Server:** the lead's b0812d6 (a racer not connected by SFU_CONNECT_MS leaves alone), kept.
- **Client fallback (8715952, 68cc24d):** a game said `sfu_ready` only with every racer's channels,
  so on a server without SFU_CONNECT_MS one broken racer hung every game until the 25 s deadline
  took the whole race out. Now, READY_TIMEOUT (8 s) after its own association connected with its
  own channels open, a game says `sfu_ready` anyway. The room still releases only when every
  connected racer is subscribed both ways, so nothing is lost; against an older server only the
  broken racer is out at 25 s. Rust test `ready_without_a_racer_that_never_connects`.
- **The wait players see.** The game exchanges rides (`exchange_rides`, port/hle/online.c) in a
  blocking loop at the load screen, before the race build that reads them; rides travel as SFU
  events, so nothing comes until the release, i.e. until the room drops the missing racer. The
  loop pumps window messages but the game draws nothing, and re-presenting (`gs_vsync`) would
  show the same picture, so it can't be made to look alive from there. Making it yield needs a
  new hold in the game's load step before the race build (like `hle_online_countdown_hold`); not
  done here (a game-code patch with its own state-trace risk). Instead the wait is shorter:
- **SFU_CONNECT_MS 15 → 10 s (f7734cd).** Setups measured on 2026-11-07 PC↔Mac, from the answer
  to connected: UDP 0.69-1.46 s (92 setups, median 0.89), TURN/TLS 2.94-3.22 s (5). With the
  answer itself well under a second when a racer's `sessions/new` doesn't queue (480e0ae), that is
  at most about 4 s after the start; 10 s keeps 2.5 times that. Measured frozen wait with a racer
  that never connects: about 11 s at 15 s (T2, every race on both machines), about 6 s at 10 s
  (U1, U2).
- **A racer's own setup off the room's SFU queue (480e0ae).** Every `sfu_*` message ran in one
  queue per room, so in a full room the last racers' `sessions/new` (0.25-0.40 s each in
  production) and a TURN racer's credentials waited behind the others' publications and
  subscriptions. A racer's first session of a race and its TURN credentials touch nothing of the
  others and now run at once; a late session is still closed if the race moved on. Test: an offer
  answered and credentials minted while a subscription call hangs in the queue.

## Room SHCF93: a release that waited minutes

Production's Workers Logs (the lead's read-only query), 2026-11-07 02:28-02:36: the setup line came at 68 s
("setup after 68271 ms: 0/SA:ready 1/SA:ready 2/SA:ready") instead of at 15 s, and the release
only at 222 s, right after the lead's deploy reset the room (02:35:52). Room UXJ2ZB the same.
Meanwhile the room retried `datachannels/close` of gone sessions every 5.4 s (410 each time).
- A close answered 410 (the session is gone) counted as a failure and went to the pending cleanup,
  retried on the room's SFU queue; the alarm awaited that pass, so its setup checks ran a minute
  late.
- The reconcile flag was one for any race: an older race's reconcile, still queued, swallowed the
  running race's; it re-checked only its own race; and nothing asked for the release again (at
  SFU_SETUP_MS no racer is late when all are ready). The lead's hotfix 4 pass had the same shape
  (one `subscribing` flag, its repeat ending on the race it was first asked for).
- Fixed (8d7c005, ee9b074): a 410/404 close counts as closed; the cleanup pass runs in the
  background, one at a time; the pass's repeat runs for the room's current race and checks the
  release after each pass; every alarm tick asks for the release again while it hasn't come.
  Tests fail without each part. Also (2bf98fd) the release is logged once: a racer finishing
  released the rest again and logged "released" with fewer racers, which made production show
  bursts like "released 203945 ms" for 1-5 racers.
- Hotfix 4's pass, tested (lead's request): six racers connecting 20 ms apart get every ordered
  pair subscribed once and none twice (30 calls), and are released.

## Item 2: PC↔Mac runs

All on 2026-11-07 between this PC and the M1 Mac on the same home connection, through a
temporary test Worker (this branch's server, rate limiters on, no D1), deleted
afterwards; the method of finding 169's PC↔Mac report (a local copy of run D's
driver with a third racer). Lane build 68cc24d (PC and Mac; T1 and T2 at c3d713d, the same client
but READY_TIMEOUT 10 s and SFU_CONNECT_MS 15 s). PC game headless, one game on the PC (the owner
was gaming). "Behind" and "distance" are `puppet_lag.py` per race across the two machines
(`lag_cross.py`); the Mac's view in T2 and U2 matched only 256-385 steps (its first race had a
racer the PC didn't see), so those two rows are left out. Snapshot ages are medians of the 30 s
windows, worst window in brackets.

| Run | Racers, transport | Viewer | Age p50/p95/p99 ms (worst p99) | Windows p99 > 300 | Gaps > 100 ms / min | Behind p50/p95/p99 ms | Distance p50/p95/p99 units | Results agree | Drops / rejoins |
|---|---|---|---|---|---|---|---|---|---|
| T1, PC hosts | 2, Mac on TURN | PC | 26/40/56 (82) | 0 of 8 | 0.8 | 0/3/6 | 0.14/0.75/1.53 | 4 of 4 | 0 |
| | | Mac | 20/34/59 (501) | 1 of 8 | 1.0 | 0/4/90 | 0.13/0.98/9.15 | | 0 |
| T2, Mac hosts | 3 (+NOCONN), Mac on TURN | PC | 23/30/42 (95) | 0 of 7 | 0.6 | 1/5/8 | 0.28/1.20/1.84 | 4 of 4 | 0 |
| | | Mac | 6/25/48 (380) | 1 of 7 | 2.0 | - | - | | 0 |
| U1, PC hosts | 3 (+NOCONN), UDP | PC | 21/41/57 (106) | 0 of 8 | 0.3 | - (638 steps) | - | 4 of 4 | 1 natural mid-race / 1 |
| | | Mac | 16/37/60 (304) | 1 of 8 | 2.8 | 0/4/13 | 0.11/0.58/1.39 | | 1 forced at setup / 1 |
| U2, Mac hosts | 3 (+NOCONN), UDP | PC | 10/22/28 (895) | 1 of 5 | 2.5 | 0/4/12 | 0.12/0.88/5.37 | 4 of 4 | 1 forced mid-race / 1 |
| | | Mac | 32/112/236 (1001) | 2 of 6 | 6.7 | - | - | | 0 |
| M4, PC hosts | 4 (2 bots), UDP | | | | | | | 3 of 3 | 0 |
| M5, PC hosts | 4 (2 bots), Mac on TURN, final server | | M5_ROW | | | | | | |

- TURN carried every race of T1 and T2: the Mac connected through TURN/TLS 2.9-3.2 s after the
  answer each time (UDP blocked by `DHD_NET_SFU_UDP=0`). No regression from finding 173's
  sctp-proto patch on the relayed path: ages and gaps as on UDP, 0-1 bad window per run.
- NOCONN (a nettool racer with UDP and TURN blocked, `DHD_NET_SFU_TURN=0`) was dropped alone at
  the deadline in every race ("left at race tick 0"), the others raced; the client fallback logged
  `ready without every racer's channels` each time.
- The bad windows (p99 over 300 ms) are single pauses on one direction, with the room's ping
  spiking in the same window (U2 race 3: 301 ms); U2's drop race itself stayed at p99 294 ms.
- M4: 8 subscription calls (one per pair, before hotfix 4's pass) of 264-386 ms, all done within
  1.4 s for 4 racers. M5_NOTE

## Item 3: the shared clock under a slow one-way path

`DHD_NET_TEST_PONG_DELAY=100@20` on the Mac: from 20 s after joining, every pong it gets is held
100-150 ms, a slow return path (run C's Mac had round trips of 150-240 ms instead of 66-76). PC
hosts over UDP, 4 shared races each, the lane build (K-lane) and the base 9298a71 with only this
switch cherry-picked (K-base, both machines; local commit d1edcc1 on a temporary ref, never merged).

| | Mac's ping to the room | Age on arrival, PC sees Mac / Mac sees PC (median, per-window range) | Behind p50/p95/p99 ms, PC / Mac | Distance p50 units |
|---|---|---|---|---|
| K-lane | 168-177 ms (43 before the delay) | 23 (21-31) / 23 (1-32) | 0/4/8 / 0/4/13 | 0.14 / 0.13 |
| K-base | 178-186 ms (53) | 23 (0-33) / 36.5 (26-41) | 0/4/8 / 0/4/14 | 0.14 / 0.14 |

- Symmetric within the paths' own asymmetry, and no puppet drawn ahead or behind (behind p50 0 on
  both sides). Without finding 169's rule the Mac's offset would have moved by half the excess,
  50-75 ms (run C saw 45 ms). The base already has that rule (56c8ce1), so it doesn't skew either;
  no change to the clock in this lane.
- The limit, by design: the round-trip floor rises 0.2 ms a second, so a delay that never lets up
  is trusted after roughly 8-10 minutes (here the sessions lasted about 7).
- Separately, a few windows in other runs showed "0 ms on arrival" on one side for a while (T2's
  Mac, around U1's rejoins): an offset taken over a path whose two directions differ by a few
  tens of ms, which a round trip can't see.

## Items 4, 5, 6

- **4:** `realtime-datachannel-ordering.md`, with the lead (not in the repository, not
  sent): subscriber channels ordered and reliable whatever policy is asked, finding 173's numbers,
  steps with the public API only, no ids or addresses.
- **5 (32980fa):** the junction's target is a folder named node_modules
  (`DHD_SCRATCH/worktrees/<name>/online_server/node_modules`) and setup runs `npm ci` there next to
  copies of package.json and the lock, again when the lock changes (`--npm`: always); `--remove
  PATH` unlinks the junctions `git worktree remove` leaves. Checked on a fresh worktree on C::
  typecheck, `npm test` 142/142 and `local_server.py`'s wrangler dev through the junction, a rerun
  up to date, then removed.
- **6 (ac2c9b6):** over 240 a second per bundle index, a message whose stamp is newer than the
  newest taken still counts, at most 240 more a second (a flood stays at twice the limit). Test
  `snapshot_flood_keeps_the_newest`: a 5 s backlog of 300 arrives whole, newest last.

## Decisions

- Item 1: the race goes on over the SFU through the drop; one rejoin path for room and race (the
  token). `race_rejoin` is a hello feature, not a protocol bump. A session still being set up at
  the rejoin is started again rather than resumed.
- The start wait: shortened (10 s), not made non-blocking (above).
- 10 s kept after hotfix 4 (the lead left it to me): with a racer's own session and TURN off the
  queue, connecting no longer depends on the room's size or on the subscription pass.
- My batched subscription call reverted for the lead's hotfixes 3 (same idea, slower in production)
  and 4.
- Item 3's switch delays incoming pongs, the direction that skewed run C.
- Item 5 installs into the target rather than relying on guidance alone.

## Surprises

- The client never tried a rejoin in a race at all (above).
- In the room, a rejoin whose attempts ran out stayed `NET_RECONNECTING` forever, and a refusal
  reset the Rust backoff to 1 s.
- str0m panics on a duplicate negotiated stream before the association is up; our check now also
  looks at our own channel list.
- `git worktree remove` leaves the junctions; one points into the main checkout's
  `tools/compilers`.
- Creating a junction on F: started to fail during the lane ("The file or directory is corrupted
  and unreadable", any target, cmd/PowerShell/Python); junctions on C: pointing at F: and folders
  on F: work, no NTFS or disk event logged. Not this lane's to repair.
- Two local two-game runs on this PC ran the second game out of GPU memory (the owner's game held
  5.7 of 8 GB) and showed the out-of-memory box: the PC↔Mac runs used one headless game here.
- A killed host now stays in its race for the 20 s grace, so a public room can stay listed that
  long: K-base's first try joined such a room and was refused (`room_not_found`).
- A natural room drop mid-race happened in U1 (route change); the fix carried it.
- `wrangler tail` sampled the test Worker's logs (11 of about 200 events kept), so setup times
  come from the clients' logs and the few calls it kept.

## Result
