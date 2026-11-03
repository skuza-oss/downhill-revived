# iOS 0.7.9 and external launcher

Merged the official v0.7.9 tag at 7cd9543f3144bd03beb27f20bbd4e0739c453128
into the iOS build 70803 source. Build 70901 includes the upstream spectator
camera corrections and shared track pickups. The online server configuration
now requires 0.7.9. The matching game sources remain identical to the tag.

UIKit exposes AirPlay and HDMI using the same noninteractive external-display
scene. See Apple's [external display documentation](https://developer.apple.com/documentation/uikit/presenting-content-on-a-connected-display).
The existing scene accessory on iOS 27 and the scene manifest on older iOS
both reach the same delegate. The TV hosts a read-only launcher driven by
the phone's navigation state, above the persistent Metal host. In a game,
the launcher disappears and the phone keeps input and pause menus. The scene
uses UIKit's negotiated bounds. If AirPlay offers a single mode, the delegate
keeps it.

The 3x internal TV default is shared by the renderer model and the scale
picker. A session override applies until disconnect. The phone restores its
own scale and the TV still fits 16:9 with the existing output cap policy.
All eight language catalogs describe the default and display connection.

Linux checks cover the portable iOS suite, upstream pickup fixture and source
patch anchors. Xcode compilation and physical AirPlay/HDMI checks remain
pending. The source ZIP excludes the IPA, disc inputs and generated caches.
