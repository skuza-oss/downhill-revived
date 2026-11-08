# 177: native iOS control placement and button bindings

Status: current
Checked: 2026-11-07

The iOS source revision is 0.7.12, build 71206. Both the launcher's Controls &
Input page and the in-game Menu open Customize Controls.

## Fullscreen touch editing

Arrange and remap touch controls opens a fullscreen cover with a black
background. The canvas is the actual live safe content area of the application
window, with a shared 16-point ergonomic inset. It is not a scaled reference
preview. The system safe area accounts for the notch, Dynamic Island, corners
and home indicator; no device-name or physical-pixel lookup table is used.

TouchLayoutViewport and TouchControlSurface are shared by editing and gameplay.
The portrait game display stays above its controller-deck background, but the
control surface now spans the available safe window in both orientations. Users
can therefore place a portrait control above the old deck as well. New portrait
defaults keep the controls in the lower half. An external display uses the same
phone control surface while the TV continues to show the game.

Tap a control to select it, drag to move, and change its size on the floating
slider. Taps do not write positions. The tools dock opposite the selected
control and disappear during a drag. The Control menu selects hidden or
obscured slots. Control options opens an adaptive popover for action swaps,
visibility and precise position sliders. It never reduces the canvas size.

Rotate the device to edit its other orientation at the real window size.
Each orientation has its own draft. Done saves both drafts atomically and
closes the editor, including when no manual edit was made. Cancel confirms only
when the draft differs from the opening state. Reset confirms and changes only
the current orientation's draft. A write failure keeps the editor and draft.

## Editor refinement in iOS r6

Undo and Redo keep independent session histories for portrait and landscape.
Each holds up to 40 complete layout snapshots. A drag or continuous size/position
slider adjustment is one transaction. Remapping, visibility and reset are
individual edits. A new changed edit clears redo; a no-op does not. History
is not written to the user's configuration file. Done and Cancel keep their
previous save/discard semantics.

The floating tools can collapse to their action bar. Selecting a control
expands them again. The selected label shows its assigned action and whether
it is hidden. Undo/Redo remain available in the Control menu when collapsed.

The editable surface has a size-based identity, cancelling the old gesture
when the window resizes or rotates. Open transactions finish before viewport
changes, deactivation or changing selection; a captured orientation prevents
late move/slider callbacks from writing into the other layout. Drag selection
happens once per gesture, so repeated move events do not split undo history.
Gesture completion and cancellation both release the floating-tool state.

## Layout persistence and migration

The current file is touch_layouts_v2.json, version 2. A layout includes a
coordinateSpace (screen or legacy) and a baseScale. Positions remain fractions
of the shared control canvas; size is still a per-control scale.

If no v2 file exists, v1 is read without changing or deleting it. Layouts with
no coordinateSpace decode as legacy. Projection maps their old deck/overlay
positions into the shared canvas and compensates the reference fit through
baseScale, preserving physical button sizes. Centers are constrained at the
new safe boundaries. The portrait header has an explicit height so the old
deck projection uses the same screen geometry as the current game view.

Gameplay projects a legacy layout for display without writing it. Opening an
orientation in the editor projects its draft and opening baseline together,
so this automatic conversion alone does not trigger a discard warning.
Unvisited orientations may remain legacy in the v2 file until edited. Current
screen layouts are never projected twice. Done commits the converted layout;
Cancel leaves the saved configuration intact.

The older touch_controls_layout.json is also imported when its positions,
visible sizes or visibility were edited. Native default snapshots keep the
new defaults. Menu, stick, Cross, Triangle, START and R1 stay accessible.
Assignment swaps actions without changing positions or sizes; an essential
action moved to a hidden slot reveals that slot.

## Input and physical controllers

ControlsCustomizationView owns a counted editing scope. A separate scope spans
fullscreen presentation through onDismiss, keeping native input blocked while
the parent view disappears and reappears. GameController launcher callbacks
also ignore input during editing. Scene changes, menus, layout changes and
resizing clear native input and replace UIKit touch owners. Editing gestures
never send gameplay commands. The editor preserves the current pause state;
pause an offline race with START before opening it.

Physical-controller bindings still use the upstream dhd_settings parser and
rules through controller_settings.c, including main/alternate input, players
1/2, stick swap, inversion and essential bindings. Changes save immediately
to dhd.ini and queue under the native input lock for the game thread. Reset
confirms the selected player. Touch and physical deadzones default to 5 percent
for new preferences; explicit saved values keep their previous meaning.

## Validation and remaining checks

The test runner discovers 31 tests: 25 portable checks pass and six Swift
checks are skipped because macOS Swift/Combine are unavailable. Portable checks
cover the C binding adapter, native input suppression, pending input/settings,
analog response, project generation, localization, packaging and graphics math.

The six Swift checks compile production TouchLayout and TouchLayoutStore on
macOS. They cover essential swaps, corrupt values, save/reload and failed-write
isolation, legacy migration, viewport projection on phone/iPad window
sizes in both orientations, and undo/redo transactions and history bounds. The viewport check verifies coordinate bounds,
projection idempotence, physical sizes, known portrait positions and v1-to-v2
save/reload while retaining the old file. These checks are included but have
not run here. Localization and the native input reset are their only substitutes.

All nineteen Swift source files and the model/store test harness pass a
tree-sitter syntax scan. This is not Swift type checking, UIKit/SwiftUI rendering,
Foundation execution or an Xcode build. The full port check remains blocked by
missing LLVM/WASI/WABT and platform tools. The previously tracked root game
media also fails the upstream never-commit check; no new game media was added.

Before installation, run the iOS unittest suite on macOS and build with Xcode.
On iPhone and iPad, check the fullscreen canvas against gameplay, notch/home
indicator margins, controls near every edge, selecting without moving,
continuous size changes, both rotation directions, resize, Cancel, Done and
relaunch. Check drag cancellation, hidden controls, floating-tool access,
popover adaptation, physical-controller gating across nested presentation,
legacy layouts and external display connection/disconnection. Device testing,
a full build and game state traces remain pending. Also check one-step undo of
a complete drag and slider gesture, redo invalidation after a new edit, undo
of remapping/reset/visibility, separate orientation histories, tool collapse,
rapid resize/rotation during a drag and interruption while adjusting size.

## References

- https://developer.apple.com/documentation/swiftui/view/fullscreencover(ispresented:ondismiss:content:)
- https://developer.apple.com/documentation/swiftui/view/presentationcompactadaptation(_:)
- https://developer.apple.com/documentation/swiftui/geometryreader
- https://developer.apple.com/documentation/swiftui/gesturestate
- https://developer.apple.com/documentation/foundation/nsdata/writingoptions/atomic
- https://developer.apple.com/videos/play/wwdc2024/10085/
- https://github.com/skuza-oss/downhill-revived/blob/v0.7.12/port/render/settings/src/controls.rs
- https://github.com/skuza-oss/downhill-revived/blob/v0.7.12/port/render/include/dhd_settings.h

- https://developer.apple.com/documentation/swiftui/view/id(_:)
- https://developer.apple.com/documentation/swiftui/slider
