# 176: iOS 0.7.12 and progressive analog input

Status: partly superseded by 177 (native controls UI and deadzone defaults)
Checked: 2026-11-07

The iOS fork at `46261424` is merged with the complete upstream `v0.7.12`
tag, commit `610a0bf95011a7611aebd488a3db60494b2d6c1f`. The iOS source version
is 0.7.12, build 71201. A device-built IPA is still pending.

## Integration

The upstream merge brings the shared renderer optimizations of 0.7.10, the
wide-screen inventory shadow fix of 0.7.11, and 0.7.12's controller layouts,
settings tabs, startup frame guard and network changes. The iOS launcher,
external display, live graphics and input integrations continue through their
existing platform layer. No state-trace equivalence or iPhone speedup was
measured in this environment.

Conflicts were resolved in the fork README, state document, host input,
widescreen, build target table and vendor patch notes. Host input uses the new
raw-controller/layout path once and then merges native iOS touches. The shadow
vertex now uses upstream's 16:9 limit. Live aspect and the existing iOS inventory
cap remain. The iOS findings were moved to 167 through 175 to avoid duplicate
numbers with the imported findings. Existing published 0.7.9 IPA references keep
their original filenames and hash.

## Analog input

Finding 134 documents the retail input problem. `func_00220AE0` transforms raw
bytes, riding actions square the result, and `func_0012DEB0` treats magnitude 1
as a full digital press. Mapping a normalized stick directly to centre + 127*x
therefore produces full steering in the raw 174 through 177 / 77 through 80
bands. Changing only the touch deadzone cannot eliminate these bands.

Touch now applies its existing radial deadzone and sensitivity once, then uses
`port/hle/stick.h` with zero additional deadzone. This is the same safe encoding
as upstream's physical-controller numeric deadzone. Both portrait and landscape
call the same native input function. Fixed-coordinate touch-down also runs the
normalization used on movement. Neutral touch preserves a real controller's
axes, and neither stick generates digital d-pad buttons.

The physical-controller deadzone defaults to 15 percent. Its new native slider
ranges from 0 to 40 percent and is translated in all eight launcher languages.
The preference is saved independently from the touch deadzone. UI updates queue
under the native input lock. The game thread applies them to the existing
`DHD_STICK_DEAD_ZONE` setting at its next touch merge, and the next controller
poll uses them. This avoids UI-thread writes to the engine's environment.
The engine's own controls page continues to update its setting until the
native slider changes again. The next application launch restores the native
saved preference. Stick inversion, swapping, bindings and the retail response
curve follow the upstream controller path.

The build copies the shared stick header into the generated Xcode input folder.
The broad `assets/` ignore rule had hidden the iOS artwork folder in the fork.
The rule now targets only the root game-data folder. The iOS artwork was
recovered from the previous source package. The original Apple team default
was removed. Xcode uses `--team` or `DHD_APPLE_TEAM` when supplied.

## Validation

`python3 -m unittest discover -s port/ios/tests -v` passes 24 tests.
The input fixture compiles production touch handlers, `gamepad_apply`, shared
stick encoding and the production retail-axis decoder from `keyboard.c`.
It sweeps 65,536 signed positions for each of touch and physical input at the
default deadzone, checking monotonic steering, neutral input, low output just
past the zone, full travel and absence of magnitude 1. It also checks queued
deadzone changes, zero/NaN handling, fixed touch-down, short taps, releases,
button pressure and preservation of physical axes. The remaining tests cover
display rotation, inventory transitions, localization, packaging and the clock.
UIKit, OS locks, haptics and environment writes are substituted in this fixture.

`python3 port/tools/test_patch_guard.py` passes. Python sources compile. Native
Linux syntax checks pass for `host.c`, `gamepad.c`, `gpu_rust.c` and `present.c`
with the pinned SDL3 headers. The GPU source also passes a syntax check with
the iOS renderer code enabled and its portable Metal bridge declarations.
These checks do not compile Objective-C, Swift, Rust or the Apple platform API.

`python3 tools/check.py port --keep-going` could not complete. The full tree
check reports the fork's already-tracked root artwork and gameplay captures.
The build/runtime/network suites require LLVM, WASI, WABT, Rust and platform
toolchains absent here. No test disc, generated core or state traces are
available. Full compilation, code signing, real controller behavior, Metal,
AirPlay and an offline race must be checked on a Mac and iPhone.

## Device acceptance

1. Rebuild the core and application from this checkout on macOS. Reusing a
   0.7.9 wasm or IPA does not include the update.
2. In Controls & Input, start with both deadzones at 15 percent and touch
   sensitivity at 1.0. Test a slow movement from the centre to full travel in
   each direction, then diagonals, with physical input and both touch layouts.
3. Confirm idle drift is absent, steering grows with travel, full travel still
   reaches full steering, and the d-pad and menu navigation remain usable.
4. Repeat after disconnecting a controller, rotating the phone, pausing,
   backgrounding and foregrounding. Verify the rider in the inventory and
   return to the selected race aspect, including on an external display.

## Sources

- https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.10
- https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.11
- https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.12
- https://github.com/skuza-oss/downhill-revived/blob/v0.7.12/port/hle/stick.h
- https://github.com/skuza-oss/downhill-revived/blob/v0.7.12/docs/findings/134-controllers.md
