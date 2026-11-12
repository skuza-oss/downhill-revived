# 222: a streamed sound whose emitter is gone (the crash on the rider screen after an online race)

Status: in progress

Branch `fix/sound-emitter-gone` from `main` d9714d0a, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The 0.8.0 gate (finding 211's successor run, `race_menu_check.py --scenario end`, about 1 run in 7)
saw the game die back in the room on the rider screen after an online race ended abruptly:
`host64 memory: read of uncommitted guest address 0x000000000` in `func_00208C60` (the angle to the
sound listener) <- `func_001BBF30` <- `func_001BC4F0` (the update of the four-slot streamed sound
manager) <- the front end. Done means:

- the mechanism named from the code (which sound, which path, why only sometimes);
- whether 0.7.16 and offline play have it;
- a deterministic (or at least 30-run) repeat that crashes before and not after;
- the cause fixed in `port/` (no general readable address 0), siblings checked;
- all ten `race_menu_check.py` scenarios pass; boot and race state traces equal to the reference
  build; `check.py port` green on the last code commit, `check.py docs` on the last.

## Progress

- [x] 2026-11-12: finding reserved

## Decisions

## Surprises

## Result
