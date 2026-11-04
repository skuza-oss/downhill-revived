# iOS widescreen, TV cadence and live graphics

The previous live aspect loop read 32 entries from D_0075A9D0. The 20-byte
entries have 0xA0 bytes before D_0075AA70, and 0023D6C8 initializes six slots.
Only those six slots are visited. The update snapshots focal lengths and Y
factors, then rebuilds the projection and camera matrices via 0023D8D8 and
0023CB00. It preserves the product of focal length and vertical factor. The
inventory sets its final projection before building combined matrices and uses
at most 16:9 in iOS Wide. The inventory flag and shell screen 18 come from
00139BF8 and 001F1500, respectively. The original game sources remain matching.

Findings 043 and 056 reference the PCSX2 widescreen patch used as the original
map. Disassembly of the VU1 package at 0x269C50 with vudis.py identifies three
additional X half extent or X scale immediates at 0x26C448, 0x26C7D8 and
0x26C8E8. The corresponding Y operations retain their retail values. The
shared instruction table verifies the original upper word before editing.
On iOS a live transition also updates matching instructions already uploaded
to VU1, after draining graphics work and invalidating cached code matches.
This covers the identified sprite paths. Visual confirmation of the inventory
and power-ups requires the device build.

The TV scene fits a 16:9 Metal view and selects the largest available 16:9 HDMI
mode within the prelaunch cap. When the adapter offers only a different mode,
the content still fits in a 16:9 rectangle. UIKit's active screen maximum and
power and thermal policy determine the 60 or 120 FPS target. The core observes
target changes at VBLANK and carries fractional game fields and animation ticks
into the new rate. The previous iOS latency override forced two buffers even
under FIFO. The renderer now selects its mode-specific default. Apple documents
the available cadence and system policies at
https://developer.apple.com/documentation/quartzcore/cadisplaylink/preferredframeraterange.

The iOS game menu exposes live profiles, internal and output resolution and
presentation filtering. The renderer carries each frame's aspect with its
graphics marker. Internal scale changes reuse the web renderer's surface
reset at a field boundary, after queued pictures have finished. The first
field can lose effects held only in the old GPU surfaces. The TV starts at
1x and restores the phone's scale preference when disconnected. VSync and
the uncapped TV mode are prelaunch settings.

Portable suite: 22 tests pass, including 23 clock scenarios, 1,000 aspect
round trips and the six sprite width words. Source patch anchors were checked.
Xcode compilation, HDMI handshaking and physical rendering remain to be verified.
Build 70704.
