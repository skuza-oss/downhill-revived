# 168: iOS performance review

Status: partly superseded by 176 (0.7.12 integration and analog input)
Checked: 2026-11-07

Baseline: the user's DownhillRevived-iOS-v0.7.7.zip. Upstream reference:
`01f8d2bd79908f5eadd2d784de6edbea32ade1a2`.

Imported finding 139 as a coordinated change, including its clock helpers, source
patches, counter fixes, audio tick accounting, input handling and unit programs.
Also imported its surface latency change and the graphics timeline test stub.
The matching `src/` and `include/` trees are unchanged.

The iOS launcher now establishes a 1x/60 baseline once. Internal Auto means a
thermal-aware choice at startup; it does not pretend that existing colour/depth
surfaces can be resized by changing a variable. A separate final presentation cap
is 720/900/1080 pixels on the short side by profile. The one-second thermal/low
power policy can lower this output cap to 720 without invalidating internal GS
surfaces. Explicit 120 simulation steps remain opt-in and increase CPU demand.

The native present hook records monotonic submission intervals into a 240-sample
ring. Its mutex protects only an insertion or snapshot copy; sorting for p99
runs on the reader. Long frame intervals remain in the statistics. UIApplication
inactivity explicitly resets them. These are not GPU timestamps or physical
display scanout measurements.

CADisplayLink supplies a display refresh hint and observed callback period. It
never draws, acquires a drawable or presents. Rust remains the sole presenter and
owns drawableSize after initialization. The UI publishes desired pixel sizes and
requests surface reconfiguration through the existing resize handoff. UIKit calls
stay on the main queue. The SDL host loop still runs on the existing auxiliary
thread; changing its threading model was outside this revision.

The native game state is atomic; duplicate starts return an error. Inactivity is
observed at the game thread's message boundary, where the existing renderer reset
operation drains/drops the surface. The UI thread does not wait for game work.
On return the same retained Metal layer is rebound. Device testing of lock/unlock,
calls, Control Center, rotation and background/foreground remains required.

build_ios.py now builds the source module, host and Rust library before copying
the archive; --wasm is an explicit alternate input. The source path restores
missing split assets from the user's local executable. DerivedData is local to
this build, the output app path is deterministic and IPA packaging removes the
previous zip before adding files. Existing local signing behavior is retained.

Validation: seven portable test groups passed on Linux, including 22 executions
of production fps.c with the upstream test_fps.c fixture, native frame-statistics
checks and mocked build ordering/failure checks. All 226 source patch anchors
matched. Python sources compile. Neither Xcode/Swift/Objective-C nor the Rust iOS
target was built here. No iPhone frame-rate gain is claimed.

Sources: existing project and upstream finding 139; Apple's primary ProMotion
guidance at
https://developer.apple.com/documentation/quartzcore/optimizing-iphone-and-ipad-apps-to-support-promotion-displays .
