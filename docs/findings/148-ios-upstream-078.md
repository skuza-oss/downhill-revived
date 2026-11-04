# iOS integration with v0.7.8

The upstream base is v0.7.8, commit b3a8acf8ebb53df0d38837df35bb3dff53da4278.
Release notes: https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.8.
The matching sources and headers are identical to the tag. The iOS source
snapshot keeps its app icon, bundle identity, Documents saves, UIKit input
and external display. Renumbered the four local iOS findings to 144–147 so
upstream findings 140–143 can retain their original numbers.

- The game's variable-rate clock, replay grid and exact physics step are
  upstream. iOS can change the live refresh target without changing the replay
  grid's recorded period or the countdown's 60 Hz cadence.
- The queued native picture now carries both upstream's 4:3 card marker and the
  iOS aspect snapshot. Loading cards and movies use the upstream blurred
  background. The iOS inventory limit and VU sprite compensation still apply.
- The 6x phone ceiling also applies to the iOS render path, within GPU limits.
  The TV still starts at 1x. Native iOS touch is merged once; the Android touch
  UI and update notice stay on their original platform.
- iOS retains its online entry point and inherits the 0.7.8 reconnect token.
  The desktop/Android release checks ship with the upstream tree. The official
  release has no iOS IPA for an iOS updater to install.

Checks: 22 portable iOS tests, including refresh changes during the countdown
and replay; 14 native replay/ghost cases from 30 to 240 FPS; 242 source patch
units; and Linux C syntax checks on the shared renderer and selected iOS
branches. Xcode, Metal output, physical HDMI and online play on iPhone remain
unverified. The source ZIP leaves out build caches, IPA and disc-derived
inputs; use the existing local game executable to regenerate the latter.
