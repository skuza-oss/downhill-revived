# Visual sweep (round 48)

Status: current
Checked: 2026-11-04

Follow-up implementation: [finding 068](068-visual-sweep-fixes.md). The boot loading-card cause is
isolated and corrected (masked high-byte uploads); fresh native pictures, identical-stream replay
and retail confirm the card. The Fog Editor grey bar is confirmed as retail behavior, with its
matching source and GS vertex colours identified. Dense Japan captures and replay establish the same
high-byte ownership bug there; the correction preserves its loading card too. Final gate/delivery
are recorded in that follow-up; historical open statements below describe the survey's original
handoff.

## Autonomous idle-PC continuation (2026-10-18)

The runner now waits up to three minutes for a foreign instance before deferring a launch.
An initial resume accidentally selected the PAL disc at the drive root; its USA-address
navigation was invalid. That run was stopped by owned PID, retained under the previous-arm
evidence directory, and is excluded from coverage. The runner now checks SYSTEM.CNF for
SCUS_971.77 before launching either arm. All subsequent runs reference the same local USA
image as the initial survey.

Live inspection of the corrected AUDIO/OTHER route shows the expected volume
dialog, but its modal loop advertises menu **13**, not 2. The runner's target is
corrected accordingly. VIDEO CLIPS and the card dialogs also advertise 13 in
their live modal loops; the OPTIONS menu's case 5 / menu 20 is the locked
PICKUP EDITOR, not VIDEO CLIPS. These are capture-tool
changes only; no game state or renderer implementation was changed.

The guard also had a time-of-check race: it computed descendants from one
process snapshot, then searched for foreign games in a second snapshot. A new
reservation child born between those snapshots was falsely classified as
foreign. Both calculations now use one snapshot. Sequential native launches
after this correction proceed without that false block.

Reviewed every selected image in these three-row retail/native sheets:
`audio-other/contact.png`, `video-clips/contact.png`, `load/contact.png`,
`save/contact.png`. The volume values/highlight, complete video list,
load-unsaved-data confirmation, and no-card save message agree. Aspect/font
and field placement differ intentionally; background animation timing differs.
No new player-visible defect was established. SAVE's missing card is expected
with cards disabled on both arms; successful card persistence is not covered.

The current fresh-save unlock mask was read without writing RAM:
`D_004225A0 + 0x12114 = 0x010020C3`. Matching readers `func_00193F38`,
`func_00193F68`, `func_001F8440` and the local retail table identify the six
enabled tracks: Italy FR/MX/TD and Utah FR/MX/TD (EE track IDs 0/6/24/1/7/13).
New named routes select them through the game's own list, with a countdown RAM
check of the selected track ID; an incorrect selection is marked wrong-track.
Other tracks remain locked in this fresh-save baseline. The earlier cheat
research was checked again against GameFAQs and the Downhill Domination Wiki;
unlock-everything also enables other cheats. Retain the unmodified baseline
rather than silently mix cheat-altered gameplay into visual comparisons.

Pause/loading field-capture routes, shop subpages, alternate controls and a
two-pad retail NAV option are being added. Their presence in `--list` is not
coverage proof: each still requires a successful run and image review. The
two-pad option is opt-in and mirrors the port's existing all-pad NAV behavior;
default PCSX2 comparison input remains player 1 only. No external code copied.

Also reviewed `replay-load/contact.png` and `rider-info/contact.png`, all three
rows in both arms. Replay load correctly reports no card in the disabled-card
configuration. Rider statistics, country/age/height/weight, unlock instructions,
flag/name and character are present in both; poses differ with idle timing.
The biography movie remains locked, as its on-screen instructions state.

An additional `alpine-finish` exploratory route holds the pedal for up to ten
game minutes and captures periodically after boot, alongside start/mid/late
clock windows. It records a candidate end-state transition and waits another
600 fields before stopping. That transition is explicitly `needs-review`, not
proof of results or a finish: the pictures and state logs must establish what
actually happened. This avoids inventing a race-clock anchor for a stopped
results screen. The retail movie-length override is opt-in; existing race
comparisons keep their original length and input rules.

### Unlock probe and pause coverage

To pursue the requested locked content, added a separate built-in input probe
rather than treating the fresh-save lock as a permanent blocker. The normal
30-field NAV gap fails the game's code recognizer: `001E6F58` clears partial
input after 20 neutral ticks. One-field clock-predicate pulses can also fall
between the retail game's pad samples. Adjacent clock predicates hold each
button for two fields, with six ticks between buttons. No RAM was written by
the tool. The game's own unlock routine `001F0780` changes the shell menu word
to 2 and the unlock mask to **0x035FFFFF**. Both independent retail and native
runs now prove that mask; the native manifest retains the observed word.

Reviewed all three rows of `unlock-probe/contact.png`: both show UNLOCK
EVERYTHING. These deliberately cheat-altered pictures are navigation proof,
not ordinary race parity evidence; their rider positions, trick scores and
message duration differ and are not classified as renderer bugs. The separate
`pause/contact.png` covers an ordinary paused race (clock 240 in retail), all
three rows reviewed. Labels, highlight, dimming, logo and legends are present
in both. Background poses/positions differ; aspect/font remain intended.
`unlock-pause/contact.png` is also reviewed in full: the unlocked pause menu
adds FOG EDITOR and POSTER MAKER in both arms. This shifts QUIT from row 6 to
row 8; the exploratory quit route accounts for it.

New-level selection after this probe is being tested so that later tracks can
start with fresh rider state. `0012C450` resets the ordinary rider cheat flags
on initialization; later runs still need their state and screenshots checked.
The fresh-save comparisons and the plain renderer gate do not use this probe.
No game or renderer implementation has changed, so there is no new game-state
fix to audit against retail. The original references and exclusions are intact.

Fresh final clean build at **604253a5**: 3,797/3,797 units compile, zero errors;
714 recompiled assembly-only functions, no unresolved symbols. Executable
`build/port/clean/604253a5/dhd.exe`, SHA-256
`457c884155537b91adf376c251b94672423a05b2be5132d65eb5565b67f0ca35`.
Subsequent edits are survey tooling/documentation only. Gate remains pending.

### Unlocked-track setup and read-only RAM checks

The NEW LEVEL list uses **left/right**, unlike the front-end track selector's
up/down. An initial down-arrow route is retained as failed navigation. The
corrected route loads Japan (track 12) in both arms. All three paired rows in
`unlocked-japan/contact.png` are reviewed: track, riders, HUD, dust and bright
sun are present. Positions and timing differ; these are exploratory setup
captures, not normal-race coverage. Retail's first successful run auto-ended
the flyby while the old capture target waited for menu 9; its actual menu 2,
phase 3 and track 12 are proved by the per-field log. Fields 10920/10950/10980
were recovered for its sheet. Native's corrected route also proves track 12.

PCSX2's existing `EEmem` debugger export
([Memory.cpp](https://github.com/PCSX2/pcsx2/blob/fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3/pcsx2/Memory.cpp), pinned comparison
revision fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3) makes targeted independent
RAM reads possible without rebuilding the adapter for each diagnostic. The
tool maps its export table without executing the executable, finds the owned
process module, and opens **PROCESS_VM_READ only**. No process memory write or
game-data download is used. No upstream implementation was copied.

This catches a setup limitation: `0012C450` does **not** reset rider +7BB2,
Always Stoked. After reload, both arms have ordinary flag words at rider
+7BA8/+7BAC/+7BB0 = `0, 0, 0x00010000`, plus the retained unlock marker word
at +7BB4 = 1. The retail row-19 input code (Down, Square, Square, Left, Circle;
also documented by GameFAQs) toggles that flag. New survey routes turn it off
before NEW LEVEL and guard navigation on the observed flag word. The next
countdown must prove all three ordinary flag words are **zero**, not merely
that the requested track loaded.

Added opt-in `DHD_RACE_SYNC_UNLOCKED` / `DHD_COMPARE_RACE_UNLOCKED` support for
that survey. It leaves NAV in charge until the unlock mask is 0x035FFFFF and a
fresh rider's three ordinary flag words are zero, then uses the existing
countdown/GO input and five-field capture machinery with menu 2 accepted.
Normal menu-9 comparisons retain their original arming rule. This is diagnostic
input/capture support, not a gameplay or renderer fix. A fresh clean build and
the unchanged plain gate must validate the default path before delivery.

The planned 24 ordinary-track routes use the game's own unlocked NEW LEVEL
list. Mosh Pit and training are separate modes. Their requested landmarks are
countdown, GO+0.5/+30/+150 seconds; **a clock label alone is not a near-finish
claim**. An independently read human-finished flag can request additional
results-tail captures. Each resulting image still needs review. Race manifests
now count each requested five-field window individually, rather than allowing
unrelated periodic pictures to make an incomplete race appear captured.

The first full-pedal retail attempt stops at its ten-game-minute bound without
a human finish. A read at clock 27797 found checkpoint 19 of 28 and human
finished byte 0. Native **does** finish: independently read human finished byte
1, checkpoint 28 of 28, finish time **23551 ticks**, stopped race clock 23871.
The old phase-based detector missed it because the game remains in phase 3.
Stopped only that owned run's PID tree after preserving its captures and logs.
This demonstrates a route/input limitation and long-run simulation divergence;
it does not establish a renderer defect. A further bounded twenty-minute input
attempt is queued for retail. No finish or results picture is claimed yet.

Inspection also found that `present.c` deliberately prioritizes race-clock
captures over `DHD_CAPTURE_FRAME`; the initial periodic-tail request was ignored
in native race mode. Added explicit opt-in `DHD_RACE_CAPTURE_INTERVAL` and
`DHD_RACE_CAPTURE_FROM` support alongside named race windows. Named consecutive
windows retain priority. The default path does not take extra pictures. Finish
watching now reads the actual human-finished byte, rather than guessing from a
phase number. Both the new runtime capture branch and the earlier opt-in arming
support require a fresh clean build and plain gate; old images are not relabeled
as results proof.

### Shared-agent PC update

A foreign PCSX2, a game outside the repository, or an unreadable game path still defers the
survey with bounded waiting/retry. PID family classification continues to use one process
snapshot. Screenshots are produced inside the owned game or emulator, not taken from the
foreground desktop.

## Latest validation (2026-10-18)

**NATIVE RENDERER GAME-STATE GATE MATCH** for clean runtime revision **044170e8**,
executable `build/port/clean/044170e8/dhd.exe` (full hash above).
`build/port/r48sweep/final-gate-044170e8/result.json` is PASS, no failures. All eight
default **r46-plain** frozen-reference comparisons and all six paired exact/native
checkpoints through GO+4500 MATCH. **1,124 readbacks / Z reads MATCH through virtual GS
field 9045**; both final captures are field 7715. Native presentation was exercised (8,759
presentations). No widescreen on either arm; native scale 2 only. Gate code, exclusions and
reference remain unchanged. No game-state correction was made, so no new differing-word
audit is needed.

## Method and scope

**Partial survey, continuing on the released/shared-agent PC.** The historical
blocked run is retained below; the continuation and latest validation take precedence.
This is a visual survey, not an FPS benchmark. PCSX2 independent
retail boot is the referee. A matching renderer game-state gate does not prove
that its pictures match retail.

`port/tools/visual_sweep.py --list` describes the reproducible navigation cases.
Set `DHD_ISO` to an existing local USA disc image, then use e.g.:

```text
python port/tools/visual_sweep.py --case main --case options --case controls --case difficulty --case track
python port/tools/visual_sweep.py --case alpine --arms retail native exact
```

All captures, logs, manifests, executable snapshots and contact sheets stay
under ignored `build/port/r48sweep/`. Each arm runs **sequentially**. The runner
requests windowed 640x448 port windows, resizes its own emulator windows without
activating them, and requests `SW_SHOWNOACTIVATE` at process creation. The game
can explicitly show its own window; the runner never sends keyboard input or
calls SetForegroundWindow. Its deadlines stop only the launched PID tree.

Port baseline: the plain clean release build `bde3cf92`, native renderer, scale
2, widescreen 16:9, sharp font, virtual clock, seed 81257. Exact is an additional
diagnostic arm. Both bypass port movies, disable memory cards and sound, ignore
the player's settings file, and leave saves and settings untouched. Retail uses
the existing comparison adapter, software GS, original aspect and no patches.
BIOS and ISO are referenced in place. No game data is downloaded.

Menus use the shared event-driven `menu_nav.h` navigation. Three selected samples
are retained in each contact sheet, at approximately +60/+90/+120 fields. These
are **not consecutive** fields and do not establish sub-frame effect durations.
Race events capture five consecutive fields using the existing countdown/GO
phase and clock anchors. After physics diverges, equal race clocks do not imply
equal positions or the same pickup; event identity must be checked separately.
No image similarity score is used to declare a bug or a match.

Contact sheets correct raw framebuffer display geometry: retail is displayed
at 4:3 and the port at 16:9, at a common height. Originals are retained unchanged.
The port's wider world view, centered 4:3 menu elements, sharper font, removed
field bob, ONLINE/EXIT GAME rows and PC SETTINGS row are intended differences.

Two capture-tool problems were corrected before extending coverage:

- PCSX2's Windows loader rejected the forward-slash ISO argument despite the
  file existing. Pass the resolved Windows path; detect startup errors promptly.
  The failed startup log remains in `startup-path-failed/`.
- A navigation step can finish at a transition menu (32/33/34). Later cases
  verify the actual final menu using retail's existing per-field state log and
  read-only port RAM inspection in the launched process family. The launcher's
  unused RAM must not be mistaken for the game child's state. Merely observing
  a step change is not sufficient coverage evidence.

## Initial comparison table

| Screen/moment | Difference | Native only / exact | Player visibility and disposition | Suspected cause / evidence |
| --- | --- | --- | --- | --- |
| Main, OPTIONS | Additional PC rows; centered menu sizing; sharper glyphs | PC feature / both | Visible, intended | Port menu extensions and widescreen/font settings |
| Main, OPTIONS, difficulty | Background animation phases differ slightly | Both | Visible, comparison limitation | Different boot/load lengths; arrival is synchronized, animation clocks are not |
| CONTROLS | Port glyphs smoother; diagram and world use widescreen presentation | Native sharp font; aspect both | Visible, intended | Sharp font and aspect normalization |
| Single event menu | Rider pose and framing vary across samples | Both | Open timing comparison, no fix justified | Idle animation and wider view; no identical GS stream established |
| POSTERS / PICKUP EDITOR | Disabled on a fresh save | Retail and port | Coverage open, not a rendering defect | Disabled rows visible in OPTIONS; game skips them during navigation |

Initial contact sheets: `main/contact.png`, `options/contact.png`,
`controls/contact.png`, `difficulty/contact.png`, `track/contact.png`,
`single-player/contact.png`, all relative to `build/port/r48sweep/`.
The historical case name `track` currently captures SELECT SINGLE EVENT RACE;
it is not evidence that a track-selection screen was covered.

## Completed coverage and outstanding work

Ten screens have independently booted retail and native captures. Five also
have exact captures. The historical case names remain stable so existing local
evidence paths do not silently change; `--list` and `coverage.json` name the
actual screens.

| Actual screen | Case / contact directory | Retail + native | Exact | Review |
| --- | --- | --- | --- | --- |
| Main | `main/` | Yes | Yes | Intended PC rows, aspect/font; animation phase differs |
| Rider selection | `single-player/` | Yes | No | Rider/model/name/flag/ball present; idle phase differs |
| Start new / continue | `rider/` | Yes | No | Same enabled/disabled choices; wider view and idle phase |
| Single-player modes | `single-modes/` | Yes | No | Same choices and disabled modes; no clear new defect |
| Single-event race type | `track/` | Yes | Yes | Same choices/model; wider view and idle phase |
| Bike shop | `bike/` | Yes | No | Initial pair caught retail fade; latest captures show the shop correctly |
| Multiplayer player count | `multiplayer/` | Yes | No | Same player-count choices; not a split-screen race test |
| OPTIONS | `options/` | Yes | Yes | Same original rows; PC SETTINGS added intentionally |
| CONTROLS | `controls/` | Yes | Yes | Diagram and labels present; sharper native glyphs intended |
| DIFFICULTY | `difficulty/` | Yes | Yes | Same three choices and default MEDIUM |
| Career type | `career/` | Native only | No | Retail route retry remains open; not a completed comparison |

Each completed directory contains `contact.png`; `--report` also writes
`contact-latest.png` from the last three actual captures. These latter sheets
are useful for distinguishing fades from lasting differences. For the shop,
the original retail samples at fields 2310/2340/2370 are still fading. Retail
field **2490** and native field **1260** both show the fully visible shop
(`bike/contact-latest.png`, last row). Raw images:
`bike/retail/retail/field-002490.png` and `bike/native/shot-040.bmp`.
The world, bike, jersey, rider, bank and legend are present in both. These are
not exact animation/frame matches and establish no new rendering defect.

Further capture corrections:

- The bike-shop menu ID appears before its fade completes. New shop/track
  selection runs wait 600 fields before accepting their selected samples.
- The initial AUDIO/OTHER route actually reached CONTROLS: one-shot cursor
  presses can be ignored during a menu fade. New menu steps wait 240 fields
  before pressing. That corrected route is implemented but **not yet verified
  live**. Existing successful cases retain their original recorded NAV strings.
- A wrong initial career route tried the disabled continue-save choice. The
  corrected native run reaches career menu 12. Its retail retry first hit a
  directory-preservation error, then the shared-PC instance guard; no retail
  career comparison is claimed. Failed-route evidence stays under ignored
  `career-route-failed/` and retained previous-arm directories.
- `--retry-blocked` and `--recapture` preserve previous arm directories with
  unique names. `--report` regenerates sheets and `coverage.json` without
  starting a game. Capture status means artifacts exist; it is not a visual
  correctness verdict. Actual screen labels still require review.

**Open coverage:** corrected AUDIO/OTHER; VIDEO CLIPS list; LOAD/SAVE and replay
load dialogs; rider biography; track-selection confirmation; remaining
single-player career/arcade/training branches; shop inventory subpages and
alternate controls; all tracks' countdown/mid-race/near-finish moments; loading
cards; pickups/combat/crashes/tricks/dust/water/weather/night/sun/replay sequences;
pause; results; and a 2-player split-screen race. No race/effect from this round
was captured before the instance guard blocked live work. Finding 061's older
pickup/sun captures are not relabeled as this round's coverage.

POSTERS and PICKUP EDITOR remain locked on a fresh save. Other track unlock
coverage is unverified. Split screen additionally needs retail player-2 input
(the current retail NAV adapter injects only player 1, whereas the port scripts
all connected players). Pause/results need state-based capture anchors: the
race-clock-only helper cannot capture a stopped pause clock or prove a finish.
The runner lists these as open cases rather than providing a known-broken
pause race script. This is not an exhaustive whole-game sweep.

## Decisions, fixes and research

No renderer or game-state fix was justified by the completed samples.
Do not infer a fix from different rider poses or from differently stretched raw
framebuffers. The capture tooling changes are separate working-step commits.
The unchanged r46-plain reference and gate checks remain authoritative.

Investigated whether the game's own unlock code could expand fresh-save track
coverage, using the [GameFAQs cheat list](https://gamefaqs.gamespot.com/ps2/914603-downhill-domination/cheats)
and [Downhill Domination Wiki cheat list](https://downhill-domination.fandom.com/wiki/Cheat_Codes).
Unlock-everything also enables other cheats, so it was not applied to the
baseline survey. No external implementation was copied or adapted.

## Validation

Capture runner: `python -m py_compile port/tools/visual_sweep.py` passes.
Fresh `build_clean.py --tree build/port/clean/tree-r48sweep -j 6` build:
all 3,797 source units compile with zero errors; link succeeds without undefined
symbols (714 recompiled assembly functions). Clean implementation revision
**5760505b**, executable `build/port/clean/5760505b/dhd.exe`, SHA-256:
`04f4adcc99c9cddde994c243eb3862d320786ac137d648a364a2d36e558ecd65`.
Subsequent changes are capture tooling and documentation only.

The default **r46-plain gate is INCOMPLETE, not MATCH or FAIL**. The unmodified
gate started this clean executable's exact arm, but another player-owned game
was observed before race captures. Stopped only the gate's own runner/game PID
tree, leaving the player's process alone. No checkpoints, readback verdict or
`result.json` were produced. Evidence: `build/port/r48sweep/gate/method.json`,
`gate/exact/port.log`, `gate.log` and local `run_gate.py`. Review also caught that
the interrupted attempt requested widescreen on the native arm: widescreen
deliberately changes EE state and must not be used with this frozen reference.
The retry helper is corrected to plain gate settings plus native scale 2, with
small window sizes for both arms, and a fresh `gate-clean/` output directory.
That corrected attempt remains blocked. No comparator, exclusions or reference
changed, and no new reference was recorded.

The runner now checks for existing `dhd.exe` / PCSX2 processes both before launch and while
capturing. A foreign instance defers the case and stops only the runner's own active child.
Tested the guard while the player-owned game was running: all remaining requested live arms
report `blocked`, without launching another game. `coverage.json` preserves those reasons
and partial coverage.

Working-step commits: `c0909751` (initial sequential runner), `5760505b`
(fresh-save route inventory), `97b34ea0` (target-state validation and initial
finding), `6a7f6f95` (retry preservation and shop settling). Final tooling adds
fade-aware one-shot steps, process guards and offline coverage reporting.

To resume on an idle PC, use `--retry-blocked` for remaining arms and
`--recapture --case bike` for the longer shop settling schedule. Do not accept
the corrected routes as covered until their resulting screenshots are reviewed.

### Results input and latest clean build

The clean build at **044170e8** compiled all 3,797 units with zero errors,
linked 714 recompiled assembly functions without unresolved symbols, and produced
`build/port/clean/044170e8/dhd.exe`, SHA-256
`ce5e635f3d086ff6a2dc642a30a411f5681f87efe9dde837c313a264796a9420`.
The default plain r46-plain gate is now running; no result is claimed yet.

For stopped-clock results/replay exploration, the comparison adapter accepts an
opt-in `DHD_COMPARE_LIVE_PAD` file with a hexadecimal active-high PS2 button mask.
An absent/invalid file leaves the movie input unchanged. The port already has
`DHD_RACE_PAD=@file`; the survey now uses both hooks for its full-pedal finish
probe and releases CROSS immediately after independently observing the human
finished byte. This prevents held pedal input from dismissing a results dialog.
Normal comparison/gate input is unchanged. This is scripted controller input,
not a memory write or game correction. Microsoft documents that `fopen_s` does
not share its open movie file, so replacing the running p2m2 movie was rejected
as a control strategy ([CRT fopen_s](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-s-wfopen-s?view=msvc-170),
[shared opens](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fsopen-wfsopen?view=msvc-170)).
No external implementation was copied; the adapter remains GPL-3.0+.

### Clean-tour navigation retry

The first `tour-italy-fr` bootstrap independently read ordinary flag words
`0x01010001, 0x00000100, 0x01000001`: Always Stoked was successfully off,
but the rider was still in the bootstrap race, before NEW LEVEL reset the other
flags. The delayed NAV then waited for phase 2 after the race had advanced to
phase 3. This is a survey-route failure, not game coverage or a renderer bug.
The clean-tour guard now accepts either racing phase by checking menu 2 and the
actual `0x01000001` flag word. The following flyby step still requires phase 5,
and the eventual countdown still requires all three ordinary words zero.
Stopped only owned failed-route runner PID 66864 and emulator PID 45108;
its evidence is preserved for the recapture retry.

Windows briefly retained the killed emulator's directory handles, causing the
first immediate retail preservation rename to fail with WinError 5. Preservation
now retries only PermissionError for up to 30 seconds, then reports the block;
it never deletes evidence. A retail-only retry is queued after the current native
pilot. Its native countdown dump already proves track 0 and ordinary flag words
`0,0,0`. Reviewed all twelve plain-gate screenshots in `gate-images.png`; expected
font/field placement differences, no additional clear defect.

Race manifests now retain the last independently read track/finished/clock/
checkpoint/plane-count words, so late clock windows can be distinguished from
actual near-finish progress. The race runner also honors its explicit timeout;
the old hidden 900-second minimum made short navigation probes wait too long.
The first clean native Italy pilot's twenty selected frames are reviewed in
all four contact sheets: countdown/sun, start, energy-pickup fragments and late
crash/red particles/dust. Its late picture is a wall crash, **not near finish**.
Retail comparison remains in progress. No rendering fix inferred from it.

### Verified ordinary-track pilot

`tour-italy-fr` now succeeds in both arms: selected track 0, all three ordinary
cheat words zero, and 5/5 frames in countdown, start, mid-clock and late-clock.
Reviewed every row in all four paired contact sheets. Countdown/scene/HUD/sun
are present in both; countdown changes from 3 to 2 within the consecutive-field
window as expected. Later trajectories differ: native collects an energy pickup
at +1800 while retail is on a different path; native is crashing against a wall
at +9000 while retail is riding downhill. Those are timing/trajectory samples,
not evidence that an effect is missing from either renderer. No near-finish or
retail pickup parity is claimed from these windows. Exact was not run for this
tour, so its later retail difference is not classified native-only. The remaining
track tours were released only after both independent flag/track checks passed.

| Difference | Triage | Scope | Severity / action |
| --- | --- | --- | --- |
| Wider framing and sharp glyphs | Intended PC presentation | Native; exact diagnostic retains its own rasterization | None |
| Countdown numeral/idle poses on adjacent fields | Timing only | Retail/native; plain exact/native scenes also reviewed | None |
| Italy later path, pickup occurrence, crash time | Timing/trajectory divergence; visual bug unproven | Retail vs native; tour exact not run | Open simulation investigation; no renderer fix |
| FOG EDITOR gray slider fill | Unresolved visual difference | Both native and exact vs retail | Low candidate; open pending producer/state/aspect isolation |
| Fresh-save locked cards/movies/subpages | Intended unavailable content in this setup | Both arms | Persistence/full unlock UI coverage remains separate |
| Italy close dark plane near GO+507 | Inherited scene/camera occlusion, resolved by three-arm probe | Retail, exact and native | No port bug; disappears by GO+512 |
| Loading card alternating black/partial strips | Confirmed rendering bug; root under investigation | Native only in 41-field exact/native boot probe; retail window deferred | Low; complete card in exact fields 130-149, no speculative fix |
| Japan NEW LEVEL loading mosaic | Open transient candidate; settled course correct | Native observed; exact boundary and retail dense window not captured | Low candidate; native shots 212/213, no speculative fix |
| Incomplete trailing image on forced process stop | Capture failure, excluded from coverage | Retail PNG and native BMP files listed below | Tool evidence only; no player-visible bug inferred |

A separate `alpine-actions` route now attempts combat, charged bunny hops and
shoulder-button tricks using the mappings shown in the reviewed retail CONTROLS
page (Circle/Square combat, R2 hop, L1/R1 tricks). Six consecutive-field windows
cover the attempts through GO+1800. These are requested actions, not coverage
claims; only visible effects in the resulting images will count. No gameplay
implementation or retail memory is modified.

The review now includes **every actual screenshot** from the successful Italy
pilot: 142 retail + 125 native, inventoried in twelve `review-pages/*.png` sheets
and `review-pages/index.json`, all reviewed manually in addition to the larger
paired windows. This adds flybys/loading transitions, crashes, dust, route
markers and pickup fragments to the observations. Native `race-f008160.bmp`
shows a bright initial energy-pickup flash; it is absent at the next 120-field
sample. This is not evidence of a prolonged flare: its duration and independent
retail equivalent remain unresolved. No suppression/fix is justified from sparse
sampling (see finding 061's two-field-flash caution). The native loading-card
sample is transitioning rather than fully settled; its dedicated case remains
open. Bootstrap cheat effects are excluded from ordinary race coverage.

`--review-pages` now tiles and inventories every actual capture, including
incidental periodic images. It can accompany a capture or `--report`; the index
maps every thumbnail to its raw screenshot. An image/status existing still does
not mean it was reviewed or that the screen is correct. Output stays ignored.

### Native finish, results and replay evidence

The clean 044170e8 native full-pedal route repeats the earlier finish exactly:
human finished byte 1, finish time **23551**, 28/28 checkpoints, stopped clock
23871. The probe driver was replaced by an owned-process guard before finishing;
other queued launches were paused until this game closed. Live input released
CROSS, then selected REPLAY using three separately released Down presses and
Cross. No focus or RAM write was used. Stopped only game PID 73364 / its child
63920 at the end, then resumed the sequential queue.

Reviewed `native/results-native.bmp`: RACE RESULTS, 10th-place COSMO time
6:32:31, awards/bank and results menu are readable and present. Three Down
presses visibly highlighted REPLAY before selection. `replay-native-1.bmp` and
`replay-native-2.bmp` show subsequent race playback with the live input file
empty; rider, track, camera and HUD render. These establish **native-only
partial coverage**, not retail/exact parity. The nearby pre-finish fade sample
is black except PUNCH, then the course/finish/results return; it is not treated
as a persistent black-screen bug. `race-f024720.bmp` shows the last course
section around checkpoint 26/28; the later finish view shows the FINISH banner.

State/input notes are in `native/results-state.json`, `replay-state.json`,
`replay-end-state.json`, the updated manifest and local guarded driver. The
current native run has 375 actual screenshots, including replay; its complete
review pages are being checked before final coverage. A retail-only twenty-game-
minute finish retry is queued. The `results` inventory entry is now an executable
finish probe rather than an unavailable case; retail/replay comparison remains
open until independently reached and reviewed.

Completed manual review of **all 375** native full-run/replay screenshots in
sixteen indexed pages. Additional observed native effects include burning trees,
smoke, rain, pickup fragments, dust and repeated crash fades. Early replay frames
`race-f033000.bmp` / `race-f033120.bmp` visibly show **REPLAY / O TO SWITCH CAMERA**,
so playback identification does not depend only on neutral-input inference.
No defect is inferred from the brief fade-to-black samples, which return to the
course. Retail/exact equivalents of these late effects remain open.

The complete review identified a large close dark plane in
`alpine-finish/native/port/race-f003720.bmp` (about GO+507). It could be nearby
geometry/camera intersection or a rendering fault; no root cause is established.
Added `alpine-occluder`, three five-field windows at +500/+506/+512 with identical
full-pedal input in retail/native/exact, to isolate it before choosing a fix.
Provisional severity: medium if a port defect; scope unresolved pending captures.

The first split-screen route incorrectly reused single-player shop predicates.
A diagnostic screenshot selected **only owned emulator PID 76452's window** by
PID (Pillow window capture, no focus call) shows the combined PLAYER ONE / PLAYER
TWO SELECT RIDER page, menu 34. The existing local `lockstep_local.py::NAV4` confirms
34:Cross is the missing step. Corrected the two-player route to confirm that page
on both injected pads and load the event directly. Stopped only the stale owned
runner 65544 / emulator 76452; preserved the failed attempt. The corrected retail
run reaches countdown and the +1420 race window; native/image review is pending.

Reviewed all 30 initial corrected retail split-screen screenshots. Both viewports,
countdown/HUDs and world render, but player 2 remains stationary: the race movie
serializer previously wrote input only to player 1, despite both pads navigating.
Native already applies the fallback race script to both connected players.
Added an opt-in two-pad movie serialization argument, selected only by the
2-player survey. Default and explicit one-player movies are byte-identical;
a four-field independent format check verifies two equal pad records, neutral
boundaries and the expected active-low CROSS mask. No game/adapter runtime change.
A retail-only recapture is queued so the final comparison exercises both riders.
The initial stationary-P2 pictures are input-tool evidence, not a game bug.

### Reviewed two-player widescreen race

The corrected two-pad retail recapture and native run each contain all six
5-field windows (countdown, start, sun, mid, pickup-clock, later), **30 images per
arm / 60 total**, all manually reviewed in four indexed review pages. Both riders
now move on both arms. Native uses the survey's 16:9 widescreen setting; retail
retains its independent 4:3 reference presentation. Both viewports, divider,
places, energy bars, speedometers, countdowns, riders and world are present and
stay within their respective viewports. Different later paths/poses are timing/
trajectory differences; no clear new split-screen renderer defect was found.
Exact split screen was not run because no isolated native defect was established.
This covers a two-player race through +3000, not split-screen results/replay.

The opt-in retail live-mask hook now also fills all twelve pressure bytes using
the same documented pad order as p2m2 and the port; digital-only mask injection
would otherwise give pressed buttons zero pressure. Default recorded-movie input
is untouched. The rebuilt adapter is pending before the full retail finish probe;
its current runs use private frozen runtime copies.

### Dark-plane isolation resolved

All **45** screenshots from `alpine-occluder` are reviewed: 15 each in independent
retail, native and exact, across GO+500/+506/+512. The same large dark plane
occludes the close right side in **all three arms**, then leaves the view by
+512. Retail fields 5450-5459 and port `race-g+500*` / `race-g+506*` establish it
independently. This resolves the provisional candidate as inherited retail scene/
camera occlusion, not a port rendering defect; no suppression or renderer fix.
Its exact asset identity was not needed to determine that it is also retail.
The table's scope is now retail + exact + native, triage intended parity / no
new port bug, severity none for this sweep.

The live-pressure adapter rebuild completed successfully (exit 0), retained in
`adapter-live-pressure.log`. Pending launches were held during replacement and
released afterward; existing runs used private emulator copies throughout.

### Incidental capture integrity and boot-transition probe

Full front-end inventory found incomplete **trailing incidental files** after PID
termination: `controls/native/shot-013.bmp`, `difficulty/retail/retail/field-001710.png`,
`difficulty/native/shot-014.bmp`, `track/native/shot-033.bmp`,
`track-select/retail/retail/field-004410.png`, `track-select/native/shot-103.bmp`.
They are visibly marked UNREADABLE CAPTURE and listed in each review folder's
`errors.json`; they cannot count as reviewed screenshots. Earlier settled selected
samples remain valid. The race runner now waits for the final requested BMP's
complete header-declared length and counts only complete named BMPs. A scan of
all current named native race BMPs finds **zero incomplete files**. This strengthens
capture completion; gate code/comparators remain untouched.

The old main-menu inventory also shows a transient loading/title wipe difference:
`main/native/shot-003.bmp` contains extra black strips, while the exact sample has
a different partial wipe. Both later settle correctly. Equal boot field numbers
alone do not prove equal loading/wipe state, so this is a **low unresolved timing/
rendering candidate**, not a justified fix. `boot-transition` now captures every
port-local field 130-170 in native and exact to investigate it. It uses the existing
explicit-field capture hook, waits for every complete BMP, and does not pretend
that retail's much later boot field numbers are equivalent.

Its interrupted images/logs are preserved; exact is queued again after the remaining work,
not accepted as captured. An interrupted arm also requests that quiet period explicitly
before its retry. Initial idle launches remain immediate.

### Trick/action sequence review

All **60** `alpine-actions` screenshots are reviewed. Both arms show the charged
jump and **ONE HANDER**, the raised-hand pose, BASE/HOLD/TOTAL scoring, and the
subsequent 72-point crash. Retail later shows dust/red crash particles where
native's later route differs; no missing effect is inferred from those positions.
The attempts do not yet prove a combat hit. All six snapshots in both arms have
zero human combat-point sum at rider +7AC8..+7AD8 (`001301B8` / `001301E0`).
Rider +79AC counts hits **received** (`0012E940` increments its victim), so it is
not used as proof of hits dealt. No gameplay changes or renderer fix.

Added a separate `combat-pair` attempt: two human riders stay close at the start,
P1 alternates released right/left punches before pedaling, P2 stays neutral.
Retail deliberately records only P1 for this case and native explicitly gives
P2 an empty script; this is distinct from the reviewed both-riders-moving
split-screen case. Eight five-field windows and RAM snapshots must establish
actual contact before combat-hit coverage is accepted.

All 41 native `boot-transition` fields are reviewed. Fields 130-149 alternate
mostly black loading output with partial card tiles; the later title settles.
This establishes a reproducible transient symptom, but exact/retail scope and
root cause remain open while the foreign-instance guard delays live retries.

The loading investigation now includes an independent retail window (fields
930-970, where the earlier retail boot shows its loading card), explicitly
separate from port fields 130-170. No claim is made that boot clocks match.
A separate `boot-transition-trace` case records the existing field trace and a
32-field GS stream beginning at 125; it finishes before the last screenshot.
Its screenshots must be checked against the non-dump baseline because settling
GS state for a dump can perturb cache/coherence symptoms. Neither probe changes
gameplay or renderer logic. Scope/root cause remain open pending those runs.

### Full incidental menu review continued

Reviewed every readable image on the indexed pages for `career-super` (88 native;
retail deferred by the foreign-instance guard), `controls-alt` (140 retail/native),
and `multiplayer` (57 retail/native). Super Career reaches the inventory/bikes/
jerseys/color shop correctly. Its trailing `native/shot-088.bmp` is incomplete
and excluded, like the earlier interrupted trailing files. Settled shop images
are readable. The additional frames reveal no new settled-screen defect.

`controls-alt` visibly switches **VIBR. OFF to ON**, with both arms still on
CONTROL FREAK. Source `func_001F0A28` confirms that the horizontal value encodes
`scheme * 2 + vibration`; one arrow cannot cover a second scheme. Added a separate
two-arrow `controls-scheme` case and queued it. This is a coverage correction,
not a game fix. The recurring native boot strips remain the same open loading
candidate; ordinary menu fades and later pages settle correctly.

Completed the full indexed review of all readable current images in the initial
18 front-end cases. Additional totals: options 74, controls 72 (one incomplete),
difficulty 76 (two incomplete), bike 116, rider-info 99, career 140, single-event
race type 134 (one incomplete), audio-other 156, load 196, save 211, replay-load
228, video-clips 324, and track-select 241 (two incomplete). The earlier main,
single-player, rider and single-modes totals and the multiplayer total above
complete that inventory. Each thumbnail was inspected; settled detailed contact
samples were already reviewed at larger size. No score or capture status stands
in for the visual review. Career's retail retry did reach the correct career-type
page; the historical inventory table's native-only note is superseded.

The extended transition frames show the retail shop-entry dust and longer disc-load fade,
while native reaches the next page sooner. They are timing-only observations, not evidence
of missing race particles. The native loading strips are the recurring separate candidate
described above. No outside PID was stopped or guard bypassed.

### Dense loading comparison establishes native scope

The launch window allowed the ordinary exact probe and native GS-traced probe
to complete. Reviewed all **41 exact** and **41 traced native** screenshots.
Exact retains a complete loading card through fields 130-149. Native alternates
almost black even fields and partial-card odd fields, identically with and
without the GS dump; the trace did not mask the symptom. Both render the later
title correctly. This supersedes the earlier unresolved timing classification:
the missing card pixels are a **low-severity native renderer bug**. The later
partial card/title transition at 150-153 occurs in exact too and remains separate.

Registers alternate display framebuffer page 56/112 (DISPFB2 0x9038/0x9070),
PSMCT24, during the bad window. The first dump starts at field 125, after the
card is already damaged, so its synchronized initial VRAM cannot establish
the producer of that damage. Added `boot-transition-history`, starting the GS
stream at field 60, plus per-field native command/import/copy counters. Exact
and native history arms are queued before ordinary coverage; no renderer code
is changed until the earlier uploads and ownership transitions establish a cause.

### Combat contact verified

Reviewed all **80** `combat-pair` screenshots (40 per arm). Both arms show
**PUNCH! 50 PTS** by GO+36, attacker score 50, the attack pose, and the contact
effect. P2 remains at the start while P1 later pedals away, as this case requests.
The corresponding read-only EE snapshots are retained in `port-ee` / retail
output. This closes the earlier unconfirmed combat-action coverage, without
pretending that simply sending a punch button proves contact. No clear effect
or split-screen renderer difference is found; later trajectories differ.

The late-start native GS stream replays with missing card pixels in both offline renderers,
confirming why it is insufficient for locating the initial damage. All 64 replay output
images were inspected. The ignored replay harness uses the unchanged GS renderer, original
font/no texture replacements, no swap chain or game simulation, and no pacing waits. It is
diagnostic work, not an FPS benchmark.

### Full pause / unlocked incidental review

Completed all 38 pages in `remaining-menu-review`, including all 909 newly distinct
images. To avoid repeatedly inspecting literally identical stopped frames, the
inventory groups only equal dimensions plus SHA-256 of fully decoded RGB pixels;
no similarity threshold or visual score is used. Every group representative was
visually inspected, with every alias retained in the ignored index. Another 1,395
files match previously reviewed images exactly; the new groups account for 1,907
files. This choice preserves every distinct picture while removing only exact
pixel duplicates. No unreadable images occur in this inventory.

The resulting fully reviewed current totals are pause 388, unlock-pause 405,
unlock-probe 385, fog editor 625, exploratory Japan 948, unlocked level list 429,
and retail Specialized Career 122. The native Specialized Career retry remains
blocked; stale images from its interrupted attempt are excluded. Previously
selected settled-screen comparisons still supply detailed pause/Fog comparisons.
Japan's flyover, countdown, sun, dust and energy pickup flash are visible; its
exploratory run then stalls at a fence. It does not establish normal cheat-free
near-finish coverage. Native Japan transition shots 212/213 show a tiled remnant
of the previous Italy scene during loading; retail transition sampling misses
that brief boundary, so its exact/retail scope remains open, alongside the dense
boot loading defect. The settled Japan scene is correct.

Also reviewed all 11 pages of the newly completed Career Freeride case: 245
readable retail/native images, excluding the truncated final retail field 4500
and native shot 104. Both arms reach the entry-bike-acquired modal in the shop;
ordinary camera/fade timing differs. No new settled-screen defect. The combat
RAM check independently gives dealt-hit components `(0, 50, 0, 0, 0)` at GO+36
and later in both arms, versus all zero at GO+12; screenshots show the same
50-point punch. This is read-only evidence, not a gameplay state change.

At 03:33 local the earlier loading-history probe is still retrying three-minute idle
windows. No speculative renderer change is made: the late GS snapshot begins after the
damage and cannot establish the initial framebuffer owner/import.

### Guard-blocked handoff, 03:36 local

The full early-history case finished with **both arms blocked**, after repeated three-minute
waits and the built-in second attempt. Its exact launch was also interrupted during D3D
initialization before field 60, leaving no usable early GS stream. This follows many earlier
guarded retries across the continuation; it is not an immediate exit on first detecting
another instance. The last snapshot includes games in an outside-repo artifacts directory.

No owned game/emulator was active at that boundary. Queue state is retained locally: Russia
`deferred-guard`, 36 pending jobs, six old cases waiting for optional build/setup, and
completed jobs. A finished subprocess status never substitutes for an arm's
captured/reviewed status. All successful current case inventories have manual-review
entries; interrupted/unreadable output is excluded explicitly.

**Round remains partial / open.** Remaining work: native Specialized Career;
second controls scheme; Arcade/Training; shop bikes/jerseys/color; pause Options,
Trick List and Music Playlist; unlocked Poster Maker/Playlist; remaining 23 normal
track tours, all-track actual near-finish scenes, water/snow/night/weather effect
parity; retail finish/results/replay; early loading GS producer trace and native
loading-card fix; Fog Editor gray-fill isolation. Italy GO+9000 and the stalled
Japan exploratory run are not relabeled as near-finish. A broader phase cannot
be declared complete from these captures.

No player-visible renderer or gameplay fix is committed during this continuation:
the clear loading defect lacks root-cause evidence and remains open. Capture,
navigation, process-guard and review improvements are separate working steps in
the commit history above. Opt-in capture runtime **044170e8** is the last own
runtime change and was clean-built and gated. Later own changes are tools/docs.

Final available validation is unchanged: clean build 044170e8, all 3,797 units,
zero compile errors and successful link; executable SHA-256
`ce5e635f3d086ff6a2dc642a30a411f5681f87efe9dde837c313a264796a9420` rechecked at
handoff. `final-gate-044170e8/result.json` is **PASS / MATCH**, eight frozen
r46-plain reference comparisons plus six paired EE comparisons, 1,124 readback/Z
checks through GS 9045, zero failures. Plain settings, no widescreen on either
arm, unchanged reference/comparators/exclusions. This validates the pinned own
runtime, not unrelated concurrent runtime commits. All twelve gate screenshots
were reviewed. Clean exe: `build/port/clean/044170e8/dhd.exe`.
