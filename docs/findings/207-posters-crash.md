# 207: Posters in OPTIONS closes the game (issue 87)

Status: in progress

Branch `fix/posters-crash` from `main` ad0135bd, started 2026-11-12. This file is the lane's plan
while it runs and its finding when it merges. Someone who has only this file and the git log must
be able to pick the lane up.

## Goal and acceptance

GitHub issue #87: on 0.7.16 (Windows) choosing Posters in the game's OPTIONS menu closes the game
at once, every time. The reporter's log has `host64 memory: write of uncommitted guest address
0x006040478` and runs with Unlock All on (finding 095), widescreen, the native renderer at scale 2,
`PROMPTS=auto`, `TEXTURES=on`, an Intel UHD GPU.

Done means:

- the crash reproduced on current main (hidden desktop, untimed) with the exact commands, and gone
  after the fix with the same commands;
- the real cause fixed in `port/` (a patch in `port/patches*.py`, `src/` keeps matching), not the
  row hidden and not the fault caught;
- the neighbouring OPTIONS rows and galleries that Unlock All opens, or that load pictures the same
  way, opened and closed with Unlock All on and off without a fault;
- Posters captures at `DHD_SCALE=4`;
- a cheap test if one can be made (a headless walk of the OPTIONS rows);
- `python tools/check.py port` and `check.py docs` green on the last commit;
- with Unlock All off and never touched the game state is unchanged: the seven state-trace recipes
  against the format-2 references are left for the lead (listed under Result, with why).

Out of scope: state traces (the lead runs them), benchmarks, the online rider code
(`port/hle/puppet.c`, `puppet_sync.h`, `online.c`), the Bike Shop crash on Android (208) and the
flyover skip with touch controls (209).

## Progress

- [x] 2026-11-12: finding reserved
- [ ] name the frames of the reporter's log in the 0.7.16 linker map
- [ ] reproduce on current main
- [ ] cause, fix, neighbours

## Decisions

## Surprises

## Result
