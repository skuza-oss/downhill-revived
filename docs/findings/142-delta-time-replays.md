# 142: race replays, the attract demo and the ghost on the delta-time clock

Status: current
Checked: 2026-11-04

Branch `feat/delta-replays` (from main 01f8d2b, finding 139's delta-time clock), 2026-11-02.
(Number 142: 140 is widescreen everywhere, 141 the touch controls.)

## The problem

Since finding 067 the race recorder (`func_001A8DE8`, called by the race update `0011A100`) and
the time trial's ghost sampler (`00138E08`) were switched off on the delta-time clock
(`if (port_delta_clock()) return 1;` / `return;` in `port/patches_fps.py`). That was only above
60 until finding 139 put paced play at every rate, 60 included, on that clock: every player lost
them.

## What players reach

Traced from the callers and the menu item ids (`D_0026F540`, the in-race menus' label table:
item id = index).

| Feature | How a player reaches it | Code | On main (delta-time clock) |
|---|---|---|---|
| **REPLAY** after a race | race results menu, item 0x1D "REPLAY" (`0014EFA0`, also `0015CF48` with TRY AGAIN) | `0015C920`: `x35C = 1`, recorder `x44 = 1`, `001B8B10` resets the race, `001A8CE8` starts the playback | the race restarts with nobody replaying: the live pad drives the rider and "Replay" shows |
| **Replay menu** | shown when a replay ends or is stopped ("End of Recorded Replay", "Controller Unplugged, Replay Stopped.": `0015F730`) | items RE-START 0x22 (`0015F838`, `0015FAF0`: plays it again), SAVE 0x23 (memory card), QUIT, CONTINUE | as above |
| **Save / Load Replay** | SAVE in the replay menu (`00188DD0`: header block 0x7D0 + run-length streams of buttons, sticks and the step byte); "Load Replay" 0x1E, then mode 19 (`001EB1D8` sets track, rider, bike from the header) | `001A9790` / `001A9B70` | a loaded replay plays nothing |
| **Attract demo** (mode 10) | 60 s (3600 fields) idle on the title (`001EB1D8.cpp:636`) | `001A8DE8`'s mode-10 branch, `001A9EA8`: camera cuts between riders, two minutes (0x1C20 fields), any button ends it | the branch never ran: no camera cuts, no time limit, only START (`001B4900`) ended the demo |
| **Time trial ghost** (mode 18) | Single Event, row 3 "TIME TRIAL" (`001EB1D8` c8: RACE / RACE (no AI) / FREERIDE / TIME TRIAL), "RACE vs. BEST TIME", "SAVE TIME TRIAL" | `00138E08` every rider update, `00138B40`, `00139660`, `00138F70` (sample), `00139308` (ghost advance), `00138868` (draw) | no ghost samples, no ghost |

What the recorder is (from `001A8DE8`, `001A8EF0`, `001A9058`, `001A9640`, `001A8FE0`):

- An **input recording**: from GO, pad 0's buttons and four stick bytes, one 8-byte sample a
  frame (`short buttons; u8 stick[4]; u8 step; u8 unused`), at most 0x626F samples (seven minutes
  at 60). A replay restarts the race with the same seed (`0016B8C8` keeps it when `x44`) and runs
  the whole simulation again with those inputs.
- The **step** byte: `001A8EF0` writes 1, and on a two-field frame writes the 2 into the
  *previous* sample. `001A9640`, between the clock's wait and its step in the race loop
  (`001B40A0`), sets the field counter from the last sample's step byte during a replay, so each
  replayed frame has the recorded step.
- A **check**: every 60 samples the recording keeps the player's z (`header + 0x140`, `[count/60]`),
  and the replay stops at the first one that differs (exact float compare). A replay must be the
  same simulation, step for step and input for input.

## The design

The replay has to repeat the race exactly, so it needs every update's step and input. On the
delta-time clock the steps are wall time (60/N to 2 fields), and a buffer of one sample per update
would hold only 175 s at 144 and 105 s at 240 (races run 5-7 minutes). So an offline race runs on
the **field grid** (`port/hle/fps.c`, `port/hle/replay.c`):

- **One sample per field of race clock.** An update that ends w whole fields (0, 1 or 2) records
  w samples, so 60 samples are a race second at every rate, the buffer keeps its seven minutes,
  and a replay plays one sample per field. The sample format is unchanged.
- **The step is chosen when a field ends** and kept for the updates until the next field ends
  (`GRID_RECORD`): the clock's smoothed wall step (finding 139), rounded to a code in 1/120 field
  (30-240; 1/7200 s), with the rounding left owed so time is conserved. The code goes into the
  last sample's step byte (the byte retail uses for the step; retail's 1 and 2 are whole fields,
  codes are 30 and up). A replay (`GRID_PLAY`) forces the same codes from the samples in
  `001A9640`'s place. The part of a field run and the animation subticks are counted in integer
  1/120 fields on the grid, so the same codes give the same whole fields and subticks at any
  selected rate.
- **The pad is read when a field ends**: updates in between get the last sample's pad (the
  replay's own `002202B8`/`002202D0` override), so a field's sample holds every update's input.
  The game reads its controls 60 times a second in a race, as the original does; pictures and
  physics still run at the selected rate. At 60 nothing changes (about one update per field); at
  144 an input waits up to one field (16.7 ms, 5 ms on average) more for the physics than before.
- **From the race load to GO** (race phases 5 and 1, reported by `0019EE98` every frame) every
  step is exactly one field (`GRID_WHOLE`), pictures every 1/60 s: the pre-race wait and the
  countdown run as the original's at 60, the race clock starts on a field, and GO's update has
  the same step (1 field) in the race and its replay. Entering the grid resets the part of a
  field, the subticks and the per-body clocks (airborne ages, finding 139), the same in both.
- **A replay's pictures are held for the recorded steps** (`port_frame_period`): it plays at the
  speed it was ridden, whatever rate is selected now; a PC slower than the recording plays it in
  slow motion (as the PS2 did).
- **Online** races record nothing (state sync, finding 044): the grid is off there and the online
  code is unchanged. The attract demo is not recorded either.
- **A deterministic run at 60** (`DHD_FRAME_CLOCK=1`, no `DHD_STEP_CLOCK`) keeps the original
  functions: every change is behind `port_delta_clock()`.

Other conversions:

- **Attract demo**: `001A9EA8` (camera cuts, the two minutes, any button ends it) runs once per
  whole field of the update (0-2 times), as once a frame at 60.
- **Replay camera director** (`001A9230`, while recording when `x48`): once per field.
- **Time trial ghost** (`00138E08`): it already counted whole fields (`x316EC += step`, class A in
  finding 049). On the delta-time clock a two-field step past the mark keeps its extra field
  (`-= 8` instead of `= 0`), so a race second holds 7.5 samples at every rate (retail dropped a
  field whenever a two-field frame overshot), and the ghost is drawn at
  `(fields since its sample + the clock's part of a field) / 8` (`port_counter_fields`): it moves
  smoothly between its samples above 60. The ghost is a position recording, so it needs no exact
  steps.

Rejected: recording on the field grid while the race steps freely (a replay re-simulated on other
steps leaves the race within seconds: finding 139's drift table, positions hundreds of units
apart after 10 s; the game's own check would stop it); one sample per update (the buffer); host
side step logs (memory-card replays would lose them); interpolated replays of positions (the
replay is the game's own re-simulation, finding 049 rules out synthesized pictures).

## Checks

**Unit tests**, `port/tools/test_replay.py` (new; `port/hle/test_replay.c` runs `fps.c` and
`replay.c` in the core's wasm32 with a stand-in race whose "physics" moves z by each update's
step times the pad it sees, and the retail `001A8EF0` as a stub): at 30, 45, 60 (delta paths),
90, 144, 165 and 240 steps a second, each on the fixed clock and paced with frames of 1-1.6
periods:

- 60 s of race clock records 3600 samples (3601 when the last update ends two fields): 60 a
  race second at every rate; paced, the race clock keeps wall time (60.00 s +- 0.02 s; the
  deterministic 165 runs 0.8% fast, below). The replay camera director runs once a field.
- The replay at another selected rate (144, or 45 for 144) with frames of other lengths gives
  every update the recorded step, input and whole fields, plays one sample per field, passes
  every z check, stops once at the end of the recording; its pictures are held for the
  recorded steps.
- Ghost: 600 fields give 75 samples at every rate; paced with uneven frames retail's count from
  0 loses fields (45: 90 samples for 763 fields where 95 are due).
- `test_fps.py`, `test_fps_envelopes.py`, `test_input_edges.py`, `test_defined_producers.py`,
  `test_proximity_float.py`: pass (the clock's free path is unchanged; `clock_spend` is a split
  of the same arithmetic).

**In the game, headless**: `port/tools/replay_check.py` (new) rides the state trace's race route
(first track, seed 81257, the race-clock pad), starts the replay at race second S exactly as
the results menu's REPLAY does (`DHD_REPLAY_TEST`: `func_0015C920`, then the race loop ends with
a reason `001BA7D0` opens no menu for, and the race screen starts the race again as its replay),
and logs every rider's position at each field of race clock (`DHD_REPLAY_TRACE`).

| Run | Race | Replay | Riders compared | Largest difference |
|---|---|---|---|---|
| frame clock, 45 steps a second (1.33 fields) | 30 s, 1801 samples | to the end of the recording, no check stopped it | 10 riders x 1351 fields | 0 (every coordinate equal) |
| frame clock, 45, branch head | 60 s, 3601 samples | to the end | 10 x 2701 fields | 0 |
| frame clock, 144 (0.42 fields), branch head | 60 s, 3600 samples | to the end | 10 x 3599 fields | 0 |
| paced (wall clock) at 144, frames 9 +- 4 ms (`DHD_SLOW_FRAME=9,4`) | 60 s, 3600 samples; race clock x0.999, steps 0.41-0.83 fields | to the end | 10 x 3599 fields | 0 |

The paced replay ran at x0.954-0.960 of real time: each replayed frame lasts the longer of its
recorded step and its own (jittered) frame, with no catching up (open).

**Attract demo**, idle on the title (frame clock at 45, an empty memory card, no pad), 420 s:
main loaded the demo once and it never ended; this branch cycled title -> demo -> title 11 times,
the demo's 0x1C20-field countdown going 7200 -> 0 every 600 fields as at 60.

**State trace** (`state_trace_run.py`, deterministic 60, the original clock): main 01f8d2b built
here and traced as the reference (not `next077-ref`); this branch's race 5000 fields and boot 600
fields: **all rows match**, on the build before the last code commit and again on the branch
head (`0efac73`'s code).

## The pacing grid when the period changes (review, 2026-11-03)

`port_frame_period()` is no longer a constant of the session: 1/60 from the load to GO, each
recorded step in a replay, 1/N otherwise. The two pacing grids (`kernel.c` `pace()`, used online
and with `DHD_LATENCY=0` or `DHD_INTERP`, and `host_pacing_wait`'s slow stretch, a PC or phone
missing the rate on more than one frame in ten) set each frame's due time to
`base + frames x period` with the current period, so a longer period moved the due time by every
frame since the grid's start: 1000 frames at 144 then the load's 1/60 waited 9.7 s, 200 replay
frames at code 120 then 150 waited 0.83 s. Both now use `port/hle/pace_grid.h`, which re-anchors
the grid at the last frame's place when the period changes (the frames before keep the period
they had). `test_fps.py` checks it on a fake clock: no wait longer than one new period at either
change, a shorter period keeps the grid, quick frames still repay a slow one, and more than two
periods behind still starts again.

## Limits and open

- **Replays recorded on the original clock** (by the PS2, or by the port up to 0.7.7, whose
  paced 60 was the original arithmetic) carry step bytes 1 and 2. Played on the delta-time clock
  at those whole-field steps they leave the race, because its arithmetic is not the original's
  (finding 139: "60" and "60d" differ), and the game's z check stopped them about a second in
  with only "End of Recorded Replay" (review, 2026-11-03). Now `replay.c` recognises such a
  recording when its replay starts (any step byte of 1 or 2: this clock writes codes of 30 and
  up), writes it to dhd.log, shows "REPLAY FROM AN OLDER VERSION / IT CANNOT PLAY IN THIS ONE"
  over the countdown, and ends the replay at GO with the game's replay menu, without playing
  inputs that can only diverge. `test_replay.py` checks it at every rate on both clocks: the line
  shows through the countdown, the replay stops once, at GO, with no update played.
  Playing them on the original arithmetic instead (the deterministic run at 60 still does,
  `DHD_FRAME_CLOCK=1`) was rejected for now: it means switching the whole clock mode at the
  replay's race reset and back after it, and the delta-time clock's state does not convert both
  ways: animation timestamps are 1/1024 field on it and 1 field on the original (stored in
  objects outside the race too), the picture pacing and the VBLANK clock follow the selected
  rate, and the original clock exists only at 60. A wrong conversion breaks the game well beyond
  old replays, so it waits for a design of its own.
  Replays saved by this build play on any later build with the same clock arithmetic: a change
  to the delta-time physics breaks saved replays, as any change to the simulation would.
- **Input at 60 a second in races**: the price of exact replays at high rates (above). Not felt
  in a test here (no computer use); worth a player's word at 144+.
- **Countdown at 60 pictures a second**: from the load to GO the pictures follow the one-field
  steps; a PC below 60 fps runs the countdown slower than real time (the race itself is not
  affected).
- **Deterministic runs at rates that do not divide 7200** (e.g. `DHD_FPS=165` with
  `DHD_FRAME_CLOCK=1`): the grid's code is the nearest 1/120 field (44 = 0.3667 for 0.3636), so
  such a run's race clock runs 0.8% fast. Paced play is not affected (its steps are wall time).
- A replay's z check is the game's; the replay check here also compares every rider's position
  each field.
- Two- to four-player split screen: the recorder keeps pad 0 only (retail). Untested.
- Not built or run: macOS, Linux, Android, the browser (shared core code).
