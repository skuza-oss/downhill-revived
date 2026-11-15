# 254: online riders and reconnects on bad networks (late or lost snapshots, outages, drops)

Status: in progress

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

- [ ] 2026-11-15: lane plan (this file).
- [ ] The measuring tools: the test link's held outages and one-off cuts, the bench's network
      profiles and its new counts (teleports, longest freeze, heading).
- [ ] Before: the profiles on the base model.
- [ ] Fixes, one at a time, each with its numbers.
- [ ] Reconnects: 1 s, 5 s, 20 s, room and race.
- [ ] Checks.

## Decisions

## Surprises

## Result
