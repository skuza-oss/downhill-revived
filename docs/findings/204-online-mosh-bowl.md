# 204: online MOSH PIT rounds run by the game's Mosh Bowl rules

Status: in progress

Branch `feat/online-mosh-bowl` from `main` 1578b6c5, started 2026-11-11. This file is the lane's
plan while it runs and its finding when it merges. Someone who has only this file and the git log
must be able to pick the lane up.

## Goal and acceptance

An online race on the Mosh Pit arena (track 23, `MOSH PIT (ARENA)`, the host's pick only, 128)
ends the way the original game's arena mode ends: the game's Mosh Bowl rules (mode 15), a
two-minute timed contest with its own scoring, then the results. Before this lane the room ran
every track in Single Event mode (12), where the arena has no finish (203).

Acceptance: a two-game online Mosh Pit round in a local room (and one with bots up to the limit)
starts, plays two minutes by the mode's rules with hits scoring, ends by itself, and shows the
same score table on both games and in the room; offline Mosh Bowl unchanged; unit or tool tests
for the score messages and the final table; `check.py port`, `check.py server` and `check.py
docs` green.

## The game's Mosh Bowl, as the decompiled source has it

Names below are this finding's, for the functions and fields as identified; `src/` keeps its
splat names.

**Starting it.** The single-player modes page (`func_001ED900`) has Mosh Bowl as its fourth row,
locked on a new save until `D_004225A0 +0x1219B` is set (the port's Unlock All opens it). The
Mosh Bowl menu (`func_001EB1D8`, the shell's menu code, around its row
handler) has two rows; both set `SETUP +0x10C = 15` (the mode) and `+0x110 = 0`, and track 23
(`SETUP +0x2A0 = 0x17`). Row 0 sets `SETUP +0x364 = 1`, row 1 sets it to 0: **two variants**.
The race build (`func_001B7198`, its end) sets the round timer `D_004B1F40[0] = 0x1C20` (7200
ticks, two minutes) in mode 15, whatever the variant. `func_001B89C8` (the computer riders'
count) gives the arena five, as in any mode on it; mode 15 changes nothing there. The loading bar
is `LMOSH` (`func_00182330`, 203).

**The race loop.** `func_001B40A0` (a race frame) calls `func_001B4718` (the **round's end
check**, `obj` = `D_004B1F40`) in mode 15 only. The HUD of mode 15 is `func_001772B0`
(`func_00176ED8` calls it instead of the race HUD).

**Variant 0, the timed round** (`+0x364 = 0`; what this lane runs online):
- `func_001B4718` counts `D_004B1F40[0]` down by one each race tick in the race phases
  (`D_0043F348` 2-4; the port's delta clock counts whole ticks, `port/patches_fps.py`). At 0 it
  puts the end message on each local player's HUD (`func_00175708(hud, 0xAA)`) and ends the race
  120 ticks later (`func_001B45F8(obj, 1, 120)`).
- **Points.** Every hit a rider deals (`func_0012E940(victim, kind, direction, attacker)`, the hit
  handler) gives the attacker points by the attack (`func_001301E0(attacker, kind % 5)`, the
  **score adder**): punch (0) 50, kick (1) 100, check (2) 200, stick (3) 300, bottle (4) 400, times
  the rider's multiplier `+0x7BA0` (always 1.0: `func_0012C450` and `func_001576D0` set it). They
  go into five counts (`+0x7ACC`, `+0x7AD0`, `+0x7AC8`, `+0x7AD8`, `+0x7AD4` for kinds 0-4), whose
  sum is the rider's score (`func_001301B8`, the **score**). The adder also shows "+N pts" on the
  attacker's HUD (a local player's) and adds to the race score object (`+0x6610`).
- **Every hit is a crash** in mode 15: `func_0012E940` raises any force below 3600 to 3601, so the
  victim always takes the big reaction (`func_00155E78(+0x3FD0, 5/8/9)`, the crash) and respawns
  as after any crash.
- **Hits taken**: the victim's `+0x79AC` counts every hit it takes (`func_0012E940`, after its
  first returns: a victim already crashing, `+0x4098` bit 2, or with `+0x7BB6`, takes nothing).
- **HUD** (`func_001772B0`): the time left in seconds (`D_004B1F40[0] / 60`) at the top left;
  the six-digit counter shows the rider's score instead of the race's points (`func_00177F80`).
- **End message** (`func_00175730`, the **round's end banner**): `CONGRATULATIONS!` when no other
  rider's score is higher than this rider's, else `NICE BATTLE`; it reads the scores every frame
  while it shows.
- **Results** (`func_001BA7D0(obj, 1)`, the race's end, opens `D_0038ACE0`): **COMBAT RESULTS**
  (`func_00160A48`, the **combat results' table**): every rider, by score, highest first (a tie
  goes to the higher rider slot: `>=`), with its rider's name, score and hits taken (`HITS`); then
  `COMBAT RECORD:` and the best score ever (`D_004225A0 +0x12214`, raised to any rider's score:
  the save's record). Its rows (`func_00160980`) are TRY AGAIN (text id 0x28, `func_0015C990`),
  SAVE (0x23, `func_00187AB0`) and QUIT (0x1A, its question `D_00387A90`); its accept is
  `func_00161050`. The end banner still fades over the table's first second (the table's own draw
  fades it, `func_00175730(hud, -2)`).

**Variant 1, last rider standing** (`+0x364 = 1`): the HUD lists every rider with its hits taken
(`RIDER`, `HITS`, `OUT` past one); a rider with two hits is out (`func_00156580`: it never
respawns; `func_0012F7D0`, `func_00155E78` skip it); the round ends when fewer than two remain
(`func_001B4718`, then results after 300 ticks), the time of the last one standing goes to the
save (`D_004225A0 +0x12210`, `TIME:` and `BEST TIME:` with one player). Not used online: the
owner's decision is the timed contest.

**Not different from Single Event on the arena:** pickups and weapons (the same arena items,
shared as events online, 144), the computer riders and their attacks, the camera and the arena
itself. `func_00173A18` skips a course state machine in mode 15 (the arena has no course).

## How the round runs online

- **The mode.** An online race on track 23 runs in mode 15 with `+0x364 = 0` when the room's
  server ranks by score (the welcome's `"score"` feature; `NetLobby.server_scores`):
  `port/hle/online.c` writes it after the race setup (`pads_begin_field`), where every other room
  race keeps Single Event (12). With an older server the arena races as before.
- **Points have one owner each.** A hit online is decided on the attacker's PC and applied by the
  victim's (044). A hit by a rider this PC runs on a puppet goes out as an event and the game's
  hit handler does not run here (`hle_puppet_hit`), so before this lane an online attacker got no
  points at all. Now `hle_puppet_hit` calls `hle_online_hit_dealt`, which in the round runs the
  game's own `func_001301E0` on the attacker (with the same first returns as the hit handler, on
  the puppet as this PC shows it). Hits taken are counted by the victim's PC as before. So each
  rider's two numbers are made by one PC: the one that runs it (its player's, or the host's for the
  computer riders and a player who left).
- **Scores travel as events.** EVENT payload v2 type 10 (`port/hle/mosh_round.h`): rider (global
  id), final flag, the five counts (u32 each), hits taken (u16); 26 bytes, sent over the reliable
  ordered channel whenever an owned rider's numbers change, and once more, final, when the owner's
  round is over. Every other PC writes the owner's numbers into its copy of the rider each frame
  (so the end banner and the HUD read the owner's). A final one never changes.
- **The clock.** Each game counts its own two minutes from its own GO (the game's timer). At 0 the
  patched `func_001B4718` calls `hle_online_round_end` instead of ending: the end banner shows, the
  owned riders' scores are frozen and sent final, and the game waits (`TIME UP - WAITING FOR THE
  OTHERS...` after a second) until every rider's final score is here (or 15 s:
  `MOSH_TABLE_WAIT`). A human's final also counts when the room has that player finished with a
  score (a bot's only way).
- **The table** is a rule on the final numbers alone (`mosh_order`): by score, highest first, a
  tie to the lower global id (the room's grid order). Every game holds the same finals, so every
  game draws the same table. The game then sends its finish to the room with its score and its
  place in that table (computer riders included), and runs the game's own end: COMBAT RESULTS,
  patched to list the riders in that order (`port_online_round_rider`) with the players' names
  (`port_online_round_name`; two players may ride the same rider), and in a room one row, BACK
  TO THE ROOM (`port_online_round_rows`, `port_online_round_accept`: as QUIT's Yes), which a
  player who stays on the page takes after 15 s (`MOSH_RESULTS_HOLD`).
- **The room** (server `online/server/src/room.ts`): a finish may carry `score` (0-999999); a race
  whose finishes carry scores ranks its finishers by score (a tie in grid order), DNFs last, and
  each member's race state shows its score. The room page's cards show `2ND 1350 PTS` where a race
  shows the time (`port/hle/online_menu.c` `finish_text`). Ready, the next race, a player who
  leaves becoming a computer rider and the room's results work as for a race.

## Progress

- [x] 2026-11-11: lane started, finding reserved (038afbee).
- [x] 2026-11-11: the mode's rules read from the decompiled source (above).
- [x] 2026-11-11: server: `score` in a finish, scored ranking, the `score` feature, tests
  (389246c3, 096ac2ca).
- [x] 2026-11-11: the score event and the table, with a unit test (faf7e1a8).
- [x] 2026-11-11: the round in the game, the net layer, the browser's `net.js`, the room page,
  nettool `--score` (bc8e0715); Single Event again after the round (the shop, d9ec1419); points
  only for an owned rider (6f3f3d6c).
- [x] 2026-11-11: `port/tools/mosh_round_check.py` (c9d476bc, 6f3f3d6c) and the runs below.

## Decisions

- **The timed variant online.** The owner asked for the two-minute contest; last rider standing
  would need an elimination order agreed across games and is not asked for.
- **Each owner's numbers, no host table.** The host could collect and publish the table (as the
  old EVENT_RESULTS did for races outside rooms), but that needs a host that stays; per-owner
  finals over the reliable channel give every game the same numbers whoever leaves, and the room's
  table comes from each player's own report, the same number.
- **The finish goes to the room when the table stands**, not at the time up: its place is then
  the table's, the same on every game, and the room's places match the screens (the room raises a
  place only past an inconsistent report).
- **Only with a server that lists `score`.** An older server would ignore the score and rank the
  round by its length (about 7200 ticks for everyone): a wrong order. With it absent the arena
  races as before, the fail-safe.
- **Scores capped at six digits** (999999, the HUD's width): it keeps ten members' snapshots in the
  4 KB budget (a scored member shows its score instead of `finishTicks`). A two-minute round
  scores a few thousand (the runs: 50-10800 a rider).
- **No protocol bump.** Rooms take one build only, so an older game never meets a score event or
  a scored snapshot; an older server ignores the field (and the game then does not play the
  round); 0.7.13-0.7.16 games ignore the welcome's feature list.
- **COMBAT RESULTS, then the room.** The round ends as the mode does, on the game's own results
  page, with one row (BACK TO THE ROOM, as QUIT's Yes); a player who stays there goes back by
  itself after 15 s, so the room's Ready and next race go on as after a race. The game's TRY AGAIN
  and SAVE have no meaning in a room.
- **The players' names on COMBAT RESULTS**, not the riders': two players may ride the same rider
  (issue #17), and the room and the names over the riders use the players' names. Ten characters,
  capitals (the column's width; the game's names are capitals).
- **The table waits at most 15 s** for a final score; a rider whose final never came (its game
  stalled, paused, or lost the link) counts with its last score then. A human's final also counts
  from the room (its finish's score: a bot's only way).
- **COMBAT RECORD stays each save's own** (the game raises it to any rider's score, the record of
  the best round seen): offline the same.
- **A test switch, not a feature:** `DHD_NET_NO_AI` also works in a scripted menu room
  (`docs/TESTING.md`).

## Surprises

- **The Mosh Bowl has two variants** (`SETUP +0x364`): the timed contest and last rider standing.
- **It is locked on a new save** (`func_001ED900`: row 3 needs `D_004225A0 +0x1219B`): the first
  offline run's menu script landed on Training. Rooms unlock everything while online.
- **Online attackers scored nothing before.** The attacker's PC skips the whole hit handler for a
  hit on a puppet (the victim's PC applies it), and the victim's PC added the points to its copy of
  the attacker, which nobody else saw. In a Single Event race that is the race score's few points
  per hit (`+0x6610`); this lane leaves races as they are and fixes it for the round only.
- **A remote rider without a snapshot yet is no puppet** (`puppet.c` `puppet()` needs a snapshot):
  the PC runs it with its own AI and sends its hits as events. Nettool bots that echo no computer
  rider (a room full of humans: no computer riders to echo) send no snapshots, so in the six-player
  run the host's game rode the bots' riders, which hit the two games' riders (9 and 11 hits taken).
  The round's numbers stay consistent (the points of a rider are only its owner's:
  `hle_online_hit_dealt` checks ownership, and a bot's final comes from the bot), but the hits
  themselves come from a rider nobody controls. Lane 202 rewrites how remote riders run.
- **The scripted riders land few hits.** Attacks pressed every half second keep the rider from
  pedalling (1 mph, stuck up the bowl's wall); one every 2.5 s lets it ride. Even so the human
  riders of the online runs scored 0 (the offline one scored 200): the computer riders hunt them
  down. The attacker's points across games were shown by the computer riders (below); a human's
  points on a puppet go through the same code (`hle_online_hit_dealt`), not yet seen in a run.

## Runs

All on this PC: a local server per run (`port/tools/local_server.py`), P2P rooms (no Realtime
credentials here), games on hidden desktops at `DHD_SCALE=4` (1280x960), untimed game lock,
`python port/tools/mosh_round_check.py OUT [--bots 4]`. The scripted host starts its first race
before the guest is in (as in `cross_race.py`), so each run's first round is the host's (with bots
when there are), the second has both games.

| Run | Players in the round | Computer riders | Result |
|---|---|---|---|
| run 1 (`--then next`) | host alone | 5 | round ends at race tick 7199, table at once, COMBAT RESULTS, back to the room after 15 s; next race ALPINE in Single Event (guest joined), finished as a race |
| run 2 | host, guest | 4 | the same table on both (`1. Ajax (AI) 3300 (1) ... 5. HOST 0 (14) 6. GUEST 0 (6)`), room results `5. HOST 0 PTS; 6. GUEST 0 PTS` on both; the guest's 6 hits taken = the host's 6 hit events (tool regex failed: fixed) |
| run 4 (`--bots 4`) | host, guest, 4 bots (the limit, 6) | 0 | PASS: tables equal (bots' 400/300/200/100 first), room results of the round equal on both, in the table's order, points and places |
| run 5 | host, guest | 4 | PASS: tables equal (`1. Mai (AI) 5900 (1) ...`); Mai's hits on the guest went over events (3, the guest's 3 hits taken) with Mai's points made on the host |
| offline (`--offline`) | one player, 5 computer riders | 5 | the game's own Mosh Bowl: HUD (time left, PUNCH, the points counter), the end, COMBAT RESULTS with TRY AGAIN / SAVE / QUIT, no `online:` line in the log |

Pictures (not committed; `captures` in the lane's scratch folder named in its brief): the HUD
online and offline, the end banner over the results, COMBAT RESULTS on host and guest (run 2,
run 5, run 4's six players), offline COMBAT RESULTS, the room page after a round (`5TH 0 PTS`).

## Result

An online race on MOSH PIT (ARENA) is now a Mosh Bowl round: the game's own timed mode, two minutes
on each game's own clock, the game's HUD, every hit a crash and worth the game's points, then
COMBAT RESULTS with the same table on every screen and the room's results ranked by the same
scores. With a server that lacks `score` the arena races as before.

What the release must do:

- **Deploy the server** (`online/server`: the `score` feature, `finish.score`, the scored
  ranking) before or with the game release. A game with this lane on an older server races the
  arena as before (no round), so the order does not matter for safety, only for the feature.
- **No `MIN_VERSION` change and no protocol bump**: older games never see a score (rooms take one
  build), and they ignore the welcome's new feature.
- The core changes (patches, online code): the core id changes, as with any core change.

Left for the lead:

- Boot and race state traces against 1578b6c5 (`state_trace_run.py boot` and `race`,
  `race1`-`race5`): they must be equal. Every hook is behind the online switch (`online_mode()`)
  or returns the game's own value offline; the offline Mosh Bowl run above was seen once.
- A real two-machine round in an SFU room (this PC's runs were P2P rooms), ideally with two humans
  who hit each other: a human's points on a puppet (`hle_online_hit_dealt`) were not seen in a
  scripted run.
- A round with a phone (Android: the same core) and one in the browser's P2P room (`net.js`'s
  score and layout were regenerated, `gen_net_layout.py --check`'s text part passes; its clang step
  needs a wasm32 libc on this PC and did not run).
- After lane 202 merges: the one-line hook in `port/hle/puppet.c` `hle_puppet_hit`
  (`hle_online_hit_dealt(other, self, kind)`, in the branch that turns a local rider's hit on a
  puppet into an event) must stay in the new puppet code at the same point: where this PC's rider
  hits a remote rider and the hit is sent to the victim's PC.
- `docs/STATE.md` "Online play" and the STATE-LOG entry at the merge.
