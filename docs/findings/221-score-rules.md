# 221: score rules: the server's mode, one final per rider, a table that stands

Status: current
Checked: 2026-11-13

Branch `fix/score-rules` from `main` c5f839a2, started 2026-11-12. Release-blocking fix lane for
0.8.0, from an outside review of v0.7.16 to c5f839a2 (its findings 4, 7 and 9). Another lane (220)
fixes the review's other findings in the puppet code; this one touches only the score rules of
finding 204.

## Goal and acceptance

Three defects of the Mosh Bowl round (finding 204), each fixed with a test:

1. A finish's `score` on an ordinary race could switch the whole race's ranking to score.
2. Two finals for one rider (the player's own and the host's takeover) were arbitrated by arrival
   order.
3. A published score table kept changing after it was published.

Done: `python tools/check.py server` and `python tools/check.py quick` green; `docs/online-protocol.md`
says when `score` is accepted and how a scored round ranks. No protocol version change, no
`MIN_VERSION` change, no change to the score message's format. A 0.7.16 game behaves exactly as
before in every room it can join. Out of scope: `port/hle/puppet*.c`, `state_bundle.h`,
`port/render/net`, a server deploy (the lead deploys with a canary).

## Progress

- [x] 2026-11-12: lane started, finding reserved (e41ffc65).
- [x] Server: the race's mode follows its track, tests (29c6906a). `check.py server` green.
- [x] Game: one final per rider by authority, tests in both arrival orders (8922e9bb).
- [x] Game: the published table is a copy, test (6b9f1a5a).
- [x] `docs/online-protocol.md` (29c6906a); `check.py quick` green on the code of the last commit
  (docs only after it).

## Decisions

- **The mode is the track.** The server has no other source: the game decides its round the same
  way (`lobby.race_track == MOSH_TRACK && lobby.server_scores`, `online.c`). `room.ts` gets
  `SCORED_TRACK = 23` (mirrors `MOSH_TRACK` in `port/hle/mosh_round.h`) and `isScoredTrack`. It is
  read from `race.track`, which `startRace` sets once from the track the grid locked on, so the
  race "knows at its start" without a new stored field (an extra field would be state a rollout's
  previous version cannot know; the track is already stored and never changes during a race). The
  room's `pickOnly` is not used: a host sets it, a track list is not a rule.
- **A score on an ordinary race is ignored, not refused.** The finish itself is valid; an error
  would only add a message to handle. A 0.7.16 game never sends `score`, so both are compatible;
  ignoring cannot drop a finish.
- **Finisher without a score in a scored round.** It ranks after every finisher with a score, by
  ticks, before DNFs (one comparator: group, then key; no "mode switches when any result has one").
  Rooms keep builds apart (a joiner's version must equal the host's), so the mix of an older game
  and a scoring one in one arena round does not occur; the case that does occur is a room whose
  games all send none (0.7.16 on the arena, Single Event): everyone is in the no-score group and the
  race ranks by time, exactly as before. The room's per-member state shows `score` only for a
  scored round's finisher that has one, else `finishTicks`.
- **Finals: authority, not arrival.** `mosh_take(held, got, sender, by_player)` (`mosh_round.h`).
  `by_player` is "the sender's slot is the rider's global id", which `receive_score` already
  computes (`own_player`) from what it knows: the sender and the rider id, no wire change. The
  player's own final replaces a takeover final and nothing replaces it; between takeover owners the
  one that differs from the held final's sender wins (the receiver only accepts finals from the
  rider's current owner, so the different sender is the later owner). The held score carries
  `from` and `by_player` (not on the wire). The room's report of a human's finish counts as the
  player's own (`by_player`).
- **The published table is a copy** (`on.table`, `mosh_publish`) made when `table_done` is set,
  with the last known score for a rider whose final never came. The results rows
  (`port_online_round_rider`), the logged table, the reported place and finish score, and the
  numbers written into the riders for the results screen after that (`mosh_step`) all read it. The
  15 s wait is unchanged.

## Surprises

- The game side needed no wire change for item 2: the receiver already knows both the sender and
  the rider's own player, as the review's text suggested.
- The results screen reads the rider objects' point fields, which `mosh_step` rewrote every frame
  from the live scores: copying the table for the order alone would not have made the points stand.
  Now `mosh_step` writes the table's numbers into the riders once it is published.

## Checked on the merged main (2026-11-12)

On `main` 3073462c (merged at 2872444f), by the lead's verifier.

- `python tools/check.py server` (`npm test`, `npm run typecheck`) and `check.py port`: OK.
- `mosh_round_check.py` (default, `hit`): PASS.
- A round where a player leaves: `mosh_round_check.py --scenario ride --bots 1` through a scratch
  wrapper whose nettool bot gives up 60 s into each race (`--give-up-after 60`: the room retires it,
  each game hears `left`, the host takes its rider over). In the round both games played, both
  published the same table 0.0 s after time up: Mai (AI) 5450, Ajax (AI) 2050, Bot0 1500, HOST 350,
  Jonnie (AI) 150, GUEST 50. Bot0's 1500 is the host's takeover final (`(sent)` on the host,
  `(from player 0)` on the guest); the bot sent no final of its own, so the rule had no own final
  to prefer. Each game's own reported finish matches its table (HOST 4th, 350 PTS). The room's
  results list the leaver last with no points: `4. HOST 350 PTS; 5. GUEST 50 PTS; 6. Bot0 DNF GAVE
  UP` on both games. That follows the server's rule here (a leaver is a DNF, ranked after every
  finisher) but differs from COMBAT RESULTS, where Bot0 is 3rd and GUEST 6th, so
  `mosh_round_check.py`'s own order check fails on such a round (it compares the room's order with
  the table's). Not a difference between two players: both games show the same table and the same
  room results.
- An ordinary race on the local server ranks by time: `online_race.py` with both finishing (70 /
  72 s) gave `1. Host 1:10.03  2. Guest 1:12.03` on both games, and so did the SFU room's race.

## Result

Server (`online/server/src/room.ts`): a race on track 23 is a scored round; `score` on any other
race is ignored. Tests (`test/session.test.ts`): the review's case (A 600, B 900, C 1200 with
score 1: A, B, C, C has no score in its state), a scored round with and without scores, the
existing scored tests now set the arena track. `docs/online-protocol.md` says so.

Game (`port/hle/online.c`, `mosh_round.h`): `test_mosh_round.c` covers both arrival orders of the
player's final 500 and the host's takeover final 400 (500 both ways), two takeover owners in
both orders, and a published table that stays when a final arrives after it.

**A 0.7.16 game sees nothing different.** It sends no `score`; its finishes take the same path in
`room.ts` (ranked by ticks, `finishTicks` shown, DNFs last in grid order), also on track 23, where
it plays Single Event: the test "a scored round ... no scores at all ranks by time" posts exactly
that. No welcome, message or snapshot field changed. The game changes are in code 0.7.16 does not
have.

**What two players can still see differently after this** (no more machinery was added):

- A rider's final that arrives after one game's 15 s but before another's: the first publishes its
  table with that rider's last score, the second with the final. Their tables, and the places they
  report, can differ for that rider. It needs a stalled or disconnected owner (a final goes out at
  the owner's own time up and arrives within a round trip), so rare; the room's own ranking is
  whatever each player reported.
- A takeover final from a new owner that reaches a game before that game has learned the rider
  changed hands is dropped by `receive_score`'s owner check (older than this lane, unchanged); that
  game counts the rider with its last score at 15 s. Only when the leaver's own final also never
  arrives.
- A rider that changes hands after a game published its table is not reflected there at all.
- A rider whose player left mid-round: its table row is the takeover final (both games alike),
  but the room's results rank it last as a DNF with no points, so the room page and COMBAT RESULTS
  order differ (Checked on the merged main).
- Different clocks: games publish at different moments (each starts its two minutes at its own
  GO), so a table published by the first game is by design its own.

Review notes: the review's three findings are right as described. For item 4 it suggests "reject";
this lane ignores instead (above). For item 9 it offers deferring publication as the alternative;
not done, to keep the timing.

Not checked: an SFU room, a phone or the browser, the core's behaviour after a rider's
takeover final in a real round.
