# Building an iOS IPA with ChatGPT, Gemini or Claude

[Back to README](../README.md) | [Português (Brasil)](BUILD_IPA_WITH_AI.pt-BR.md)

**AI is a coding assistant, not an iOS compiler or an Apple signing authority.** The supplied iOS fork already has `port/ios/build_ios.py`. Use AI to inspect the build script and logs, configure dependencies, propose precise fixes and explain error messages. Real compilation needs a Mac, Xcode and the project's native toolchains.

## What you need

- macOS, full Xcode and the iPhoneOS SDK, Git, Python 3, XcodeGen, Rust/Cargo, LLVM/Clang and the upstream build prerequisites.
- wasi-sdk (34), WABT/wasm2c and SIMDe, installed/configured following [`docs/BUILDING.md`](BUILDING.md) in the upstream source.
- Your own legitimate **Downhill Domination USA (SCUS-97177)** disc image, and its locally extracted executable `iso/SCUS_971.77` to generate the core/derived resources. Game files must never be committed or bundled in releases.
- For signed output, your own Apple Team ID and appropriate signing/provisioning access.

The supplied source snapshot does **not** contain the ISO, `iso/SCUS_971.77`, `assets/vutext.bin`, `build/wasm/dhd_core.wasm` or the compiled native library. A fresh checkout requires the proper upstream build initialization before the iOS build can succeed.

## Prepare the Mac

```bash
xcodebuild -version
xcrun --sdk iphoneos --show-sdk-path

# If using Homebrew
brew install git python xcodegen rustup-init

# Ensure your Rust setup is complete (https://rustup.rs)
python3 --version
xcodegen --version
rustc --version
cargo --version
```

Read upstream `docs/BUILDING.md` carefully for `requirements.txt`, `WASI_SYSROOT`, `WABT_DIR`, `SIMDE_DIR`, `DHD_DEVCACHES` and any platform-specific adjustments. Install Python dependencies in an appropriate environment (such as a virtual environment).

Clone **your fork**, not an unrelated prebuilt IPA:

```bash
git clone https://github.com/YOUR-ACCOUNT/downhill-revived.git
cd downhill-revived
python3 -m pip install -r requirements.txt
```

From your original USA ISO, extract `SCUS_971.77` into `iso/SCUS_971.77`, according to upstream `docs/BUILDING.md`, then:

```bash
shasum -a 1 iso/SCUS_971.77
python3 tools/split.py
```

The expected **SHA-1 of the extracted game executable** is `4e25a222ad8a475b5d26c2e928e90b5f2b7075eb`. This is **not** the entire ISO's checksum. Never share the game ISO, extracted files or signing secrets with an AI tool or the repository.

## Compile the IPA

```bash
# Unsigned IPA (inspection/development only; not installable as-is)
python3 port/ios/build_ios.py --unsigned

# Generate the Xcode project without building the native core
python3 port/ios/build_ios.py --bundle-only

# Signed build under YOUR Apple Developer Team
python3 port/ios/build_ios.py --team YOURTEAMID --allow-provisioning-updates

# Signed build and installation on an eligible paired device
python3 port/ios/build_ios.py --team YOURTEAMID --allow-provisioning-updates --install
```

Replace `YOURTEAMID` with your valid **10-character** Team ID. Review the `PRODUCT_BUNDLE_IDENTIFIER` if a unique app ID is required. Do not rely on the build script's fallback Team ID because it may belong to another person. Xcode signing and device provisioning must succeed. Developer-signed apps have distribution and validity limitations.

The default output is `build/ios/DownhillRevived.ipa`. Check its metadata and checksum:

```bash
shasum -a 256 build/ios/DownhillRevived.ipa
python3 scripts/verify_release.py build/ios/DownhillRevived.ipa
```

The published `4181572b...` hash matches the **specific unsigned 0.7.9 build 70902** supplied with this kit, not new builds or re-signed archives.

## Reusable AI prompt

> Act as a senior iOS game-porting engineer. I am maintaining an independent GPL-3.0-or-later iOS fork of https://github.com/skuza-oss/downhill-revived at upstream version 0.7.9. First inspect `port/ios/build_ios.py`, `port/ios/project/Info.plist`, `docs/BUILDING.md` and the actual build logs I provide. Help me build reproducibly on a Mac with full Xcode and Apple's iOS SDK, keeping iOS 17.0 minimum, ARM64, Metal, SwiftUI, SDL/GameController input, HDMI/AirPlay paths and optional 120 FPS. The build depends on source-derived core data from my own USA SCUS-97177 disc. Explain exact dependencies and propose minimal, testable patches. Never pretend to have executed Xcode or tested a physical phone, never request/share signing keys or game assets, and never propose signing bypasses. Respect the upstream author skuza-oss, GPL-3.0-or-later, third-party notices and the need to publish corresponding source code. Give exact commands and wait for me to supply real command outputs before diagnosing failures.

Use that prompt in **ChatGPT, Gemini or Claude**, along with relevant source files and logs. It is platform-agnostic and works best when you share concrete compiler errors, one at a time.

## Practical verification

Test actual hardware before publishing performance claims: launch and ISO import, saves, touch input, gamepads, localization, 60/120 FPS and thermal throttling, app background/foreground, rotation, HDMI, AirPlay, display aspect ratio and sound. Do not label experimental features as validated when they have only passed static or portable tests.

## Common problems

| Symptom | Check |
| :--- | :--- |
| `Missing build tools: xcodegen` | Install XcodeGen and fix `PATH` |
| Missing iPhoneOS SDK | Install/select full Xcode and accept its license |
| Missing `assets/vutext.bin` | Follow upstream disc extraction and `tools/split.py` |
| Rust iOS compilation failure | Verify toolchain and `aarch64-apple-ios` target setup |
| Provisioning/signing failure | Use your own Team ID, valid bundle identifier and provisioning profile |
| IPA fails to install | An `--unsigned` IPA cannot install directly; build with valid signing |

See [installation](INSTALLATION.md) and [legal information](LEGAL_AND_CREDITS.md).
