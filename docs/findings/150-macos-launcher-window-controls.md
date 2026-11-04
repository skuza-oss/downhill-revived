# 150: Native macOS launcher window controls

Status: current
Checked: 2026-11-04

Contributed in PR #39 (numbered 145 there; renumbered at the merge, 145 is the Android surface fix).

The launcher previously disabled decorations on every platform and drew its own minimize and
close buttons. On macOS it now enables decorations with Tauri's `TitleBarStyle::Overlay` and
`hidden_title`, keeping the existing 32-pixel drag region under the native window controls.
The snapshot tells the UI to hide the HTML buttons on macOS. The other platforms keep their
existing configuration and HTML controls. The fixed-size window remains non-maximizable.

The change uses the existing main window configuration in the generated Tauri context, with a
macOS compile-time guard; development and release builds follow the same path.

Reference: [Tauri window configuration](https://v2.tauri.app/reference/config/#windowconfig),
specifically `decorations`, `titleBarStyle` and `hiddenTitle`. No third-party code was copied.

Validation: `cargo check --offline` on macOS and `node --check` on the UI passed, as did
`git diff --check`. Cargo reports an existing unused Windows uninstall constant. The window's
appearance has not been checked in a running launcher.
