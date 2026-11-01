# Playing Downhill Revived

Everything about playing the game: what it does, how to install and update it, the controls, online races, texture packs and every setting. The [README](../README.md) has the short version.

## Features

What works today:

- **A launcher**, `DownhillRevived.exe`: it opens before the game and holds the settings, your
  rider name for online races, the disc image, updates, the release notes, the public races on
  the server and a link to the [Discord server](https://discord.gg/XrSeTyzfr2). It works with a controller too.
- **Discord Rich Presence**: your Discord profile can show the menus, the race with its track and
  your place, the career or the online room (never a private room's code). Off in the launcher:
  Online > Discord status.
- **Native renderer** on Direct3D 12 or Vulkan, written in Rust on [wgpu](https://wgpu.rs). The
  original PS2 graphics hardware is reproduced on the GPU. An exact emulation path stays in the
  code as the reference it is tested against.
- **60 fps or more, with even frame pacing.** The game runs at its original 59.94 Hz, and each
  picture is shown on a fixed grid, so frames don't come out long-short-long. PC SETTINGS > Frame
  Rate also offers 120, 144, 165 or 240 fps, or the monitor's rate, online too (experimental:
  players at different rates race together). Pad input is read as late as possible before each
  frame.
- **Higher resolution and widescreen.** Internal resolution 1x to 8x (or matched to the window).
  Widescreen at the monitor's shape (16:9, 21:9 or any other) is Hor+: a wider view with the same
  vertical framing, and the HUD, menus and 2D screens keep their shape. Windowed, borderless and
  exclusive fullscreen. A new install starts borderless at 2x in widescreen, at the monitor's
  resolution.
- **The movies play**: the intro, the attract loop and OPTIONS > VIDEO CLIPS, decoded with
  libmpeg2 from your disc. Skip Movies in PC SETTINGS (on in a new install) skips them.
- **PC SETTINGS in the game's own OPTIONS menu**: resolution, display mode, window size, picture
  shape, scaling, widescreen, skip movies, vibration, texture pack and font, drawn with the
  game's own menus and font. Changes are saved to `dhd.ini`. V-Sync (on in a new install) shows
  each picture without tearing.
- **Unlock All** (PC SETTINGS, off by default): every track, rider, bike and shop item reads as
  unlocked while it is on. Your save isn't changed, so turning it off brings your own progress back.
- **A sharp font**: the game's text is drawn with clean edges at any resolution, made in memory
  from the font on your disc (PC SETTINGS > Font: Sharp or Original).
- **Texture packs**: replacement packs made for PCSX2 (PNG files named by PCSX2's texture hash)
  work unchanged with the native renderer (see [Texture packs](#texture-packs)).
- **Automatic updates**: at start the game looks for a newer release and offers to install it;
  your settings and saves are never touched.
- **The mouse in the menus**: hovering selects, a left click is X, a right click goes back and
  the wheel scrolls a list or changes a value.
- **EXIT GAME** in the main menu.
- **Online races** for 2 to 10 players (experimental): an ONLINE entry in the main menu, a list of
  public races, private races joined with a 6-character code, and a race room where the host
  picks the track and the rules, on the project's official server (see [Online](#online)). The
  room stays open between races. Each track takes as many riders as the game gives it (10, 8
  or 6, shown in the track picker); when more players are ready than the track takes, the race
  moves to the next track that fits. After your finish the camera follows the riders still
  racing (Left/Right change rider) until the race closes; BACK TO THE ROOM in the pause menu
  leaves earlier.
- **Memory card saves**, stored as plain files in your data folder (see [Installing](#installing)).
- **Controllers**: Xbox pads, DualSense, DualShock 4, Switch Pro, 8BitDo and most other USB or
  Bluetooth pads (through SDL3 and its controller database), with vibration where the pad has
  motors. Buttons go by position, as on a PlayStation pad. The 2-4 player modes use every
  connected controller.
- **Keyboard play**: a whole race on the keyboard, with gradual steering; letters go by their
  place on the keyboard, so AZERTY and QWERTZ work too (see [Controls](#controls)).
- **Sound**: the game's sound driver is replaced by a native mixer, with music, effects and reverb.

Known gaps:

- Movies are shown 4:3 in widescreen, as on the PS2.
- The 2-4 player split screens have not been checked in widescreen.
- Online races have been tested with two games (PC and Mac) and up to nine scripted players, not
  yet with ten people on ten PCs.
- The browser version (experimental) at <https://downhill-revived.pages.dev/play/> needs Chrome or
  Edge 137 or later and a fast PC; online races are off there. It reads your `.iso` in place in
  the browser and uploads nothing. Its source is `port/web`.
- The game has only been tested on a few PCs. Please report problems.

## Installing

Each release has an installer and a zip for every system, on the
[Releases](https://github.com/skuza-oss/downhill-revived/releases) page. Both hold the same game
and update themselves the same way; take the installer unless you want a portable folder.

**Windows: installer** (`DownhillRevived-<version>-win-setup.exe`). It installs for your Windows
user only, without administrator rights, into `%LOCALAPPDATA%\Programs\Downhill Revived`, with a
Start menu shortcut, a desktop shortcut (optional) and an entry in Settings > Apps > Installed
apps to uninstall it. The installer isn't signed with a code-signing certificate (the project
has none), so Windows SmartScreen may say it doesn't recognise the app: click **More info**, then
**Run anyway**. Your settings, saves and log go to `%APPDATA%\Downhill Revived`.

**Windows: zip** (`DownhillRevived-<version>-win.zip`), portable: unzip it into a folder you can
write to (not Program Files). It holds `DownhillRevived.exe` (the launcher), `dhd.exe` (the game),
`dxcompiler.dll` (Microsoft's shader compiler), `README.txt`, `LICENSE.txt` and
`THIRD-PARTY-NOTICES.txt`; nothing else is needed. Your settings, saves and log stay in that
folder.

**Mac** (experimental, Apple silicon, macOS 11 or later): open
`DownhillRevived-<version>-mac-experimental.dmg` and drag Downhill Revived to Applications (the zip
holds the same app). It is signed ad hoc, not by an identified developer, so the first start is
blocked: open System Settings > Privacy & Security, click **Open Anyway** next to Downhill Revived
and confirm. Start it from Applications, not from the disk image, or it can't update itself. Your
files go to `~/Library/Application Support/Downhill Revived`.

**Linux** (experimental, 64-bit PC with Vulkan, glibc 2.35 or later; Steam Deck too):
`DownhillRevived-<version>-linux-experimental.AppImage` is one file with everything the launcher
needs inside (its window library, WebKitGTK, which a Steam Deck lacks). Make it executable
(Properties > Permissions, or `chmod +x`) and run it; on a Steam Deck in Game Mode, add it to Steam
as a non-Steam game from Desktop Mode. Your files go to `~/.local/share/downhill-revived` (or
`$XDG_DATA_HOME/downhill-revived`). The zip (`-linux-experimental.zip`) stays a portable folder
and needs WebKitGTK 4.1 from your distribution (`libwebkit2gtk-4.1-0`, already there with GNOME).
Neither is signed.

Then:

1. Start Downhill Revived (`DownhillRevived.exe` in the zip; `dhd.exe` opens it too) and press PLAY.
2. The first time, choose your disc image: your `.iso` of the USA disc. It is checked and
   remembered in `dhd.ini`; you won't be asked again unless the file moves or changes.

To start the game without the launcher, turn off Settings > Game > Open the launcher first in the
launcher (or put `launcher = off` in `dhd.ini`); `dhd.exe` then starts the game straight away.

Files the game keeps in your data folder (the zip's folder, or the folder named above for an
installed copy):

| File | What |
|---|---|
| `dhd.ini` | Your settings, one `key = value` per line. The game adds your disc image; the PC SETTINGS page (OPTIONS) writes the rest. |
| `saves\card1\` | Memory card 1, made at the first save. Copy this folder to back up your saves. |
| `dhd.log` | Messages from the last run. Attach it when you report a problem. |

Moving from the zip to the installer: copy `dhd.ini` and `saves\` from the zip's folder into
`%APPDATA%\Downhill Revived` (start the installed game once first, so the folder exists).

**Uninstalling.** Windows: Settings > Apps > Installed apps > Downhill Revived > Uninstall. It
removes the game and everything updates put in its folder, and never your settings, saves or log
in `%APPDATA%\Downhill Revived`: delete that folder yourself if you want them gone. Mac: drag the
app to the Bin. Linux: delete the AppImage. Your files stay in the data folder on both.

### Updates

The launcher looks for a newer release when it opens and shows **Update** when there is one; it
downloads the release, checks its SHA-256, puts it in place and opens again (`auto_update = on`
installs without asking). Without a network the game just starts. An update replaces `dhd.exe`,
the launcher and the other files of the release (the text files, `dxcompiler.dll`), never `dhd.ini`
or `saves\`. Installed copies update the same way, from the same release files (the AppImage
replaces itself with the release's new AppImage); the installers are only needed once. You can
also update a zip copy by hand: copy the files of a newer release over your folder (a release
holds no `dhd.ini` and no saves, so yours are kept). Online races need the newest version; an older one is offered
Update Now in the ONLINE menu.

### Controls

A controller works out of the box. The buttons go by position, as on a PlayStation pad:
bottom = Cross, right = Circle, left = Square, top = Triangle. A pad SDL doesn't know can be added
with a line in SDL's format in `gamecontrollerdb.txt` beside `dhd.exe`.

No controller? The keyboard plays a whole race (only while the game's window is in front; it
works together with a controller):

| Key | Button | What |
|---|---|---|
| Arrow keys | d-pad / left stick | move in menus; steer and lean while riding (gradually: a tap steers a little, holding steers fully) |
| Space or X | Cross | confirm; pedal (press twice: sprint) |
| Esc, Backspace | Triangle | back in menus; while riding Esc pauses, Backspace brakes |
| S | Triangle | brake (in the air: tweak a trick) |
| Enter | START | start the race on the race setup screen, pause |
| Tab | SELECT | hold + up/down changes the camera, with Q looks back; press twice to bail out |
| Z / C | Square / Circle | punch / kick |
| A / D | L1 / R1 | tricks (both together: Superman) |
| Q / E | L2 / R2 | tricks; E (R2) is also bunny hop |
| W | R3 | sprint |
| F | L3 | powerslide |

Letters are places on the keyboard as on a US one: on AZERTY or QWERTZ use the keys in the same
places. The game's own controls screen (OPTIONS > CONTROLS) names the PlayStation buttons; the
keys above stand for them.

### Online

Online works out of the box: choose ONLINE in the main menu to create a public or private race,
or join one from the list or with its code. The game uses the project's official server,
`wss://dhd.skuza.app`. Every player needs the same USA disc image.

Running your own server is optional. `online/server/` is the server (Cloudflare Workers), and
[online/server/README.md](../online/server/README.md) explains how to deploy it. Then put its
address on the `server =` line of `dhd.ini` (the game writes the file at the first start):

    server = wss://YOUR-SERVER-ADDRESS

An empty `server =` line means the official server.

### Texture packs

Texture replacement packs made for PCSX2 work unchanged. Packs are made by their authors and are
not part of this project. Either put the pack's files in `textures\SCUS-97177\replacements`
beside `dhd.ini` (your data folder, see [Installing](#installing)), or name the pack's folder on the `texture_pack =` line of `dhd.ini` (for
example PCSX2's own `textures\SCUS-97177\replacements` folder in your Documents). PC SETTINGS >
Texture Pack turns it off or on at the next start.

### Settings

`dhd.ini` in your data folder holds `key = value` lines; the game writes it, with comments, at the
first start. The PC SETTINGS page writes most of them for you.

| Key | Default | What |
|---|---|---|
| `iso` | (asked at the first start) | your disc image |
| `renderer` | `native` | `native`, or `exact` (the slow reference path) |
| `scale` | `2` | the 3D picture's resolution, `1` to `8` times the PS2's, or `window` |
| `window` | `borderless` | `windowed`, `borderless` or `fullscreen` |
| `widescreen` | `1` | `1` the monitor's shape, `0` off, or a shape such as `21:9` |
| `DHD_ASPECT` | `wide` | the picture's shape: `wide`, `4:3`, `16:9`, `stretch` or `pixels` |
| `skip_movies` | `1` | `1` skips the movies, `0` plays them |
| `vibration` | on | `0` turns rumble off |
| `texture_pack` | (empty) | a texture pack's folder; empty: `textures\SCUS-97177\replacements` beside `dhd.ini` |
| `textures` | `on` | `off` draws the game's own textures |
| `texture_cache_mb` | half the card's memory, at most 2048 | graphics memory for replacements |
| `auto_update` | `ask` | `ask`, `on` (update without asking) or `off` (never look) |
| `update_url` | (empty) | where to look for updates; empty: this project's GitHub releases |
| `server` | (empty) | the online server; empty: the official one |
| `font` | `sharp` | `original` draws the game's font as the PS2 does |
| `vsync` | `1` | `1` shows each picture without tearing, `0` sends it at once (may tear) |
| `unlock_all` | `0` | `1`: everything reads as unlocked, without changing the save |
| `name` | (empty) | your rider name in online races |
| `launcher` | on | `off`: `dhd.exe` starts the game without opening the launcher |

Each key has a `DHD_*` environment variable (`scale` is `DHD_SCALE`, `server` is
`DHD_NET_SERVER`; see [docs/BUILDING.md](BUILDING.md)), which wins over the file.

### System requirements

- Windows 10 or 11, 64-bit.
- A graphics card with Direct3D 12 or Vulkan. Without one the game draws with Windows' software
  renderer (WARP), slowly.
- A reasonably modern CPU: the PS2's vector units are run on the CPU.
- Or, experimental: a Mac with Apple silicon (M1 or later) on macOS 11 or later; or 64-bit Linux
  with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Linux Mint 21 and later), a Vulkan
  driver and WebKitGTK 4.1 for the launcher.
