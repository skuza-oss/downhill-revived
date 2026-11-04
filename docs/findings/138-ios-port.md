# 138: Native iOS Port (iPhone 17 Pro Max / ARM64, v0.7.7)

Native iOS port of Downhill Revived for Apple Silicon (iPhone 17 Pro Max / A19 Pro, iOS 17.0+).

## Architecture

The iOS port builds on the portable 64-bit game core (finding 081) and native Metal rendering (finding 099):

- **Core**: Ahead-of-time (AOT) wasm2c translated module compiled into native ARM64 code (`-arch arm64 -miphoneos-version-min=17.0 -mcpu=apple-m1`). No JIT is used, adhering to iOS sandbox and App Store guidelines. Bounds-check branches are eliminated via zero-cost memory protection mirrors and `-DWASM_RT_SKIP_BOUNDS_CHECKS=1`.
- **Rendering**: Native Metal backend using `wgpu` (`aarch64-apple-ios`, `libdhd_render.a`). Renders into an SDL Metal layer (`CAMetalLayer`, `MTLPixelFormatBGRA8Unorm`). Triple-buffered with `presentsWithTransaction = NO` and `framebufferOnly = YES` for Apple GPU lossless compression and minimal latency.
- **Display**: Tailored for the 6.9-inch OLED display of the iPhone 17 Pro Max (2868 x 1320 native pixels, 460 ppi, ProMotion 120 Hz). Hor+ widescreen framing with fixed-aspect HUD. Dynamic Island and rounded corner safe areas respected.
- **Launcher**: Native SwiftUI application with fully responsive adaptive layout (supporting both Portrait and Landscape orientations, safe areas, Dynamic Island, and ProMotion refresh rates). Provides Home, Disc Manager, Graphics Settings, Controls Customizer, Game Options, Online Lobby, and About screens.
- **App Icon**: Full Xcode asset catalog (`AppIcon.xcassets`) supporting all iOS and iPadOS icon specifications compiled via `actool` into `Assets.car`.
- **Touch Controls**: Multitouch virtual analog stick and PlayStation button layout (Kenney CC0 transparent assets + vector symbols). Supports floating or fixed stick, deadzone, sensitivity, and haptic feedback.
- **Physical Gamepads**: DualSense, DualShock 4, Xbox Wireless, and MFi controllers automatically recognized through SDL3 / GameController framework. Optional auto-hiding of virtual controls when physical pads are active.
- **Storage & Saves**: Fully sandboxed under `Documents/`. The original USA PS2 disc image (`SCUS-97177.iso`) is streamed into local storage via `UIDocumentPickerViewController` with identifier validation. Save files and `dhd.ini` persist across restarts.

## Directory Structure

All iOS-specific code resides in `port/ios/`:
- `port/ios/launcher/`: SwiftUI screens (`HomeView.swift`, `DiscManagementView.swift`, `GraphicsSettingsView.swift`, `ControlSettingsView.swift`, `GameSettingsView.swift`, `OnlineView.swift`, `AboutView.swift`, `TouchControlsOverlayView.swift`, `LauncherModel.swift`, `LauncherApp.swift`).
- `port/ios/input/`: Touchscreen input system (`touch_input.h`, `touch_input.m`).
- `port/ios/host/`: Platform entry, sandboxing, ISO streaming copy, and lifecycle (`ios_main.h`, `ios_main.m`).
- `port/ios/render/`: Metal layer configuration and display synchronization (`metal_bridge.h`, `metal_bridge.m`).
- `port/ios/assets/touch-controls/`: Transparent Kenney CC0 vector and raster sprites, PlayStation symbol icons, and license.
- `port/ios/project/`: Application `Info.plist` with ProMotion 120Hz, landscape orientations, and document types.
- `port/ios/build_ios.py`: Automation script to prepare the project bundle and trigger `xcodebuild`.

## Building and Deployment

On Windows:
```powershell
python port/ios/build_ios.py --bundle-only
```
This generates the full project bundle in `build/ios/project/`.

On macOS (Xcode 15+):
```bash
python port/ios/build_ios.py --release
```
Or open `build/ios/project/` in Xcode, select your Development Team in Signing & Capabilities, and press **Run** targeting an iPhone 17 Pro Max or generic iOS Device.

## Fullscreen & Responsive Launcher Polish

- **Native Resolution & Elimination of Letterboxing**: iOS requires either `UILaunchStoryboardName` or `UILaunchScreen` in `Info.plist`. Without them, iOS runs applications in legacy letterboxed mode (shrinking viewport and creating black pillar borders). Added native `LaunchScreen.storyboard`, `<key>UILaunchScreen</key><dict/>`, and `<key>UIRequiresFullScreen</key><true/>`.
- **Adaptive Proportional Layout**: Launcher screens (`HomeView`, `DiscManagementView`, `GraphicsSettingsView`, etc.) adapt intelligently between Portrait (vertical scrollable stack with 2-column action card grid) and Landscape (proportional two-column hero + 3-column action card grid). Text elements and version badges use fixed sizes and minimum scale factors to prevent vertical word wrapping.
- **Dynamic Library Signing (dyld crash resolution)**: Xcode 16+ generates nested debug dylibs (`Downhill Revived.debug.dylib` and `__preview.dylib`). Standard `codesign --deep` on the outer `.app` fails to sign arbitrary loose dylibs placed in the root of the bundle, causing `dyld` fatal termination on launch (`missing code signature in Downhill Revived.debug.dylib`). `build_ios.py` traverses and signs all nested `.dylib` and `.framework` files individually first, followed by the outer bundle with entitlements.
- **ISO 9660 Parser & Documents Auto-Detection**: Replaced flawed 1MB-stride sector scanner with standard ISO 9660 Primary Volume Descriptor (LSN 16) and Root Directory `SYSTEM.CNF` reader matching `host.c` (`disc_problem`). Added continuous 64KB block streaming fallback for raw/bin images. Added automatic sandbox scanning for any `.iso`/`.bin` placed directly into the app folder via the iOS Files app, in-place validation without redundant 3-4 GB copies, and detailed disc information (`disc_id`, filename, size in GB).
- **Handheld Game Boy Console Mode & Session Lifecycle**: Added native `MetalGameView` (`CAMetalLayer`, ProMotion `CADisplayLink`, triple-buffered). Portrait gameplay provides an authentic handheld console layout: top 45% houses an aspect-fit 16:9 game display unblocked by thumbs, while the bottom 55% houses a dedicated physical-style controller deck with analog stick, PlayStation action diamond (✕, ○, □, △), shoulder triggers (L1/L2, R1/R2), Select/Start, and haptic feedback. Landscape provides full-screen Hor+ panoramic display with translucent overlay buttons.
- **Landscape Screen Rotation Fix**: Persistent singleton `MetalGameView` inside `MetalHostViewController` reparented between SwiftUI orientation transitions, preserving the `CAMetalLayer` and `wgpu` surface binding without black-screen tearing.
- **Apple Silicon Performance Tuning**: Elimination of software bounds check branches via `prepare_wasm2c.py`, `-mcpu=apple-m1` tuning, `QOS_CLASS_USER_INTERACTIVE` P-core priority for core threads, elimination of dummy SDL window polls, and full Release optimizations (`-Owholemodule` / `-O3`).





