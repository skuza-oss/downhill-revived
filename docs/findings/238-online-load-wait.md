# 238: the online race load: a live load screen while a game waits, and where a phone's seconds go

Status: current
Checked: 2026-11-14

Branch `fix/online-load-wait` from `main` b1d5e515 (0.8.3), started 2026-11-14. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

The 0.8.3 production check (an 8-member private SFU room on the official server: a hidden PC host
game, 6 bots, the Mi 11i on the published APK) showed the phone's rider "loaded" 29 s after the
others, and one `freeze: no field for 5 s ... menu 9 phase 0` line on the phone (about 21 s) and
one on the PC host (about 29 s). Two questions:

- A. Where do the phone's extra seconds go? Measured with timing lines in dhd.log, offline (SINGLE
  EVENT, the same track) and online (2, 4, 8 members).
- B. Why does a game produce no field while it waits for a slower member? A waiting game must go on
  drawing (no `freeze:` line), with the freeze watch still meaningful.

Done: the 8-member SFU room with the phone again with zero `freeze:` lines on the phone and the PC
host, the race run and results on both; a 2-member room and an offline race on the phone as
regressions; `check.py quick`, `port`, `docs`; boot and race state traces equal to the base build.

## What the 0.8.3 logs show (before any change)

The PC host's log (its own clock): released about 102.6 s; the scripted host (DHD_NET_CREATE, a
test harness game) then walks its menus from the title to the load screen, about 20 s (nav steps
1-7), and reaches the load screen at 123.0 s. There `exchange_rides` (port/hle/online.c) sent its
ride once and looped until every player's ride came, at most 30 s, inside one field. The phone's
ride never came (it was sent once, about 20 s earlier, before the host read its frames), so the
host waited the whole 30 s (`stall: field 1228 came 30005 ms after the last`) and gave the phone's
rider the default bike (`slot 1 (player 1) rides Cosmo ... (default)`).

The phone (a room player, `from_menu`): released, on the load screen 2.8 s later (the barrier's own
"starting in" wait, under the freeze watch's 5 s), then in `exchange_rides` 21.6 s for the PC
host's ride, which came when the host reached its load screen. Then the race's loop about 2.7 s
later, and the flight over the track (race phase 5) played to its end, about 25 s (fields 9188 to
10676 at 60 fps): a room player's pad drives the flight after the rides (`pads_read_now`), and
nobody tapped the phone; the scripted host skips it with its script's START. The phone's LOADED
(sent at the countdown) reached the host at 152.1 s: the "29 s after the others" is measured from
the bots, which say LOADED at once, and is the unskipped flight, not loading work.

So: B is the blocking loop (finding 175 already named the fix: a hold in the game's load step); the
host's 30 s is a ride sent once and lost; the phone's 21 s wait is the known harness effect of a
scripted game walking its menus (findings 180, the `DHD_NET_JOIN` note); A is the flight.

## Progress

- [x] 2026-11-14: the logs read (above).
- [x] the hold and the per-field exchange, the ride said again, load timing lines (eb1a1972)
- [x] phone measurements offline and online (2, 4, 8 members) on a release-signed PGO build of
  this branch (core aed3e9a7c1df695d)
- [x] PC check: two hidden games on a local server, one slow
- [x] proof: the 8-member room with the phone; a 2-member room; an offline race

## Decisions

- The rides are exchanged one step a field (`exchange_step`) while the load screen holds its count
  (`hle_online_load_hold`, a patch of `func_001F43C0`): the screen is the game's own load card,
  drawn every field, so the freeze watch has nothing to report and stays meaningful elsewhere.
- The ride is said every half second while waiting, and again with LOADED at the start, as LOADED
  already was (finding 187's reason): a message sent once is lost when it comes early.

- The load timing lines (`load: the race's loop ... after the load screen came`, `load: the
  countdown ... after the race's loop`) are logged offline and online: they only read state.

## Surprises

- The phone loads a track as fast as it does offline: about 4 s from the load screen to the race's
  loop in every room size. The "29 s late" of 0.8.3 was never loading work.
- The flight over the track is about 25 s and belongs to the player: a room player who does not
  tap SKIP watches all of it, and the others wait at their countdown meanwhile (drawing, since
  this lane). Whether online should skip it for everyone is a feel decision, not taken here.
- In 0.8.3 the host's 30 s freeze also cost the phone's rider its bike: the ride, sent once, was
  lost, so the host raced a default rider for the phone. Saying it every half second fixes both.

## Result

Mi 11i, release-signed PGO APK of this branch; private SFU rooms on the official server with a
hidden PC host game of this branch and bots; Mt. Midoule FR in every run.

| Run | Rides exchanged (phone) | Load screen to the race's loop (phone) | Flight (phone) | PC host | `freeze:` |
|---|---|---|---|---|---|
| Offline SINGLE EVENT | - | 3.9 s, 3.8 s | 25.5 s (unskipped); 7.7 s (SKIP) | - | 0 |
| 2 members | 25.6 s, drawn | 3.8 s | 25.0 s | rides at once; 26.5 s at its countdown, drawn | 0 / 0 |
| 4 members (2 bots) | 26.7 s, drawn | 4.0 s | 25.0 s | 26.6 s at its countdown, drawn | 0 / 0 |
| 8 members (6 bots) | 27.9 s, drawn | 3.8 s | 25.0 s | rides at once; 26.4 s at its countdown, drawn | 0 / 0 |

The phone's rides wait is the scripted host walking its menus (the harness effect above); a room
player who is host would be on its load screen with the others. In the 8-member room the PC host
got the phone's own ride ("the player's choice"), where 0.8.3 gave it the default. Every room ran
its race and showed the results on the phone and the PC; the offline race ran to its finish and
results at 60 fps.

PC check (two hidden games of this branch, a local server, a P2P room; game B scripted, so about
20 s slower): game A held its load screen, drawn, 26.6 s until B's ride came; 0 `freeze:` lines in
both logs; both finished (0:20.03 each) with results.

Before (0.8.3, the production check): the phone frozen 21.6 s on its load screen, the PC host
frozen 30 s (`stall: field 1228 came 30005 ms`), the phone's rider on the default bike.

Open: the barrier's own wait before the load screen still shows one still picture of about 3 s
(`stall: ... (menu 9 phase 3)`, under the freeze watch's 5 s); the flight is not skipped
automatically online.

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- On this PC, two hidden games: a P2P room on a local server (direct link, `1. Host 1:10.03  2. Guest 1:12.03`
  on both) and a private SFU room on the official server (SFU via UDP on both, equal results); PC <-> Mac (the
  Mac's own build): a private SFU room on the official server and a P2P room, equal results; zero `refused`,
  `freeze:` and `crash:` lines in every log.
- Mi 11i, release-signed PGO APK: a private SFU room with a hidden PC joiner raced to results, zero `freeze:`
  lines. A full room of 8 was not run again.
