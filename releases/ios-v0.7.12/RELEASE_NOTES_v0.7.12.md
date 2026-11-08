# Downhill Revived for iOS v0.7.12

**Native iOS Release** | **Upstream Base 0.7.12** | **Build 71206**

This release brings an updated native ARM64 iOS version of [Downhill Revived by skuza-oss](https://github.com/skuza-oss/downhill-revived), introducing full synchronization with upstream 0.7.12, highly refined physical gamepad analog responsiveness, ultrawide HUD visual alignment, and ergonomic virtual touch enhancements.

### Highlights and Improvements

- **Upstream v0.7.12 Synchronization:** Merged all upstream 0.7.12 engine improvements, including Metal/Rust graphics backend refinements, enhanced HLE stability, and audio/timing accuracy.
- **Smooth Gamepad Analog Steering:** Calibrated deadzone and axis response curves for Bluetooth controllers (DualSense, DualShock 4, Xbox, MFi). Fine, gentle analog stick inputs now translate directly to precise micro-steering on the bike, ensuring fluid navigation through tight and technical turns.
- **Ultrawide HUD Combat Text Alignment:** Fixed an issue on ultrawide displays (~2.17:1 format, such as newer iPhone Pro Max models) where action combat text ("PUNCH", "KICK") shifted downward outside the shaded 3D backing above the speedometer. 3D projection descriptors and 2D HUD elements now maintain perfect geometric lock across all display aspect ratios.
- **Refined Touch Joystick Layout:** Adjusted virtual analog margins and safe area handling in landscape mode to prevent clipping against device edges and the Dynamic Island. Improved immediate touch tracking with calibrated radial limits.
- **Neutral Xcode Configuration:** Clean project settings allowing immediate opening and compilation on any Mac with your preferred Apple development team.

### Requirements

- **iOS / iPadOS 17.0+**
- **ARM64 + Metal**
- Your own unmodified **Downhill Domination USA disc image (`.iso`, SCUS-97177)**
- Valid Apple code signing and provisioning for installation

### Download and Installation Notice

Asset: `DownhillRevived-0.7.12-unsigned.ipa`

**This IPA is UNSIGNED. It is NOT installable directly on a regular iPhone or iPad.** Developers may sign a build with an appropriate Apple team and provisioning profile. Instructions: [`docs/INSTALLATION.md`](https://github.com/ribeirogustav/downhill-revived-ios/blob/main/docs/INSTALLATION.md) and [`docs/BUILD_IPA_WITH_AI.md`](https://github.com/ribeirogustav/downhill-revived-ios/blob/main/docs/BUILD_IPA_WITH_AI.md).

**SHA-256:**
```text
936bceb240e41256893be9a5fab07e398bcd4a25284dabefbec764fc7c61ccc0
```

### Credits and Legal

Original project and credits: **[skuza-oss/downhill-revived](https://github.com/skuza-oss/downhill-revived)**. This is an independent fan-made community adaptation, not an official upstream iOS release and not endorsed by the commercial game's rights holders. Covered source code is **GPL-3.0-or-later**, with third-party licences retained. No original game ISO, BIOS, executable, sound, textures or game content is included. Users must supply their own supported disc image.
