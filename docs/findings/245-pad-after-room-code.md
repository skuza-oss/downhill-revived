# 245: Bluetooth pad after Android room-code entry

Status: current
Checked: 2026-11-14

Branch `fix/pad-after-room-code` from local main bf4f5ab6, started 2026-11-14.

## Goal and acceptance

Issue #80: reason through IME/text-entry and controller ownership, reproduce the
device/input sequence in tests, fix only evidenced defects. Gates: quick, port,
docs, Android native/Java and units; base-equal boot/race traces if HLE changes.
No phone, Mac, push, external post, visible game window or timing benchmark.

## Progress

- [x] Read issue and all five comments: Android, Bluetooth, A/Y survive after
  keyboard entry. No log attachment or controller model/version is provided.
- [x] Trace SDL/Java/text and slot transitions; reproduce the focus-routing gap.
- [x] Regression: old route receives 2/6 joystick motions, new route 6/6.
- [x] Complete quick/port/docs, Android builds/units and base-equal state traces.

## Decisions

232 postdates the report. Its sparse-HID filter already prevents an incidental
keyboard-like joystick from taking a slot. Complete generic Bluetooth pads still
qualify, including devices also reporting keyboard sources. Its intentional-press
claim rules neither cause nor fix the focus gap; preserve them.

The shared menu ends text input on leaving the name/code pages and on closing the
popup. `present.c` starts/stops SDL text input on the SDL thread and logs both
transitions. SDL's hide command already requests surface focus. No evidence
justifies a second stop/focus reset or a new controller mapping.

SDL 3.4.18's ShowTextInputTask focuses SDLDummyEdit. Both it and SDLSurface route
keys through SDLActivity.handleKeyEvent, which prioritizes joystick-capable device
keys. Only SDLSurface installs the generic-motion listener: with editor focus,
sticks, hat d-pads and triggers can disappear while buttons still work. The game
activity now routes joystick-class motion to that same SDL listener before view
focus dispatch; consumed events return once, other input follows normal dispatch.
There is one SDL motion/mapping path, shared by all Android controllers.

For the remaining hardware uncertainty, controller attach/remove/remap and first
claim logs now include SDL instance IDs and current player owners. A Java-to-native
notice records motion routed while the surface lacks focus; existing text on/off
logs remain. No typed room code is logged by these additions.

## Surprises

The issue's A/Y-only symptom does not prove which controls the unidentified pad
reports as axes versus keys, or whether its IME consumes keys before Activity
dispatch. The routing defect is demonstrable without that phone; complete symptom
resolution is not. No slot theft or stuck text-input state was reproduced.

Primary sources inspected (no implementation copied):

- [Android Activity generic-motion dispatch](https://developer.android.com/reference/android/app/Activity#dispatchGenericMotionEvent(android.view.MotionEvent)).
- SDL 3.4.18 [SDLDummyEdit](https://github.com/libsdl-org/SDL/blob/release-3.4.18/android-project/app/src/main/java/org/libsdl/app/SDLDummyEdit.java),
  [SDLSurface](https://github.com/libsdl-org/SDL/blob/release-3.4.18/android-project/app/src/main/java/org/libsdl/app/SDLSurface.java)
  and [SDLActivity](https://github.com/libsdl-org/SDL/blob/release-3.4.18/android-project/app/src/main/java/org/libsdl/app/SDLActivity.java).
- [SDL issue 9027](https://github.com/libsdl-org/SDL/issues/9027): related Android
  editor/surface focus behavior, not evidence that #80 has the same cause.

## Result

`test_pad_motion.py` runs the production router with minimal Android event/view
fixtures on the JVM: surface -> editor -> surface -> editor, six joystick motions,
keyboard/mouse/touch fallback, no duplicate delivery, absent surface and unhandled
SDL events. The old surface-only route fails at 2/6; the new route passes at 6/6.
This tests dispatch, not a real Android IME. It runs through `test_disc_image.py`.
The C mapping test adds a synthetic complete pad -> sparse keyboard device -> text
entry -> close -> pad -> reconnect sequence. Player 1 remains claimed, the sparse
device is rejected, and buttons/stick still map. No device was used or measured.

Checks: `check.py quick -j 6`, `check.py port -j 6`, `check.py docs`, Android native library
(`build_host64.py --target android-arm64 --jobs 6`), debug APK/Java compilation
(`build_apk.py --offline --jobs 6`), `test_build_apk.py` and JVM/JNI tests
(`test_disc_image.py`, including the motion test) pass. Build slots 2, compile jobs
6. The JNI test uses the documented header override for a runtime-only JDK.

Against the executable built from bf4f5ab6 before the code changes, `state_trace_run.py`
boot and race followed by `state_diff.py` pass: all 600 boot rows and all 5000 race
rows equal, unchanged trace masks/lists. These are headless deterministic checks,
not timing benchmarks. Logs/traces are in the scratch folder.

### Lead's phone check

On the Mi 11i with a Bluetooth pad, keep the player's settings. Record pad model,
mode and keyboard/IME name. Press the pad to claim player 1; verify every face
button, d-pad, both sticks and triggers before opening ONLINE. Open JOIN BY CODE,
type a code, try pad motion while typing, then submit using keyboard Enter; try
again submitting with the pad. Test invalid-code retry, Android Back cancellation,
and a valid room through rider selection, READY and racing. Repeat open/type/close
three times. Background/return and Bluetooth disconnect/reconnect must preserve
usable player 1; if a second pad is available, check player order too. Save the
whole dhd.log: text on/off, instance IDs/owners/claim and the focus-routing notice.
No phone/controller interaction was performed in this lane.

### Draft reporter reply (lead posts)

We fixed a path where opening the Android keyboard could stop controller sticks,
d-pad hats and triggers reaching the game. Please try the updated build: enter a
room code, join or cancel with Android Back, then test every controller button and
stick. If anything still stops, please attach the full dhd.log and tell us the pad
model/mode, phone, keyboard app, and whether closing the keyboard or restarting the
game restores it.

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- Android unit tests green; no Bluetooth pad was run on a phone.
