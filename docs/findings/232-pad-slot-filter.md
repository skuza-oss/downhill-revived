# 232: Keep non-gamepads out of controller slots

Status: current
Checked: 2026-11-14

Branch `fix/pad-slot-filter` from `main` 6d9d51d1, started 2026-11-14.

## Goal and acceptance

Filter devices before assigning slots in the shared SDL host. The first deliberate press can
swap a later pad into silent player 1 in single-player contexts. Unit tests, quick/port/docs
checks, Android native build, boot and race traces against the base. No phone use or benchmarks.

## Progress

- [x] Reserve the lane (22f64401); read findings 059, 134, 157 and 163.
- [x] Filter and skipped-device log; first-press swap and synthetic description/policy tests.
- [x] Quick/docs checks, Android native library, baseline and changed boot/race traces.
- [x] Final port gate, including device/claim units, renderer, network and static checks.

## Decisions

- Cause: `sdl_host.c` forwards joystick-added events to `gamepad_device`; `gamepad.c:attach`
  previously opened every joystick into the first free slot, including partial Android HIDs.
- Accept known SDL types (including the real type before mapping overrides) and named mappings.
  Otherwise require all four mapped face buttons plus all four d-pad directions or both left
  axes. Unmapped joysticks need four buttons and a hat or two axes for our default layout.
  Known non-pad joystick types (wheel, flight stick, etc.) fail the generic test.
  No name blacklist, keyboard-source veto, input-activity timeout or idle-device eviction.
- SDL exposes type, real type, mapping text, mapped button/axis presence and raw counts. Mapping
  priority/provenance is private: the pinned SDL uses `*` as its generic automatic mapping name;
  named database/user mappings bypass the capability test, preserving unusual/accessibility pads.
  Full generic Bluetooth pads pass on capabilities. Sparse mapped devices use mapped presence,
  never raw button counts (Android's count is the highest supported button index plus one).
- A skipped ID is logged once per connection, also across startup/add/remap notifications;
  remapping retries admission. Its log includes types, button/axis masks and raw counts.
- Owner addition: rising face/START/d-pad edges only can claim player 1. Player 1 wins simultaneous
  presses; a claim fixes order for the run, including after disconnect. A swap moves both device
  handles, input snapshots, icon kinds and SDL player indices. Bindings stay with slots: the
  promoted pad immediately uses Controller 1's custom layout; the displaced pad uses its new
  slot's layout (Controller 2, or defaults for 3/4).
- The game thread publishes the setup's local human count (`D_004B25A0 + 0x288`) before reading
  a pad; SDL reads only that atomic flag. Two or more disables swaps in split setup and racing;
  online has one local human. Axis noise, resting triggers and sensors cannot claim a slot.
- Issue #80 is not strongly explained by slot assignment: typing clears keyboard/text state,
  not controller slots, and remapping keeps an already mapped pad's slot. An Android input-device
  removal/re-add around an IME could reorder devices, so a related case is possible; not chased.

Research (read, no external code copied):

- [SDL Android Java classification](https://github.com/libsdl-org/SDL/blob/release-3.4.18/android-project/app/src/main/java/org/libsdl/app/SDLControllerManager.java):
  accepts joystick, d-pad or gamepad sources; nativeAddJoystick forwards masks/counts, not sources.
- [SDL mapping generation](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/joystick/SDL_gamepad.c):
  Android guesses a wildcard mapping with any face button and can map BACK to east for remotes.
- [SDL Android joystick counts](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/joystick/android/SDL_sysjoystick.c).
- [PPSSPP input classification](https://github.com/hrydgard/ppsspp/blob/master/android/src/org/ppsspp/ppsspp/InputDeviceState.java):
  distinguishes keyboard/pointer sources but merges gamepads into PAD0; that policy cannot preserve
  this game's local multiplayer slots, so it was not adopted.

## Surprises

One final race run differed from the original base at flyover field 2641 (RAM only, one
field). Fresh base/final runs with dumps at 2641 matched all 5000 fields; the earlier changed
build also matched all 5000. The isolated difference was not reproduced or attributed; logs,
traces and diagnostic dumps stay in `build/`. No comparator/mask changes.

The player's log has no capability dump: Xiaomi Consumer and S5 descriptions in the tests are
synthetic sparse-HID fixtures, not measurements from that tablet. The phone check must confirm
both skipped lines, DS4 as player 1 and working menus/race controls; test USB/Bluetooth, hotplug,
room-code typing, two pads' first press, fixed order after player 1 presses and split-screen order.

## Result

`python tools/check.py quick -j 6`, `python tools/check.py port -j 6`, and
`python tools/check.py docs`: PASS.
`python port/tools/build_host64.py --target android-arm64 --jobs 6`: PASS (native library).
Boot 600/600 and race 5000/5000 fields equal to the locally built 6d9d51d1 base, with
`SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0`; no test depends on a connected controller.
No code/build acceptance work remains; phone validation is listed above. The lead closes this
finding at merge.

Checked:
- 0.8.2 gate on a55c3aee: `check.py port -j 6` and `check.py docs` OK; all seven state traces
  equal `main-524948ba-ref-v3` (the one differing race field of the lane's last run did not
  appear; none of the seven runs differed).
- On the Mi 11i with no controller attached: no `controller:` line, so no phantom device took a
  player's place. Not checked here: a real tablet with media keys, two pads' first press,
  hotplug and split-screen order (unit tests only).
