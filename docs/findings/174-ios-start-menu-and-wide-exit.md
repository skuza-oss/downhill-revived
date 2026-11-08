# iOS startup, touch controls and the inventory exit

Status: partly superseded by 176 (0.7.12 integration and analog input)
Checked: 2026-11-07

The launcher saves the automatic start preference in UserDefaults. Its first
active scene attempts to start once per process if the verified disc is ready.
The home toggle and the in-game checkbox update the same value. See Apple's
ScenePhase and settings documentation for the lifecycle and storage APIs.

The portrait Menu button sits between the inner shoulders. In landscape it
sits below SELECT and START, with extra clearance from the top edge. The
landscape shoulders stack above the steering stick and the action diamond.
The positions also apply to the phone controller when an external screen is used.

The in-game graphics sheet holds a draft until Apply Changes is tapped.
Dismissing the sheet discards pending changes. The launcher's graphics screen
still applies changes as they are selected. TV scale remains session-specific.

Shell screen 18 also runs outside the inventory modal. The 16:9 limit now
follows the modal flag and is applied before the inventory initializes its
camera. When the flag clears, the next field restores the display aspect.
On iOS a zero-PATH1 field outside a loading state cannot be classified as a
4:3 card. This covers repeated scanouts during the shop-to-race transition.

Build 70803. The portable fixtures cover the modal exit while the shell
remains in state 18 and a loading card followed by race fields without new
PATH1 geometry. Xcode and physical iPhone checks remain pending.
