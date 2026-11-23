# 280: input latency (issue #123): where a press waits, and what of it is ours

Status: in progress

Branch `perf/input-latency` from `main` d170cb59, started 2026-11-23. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

A player on Windows (issue #123): "at 60 fps the game runs so smooth, but there is a noticeable
input lag in the controls"; the owner feels it too with a pad. Map the path from a button to the
picture as stages, measure each in fields and milliseconds, remove the delay that is the port's,
and say what is the original game's own. Default PC settings first (60 fps, V-Sync on, native
renderer, a pad through SDL), then 120 fps, then V-Sync off.

Done when: a before/after stage table from `DHD_INPUT_LATENCY=1` on the same hidden-desktop run,
`check.py port` and `check.py docs` pass, and the boot and race state traces equal the base
build's. No gameplay or feel change, no change to the online rules.

## Progress

- [x] 2026-11-23: the path mapped (below); `DHD_INPUT_LATENCY=1`, one dhd.log line per sampled
  press of player 1 with the time in each stage (`port/hle/host.c`), 8211a83c, bd630fe9, 3ab24574,
  1add312f
- [x] 2026-11-23: the cause of most of the delay and its fix, 3c43e100 (Result)
- [x] 2026-11-23: before/after on the hidden desktop at 60 V-Sync on, 120 V-Sync on, 60 V-Sync off
- [x] 2026-11-23: the game's own delay, from state traces (Result)
- [x] 2026-11-23: boot and race state traces equal the base build's; checks (Checks)
- [ ] a player with a pad on a real display (unproven, below)

## The path from a button to the picture

Offline, default PC settings (60 fps, V-Sync on, native renderer, threaded graphics). Each stage
with what can hold a press in it:

1. **The host's pad read.** SDL's own thread (`port/host/sdl_host.c`) waits for events
   (`SDL_WaitEventTimeout(4)`; a window message wakes it at once, SDL polls joysticks) and hands
   each on (`dispatch`): keys to `keyboard_key`, a pad's state to `gamepad_raw_take`. The game
   thread takes the newest state at its own read (`host_pad_device`). Nothing queues here.
2. **The game's pad read.** Once a field, at the emulated VBLANK (`kernel.c vsync()`): `pad_vsync`,
   and with just-in-time frames (finding 052) `pad_refresh` again after the wait, just before the
   EE runs the frame. A press waits here for the next read: up to one field, half on average.
   This is already the latest read possible before the frame that uses it.
3. **The picture handed on** at the next VBLANK (`host_service_gs_vsync`): the first picture the
   game could draw from this read ("+1 to its picture" in the line; every sample was +1).
4. **The game's own frame**: the game draws what a press did in the picture after that one
   (Result). Not in the line.
5. **The graphics queues**: the EE's graphics queue (at most 2 fields in flight), the GS worker,
   the native render thread (1 picture pending), then `native_present`: acquire, draw, then
   `pacing_present` holds the picture to its due (the just-in-time due, and at least a period
   after the last picture).
6. **The present** (`dhdr_surface_present`): V-Sync on is Mailbox where offered (Windows DX12 and
   Vulkan), else Fifo (Mac, most Android); V-Sync off is Immediate. Frame latency 2 with V-Sync,
   1 with Immediate (`frame_latency`, `DHD_FRAME_LATENCY`). DX12 waits on the swapchain's
   waitable object in acquire. The present call itself takes 0.2 ms.
7. **Composition**: on Windows DWM shows the picture at the display's next refresh (the line
   prints when that is, from `DwmGetCompositionTimingInfo`), unless the window gets independent
   flip, which no log can tell.

`DHD_INPUT_LATENCY=1` (off by default; `port/hle/host.c`) writes one line per sampled press of
player 1 (at most one per 0.3 s, 400 a session):

    input latency: field F (riding|menus, menu M; +N to its picture): host to the game's read A ms |
    read to the picture handed on B | to the presenter C | held to its due D | present E |
    read to presented T ms | then the display's next refresh in R ms (DWM, H Hz)

"host" is when SDL's thread handed the press on; "the game's read" is the field's last read
(after the just-in-time wait). With it off the cost is one cached flag test per pad read and per
present.

## Decisions

- **The fix is in the pacing's work samples, not in its rules.** The just-in-time rule (052), its
  margin and the slow stretch for phones (a09e2829) stay as they are; only time a picture spent
  waiting behind pictures the presenter was holding no longer counts as work. A phone that is
  really slow holds no picture, so its samples are what they were.
- **Online pacing is left alone.** Online races take `pace()` before the VBLANK (no just-in-time
  frames, finding 139), so a press there waits about a period more from the read to the picture.
  Changing that is an online pacing change; the rules for it stand (follow-ups).
- **60 fps keeps its own 59.94 grid.** Locking it to the display's refresh, as above 60 (the
  display-following pacing), would line the present up with DWM's refresh but changes the game's
  rate on a 60 Hz display; not taken here (follow-ups).
- **The diagnostic stamps a press when SDL's thread hands it on**, not with the event's time
  stamp: on Windows SDL stamps a key with `GetMessageTime`, which moves in 15.6 ms ticks, and the
  first probe read 0 to 15 ms too much there, in a pattern that drifted from press to press.

## Surprises

- **A queue in front of the presenter that never drained.** At a start the loads put more than
  one frame in ten over a field, so the pacing took the slow stretch (a09e2829, made for phones
  that cannot hold 60). There the EE no longer follows the presenter's due: it makes a picture a
  field, and the presenter shows one a field (at least a period after the last). Both run at
  exactly the period, so the 3 to 5 pictures that queued during the loads stayed queued. Each
  work sample (read to the picture's arrival at the presenter) then read 67 ms, so more than one
  in ten stayed over a field and the slow stretch never ended. The pacing CSV showed due == began,
  ready - began 67 ms, presented - due 83 ms, with each thread's own work about 2 ms. On this PC
  every session at 60 fps, V-Sync on, ran 4 to 5 fields behind the pad from the first load on.
  Possible since a09e2829 (0.7.7); whether a PC falls into it depends on how slow its loads are
  against the 1-in-10 rule. 120 fps and V-Sync off at 60 formed the same queue here (about 2 to 3
  pictures).
- **Presenter captures make their own queue.** `DHD_CAPTURE_PRESENTED` stalls the presenter for
  each picture it saves; runs with it showed queues the game without it doesn't have. The
  measurements below ran without captures.

## Result

### Before and after, 60 fps V-Sync on (the default)

Hidden desktop, keyboard keys posted to the window (`DHD_TEST_INPUT`), native renderer, scale 2,
DX12 Mailbox, the PC's display at 143.86 Hz, no captures. Medians of 29 menu presses and 49 race
presses per run (the first two and any over 150 ms, a load, left out). One field = 16.7 ms.
Before = this branch with 3c43e100 reverted, the same diagnostic.

| Stage (menus / race) | before ms | fields | after ms | fields |
|---|---|---|---|---|
| host to the game's read | 1.1 / 4.7 (p90 15) | 0.1-0.3 | 8.7 / 7.5 (p90 13) | 0.5 |
| read to the picture handed on (the EE's frame) | 0.8 / 3.6 | 0.1-0.2 | 1.0 / 3.4 | 0.1-0.2 |
| to the presenter (queues, GS, render) | **66.0 / 61.3** | **4** | 4.3 / 1.7 | 0.1-0.3 |
| held to its due | 15.9 / 15.3 | 1 | 2.8 / 3.7 | 0.2 |
| present call | 0.2 / 0.2 | 0 | 0.2 / 0.2 | 0 |
| **read to presented** | **82.9 / 80.3** | **5** | **8.3 / 9.1** | **0.5** |
| then DWM's next refresh | 3.6 / 2.7 | | 4.4 / 2.2 | |
| host to presented | 84.2 / 84.9 | | 17.1 / 16.9 | |
| the game's own frame (state traces) | +16.7 | 1 | +16.7 | 1 |

So from a press to the game's reaction on the display, on this PC: about 104 ms before, about
37 ms after (host to presented, the game's own field, DWM's refresh).

The pacing's own summary line agrees: "pad to present mean" 63 to 82 ms before, 9 to 13 ms
after, over the whole session.

### 120 fps V-Sync on and 60 fps V-Sync off

| Setting | read to presented, before (menus / race) | after | host to presented, before | after |
|---|---|---|---|---|
| 120 fps, V-Sync on (Mailbox) | 36.4 / 37.7 ms | 8.0 / 7.6 ms | 40.0 / 41.6 ms | 11.9 / 11.4 ms |
| 60 fps, V-Sync off (Immediate) | 33.7 / 36.4 ms | 8.3 / 8.3 ms | 40.5 / 38.1 ms | 15.1 / 17.8 ms |

At 120 the game's own frame is one 120 fps field (8.3 ms). With V-Sync off at 60 the pacing still
holds to 60, so the queue formed there too (2 fields).

### The game's own delay

Deterministic headless runs (the state trace recipe: `DHD_FRAME_CLOCK=1`), the same route with and
without one press, compared field by field:
- **Menus** (the main menu, Down against L3, which does nothing there): read at VBLANK 304; EE RAM
  differs from field 304 (the pad buffer), the sound (the menu's move sound) from field 305, the
  GS stream from field **306**.
- **Race** (`DHD_RACE_PAD` "2:20:cross" against the same plus a 0.3 s Left at 6 s): read at
  VBLANK 3574; the EE's registers and VU0 (the physics) differ from field 3575, the GS stream
  from field **3576**.

A press read at VBLANK N changes the game's state in the frame right after it, but the game draws
that change in the frame after that: its picture is handed on at VBLANK N+2, one field after the
first picture the port could show (the +1 of the line). That field is the original game's own
(its frame structure: it draws from the state it settled the frame before); the port adds nothing
there and it is not ours to patch.

### What changed

- `port/hle/host.c` (3c43e100): `pacing_present`'s work samples leave out the presenter's holds
  between the picture's handoff (`handed_note` at the VBLANK, `handed_at`) and its arrival
  (`hold_note`, `held_since`). Saves 4 fields (about 66 ms) at 60 V-Sync on, about 28 ms at 120,
  about 25 ms at 60 V-Sync off, whenever a session had fallen into the slow stretch with a queue
  (on this PC: every session).
- `port/hle/host.c`, `port/hle/hle.h`, `port/host/sdl_host.c`, `port/gs/*`, the tests' stubs
  (8211a83c, bd630fe9, 3ab24574, 1add312f): the `DHD_INPUT_LATENCY=1` diagnostic, off by default.

Per platform (the pacing is `port/hle/host.c`, shared):
- **Windows**: as measured.
- **Mac and Linux**: the same pacing code, Fifo where Mailbox isn't offered; a session that fell
  into the slow stretch with a queue leaves it the same way. Not measured there.
- **Android and iOS**: unchanged for a phone that is really slow (no presenter holds, so the
  samples are what they were; it keeps the slow stretch). A phone that holds 60 but fell into the
  slow stretch at a load leaves it, and from then on paces as a phone that never entered it does
  (finding 052's rule): the same frames and presents, the EE waiting before its read instead of
  pictures waiting before the present, so no added work. Not measured on a phone.
- **Browser**: unaffected (its pacing is the page's own; `pacing_present` is a stub there).
- **Online races**: unaffected (no just-in-time frames online).

### What a player can set today (0.8.11, before this fix)

1. **120 fps (or the monitor's rate) with V-Sync on** where the display is 120 Hz or more: each
   stage in fields halves, and the queue, if it forms, is half as long (about 40 ms instead of
   85 from the read to the present).
2. **V-Sync off** (Graphics): Immediate presents, frame latency 1; the queue at 60 was 2 fields
   instead of 5.
3. **Borderless or fullscreen, not a window**, so the game can get independent flip and skip
   DWM's composition (borderless is the default).

### Unproven

- A real pad: the runs post keyboard messages to a hidden window. A pad goes through SDL's
  joystick polling (about every millisecond with a pad attached) and `gamepad_raw_take`; its
  read is the same once-per-field read, but how it feels can only be judged by a person with a
  pad, before and after.
- A real display: the hidden desktop composes through DWM at 143.86 Hz; independent flip, a
  60 Hz display, VRR and the display's own processing are not in these numbers.
- The exact host-side wait: "host to the game's read" is bounded by a field (p90 13-15 ms at 60);
  its median depends on where presses fall against the reads.
- Mac, Linux, phones: no runs there in this lane.

### Follow-ups

- Online: the pre-VBLANK pacing order costs about a period from the read to the picture; making
  online races just-in-time is an online pacing change for its own lane.
- 60 fps on a display above 60 Hz: present on the display's grid (as above 60) to take out up
  to a refresh of DWM wait.
- The work margin (the worst of the last 300 samples, plus 1.5 ms) holds a picture about 3 ms to
  its due; a percentile instead of the worst would shorten that at some risk of a late picture.

## Checks

- Boot and race state traces (`state_trace_run.py`, `state_diff.py`) of 1add312f against the base
  build d170cb59: boot 600 of 600 rows equal, race 5000 of 5000 rows equal, PASS (3ab24574 too).
- `DHD_BUILD_SLOTS=4 python tools/check.py port -j 6` on 1add312f: OK. Its first run, on
  3ab24574, failed `test_runtime`: the gamepad test links `sdl_host.c` without `host.c` and
  had no `input_latency_seen`; fixed in 1add312f.
- `python tools/check.py docs`: OK.
