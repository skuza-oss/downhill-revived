# 064: Player settings across restarts

Status: current
Checked: 2026-11-04

Round 49, 2026-10-18. Implementation and validation complete. Evidence stays
locally under `build/port/r49settings/`; no game images/data belong in this commit.

## Reproduction and cause

Extracted the actual `DownhillRevived-0.2.3-win.zip` into a new folder. Launched with every
inherited `DHD_*` variable removed. The disc chooser wrote the first `dhd.ini`; there was no
memory-card save. Used Windows SendInput against the game window (keyboard converted to pad
input by the release), reading RAM only to confirm selected values.

Original preferences start with speech/effects/music raw values 921/963/512,
four control schemes 0, four vibration flags 0, difficulty 1, stereo flag 0.
Changed all four schemes to 1, difficulty to 2 (Hardcore), stereo flag to 1,
and speech/effects/music to 856/888/440. Accepted each page with Cross.
After EXIT GAME and restarting, all these original preferences returned to
their defaults. No career file was created by changing options.

PC SETTINGS already saves each change to `dhd.ini`. Tested changes: resolution
2x -> 1x, display Borderless -> Window, window size 896x672 -> 1280x960,
Picture Wide -> 16:9, scaling Smooth -> Sharp, widescreen on -> off,
skip movies off -> on, host vibration on -> off, textures on -> off,
font Sharp -> Original. Switching widescreen off intentionally also saves Picture
as 4:3; this is the page's documented coupling, rather than a lost Picture value.
Back cancels edits on PC SETTINGS; Cross accepts them. The original AUDIO/OTHER
and CONTROLS pages likewise have accept/cancel behavior.

Enabling game vibration exposed a separate release crash: the SDK's
`func_00245688` sends actuator data through `func_00244C70` and the unimplemented
`func_0025AAA0` (SIF DMA). Two attempts reproduced the missing-function exit.
The port handled the RPC actuator command but not this SDK's direct path.

The option fields live in `D_004225A0 + 0x11A60`, independently of PC settings.
`func_00195BB8` supplies defaults; `func_0015E750`, `func_0015ED18`, and the
front end's controls/difficulty screens change them. There is no option-change
disk write. Memory-card loading eventually calls `func_00193B20` to apply state.
**Correction to finding 050:** music is in the memory-card save. The accepted
edits in this run produce speech/effects/music 832/888/440 in the real
`BASCUS-97177` file at offsets `0x11A60/64/68`, plus schemes 0/1/1/1,
vibration 1/1/1/1, difficulty 2 and stereo flag 1 in the same options block.
The save code copies and writes the whole 0x12240-byte profile. Finding 050's
unchanged-file observation does not establish that music is excluded; its
input sequence was not reconstructed here. Back explicitly restores old raw
volume values, whereas Select accepts them. The demonstrated restart loss is
the lack of an automatic options write when no manual SAVE GAME DATA is done,
not a missing music field in that save. Independent preferences remove that
manual-save requirement without risking a whole-profile write.

## Design and implementation

Persist only an explicit whitelist of preferences in a versioned `game_options`
line in `dhd.ini`, using its existing temporary-file/atomic-replacement writer.
Fields: three volumes, four control schemes, four vibration flags, difficulty,
stereo, and the two unlock-dependent AUDIO/OTHER toggles. Validate the complete
line before touching any game byte. No pointers, unlock bits, records, career
data, padding, or whole game structs are serialized.

Hooks run when AUDIO/OTHER, CONTROLS, or DIFFICULTY is accepted. Cancel remains
cancel. The first front-end entry and every successful game-data load restore
preferences and call the game's audio, difficulty, and pad setters. Separate
preferences deliberately win over an older memory-card copy. An installation
without `game_options` keeps the game's defaults/loaded preferences until the
player first accepts an options page. No memory-card save is triggered, so an
empty profile cannot overwrite existing career progress.

`DHD_SETTINGS=none` (also `0`) returns before reading/writing preferences or
calling game setters. The renderer gate uses `none`. Its checker, references,
and exclusions are unchanged.

The direct pad actuator function now calls the same host vibration API as the
RPC path, honoring the pad's actuator alignment. It no longer attempts DMA to
an IOP that the port replaces. This is needed to test and use enabled vibration.

## Validation so far

- Transformed all three hooked units: compile without errors.
- Focused C regression: preference round trip, adjacent career-byte isolation,
  rejected invalid/trailing input, and tool bypass PASS. Added to
  `port/tools/test_runtime.py`.
- Clean `build_clean.py --tree build/port/r49settings/final-tree` at `feae225f`:
  all 3,797 units compile, link succeeds. Executable
  `build/port/clean/feae225f/dhd.exe`, SHA-256
  `c566aeb34192ff6a4f2d7d7e9c8c5bea9c695bb61cae6cedaf9d8ad96197a379`.
- Unchanged default r46-plain renderer gate **MATCH**: all 14 EE comparisons,
  all six paired moments through GO+75 s, and 1,143 readback records / Z reads
  through GS field 9,064. No exclusions or reference changes, no word audit
  needed. Proof: `build/port/r49settings/gate/result.json`.
- Full settings change / EXIT GAME / restart **PASS** on that same clean exe.
  Fresh `live-cycle/` fixture with no card files and no `game_options` line:
  speech/effects/music 921/963/512 -> 832/888/440; four schemes 0 -> 1;
  four game vibration flags 0 -> 1; difficulty 1 -> 2; stereo flag 0 -> 1.
  After restart every value matches exactly, including the four live pad
  vibration-enable flags, not just the saved preference bytes.
- Every PC row changed and returned correctly: 2x, Borderless, 1440x1080,
  Wide, Smooth, Widescreen ON, Skip Movies OFF, host Vibration ON, Texture
  Pack ON, Sharp font. Read the actual native page row strings after restart;
  all ten show these values without pending-restart stars. Startup movies
  actually played after Skip Movies was changed to OFF (Cross skipped the
  attract clip during the test). Both quits use EXIT GAME, exit code 0.
- First start without a career save **PASS**. Preference changes create no
  career files. Local machine-readable proof: `cycle-result.json` and
  `pc-page-proof.json` under `build/port/r49settings/`, logs in `live-cycle/`.
- Career save / load **PASS**. Played and finished the first Super Career
  event with normal pad input (double-Select bailout recovered a stuck rider).
  Select on its results retried the event; pause > Quit returned to the main
  menu, then SAVE GAME DATA wrote the four normal card files. The 74,304-byte
  profile contains changed rider/track records, and its entire `0..0x11A60`
  record region equals the live profile. After a clean exit and boot, that
  region loads byte-identically. This is a real game save, not a fabricated
  profile or a progress RAM write.
- With that career on the card, changed music 440 -> 352, accepted it and
  exited. The career file's SHA-256 stays
  `894a31b486fc9fb17fdd1799c0e09b0abc87f1e9a89b11a1ae3d4b164fdc0235`.
  A third boot keeps 352 even though the card still holds 440; manual
  LOAD GAME DATA also keeps 352. Both loads retain the byte-identical career
  record region and leave the card file unchanged. EXIT GAME returns 0.
  Proof: `career/result.json`, `career/priority-result.json`, and the three
  career logs in the local evidence directory.
- The two unlock-dependent AUDIO/OTHER rows were not offered by the fresh
  profile. Their flags are included in the validated whitelist and focused
  round-trip regression, but those locked rows were not live exercised.
  Back follows the game's existing cancel path; the new save hooks run on
  Select, without requiring SAVE GAME DATA.
- Earlier input-driver attempts are
  retained locally: a cursor-route error selected VIDEO CLIPS, and an atomic
  input-file replacement met a transient Windows sharing denial. These are
  test-driver failures, not persistence successes. The retry reads menu state
  and supplies pad buttons through `DHD_AUTOPAD`'s existing file input, with
  sharing retries; it never writes game RAM. The original release reproduction
  remains the no-tool-environment SendInput run described above.

## Changes and commits

- `9b018854`: separate preferences and ini reader (initial hook wiring).
- `3568e198`: SDK direct vibration path routed to host motors.
- `feae225f`: hook wiring through the actual port transform; this is the
  clean executable's revision. Later work in this task is tests/docs only.
- `4963e8b1`: focused regression and runtime-test runner integration.
- `eed7038b`, `41b60188`, `4e102b42`, `3736edcb`: findings/state and staged
  verification records. The closing documentation commit completes this log.

## Research

Read [ps2sdk libpad header](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/pad/include/libpad.h)
and [implementation](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/pad/src/libpad.c)
for direct actuator API semantics (six bytes, small on/off motor and big
variable-speed motor). Retail assembly established this game's direct-DMA
path. No third-party source was copied. No retail PCSX2 run was necessary to
identify the restart loss: the release reproduction and game source establish
the missing automatic write.
