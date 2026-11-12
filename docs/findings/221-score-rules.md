# 221: score rules: the server's mode, one final per rider, a table that stands

Status: in progress

Branch `fix/score-rules` from `main` c5f839a2, started 2026-11-12. Release-blocking fix lane for
0.8.0, from an outside review of v0.7.16 to c5f839a2 (its findings 4, 7 and 9). Another lane (220)
fixes the review's other findings in the puppet code; this one touches only the score rules.

## Goal and acceptance

Three defects of the Mosh Bowl round (finding 204), each fixed with a test:

1. A finish's `score` on an ordinary race could switch the whole race's ranking to score.
2. Two finals for one rider (the player's own and the host's takeover) were arbitrated by arrival
   order.
3. A published score table kept changing after it was published.

Done: `python tools/check.py server` and `python tools/check.py quick` green on the last commit;
`docs/online-protocol.md` says when `score` is accepted and how a scored round ranks. No protocol
version change, no `MIN_VERSION` change, no change to the score message's format. A 0.7.16 game
behaves exactly as before in every room it can join. Out of scope: `port/hle/puppet*.c`,
`state_bundle.h`, `port/render/net`, a server deploy.

## Progress

- [x] 2026-11-12: lane started, finding reserved.
- [ ] Server: the race's mode follows its track, tests.
- [ ] Game: one final per rider by authority, tests in both arrival orders.
- [ ] Game: the published table is a copy, test.
- [ ] `docs/online-protocol.md`; checks.

## Decisions

## Surprises

## Result
