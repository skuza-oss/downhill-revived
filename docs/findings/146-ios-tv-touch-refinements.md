# 142: iOS external display and immediate touch input

The supplied working iOS archive is the baseline. No matching sources under src/
were changed. The app identifies as 0.7.7 (iOS r3), build 70703.

## External presentation

A noninteractive UIWindowScene owns a black window with only the game's shared
CAMetalLayer. The phone owns all launcher/menu/touch UI. The same persistent Metal
view moves between windows on connection/disconnection; no second renderer or game
session is created. The bridge reads the screen from the actual window, publishes
coherent drawable dimensions and moves its cadence request to that screen.

The scene manifest handles the legacy external role. A persistent phone controller
also registers the documented UISceneAccessory on iOS 27 using public Objective-C
selectors, checked for availability at runtime. This permits building with earlier
SDKs while using the newer API when the OS provides it.

The default external drawable has at most 1080 rows and never upscales a smaller
mode. Width follows the display's shape: 1920x1080 at 16:9, 2560x1080 for a
2560x1080 ultrawide display. A prelaunch graphics toggle lifts the external limit;
phone profile limits and internal rendering scale remain separate. The delegate
selects the largest advertised mode within the cap with the preferred aspect,
or uses the preferred mode with a bounded drawable when the adapter has no such
mode. HDMI signaling and native modes remain subject to the adapter and display.
Changing that setting before starting also reconfigures an already attached TV.

The native host exposes the active aspect through the existing monitor service.
The iOS-only DHD_IOS_DISPLAY_AUTO path updates the FOV/HUD words and cached view
focal lengths on the game thread, then reapplies presentation options. Expected
old words guard the patch; replacement values derive from original constants to
avoid accumulating error. Display reads here use atomics, not UIKit or an SDL
round trip per field. Other platform paths remain opted out.

## Input and visibility

The last active input controls visibility. Any phone touch reveals controls and
the menu, even with a controller connected. Meaningful gamepad presses/axis motion
hide them when auto-hide is enabled; analog noise near neutral is ignored. Merely
connecting a controller does not suppress subsequent touch use. The phone's TV UI
starts black. After four seconds without touch, the menu hides; the controls also
hide on TV or with a gamepad. A held finger prevents the timeout. Without either,
the ordinary touch deck remains available. Portrait layout stays stable when
controls hide, avoiding a moving game viewport during input changes.

Native UIView touch handlers write button/stick samples immediately, before visual
state updates and haptic feedback. Cancellation releases inputs on inactivity,
menu entry, hiding, rotation or removal. A passive window observer sees reveal
and activity touches without delaying or cancelling the controls. Short button
taps remain pending until one game pad poll, even when pressed and released
between fields. Removed the synthetic 1.2 second CROSS hold previously introduced
on double taps: sprint now follows the game's actual button input.

The iOS presentation default is DHD_FRAME_LATENCY=1 (explicit diagnostic overrides
are retained) and the initial Metal layer uses two drawables. The Rust Metal HAL
then derives its drawable count from the same requested latency. This reduces
allowed buffering; it is not a measured claim about end-to-end latency.

## Build and validation

The default iOS build always invokes the source module build before the native
host. Previously, a supplied cached wasm silently bypassed HLE source changes.
An explicit --wasm still means the user supplied a compatible module. Source-only
output omits the supplied old build/ios/DownhillRevived.ipa to prevent installing
an obsolete binary. Existing source/assets and other supplied files are preserved.

22 portable tests pass with GCC on Linux. Added executable checks cover output
aspect/caps, immediate down/up and short taps, simultaneous buttons and steering,
release/reset, 1,000 output round trips through 21:9/4:3/16:9, unique cached view
adjustment, invalid aspect rejection and rebuilding an existing cached module.
Updated the existing device-selection test to mock the actual JSON devicectl
interface used in the supplied build script. Existing clock tests still cover
11 rates in two modes. UIKit/Metal/Swift compilation, actual signing, HDMI hotplug,
multitouch, and measured input latency have not been run here; Xcode/iPhone are
not available in this environment. Follow the device checklist in the root guide.

## Primary references

- Apple, Presenting content on a connected display:
  https://developer.apple.com/documentation/uikit/presenting-content-on-a-connected-display
- Apple, UISceneAccessory and its external-display factory:
  https://developer.apple.com/documentation/uikit/uisceneaccessory
- Apple, registerSceneAccessory:
  https://developer.apple.com/documentation/uikit/uiviewcontroller/registersceneaccessory(_:)
- Apple, UIResponder touchesBegan and UIGestureRecognizer delaysTouchesBegan:
  https://developer.apple.com/documentation/uikit/uiresponder/touchesbegan(_:with:)
  https://developer.apple.com/documentation/uikit/uigesturerecognizer/delaystouchesbegan
- SDL, current Apple gamepad backend (input state polling was retained):
  https://github.com/libsdl-org/SDL/blob/main/src/joystick/apple/SDL_mfijoystick.m

No third-party code was copied into this change.
