# Downhill Revived

A PC port of Downhill Domination.

Downhill Revived is a native 64-bit Windows port of *Downhill Domination* (PlayStation 2, Incognito
Entertainment / SCEA, 2003), with experimental builds for Macs with Apple silicon and for 64-bit
Linux. It is built on a matching decompilation: C/C++ source that compiles back to the original
game program byte for byte, built for the PC with the PS2 hardware replaced by native code.

**[Download](https://github.com/skuza-oss/downhill-revived/releases/latest)** ·
**[Discord](https://discord.gg/XrSeTyzfr2)** (news, help and races) ·
**[Players' guide](docs/PLAYING.md)** (controls, online, texture packs, settings) ·
**[Support on Ko-fi](https://ko-fi.com/skuza)**

## Features

- A launcher with the settings, updates, release notes and the public online races.
- A native renderer (Direct3D 12, Vulkan or Metal), 60 fps or more (up to 240) with even frame
  pacing, 1x to 8x resolution, Hor+ widescreen at any shape, windowed, borderless or fullscreen.
- Online races for 2 to 10 players (experimental), public or private with a code; after your
  finish you watch the others ride to the line.
- PC SETTINGS in the game's own OPTIONS menu, a sharp font, Unlock All, the movies, memory card
  saves, PCSX2 texture packs, mouse in the menus, Discord Rich Presence.
- Controllers (Xbox, DualSense, DualShock 4, Switch Pro, 8BitDo and most others) and full
  keyboard play.
- Automatic updates that never touch your settings or saves.

The full list and the known gaps: [docs/PLAYING.md](docs/PLAYING.md).

## Getting started

1. Download `DownhillRevived-<version>-win.zip` from the
   [Releases](https://github.com/skuza-oss/downhill-revived/releases/latest) page and unzip it
   into a folder you can write to (not Program Files).
2. Run `DownhillRevived.exe` and press PLAY.
3. The first time, choose your `.iso` of the **USA disc (SCUS-97177)**. It is checked and
   remembered.

Mac and Linux (experimental): take the `-mac-experimental` or `-linux-experimental` zip instead;
its `README.txt` has the details. You need Windows 10/11 64-bit (or an Apple silicon Mac, or Linux
with glibc 2.35+ and Vulkan) and a Direct3D 12 or Vulkan graphics card.

## Legal notice

- This is a fan project. It is not made, endorsed or supported by Sony Interactive
  Entertainment, Incognito Entertainment or anyone who holds rights to the game.
- This repository and its releases contain **no game data**: no disc image, no game executable
  or data from it, no IOP modules, no textures, sound, music or movies, and no BIOS. `dhd.exe` is
  the translated code only; at every start it reads the game's data from your disc image, after
  checking it is the unchanged original.
- To play you need your own dump of the USA disc. Other regions and modified images are not
  supported (why: [docs/findings/100-other-releases.md](docs/findings/100-other-releases.md)).
- The project sells nothing. Donations on Ko-fi are voluntary and unlock nothing: every release
  is free and the same for everyone.

## Decompilation progress

<!-- progress:start -->
| Region | What | Code matched | Functions |
|---|---|---|---|
| crt0 | startup code | 0.00% | 0 / 5 |
| game | game code | 98.17% | 3439 / 3474 |
| libgcc | compiler runtime (library, not decompiled) | 0.00% | 0 / 145 |
| engine | engine (same toolchain as the game) | 92.76% | 1088 / 1119 |
| sdk | Sony SDK + C/math libraries (not decompiled) | 0.00% | 0 / 802 |
| handwritten | game/engine functions written in assembly (not decompilable) | 0.00% | 0 / 7 |
| remnant | dead-strip leftovers of removed functions (not decompilable) | 0.00% | 0 / 138 |
| **target** | **game + engine: everything there is to decompile** | **97.03%** | **4527 / 4593** |
| **total** | whole executable, libraries included | **85.76%** | **4527 / 5690** |
<!-- progress:end -->

A function counts only when a tool confirms its compiled bytes are identical to retail; what
"matching" means and how it is checked: [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md).

## Building and contributing

Building needs the original game executable from your own disc; nothing here builds without it.
How to build: [docs/BUILDING.md](docs/BUILDING.md). How to help (decompiling, port fixes,
testing): [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md).

## Credits and licence

Built on the work of PCSX2, ps2sdk, SDL, wgpu, WABT, libmpeg2, OpenGOAL, splat, objdiff,
decomp.me and many others: the full list is in [docs/CREDITS.md](docs/CREDITS.md).

[GPL-3.0-or-later](LICENSE). The original game, its data, names and trademarks belong to their
owners and are not covered by this licence. Code adapted from other projects keeps its own
notice (see [Credits](docs/CREDITS.md)).
