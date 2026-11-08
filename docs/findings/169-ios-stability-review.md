# 169: iOS stability review 2

Status: partly superseded by 176 (0.7.12 integration and analog input)
Checked: 2026-11-07

Based on the local r1 source, retaining its pinned upstream delta-time changes.

## Corrections

The SwiftUI App now forwards scenePhase transitions. UIApplicationDelegate alone
does not receive all scene-based lifecycle callbacks. Per-view input state and
native button/stick state are released on inactivity, rotation or menu entry.
The shared Metal view supports a single scene, declared in Info.plist.

Native/exact presentation checks the iOS active flag before acquiring a drawable.
The vendored Metal HAL leaves allowsNextDrawableTimeout enabled on iOS so an
already-started acquire has a bounded wait if the compositor stops. Other Apple
targets retain the previous override. The render-cache key already hashes vendor
sources. This does not prove background/foreground correctness on the device.

Width and height are one atomic 64-bit snapshot. UIScreen capability reads are
performed on the main thread and cached for native workers. Zero-sized layouts
are ignored, rather than becoming a spurious 1x1 surface. Thermal recovery waits
30 seconds before increasing the final presentation cap.

The analog-stick mapping is a portable shared helper. Neutral input remains zero
with zero deadzone; invalid/degenerated input cannot produce division by zero,
NaN or an unbounded stick. Config reads return a thread-local snapshot; config
loading validates JSON types and ranges. JSON construction and file writes no
longer hold the gamepad lock. Controls are initialized before launcher preferences.

Disc mutators and game startup use one operation mutex. An active game cannot
import/delete its disc. Startup directories are initialized by pthread_once and
the original ini parser reads the stored path. A staged ISO replaces its target
using POSIX rename directly; a failed rename leaves the previous target intact.
Swift allows app-local URLs without assuming that false from
startAccessingSecurityScopedResource means inaccessible.

Xcode now performs automatic signing with the configured Team. The script does
not select a certificate by name or modify a signed app with actool. Signed
output requires an embedded profile and successful codesign verification before
packaging/install. Account provisioning changes require the explicit CLI flag.
Unsigned output is explicitly selected and cannot be installed by the script.

IPA packaging writes a new temporary archive, preserves executable permissions
and symlinks, then replaces the destination atomically. Missing tools or ambiguous
device selection fail before the expensive core build. Stale project libraries
are removed when preparing a source-only project.

## Validation

18 portable tests passed on Linux. Tests include actual native C execution of
the stick math (40,401 input pairs plus exceptional values), concurrent frame
statistics, real ZIP permission/symlink round trips and failure preservation.
Build/signing/device operations are mocked, including verification failures and
missing provisioning profiles. The unchanged production clock suite still runs
22 rate/mode cases. Python syntax and shared Linux C presentation paths were
checked; matching source patch anchors are unchanged.

Not run: Xcode, Swift/Objective-C compilation, Rust iOS compilation, real signing,
installation, Metal rendering, physical input/lifecycle tests or an FPS benchmark.
The app build number is 70702; its game/network version remains 0.7.7.

## Primary sources

- https://developer.apple.com/documentation/swiftui/scenephase
- https://developer.apple.com/documentation/uikit/uiscenedelegate/scenewillresignactive(_:)
- https://developer.apple.com/documentation/quartzcore/cametallayer/allowsnextdrawabletimeout
- https://developer.apple.com/documentation/xcode/distributing-your-app-to-registered-devices
- Existing project and upstream source at 01f8d2bd79908f5eadd2d784de6edbea32ade1a2.
