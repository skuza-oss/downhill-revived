Downhill Revived - a PC port of Downhill Domination (PS2)
=========================================================

This is a fan project. It is not made, endorsed or supported by Sony Interactive Entertainment,
Incognito Entertainment or anyone who holds rights to the game. It contains no game data: the
game reads its program data, graphics, sound and everything else from your own copy of the disc
each time it starts.

What you need
- Windows 10 or 11 (64-bit), a graphics card with Direct3D 12 or Vulkan. Without one the game
  draws with Windows' software renderer (WARP), slowly.
  Or a Mac with Apple Silicon (M1 or later) on macOS 11 or later, or Linux on a 64-bit PC with
  Vulkan and glibc 2.35 or later (the AppImage carries the launcher's WebKitGTK; the zip needs
  WebKitGTK 4.1 from your distribution: libwebkit2gtk-4.1-0, already there with GNOME).
  The Mac and Linux builds are experimental: they may have problems the Windows build doesn't.
  Please report them with your dhd.log.
- Your own dump of the Downhill Domination USA disc (SCUS-97177) as an .iso file. Other regions
  and modified images are refused.

How to play
1. Install it, or unzip it. No download is signed by a developer certificate (the project has
   none), so the system asks once:
   Windows installer (DownhillRevived-<version>-win-setup.exe): installs for your user, without
   admin rights, into %LOCALAPPDATA%\Programs\Downhill Revived, with Start menu and
   desktop shortcuts. SmartScreen may say it doesn't recognise the app: More info > Run anyway.
   Uninstall it from Settings > Apps > Installed apps.
   Windows zip: unzip this folder anywhere you can write to (not Program Files). It is portable:
   your files stay in it.
   macOS: open the .dmg and drag Downhill Revived to Applications (or unzip the zip and move it
   there), and start it from Applications. The first time it is blocked (it is not from an
   identified developer): open System Settings > Privacy & Security, click Open Anyway next to
   Downhill Revived, and confirm.
   Linux: make the .AppImage executable (chmod +x) and run it; on a Steam Deck, add it to Steam
   as a non-Steam game for Game Mode. Or unzip the zip anywhere you can write to and run
   DownhillRevived.
2. Run Downhill Revived (DownhillRevived.exe in the zip), the launcher (dhd.exe opens it too).
   It keeps the game up to date and holds the settings, your rider name for online races, the
   patch notes and a link to the Discord server. Press PLAY.
3. The first time, the launcher asks for your disc image (Choose disc): choose your .iso. It is
   checked and remembered in dhd.ini; you are only asked again if the file moves or changes.
   The launcher's Settings > Game can change it later.

Controls
A controller works out of the box: Xbox pads, DualSense, DualShock 4, Switch Pro, 8BitDo and most
other USB or Bluetooth pads (through SDL3 and its controller database), with vibration where the
pad has motors. The buttons go by position, as on a PlayStation pad: bottom = Cross, right =
Circle, left = Square, top = Triangle. A pad SDL doesn't know can be added with a line in SDL's
format in gamecontrollerdb.txt beside dhd.exe.

No controller? The keyboard plays a whole race (only while the game's window is in front; it
works together with a controller):
    Arrow keys       move in menus; steer and lean while riding (they tilt the stick
                     gradually, so a short tap steers a little and holding steers fully)
    Space or X       Cross: confirm; pedal (press twice: sprint)
    Esc, Backspace   Triangle: back in menus. While riding Esc pauses, Backspace brakes
    S                Triangle: brake (in the air: tweak a trick)
    Enter            START: start the race on the race setup screen, pause
    Tab              SELECT: hold + up/down changes the camera, with Q looks back,
                     press twice to bail out
    Z / C            Square / Circle: punch / kick
    A / D            L1 / R1: tricks (both together: Superman)
    Q / E            L2 / R2: tricks; E (R2) is also bunny hop
    W                R3: sprint
    F                L3: powerslide
Letters are places on the keyboard as on a US one: on AZERTY or QWERTZ use the keys in the same
places (e.g. Square is the key left of X, wherever its label says Z, W or Y).
The game's own controls screen (OPTIONS > CONTROLS) and its trick lists name the PlayStation
buttons; the keys above stand for them.

Settings: OPTIONS > PC SETTINGS in the game's menu (resolution, display mode, window size,
picture shape, scaling, widescreen, skip movies, vibration, texture pack, font). Changes are
saved to dhd.ini at once.

Font: the game's text is drawn with sharp edges at any resolution ("font = sharp", the
default). The sharp font is made in memory from the font on your disc each time the game
starts; nothing is saved. "font = original" (PC SETTINGS > Font, at the next start) draws it as
the PS2 does. A texture pack that replaces the font wins over both.

Texture packs (upscale mods)
Texture replacement packs made for PCSX2 work unchanged (PNG files named by PCSX2's texture
hash). Packs are made by their authors and are not part of this game; get one yourself. Either
- put the pack's files in a folder textures\SCUS-97177\replacements beside dhd.ini (your
  data folder, below), or
- name the pack's folder in dhd.ini (its full path), e.g. the one PCSX2 uses:
    texture_pack = <Documents>\PCSX2\textures\SCUS-97177\replacements
OPTIONS > PC SETTINGS > Texture Pack turns it off or on (at the next start); in dhd.ini that is
"textures = off" or "on". Replacements load in the background the first time a texture shows,
so the game's own texture can show for a moment first. texture_cache_mb = N sets how much
graphics memory they may use (default: half the card's, at most 2048).

Your files
The game keeps them in one data folder: the zip's own folder, or for an installed copy
  Windows   %APPDATA%\Downhill Revived
  macOS     ~/Library/Application Support/Downhill Revived
  Linux     ~/.local/share/downhill-revived (the AppImage)
An update or uninstall never deletes them.
- dhd.ini       your settings, one "key = value" per line.
- saves\card1\  memory card 1: your saves. Copy this folder to back them up.
- dhd.log       messages from the last run. Attach it when you report a problem.

Updates
The launcher looks for a newer version when it opens and shows Update in its footer. Update
downloads it, checks it and puts it in place. Without a network you can still play. An update
replaces the files of the release only: dhd.ini and saves\ are never touched. You can also update
by hand: copy the files of a newer version over your folder (a release holds no dhd.ini and no
saves, so yours are kept). Installed copies update themselves the same way; the AppImage replaces
itself with the new release's AppImage.
In dhd.ini:
    auto_update = ask    ask before updating (the default); on: update without asking;
                         off: never look
    update_url =         where to look; empty means the official releases on GitHub. It can
                         name a latest.json of your own (version, url of the zip, its sha256).

Online races (experimental)
Choose ONLINE in the main menu: create a public or private race, or join one from the list or
with its code. It works out of the box, on the project's official server
(wss://dhd.skuza.app). Every player needs the same USA disc image,
and the newest version of the game: an older one is told "A new version is out. Update to play
online." and offered Update Now, which closes the game and lets the launcher update it.
Running your own server is optional: put its address on the "server =" line of dhd.ini, e.g.
    server = wss://races.example.org
A server of your own can keep an older version playing (its MIN_VERSION setting); then point
update_url at your own latest.json, or set auto_update = off.

Known gaps
- Movies are shown 4:3 in widescreen, as on the PS2.
- The 2-4 player split screens have not been checked in widescreen.
- Online races have been tested with two players, not yet with four people on four PCs.

Source code, building and bug reports: see the project's repository.
Licence: GPL-3.0-or-later (LICENSE.txt). Third-party notices: THIRD-PARTY-NOTICES.txt.
The original game, its data, names and trademarks belong to their owners.
