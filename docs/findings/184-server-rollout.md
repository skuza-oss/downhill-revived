# 184: server rollouts that restart rooms only at a safe moment

Status: current
Checked: 2026-11-09

Branch `feat/server-rollout` from `main` 0574b05, started 2026-11-08. 183 is taken by the
performance settings lane; 180 (P2P) and 182 (SFU call timeouts) are other lanes on `room.ts`.

## Goal and acceptance

A deploy restarted every busy room within 5 minutes (Durable Objects' default deferred code update,
`max_delay` 300 s). Rooms never go idle while players are in them (an alarm every 2 s, a game
message every second), so they never hibernate, and the restart lands at a random moment: in a
countdown, at a grid lock, in a race. It drops every room WebSocket; during a grid lock the
Realtime subscription pass is cut short. On 2026-11-08 a deploy did this to an 8-racer room
(T7496B) at its grid lock; with slow API calls, two racers were taken out of the race.

The owner's decisions (2026-11-08):

1. Rooms adopt new code only at a safe moment: no race, no locked grid, no countdown, no Realtime
   setup pass running. The room saves its state, tells its players, and resets itself
   (`ctx.abort()`); members rejoin by token (finding 137) and keep their seats; Realtime sessions
   survive when the stored state allows. Empty rooms adopt on their next start. Worker↔room and
   stored-state changes stay forward and backward compatible, with a test.
2. `code_update_strategy: { mode: "deferred", max_delay: 86400 }`: the safe point does the work,
   24 h is only the limit.
3. A short notice before the room restarts itself, shown by the game as a footer line during the
   reconnect; 0.7.13 clients ignore it and rejoin as now.
4. One deploy command (`node online/server/tools/deploy.mjs`): `check.py server`, upload a version,
   deploy it to about 10% of objects (gradual), watch Workers Logs against the previous version,
   then 100% or an automatic rollback, and a summary.
5. An emergency override: 100% with immediate updates, as before.

Done means: unit tests (safe point, version comparison, the notice, old/new stored state);
`python tools/check.py server` passes (and `check.py port` with equal boot and race state traces
for the client's footer); an end-to-end run on a temporary test Worker (deleted after) with 6+
nettool bots: version B deployed through the tool while a race runs, the room finishing that race
on A and adopting B at its next lobby moment with a short reconnect, everyone back and the next
race running, plus the 10% step and an induced rollback. README and RELEASE-CHECKLIST describe the
new flow. Out of scope: deploying production (the lead does).

## Progress

- [x] 2026-11-08: finding reserved (7df50f2).
- [x] Server: `src/rollout.ts`, the room's hooks, the rollout row, `version_metadata`,
      `code_update_strategy` 24 h; tests `test/rollout.test.ts` (16) (2851d04).
- [x] Client: the notice as the room page's footer while rejoining; `test_session.c` (fb2bd64).
- [x] `tools/deploy.mjs` and `tools/rollout-verdict.mjs`, tests `test/deploy-verdict.test.ts` (6);
      the protocol doc (71be405).
- [x] README, AGENTS.md, BUILDING.md, RELEASE-CHECKLIST.md (ea67446).
- [x] End-to-end runs on a temporary test Worker (deleted after): runs 1, 2 and the emergency path
      (below). `check.py server` passes (198 tests).
- [x] `check.py port` passes; the boot (600 rows) and race (5000 rows) state traces of this
      branch's `dhd64.exe` equal 0.7.13's reference rows (`state_diff.py`: PASS both; the
      reference was made from the 0.7.13 release with `--env DHD_LAUNCHER=1`, as these).

## Decisions

- **A rollout row, not only the Worker's version.** The brief had the Worker pass its version to
  the room (done: header `x-dhd-worker-version`), but alone that cannot work with a canary. During
  a gradual deployment the stateless Worker splits per request while each object is assigned one
  version; a room on A hears B from 10% of requests and can't know whether Cloudflare assigned it
  A (a restart brings it back on A, for nothing: 90% of busy rooms would restart once per canary)
  or B. And a busy room gets a Worker request only when someone joins or rejoins: a room whose
  players stay put would never hear. So `tools/deploy.mjs` writes the deployed version into the
  moderation store (`settings` key `rollout`): empty while split (no room moves), the version id
  at 100% (rooms on another one restart), the old id after a rollback (rooms on the canary go
  back). Rooms read it with the kill switch's one D1 query (kept 30 s per isolate), from their
  2 s alarm. Without a row (a self-hosted server with `wrangler deploy`, no D1) the Worker's
  version decides: newer upload time than the room's: restart.
- **Futile restarts are bounded.** A restart that brings the room back on the same version (the
  deployment hadn't reached it yet, or the row names a version Cloudflare doesn't give it) is
  retried after 2, 10 and 30 minutes, then given up for that target (Cloudflare's 24 h does it).
  The record is its own storage key (`update`), so an older room never sees it in `room`.
- **The safe moment** also excludes a hello in flight (a first join cut at its connect would fail:
  it has no code to rejoin) and an automatic race due within 10 s (members must be back before the
  grid locks). Restart from the alarm only: it runs every 2 s, so the results moment (the race just
  closed, the next one 30 s away) is always seen.
- **The restart** is `ctx.abort()` after `storage.sync()`, the notice and a 1012 close per socket,
  and a 250 ms pause so the frames leave first; a `restarting` flag makes every handler, `save()`
  and `fetch()` (HTTP 503) do nothing meanwhile, so the sockets' own closes don't remove members.
  `retryAlarm: false`: the next alarm is already set. Cloudflare documents abort() for this (the
  state API; vitest's `abortAllDurableObjects` is the same reset).
- **Close 1012, not a 4xxx refusal:** every release rejoins after a close below 4000.
- **The notice is a new type** (`notice`), not an `error`: 0.7.13 shows an `error`'s message, and
  it would then also show it after the rejoin; an unknown type falls through its parser.
- **The canary's verdict** (`tools/rollout-verdict.mjs`): per version over the same minutes, error
  lines (not a restart's own abort line), invocations ending in an exception (less the restarts),
  "out of the race" lines per object; fail when a count is at least 3 and above twice the old rate
  plus a margin. Room counts are reported, not judged: busy objects assigned to the canary keep
  logging as the old version until they move, so the new version's share is not the split.
- **Thin canaries go on** by default (a small player base often gives the canary no room at all);
  `--require-traffic` rolls back instead. Ctrl-C rolls back.

## End-to-end runs (2026-11-08)

A temporary Worker `dhd-rollout-test` on the official server's account (workers.dev, its own D1
with the migrations, the four Realtime secrets, `code_update_strategy` deferred 24 h; deleted with
its database afterwards), the method of finding 175's runs: seven nettool bots from this PC in one
session room (auto next race, 45 s races), all really connected to the SFU. HOST and NEW0-2 run
this branch's client; OLD0-2 run main's (finding 181's build), which has no `notice` handling,
exactly as 0.7.13. The driver and its logs stay in the lane's scratch folder (`DHD_SCRATCH`, not
committed).

**Run 1: version B while a race runs.** `deploy.mjs --canary 10 --watch 3 --poll 30` started 10 s
into race 1. Room A8RQA6's own lines (Workers Logs, version per line):

| UTC | Version | Line |
|---|---|---|
| 21:57:26 | | deploy.mjs: rollout row "hold", then B@10% A@90% |
| 21:58:03-22:00:58 | A 3e0e3ff0 | races 2-5 released 405-2040 ms after their start, 7 racers (the canary moved nobody) |
| 22:00:42 / 22:01:04 | | deploy.mjs: B@100%, then the rollout row = B (race 5 running) |
| 22:01:47.952 | A | `restarting for new code at a safe moment: A -> B, 7 members, try 1` (race 5's results) |
| 22:01:49.262 | B 83d3f5d3 | `back 1331 ms after its restart for new code, now on B` |
| 22:01:50.112 | B | `all 7 members back 2181 ms after the restart` |
| 22:01:56.548 | B | race 6 released 294 ms after its start, 7 racers; race 7 297 ms |

Every bot: `connection closed (1012), rejoining`, then `back in room A8RQA6` (the host as host);
the four new clients logged `the server restarts the room for new code` first; the three old ones
rejoined the same way without it. All seven raced races 6-8. No `sessions/new`, no subscription
pass and no TURN call after the restart: every Realtime session, publication and subscription
came back with the stored room. (Race 1 had one racer out, `member 5 out of the race: no Realtime
connection`: NEW0's room socket dropped (1006) before race 1 on this PC's loaded uplink, room pings
then 1-5 s; unrelated to the deploy.)

**Run 2: an induced rollback.** Version C = this branch plus one `console.error` on every
`/v1/health` (a copy outside the repository), 10 requests a second at the Worker meanwhile, a
fresh 7-bot room racing. At the second poll, 62 s into the canary: `new 40 lines, 20 errors` against
`previous 1023 lines, 0 errors` → `canary failed: errors: 20 (500.0 per 1000 lines) against 0`,
B@100% again 4 s later, the rollout row = B 21 s after that; exit code 1, 108 s in all. The room
(on B) raced six races, 7 racers each, released 147-346 ms after their starts, and never restarted.

**Emergency:** `--emergency` uploaded and deployed at 100% with `immediate`, then set the row
(27 s). Not run with a room in it: that is today's behaviour (every busy room reset at once).

### What a restart costs

- The room: reset to its first request again 1.3 s (the games' rejoin backoff is 1 s, then a
  WebSocket handshake), every member back 2.2 s after the restart began. The players' footer says
  "Server updated, reconnecting..." for that long (old clients show the room page without a
  footer).
- Realtime: nothing. The sessions are in the stored room (finding 177), the games keep their
  associations through a room drop (finding 175), and the welcome says `"sfu":"ready"`: no API call,
  and the next race released as fast as before (294 ms).
- The 250 ms pause before the reset, and a hello cut in that window (HTTP 503, retried).
- Once per room per deploy, between races. Rooms nobody is in start on the new code.

## Surprises

- The canary saw no room at all in either run (`new 0 objects` until the faulty Worker's own
  requests): the existing busy room stays on its code while split, and Cloudflare assigned no new
  object to the canary in 3 minutes. With today's handful of rooms most canaries will be "thin"
  for rooms and test only the stateless Worker (lobby list, health, latest, the connects); the
  tool says so and goes on unless `--require-traffic`. A longer `--watch` gives new rooms a chance.
- `npm ci` in a worktree replaces the node_modules junction with a real folder on the system drive
  (TESTING.md says so); `tools/worktree_setup.py --move` moves it back off it.
- vitest's Workers runtime runs `ctx.abort()` faithfully: the room tests reset the object and the
  test clients rejoin a fresh instance, so the restart path is covered without a deploy.

## Result

Filled in at the merge. For the lead:

- Merge after 182 (it touches `room.ts`'s SFU path; this lane's hooks are a few lines each plus
  one new section before the helpers), then `check.py server` once more.
- The first production deploy with this code is itself still the old way for the rooms already
  open: they run 625b5b91, which has no safe-moment logic, so they reset at Cloudflare's limit.
  With this branch's `wrangler.jsonc` (24 h) that limit is 24 h, so rooms open at the deploy would
  run old code up to a day, or until they empty. Deploy it at a quiet hour with `--emergency`
  (immediate, as today) or accept the day; every later deploy is the staged one.
- Production needs nothing new: the rollout row lives in the existing `settings` table (no
  migration), the version metadata binding is in `wrangler.jsonc`, and the logs token is the
  existing read-only one (it can run `telemetry/query`).
