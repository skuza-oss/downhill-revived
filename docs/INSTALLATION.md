# Installation and signing (iOS)

[Back to README](../README.md) | [Português (Brasil)](INSTALACAO.pt-BR.md)

## Compatibility

- **iPhone/iPad with iOS or iPadOS 17.0+**.
- **ARM64 processor and Metal support**, as required by the supplied IPA's `Info.plist`.
- **Your own unmodified Downhill Domination USA (SCUS-97177) `.iso`**. Other game regions are not supported by this port.
- Sufficient free space to store your disc image in the app's local Documents area.

## Important: the supplied IPA is unsigned

`DownhillRevived-iOS-0.7.9-build-70902-unsigned.ipa` contains `CFBundleShortVersionString = 0.7.9`, build `70902` and `MinimumOSVersion = 17.0`, but has **no usable Apple signing/provisioning for direct device installation**. Merely renaming it, unzipping it or enabling Developer Mode does not sign it.

To run the port, create an appropriately signed build for your device or use a valid development/distribution signing workflow. See [Build an IPA with AI](BUILD_IPA_WITH_AI.md). Never upload private signing keys or provisioning credentials to public issues.

## Install a development-signed build

1. Configure your own Apple account and Team in Xcode.
2. Build the app under that Team, with a valid bundle identifier and provisioning profile for the target device.
3. Connect and trust your iPhone/iPad on the Mac; enable **Developer Mode** if required by that development installation.
4. Use the build script's `--install` option or Xcode's device installation mechanism for the signed app.
5. Start Downhill Revived and import the valid USA `.iso` through its disc management screen.
6. Start the game. Configure controls and TV output using the launcher before testing performance.

Development provisioning may be limited to registered devices, accounts, validity periods or entitlements. A signed IPA is not necessarily a general-public installable build.

## Checksums

The provided unsigned IPA's SHA-256 is:

`4181572bd7b78ce87e956d52309e0879b3230f82a7511d325a6abd2b50adcc4c`

Re-signing or rebuilding changes the bytes and the hash. Check the exact filename against [`SHA256SUMS`](../SHA256SUMS).

## Troubleshooting

- **App cannot be installed:** verify that it was signed, bundle identifier matches its profile, certificate/provisioning is valid, target device is eligible and Developer Mode requirements are met.
- **App opens without the game:** import your own supported USA disc image.
- **No TV output:** adapter and AirPlay receiver capabilities vary. Try native device output first, then verify the external-display settings and available screen modes.
- **No 120 FPS:** display refresh, thermal state, battery/power mode and GPU/CPU performance can limit frame rate. This is not guaranteed.

## Legal

The repository and its release files must not include the game's ISO, PS2 BIOS or disc-derived copyrighted assets. The code license does not authorize distribution of the commercial game. See [Legal and Credits](LEGAL_AND_CREDITS.md).
