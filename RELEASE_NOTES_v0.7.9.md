# Downhill Revived for iOS v0.7.9 (Experimental)

**First public iOS build** | **Upstream base 0.7.9** | **Build 70902**

This release introduces an independent, native ARM64 iOS fork of [Downhill Revived by skuza-oss](https://github.com/skuza-oss/downhill-revived), with a SwiftUI launcher and Metal rendering.

### What's included

- Native iPhone/iPad UI with launcher, game settings and local disc image import.
- Configurable touch controls and supported physical gamepads.
- Rendering settings with an optional high-refresh target up to 120 FPS on suitable devices.
- External-display architecture for compatible HDMI / AirPlay configurations, including a default 1080p TV cap.
- Widescreen support and a multilingual launcher, including Brazilian Portuguese.

**Experimental status:** Hardware-dependent features have not been fully validated across iPhone/iPad models, receivers and controllers. Performance and compatibility may vary. No fixed 120 FPS guarantee.

### Requirements

- **iOS / iPadOS 17.0+**
- **ARM64 + Metal**
- Your own unmodified **Downhill Domination USA disc image (`.iso`, SCUS-97177)**
- Valid Apple code signing and provisioning for installation

### Download and installation warning

Asset: `DownhillRevived-iOS-0.7.9-build-70902-unsigned.ipa`

**This IPA is UNSIGNED. It is NOT installable directly on a regular iPhone or iPad.** Sign/build it appropriately with your Apple development team. See [`docs/BUILD_IPA_WITH_AI.md`](https://github.com/ribeirogustav/downhill-revived/blob/main/docs/BUILD_IPA_WITH_AI.md) and [`docs/INSTALLATION.md`](https://github.com/ribeirogustav/downhill-revived/blob/main/docs/INSTALLATION.md) in the repository.

**SHA-256:**

```text
4181572bd7b78ce87e956d52309e0879b3230f82a7511d325a6abd2b50adcc4c
```

This hash matches this exact unsigned build only. Do not use it for newly built or re-signed IPA files.

### Credits and legal

Original project and credits: **[skuza-oss/downhill-revived](https://github.com/skuza-oss/downhill-revived)**. This is an independent fan-made community adaptation, not an official upstream iOS release and not endorsed by the commercial game's rights holders. Covered source code is **GPL-3.0-or-later**, with third-party licences retained. No original game ISO, BIOS, executable, sound, textures or game content is included. Users must supply their own supported disc image.
