# 222: a sound with no position (the crash on the rider screen after an online race)

Status: current

Branch `fix/sound-emitter-gone` from `main` d9714d0a, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The 0.8.0 gate saw the game die back in the room on the rider screen after an online race
(`race_menu_check.py --scenario end`, about 1 run in 7; also once after LEAVE THE ROOM and once on
a guest after its race): `host64 memory: read of uncommitted guest address 0x000000000` in
`func_00208C60` (the angle to the sound listener) <- `func_001BBF30` <- `func_001BC4F0` (the
update of the four streamed sound slots) <- the front end. Done means: the mechanism named from the
code; whether 0.7.16 and offline play have it; a repeat that crashes before and not after; the cause
fixed in `port/` without making address 0 readable; siblings checked; all ten `race_menu_check.py`
scenarios pass; boot and race state traces equal to the reference build; `check.py port` green on
the last code commit, `check.py docs` on the last.

## Progress

- [x] 2026-11-12: finding reserved (dd49d46e)
- [x] the mechanism, from the code and a logging build (below)
- [x] a deterministic repeat: two test switches (`DHD_TEST_SOUND_EVENT`, `DHD_TEST_SHELL_VOICE`)
- [x] first fix, a forced silence in the slot update (356c5ad7), replaced by the fix at the angle
  (f2eb0c3e) once the game's own rule for a null point was found
- [x] before / after: 30 of 30 boots crash before, 0 of 30 after
- [x] state traces, the ten `race_menu_check.py` scenarios, `check.py port` (Result)
- [ ] merge: Status `current`, STATE-LOG entry (the lead)

## The slots

`D_00398C40 + 0x5350` is the streamed sound manager: the SKAT dialogue queues (989 Studios' speech
system, `SKAT989.cpp` in its asserts) at `+0x4` and four slot entries of 0x84 bytes at `+0x614`.
`func_001BC868` fills an entry:

| field | what |
|---|---|
| x0 | positional: placed by distance and angle every update |
| x4 | centred: no angle |
| x8 | the emitter: a pointer to the position the sound comes from |
| xC, x10 | full volume within xC, silent beyond x10 |
| x14 | priority (`func_001BC6A0` takes an idle slot, or steals a lower one) |
| x18 | updated by its owner, not by `func_001BC4F0` |
| x1C, x20 | stop once started; set on the start's update |
| x24, x28 | fading out over 180 updates, and since when |
| x2C | the last distance, for the Doppler pitch (`func_001DB920`, one screen) |
| x30 | the owner's key (`func_001BC480`), cleared when the slot is idle |
| x34, x5C | level ramps (`func_001C2280`, `func_001C23A0`) |

Each slot's queue (`func_00235590` state) is 1 idle, 2 loading the stream, 3 loaded, 4 playing,
5 stopping (`func_00235878`). A caller queues a SKAT event on its slot (`func_00233BA8`: the event
index, e.g. 26 the rider screen's chatter, with the speaker); while the slot is idle,
`func_002337C0` rolls each queued event against its chance and `func_00233F00` picks a line that
binds (the speaker's groups, lines left); only then does the stream load and play.

`func_001BBF30` places a playing positional slot. In a race (`D_004B25A0.x118` 0) by its distance
to each rider (`func_001DB6B0` -> `func_00208E40`); in the front end (`x118` 2) by its distance
(`func_00208E40`) and its angle (`func_00208C60`) to the listener (`func_0023CF78(0)`). An amount of
0 stops the slot. Every distance is `func_00222330`, which reads a null second point as the origin
(`if (b == 0)`). The angle reads both points with `lqc2` and has no such rule.

## The mechanism

- **Which sound.** The front end's start (`func_001B9E88`, at boot twice and after every race,
  offline too) calls `func_001C4E68`, which asks the rider screen's chatter (event 26, chance 60) for
  the rider screen's place 0 (`func_001C1F50(obj->xA0, 26, obj->x58, ...)`): the rider screen keeps
  15 places at `D_004B25A0 + 0x4F0 + 0x58`, 0x50 apart, and draws places 1 to 14
  (`func_001C5358` -> `func_001C3740`); place 0 has no rider, so its position `x58` is 0 and its
  speaker `x6C` 0 (speaker 13, `func_001BFFF0`'s default). A logging build saw this start, with
  emitter 0x00000000, at every front-end start; no other start of the manager has a null positional
  emitter. The emitter is really 0, not a freed object: the port's game objects never leave guest
  memory, so a stale pointer would not fault.
- **Why only sometimes.** In 27 `end` runs on this PC (about 194 front-end starts) the chatter's roll
  passed every time and no line bound once (`func_00233F00` returned 0), so nothing played. When a
  line binds, the stream loads and plays, and the next front-end update reads the angle to address 0.
  What makes a line bind for speaker 13 (the queue's callback, the group masks, lines left) was not
  pinned down; the gate saw it three times. END THE RACE has nothing to do with it: the crash comes at
  the front-end start that follows any race (the gate also had it after LEAVE THE ROOM and on a guest
  after its race), and the rider screen is simply the first page there.
- **The PS2.** `func_00208C60` read the kernel's words at address 0 as a point and made an angle of
  them; the amount came from `func_00222330(listener, NULL)`, the listener's distance from the origin.
  The front end's listener stands at (4.7, -298.6, -44.7), about 302 units out, beyond the chatter's
  25: the amount was 0 and `func_001BBF30` stopped the line in the same update. The angle was garbage
  and never used; nothing was heard.
- **Older than this cycle.** The start is retail code, and the port faults on any read of guest
  page 0 since the 64-bit host (finding 082), so 0.7.16 has it, offline as well (any return to the
  front end, even the boot's second start). GitHub issue #10 (0.6.0, closed) died the same way
  (`read of uncommitted guest address 0x000000000` right after "the rider screen on carousel place 3",
  before any race); its build's map is not kept, so the stack is not named, but it fits.

## The repeat

`DHD_TEST_SOUND_EVENT=26` makes event 26's roll pass (the roll still happens) and
`DHD_TEST_SHELL_VOICE=1` makes the front end's start ask its chatter for rider 1, whose lines bind.
Then every front-end start plays a positional line with no position:

    python port/tools/state_trace_run.py boot --name repeat --env DHD_TEST_SOUND_EVENT=26 --env DHD_TEST_SHELL_VOICE=1

| build | boots | crashed | |
|---|---|---|---|
| the switches without the fix | 30 | 30 | at field 146, the gate's chain (rip rva 0x2a738, `func_00208C60` <- `func_001BBF30`) |
| the fix (f2eb0c3e) | 30 | 0 | all 600 fields; dhd.log: `sound: a sound's angle with no position: from the origin, as its distance` |

With the fix and a slot-state log (a local build, not committed) the line loads and plays, the game
stops it as out of range in the same update, and slot 0 is idle again (state 4 -> 1) within three
updates of each front-end start; nothing is left playing. The same switches on both games of
`race_menu_check.py --scenario end` on the fix: PASS, no fault on the host (two front-end starts,
one after its race) or the guest (after its race); the dhd.log line is written once per session.

## Siblings

- The other readers of an emitter: `func_00208E40`, `func_001DB6B0` and the Doppler
  `func_001DB920` (also at the start, `func_001BC868`) all go through `func_00222330`: null-safe.
- The other callers of `func_00208C60`: the rider screen's own chatter (`func_001C3550`,
  `func_001C3668`, `func_001C3740`, places 1 to 14, real positions) and the rider's heading in
  `func_001C5040`'s unit; the fix is inside `func_00208C60`, so they get the same rule.
- Non-positional starts with no emitter (`func_001BC968`, `func_001C0F88`, `func_001C1170`,
  `func_001C1300`, `func_001C1420`, `func_001C1548`, `func_001C1650`, `func_001C18A0`,
  `func_001C1C00`, the announcer's lines in a race): no angle, distance only through the Doppler.
- Puppet riders' sounds: their emitters are rider objects in the riders' fixed arrays, which stay in
  guest memory after a race; no pointer of this manager can lead into unmapped memory.

## Decisions

- The fix gives `func_00208C60` the distance's rule (a null point is the origin,
  `port_sound_point` in `port/hle/sound_slots.c`, patched in by `port/patches_sound.py`) rather than
  silencing the slot: the game itself then decides, as on the PS2, and decides to stop it. Address 0
  stays unreadable. Where both points are real nothing changes (the state traces are equal).
- The first fix (356c5ad7) forced the amount to 0 in `func_001BBF30` for an emitter below 0x100000
  and skipped the start's Doppler read; both were dropped: the Doppler read is null-safe, and skipping
  it would have changed x2C against retail whenever a race world is set.
- The test switches stay in the build (`docs/TESTING.md`): they change nothing unless set, and the
  repeat needs them.

## Surprises

- Forcing the chatter's roll is not enough: with speaker 13 no line ever binds, forced or not. Only
  a real speaker (the second switch) makes it play.
- A race voice queued on a slot and still waiting at the race's end was suspected first (the shell
  reuses idle slots); the logs never showed one firing after a race's end.
- One of the 27 natural runs ended in the renderer's D3D12 pipeline failure (`Shader translation
  error ... Unspecified error (0x80004005)`, then the game exited), the gate's second crash; not this
  lane's.

## Result

On the branch (f2eb0c3e, the last code commit):

- the repeat: 30 of 30 boots crash without the fix, 0 of 30 with it (above);
- state traces against the format-2 references of 524948ba (`main-524948ba-ref-v3`): boot 600 of
  600 rows and race 5000 of 5000 equal (`state_diff.py` PASS);
- `race_menu_check.py --link delay=80`, all ten scenarios (resume, room, end, leave, closed, alone,
  alone-guest, timeup, timeup-host, finished): PASS, no fault in any game;
- `python tools/check.py port`: OK.

Not checked: a natural crash on this PC (none in 27 runs before the fix, so the natural rate is not
compared before and after; the repeat is the proof); the other recipes (race2 to race6, split,
memcard); PC <-> Mac, Android, the browser (the fix is shared C, one code path).
