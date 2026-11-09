# 183: native iOS platform port (port/ios)

Status: current
Checked: 2026-11-08

Branch `feat/port-ios` from `main` db3702d, started 2026-11-08.

## Goal and acceptance

Add native Apple iOS / iPadOS platform support under `port/ios/`:
1. Isolated platform implementation under `port/ios/` (host, input, launcher, metal bridge, build script).
2. Clean separation without platform-specific `#ifdef` branches inside shared engine files.
3. Native SwiftUI launcher with disc image picker, graphics options and touch/gamepad customization.
4. Smooth analog steering calibration and ergonomic virtual touch joystick.
5. Building documentation in `docs/BUILDING.md`.

Acceptance:
- `port/ios/` contains the complete standalone port;
- `python tools/check.py docs` passes with 0 errors;
- `python tools/check_tree.py` passes with 0 errors;
- Windows, Linux and macOS desktop builds remain 100% unchanged.

## Structure of port/ios

- `port/ios/host/`: C host bridge, display lifecycle, controller bindings.
- `port/ios/input/`: analog stick math, touch input translation.
- `port/ios/launcher/`: SwiftUI frontend views and layout store.
- `port/ios/render/`: Metal bridge and static linking specifications.
- `port/ios/build_ios.py`: Xcode project generation and compilation script.
- `port/ios/package_unsigned.py`: unsigned development IPA packaging.
- `port/ios/tests/`: portable python unit tests for input and layout math.

## Result

- Added `port/ios/` standalone platform directory.
- Added iOS compilation section in `docs/BUILDING.md`.
