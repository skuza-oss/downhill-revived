# 206: the online test tools drive their riders; the room card's state beside its tags

Status: in progress

Branch `fix/online-test-riders-and-room-card` from `main` 9b919298, started 2026-11-11. This file
is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A. The online test tools' scripted riders take input. `DHD_RACE_PAD` applies only with
`DHD_RACE_SYNC=1` (`port/hle/autopad.c`), which `mosh_round_check.py` set only offline and
`cross_race.py` never: their online riders neither pedalled nor attacked (finding 204,
Surprises). Done when `python port/tools/mosh_round_check.py` (its default run) passes three times
in a row with a human scoring on another human's rider, and `docs/TESTING.md` says how scripted
pads and posted keys combine.

B. The room page's player card draws a racer's state (STARTING, READY, ...) over the HOST or YOU
tag on its first line. Done when no state the line can show overlaps the name or the tags, at
widescreen and 4:3 and the smallest name size, with captures before and after at `DHD_SCALE=4`.

Out of scope: the puppet code (lane 202), the state trace (lane 205), offline play.

## Progress

- [x] 2026-11-11: finding reserved
- [x] 2026-11-11: `port/tools/online_riders.py`, the one place for the online tools' race pads:
  `rider_env(script)` gives `DHD_RACE_PAD` with `DHD_RACE_SYNC=1`, `PEDAL`, `BRAKE`, `attacks()`,
  `pad()`. `mosh_round_check.py` and `cross_race.py` use it; `online_race.py` already set both
  and is left as it is (lane 202 changes it).
- [x] 2026-11-11: `mosh_round_check.py --scenario hit` (the default) and `--scenario ride` (the
  old run, now with input); `cross_race.py`'s riders pedal, punch and watch as its text always
  said.
- [x] 2026-11-11: `hidden_game.Game.key` refuses posted keys when a menu script is set;
  `docs/TESTING.md`: the tool rows, the two switches, two traps.
- [x] 2026-11-11: the room card's first line and second line (`online_menu.c` `draw_card`), and
  the live header above the cards, which had the same fault ("RACE RESULTS - LIVE" ran into
  "3 FINISHED - 5 RACING" in every build).

## Decisions

- **The hit scenario's recipe is finding 204's verifier's**: no computer riders, both riders hold
  the brake all round, the guest attacks every 0.6 s from 5.2 s. The first hit lands at the gates
  (race tick 471-758 in the runs below); it knocks the host's rider aside, out of reach, so a
  round here has one hit (the verifier's had two). One is all the check needs (the guest's points
  and the host's hits taken, the same on both games), and steering after the knock would depend
  on where it lands.
- **The check reads each game's own final** (`round: NAME's final score P, H hits taken (sent)`)
  and the other game's copy (`(from player N)`), of the round the table belongs to, besides the
  table and the room's results the tool already compared.
- **`cross_race.py`'s pictures**: with `DHD_RACE_SYNC=1` the GS captures follow
  `DHD_RACE_CAPTURE_INTERVAL` (fields, `gs/present.c`), not `DHD_CAPTURE_AT`/`_INTERVAL`, so the
  tool asks for every 900 fields from field 1200: the same 15 s from 20 s at 60.
- **Posted keys and menu scripts: refused, not only documented.** `hidden_game.Game.key` raises
  with the reason when the game has `DHD_NAV`, `DHD_AUTOPAD` or `DHD_AUTOPAD_FRAMES`, so
  `live_game.py`'s `key` lines fail at once instead of doing nothing.
- **The card's state keeps its size; the name line gives way.** The left part (place, name, HOST
  or YOU) takes the card's width less the state's and a gap (at most the old 112), shrinks with
  `fit` down to 60 %, then the name loses its last letters, never the tag. The second line
  likewise keeps the details left of the ping's bars. A name of nine W (the widest letter) keeps
  six or seven letters beside STARTING, seven beside READY; ordinary names keep all nine.
- **The live header** takes the longest of "RACE RESULTS - LIVE", "RESULTS - LIVE", "LIVE" that
  fits beside the counts, as the lobby's header already did with its forms.
- **"The smallest name size"**: the `names` setting (`DHD_NAMES`, off/small/medium/large) sizes
  the names over the riders in a race (`online.c`), not the room page; the card's smallest name
  is `fit`'s 60 %, which the captures' twelve-letter W names reach.
- **The browser** draws the same page from the same core (`online_menu.c`); `port/web/www/net_layout.js`
  is the network structs' layout (offsets), not the page's, so nothing there changes.

## Surprises

- **A pad script shuts the keyboard out for the whole game** (`host_pad_read`: the script, else
  the device), and `DHD_RACE_PAD` is read only inside `host_pad_script`, so a race pad without a
  menu script does nothing either.
- **The verifier's one failed hit run was a full disk** (`present capture: There is not enough
  space on the disk`, then the room's connection closed), not the recipe. A default run writes
  about 2 x 100 pictures at 1280x960.
- **The room only starts a race when every member is ready** (rules AUTO, "WAITING FOR 2 PLAYERS
  TO GET READY"): a member who stays CHOOSING holds the room, so the race captures had the
  choosing and waiting bots ready later.

## Runs

All on this PC: a local server per run (`local_server.py`), P2P rooms, hidden desktops at
`DHD_SCALE=4`, untimed game lock.

`python port/tools/mosh_round_check.py OUT` (the default, `--scenario hit`), four runs, the last
three back to back on the branch's final build:

| Run | Hit (race tick) | Table on both games | Finals (owner sent = other got) | Room results | Result |
|---|---|---|---|---|---|
| 1 (tools only) | 479 | `1. GUEST 50 (0) 2. HOST 0 (1)` | GUEST 50/0, HOST 0/1 | `1. GUEST 50 PTS; 2. HOST 0 PTS` | PASS |
| 2 | 758 | the same | the same | the same | PASS |
| 3 | 471 | the same | the same | the same | PASS |
| 4 | 473 | the same | the same | the same | PASS |

Each run's first round is the host's alone (the scripted host starts before the guest is in),
`1. HOST 0 (0)`; the second has both.

The room page, before (the build before the card change) and after, at 1280x960 (4:3) and
1706x960 (16:9; the page is drawn 4:3 and pillarboxed, the same layout): a nettool bot's P2P
room (HOST on a bot), eight more bots, the game joining by the code (YOU), ten cards, names of up
to twelve letters (`WWWWWWWWWWWW`, `MMMMMMMMMMMM`, `LONGNAMEXYZW`). States captured: READY, WAIT,
CHOOSING (the lobby); STARTING on all ten (the countdown); DONE with `1ST 0:25.00`, RACING, OUT
with `DNF GAVE UP` (live, the game back in the room); `DNF TIME UP` (after the race); the arena's
`1ST 50 PTS` / `2ND 0 PTS` with HOST and YOU beside STARTING (a hit run's room). Before: STARTING
covers HOST, YOU or the name's end on six of ten cards, READY and CHOOSING touch the longest
names, the live header's title runs into its counts. After: no overlap in any of them.

`python tools/check.py port` and `check.py docs`: OK on the branch's last commit.

## Result

A: the online test tools' riders ride. `mosh_round_check.py`'s default run is a round in which a
human scores on another human's rider, checked on both games (points, hits taken, each final,
the table, the room's results); it passed four runs out of four. `cross_race.py`'s riders pedal,
punch and watch. `online_riders.py` is the one helper; `hidden_game` refuses posted keys to a
game with a menu script, and `docs/TESTING.md` says both traps.

B: the room card's state no longer covers the HOST or YOU tag or the name in any state, at 4:3
and 16:9, with ten players and the longest names; the live header's title no longer runs into
its counts. Online room page only (`online_menu.c`, part of the core: the core id changes).

Not checked here: `cross_race.py` itself (it needs the Mac; only its environment changed, and
its pad scripts are the ones `online_race.py` already ran with `DHD_RACE_SYNC=1`), the
`--scenario ride` and `--offline` runs after the change (their pads did not change; `ride` now
takes input), the room page on a phone or in the browser (the same core code), and a name in
non-Latin letters (the name rule allows only `SETTING_NAME_LETTERS`).

Left for the lead:

- The boot and race state traces against the base build (`state_trace_run.py boot`, `race`):
  the change touches only the online room page, drawn only online, so both must be equal.
- After lane 202 merges: `mosh_round_check.py` (default) once more, since lane 202 changes how a
  remote rider runs and so where the knocked rider lands; the check needs one hit.
- `online_race.py` can take `online_riders.rider_env` once lane 202 is in (it sets the same two
  switches by hand today).
