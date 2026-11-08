# 184: server rollouts that restart rooms only at a safe moment

Status: in progress

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

- [ ] 2026-11-08: finding reserved.

## Decisions

## Surprises

## Result
