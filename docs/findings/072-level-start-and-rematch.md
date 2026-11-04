# Level starts, online restarts and widescreen (2026-10-18)

Status: current
Checked: 2026-11-04

## Flyby input and race phase

Both host and guest switched to physical input only after the countdown or a previous start. Phase 5
therefore read the boot menu script, including on later races after its final skip step was spent.
Race-screen input now belongs to the player as soon as online setup has been exchanged, including
the flyby and its skip prompt. DHD_NET_SCRIPTED retains scripted test input. The offline path is
unchanged.

GO, finish, snapshot and hit processing also used an unbounded phase >= 2 check. Retail
func_0016B5A8 defines running as 2..4; phase 5 must not set go_sent or send old rider state. Those
checks now use the bounded retail predicate. This also removes a false source of restart detection
at the first start.

Sources: retail func_0016B5A8, func_0019ED18, func_0019EE98 and the existing finding-065 online
implementation. No third-party code copied.

Validation and remaining restart/presentation work are recorded below as the live test matrix
completes. Captures and executable artifacts stay local.

## Accepted restart actions and bounded guest control

The phase-drop detector used spent GO/results flags as evidence of a new race. Phase 5 is the flyby,
not GO; a countdown restart can stay in phase 1 and never cross the detector's boundary. The guest
then retained rematch_want and repeated pause-menu presses without a limit. Results navigation also
assumed phase 3 and repeated DOWN/X on a schedule, rather than reading the current row.

The host now announces EVENT_REMATCH only at accepted retail menu actions: func_0015C990 for TRY
AGAIN / RE-START, func_0015CC90 for the in-race level list. The latter differs from the front-end
track picker func_001F8A08. The hooks run before loading yields. Guests apply the requested track
after retail reads their own cursor and before it resolves the level name. The initial front-end
selection and flyby skip cannot announce a restart.

Accepting a menu action immediately stops injected presses and their deadline. The func_0019EA38
initialization hook resets GO, loaded/results/finish state and puppet history once the new race
actually initializes, including phase 1 -> 1. While an accepted reload is pending, outgoing
state/finish processing stops. Only phase 1 enters start_together. The phase-drop inference is
removed.

Both results and pause navigation read the actual open menu and row. The results follower works in
phase 2 as well as phase 3: an early finish can open results before the usual running phase. A
request expires after 1,800 fields or 30 wall seconds, or when peers disappear; the client leaves
online play and restores local control instead of injecting forever. An accepted loader is no longer
subject to this navigation deadline. Duplicate requests while following or rebuilding are ignored.
No protocol or server change.

### Live verification

Two local instances of the incremental clean-tree executable, at 60 fps, using the command-line
DHD_TEST_PAD / PrintWindow harness and local Wrangler 4.151.0. Button presses supplied all menu choices. Results were accelerated by setting each local
rider's FINISH_NOW byte, the same flag used by DHD_NET_TEST_FINISH; no phase, menu, track or cursor
memory was forced. Starting room KFW9T6 used different riders.

Passed on both PCs:
- Normal start: host START skips; guest START skips in the repeat. A separate
  final-code-path run lets the guest's flyby finish naturally (24.46 seconds
  after the host skipped); both start with no rematch event.
- Pause RE-START, and pause NEW LEVEL from track 0 to track 6.
- Results TRY AGAIN, including an early phase-2 results screen.
- Results NEW LEVEL selecting the same track, then changing track 6 to 24.
  Same-track selection can give the host a flyby and the guest a direct
  countdown; the start barrier correctly accommodates both.
- Guest manual X on pause RE-START remains blocked; Triangle returns.

Host: exactly six accepted actions, six resets, seven countdowns. Guest: exactly six host requests,
six resets, seven countdowns. No stale-request expiry or false startup rematch. Final log agrees on
track 24. The original specified pause/menu addresses and rows were correct; rejected confirmation
input was not the cause of the original infinite loop.


The first matrix helper incorrectly required both PCs to show a flyby after selecting the current
track. Observed successful restart/start counts and the separate changed-track repeat replace that
helper condition; it was a harness assumption, not a product change. Earlier development runs
exposed the missing level-list hook and phase-2 results assumption, both repaired and repeated
above.

Synthetic tests extract the actual C function bodies from online.c and compile them with fake RAM,
clock, peer and pad backends. They pass initial flyby input for both roles, bounded running phases,
duplicate actions, countdown-to-countdown restart, guest track override, reset completion, both
timeout limits, peer loss, pause confirmation, phase-2 results row navigation and offline guards. No
proprietary input in those tests.

Build: the supplied build.sh compiled patched units with zero errors and linked with the existing 23
SDK stubs. Final additional list callback and the restored unmodified front-end picker compiled with
zero errors using the same clean-tree build.py; changed host objects were rebuilt at O2 and strictly
relinked without /force using the same object response file. The final native renderer gate and
presentation validation are recorded below.

## Loading presentation and widescreen

The first goggles loading transition is not the later 144..170-primitive tiled card. A temporary
clean-tree-only measurement recorded 3,648 PATH1 primitives per goggles field, so finding 056's
1,000 cutoff never recognized it. The transition starts on screen 18 before screen 9/phase 0. Its
screen state was also read on the renderer thread after the field was queued, allowing the
classification to observe a later state.

host_widescreen_flat_limit now allows up to 4,096 primitives in screen 18 and screen 9/phase 0.
Title and flyby retain the 1,000 cutoff; countdown, race and results retain no primitive-count
heuristic. Each queued NativeFrame records its limit at handoff, rather than sampling game state
during presentation. The existing eight-field hysteresis and 4:3 pillarboxing remain. No gameplay,
FOV or GS readback changes. The synthetic renderer test stub uses the renamed helper. The temporary
PATH1 measurement was removed from the tested executable.

This fixes both the early goggles loading picture and the pre-load shop/level selection transition;
later tiled loading cards and startup cards retain their existing handling. At 960x540, the
corrected cards occupy x=120..839 with 120-pixel black bars on each side. Pixel checks of both
clients' captures find zero nonblack pixels in those bars; the race returns to the full wide view.

W-fixed-load-0-1.png shows the offline initial goggles card. The later race/flyby images listed
above remain wide. Only 16:9 was visually exercised in this task.

Offline physical test (no DHD_NAV and no online flags): main menus, shop and track chooser, loading,
phase-5 START skip, phase-1 countdown and GO all pass. The first START/X during shop fades was
ignored as in retail; the harness repeated those presses after the screens settled. Proof:
repair-offline.log; W-final-offline-wide-loading.png, W-final-offline-flyby.png,
W-final-offline-skip-countdown.png and W-final-offline-racing.png. The earlier scripted offline run
was used only for loading captures; its DHD_NAV deliberately owned input and was not used to claim
physical skip.

python port/tools/test_native_transfers.py: all 13 synthetic pixel cases PASS, including transfer
ordering, high-byte preservation and both loading buffers. The final unchanged default offline
60-fps gate is recorded below.

## Final offline exactness and handoff

Unchanged default gate_native_renderer.py --exe build/port/clean/tree/build/port/link/dhd.exe --out
build/port/r72/gate-accepted: NATIVE RENDERER GAME-STATE GATE MATCH. PASS, failures empty. All 14 EE
checks match r46-plain and the paired arms through GO+4,500. Both final captures are field 7,715.
All 1,123 readback records / Z reads match through virtual GS field 9,044; the native renderer was
actually used. No reference, exclusions, checker, options or word waivers changed. Earlier gates
were deliberately stopped while live testing found additional fixes; gate-accepted is the final
complete run on the accepted runtime code.

The tested executable is preserved as build/port/r72/gate-accepted/frozen.exe, SHA-256
69520c8f0cbfd65f7ab1c42d66a5b701bf1b47d46c6381feec506eef314476e0. The same executable drove the
final local matrix and physical offline test. Source hashes and the incremental build description
are in build/port/r72/accepted-manifest.json. After testing, only an obsolete race_over comment was
corrected; generated instructions are unchanged. The exe is an incremental clean-tree link, not a
new all-unit plain clean build.


All requested fixes and the local test matrix are complete. Remaining scope: real separate-PC
latency testing and visual checks at aspect ratios other than 16:9 were not performed. An
unrecognised/stale host request now safely leaves online play after the bounded navigation deadline
instead of looping forever.
