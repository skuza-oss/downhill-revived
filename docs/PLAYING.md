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
  picture is shown on a fixed grid, so frames don't come out long-short-long. REVIVED SETTINGS > Frame
  Rate also offers 120, 144, 165 or 240 fps, or the monitor's rate, online too (experimental:
  players at different rates race together). Pad input is read as late as possible before each
  frame.
- **Higher resolution and widescreen.** Internal resolution 1x to 8x (or matched to the window).
  Widescreen at the monitor's shape (16:9, 21:9 or any other) is Hor+: a wider view with the same
  vertical framing, and the HUD, menus and 2D screens keep their shape; loading cards and movies
  fill the screen too (the movie at its own shape, the sides blurred). Windowed, borderless and
  exclusive fullscreen. A new install starts borderless at 2x in widescreen, at the monitor's
  resolution.
- **The movies play**: the intro, the attract loop and OPTIONS > VIDEO CLIPS, decoded with
  libmpeg2 from your disc. Skip Movies in REVIVED SETTINGS (on in a new install) skips them.
- **REVIVED SETTINGS in the game's own OPTIONS menu**, in tabs (L1/R1): resolution, display mode,
  window size, picture shape, scaling, widescreen, skip movies, vibration, the sticks' dead zone,
  the button icons (PlayStation, Xbox or Nintendo, or the controller's), your keys and buttons,
  the size of online riders' names, texture pack and font, and a Performance group (shadows and
  sun glow, lens flare, particles: see [Settings](#settings)), Native VU, drawn with the game's own menus and
  font. Changes are saved to `dhd.ini`. V-Sync (on in a new install) shows
  each picture without tearing.
- **Unlock All** (REVIVED SETTINGS, off by default): every track, rider, bike and shop item reads as
  unlocked while it is on. Your save isn't changed, so turning it off brings your own progress back.
- **A sharp font**: the game's text is drawn with clean edges at any resolution, made in memory
  from the font on your disc (REVIVED SETTINGS > Font: Sharp or Original).
- **Texture packs**: replacement packs made for PCSX2 (PNG files named by PCSX2's texture hash)
  work unchanged with the native renderer (see [Texture packs](#texture-packs)).
- **Automatic updates**: at start the game looks for a newer release and offers to install it;
  your settings and saves are never touched.
- **The mouse in the menus**: hovering selects, a left click is X, a right click goes back and
  the wheel scrolls a list or changes a value.
- **EXIT GAME** in the main menu.
- **Online races** for 2 to 10 players (experimental): an ONLINE entry in the main menu, a list of
  public races, private races joined with a 6-character code, P2P races (race data directly
  between the games, 7-character codes), the room's region, and a race room where the host
  picks the track and the rules, on the project's official server (see [Online](#online)). The
  room stays open between races. Each track takes as many riders as the game gives it (10, 8
  or 6, shown in the track picker); when more players are ready than the track takes, the race
  moves to the next track that fits. After your finish the camera follows the riders still
  racing (Left/Right change rider) until the race closes; BACK TO THE ROOM in the race's menu
  (START, which never pauses an online race) leaves earlier.
- **Memory card saves**, stored as plain files in your data folder (see [Installing](#installing)).
- **Controllers**: Xbox pads, DualSense, DualShock 4, Switch Pro, 8BitDo and most other USB or
  Bluetooth pads (through SDL3 and its controller database), with vibration where the pad has
  motors. Buttons go by position, as on a PlayStation pad. The 2-4 player modes use every
  connected controller.
- **Keyboard play**: a whole race on the keyboard, with gradual steering; letters go by their
  place on the keyboard, so AZERTY and QWERTZ work too (see [Controls](#controls)).
- **Your own keys and buttons**: every PlayStation button can be put on another key or controller
  button, a main and an alternate, for the keyboard and each of two controllers, with Swap Sticks,
  Swap L1/L2, Swap R1/R2 and Invert per stick axis, in the game or the launcher (see
  [Controls](#controls)).
- **Sound**: the game's sound driver is replaced by a native mixer, with music, effects and reverb.
- **iPhone and iPad** (experimental): the same game and the same touch controls and
  settings as on Android, from an unsigned app you sideload yourself (see [Installing](#installing));
  online: P2P races only. The iOS port was first made by Gustavo Ribeiro (`ribeirogustav`).
- **Android phones and tablets** (experimental): the same game with touch controls (Classic pad
  or Thumb zones; **Steer With** the stick, tilt or a D-pad), or a controller. Online: P2P races
  only. The game's
  codes are typed on the touch pause panel's **CODES** page (the d-pad and the four face
  buttons); **ENTER** goes back to the race and enters them. The camera's flight over the track
  before each race, and the movies (the startup movies if Skip Movies is off, the attract movie,
  OPTIONS > VIDEO CLIPS) and the title's attract demo have a **SKIP** button (low under your right
  thumb, the left one with Hand: Left; the touch controls' START); tap it as you would press START on a pad. In a race's
  replay the same button reads **MENU** (START opens the replay's menu: RESUME, QUIT, RE-START,
  SAVE, which you then tap); the race controls are not shown, since a replay plays the recording.
  - **The D-pad** (SETTINGS > Touch Controls > Steer With > D-pad, in either layout and for either
    hand; it takes the place of the stick, so the Stick rows dim) is four arrows round a rest
    point: left, right, up and down, as on a PS2 pad (up and down lean, as the stick's up and
    down). Put a thumb on an arrow to press it; a thumb between two neighbouring arrows presses
    both (a diagonal), never opposite ones; slide from one arrow to the next without lifting;
    resting at the middle presses nothing. The game sees the pad's own d-pad buttons, so steering
    is at full lock as with a PS2 d-pad, and tapping left or right twice is the game's powerslide.
    Size and Opacity apply to it as to the other buttons; Edit Layout moves it.
  - **Edit Layout** (SETTINGS > Touch Controls) shows the controls over the game: drag one to move
    it, **-** and **+** (hold to repeat) or a second finger pinching beside it change its size,
    from 25 % to 600 % as far as the screen holds it; the line under the buttons names the
    selected control with its size in millimetres and per cent. A control stays wholly on the
    screen (dragged past an edge it stops there), may touch another but not cover it, and stays
    out of PEDAL's flick lanes; dropped where it does not fit, it goes back to the last place on
    the way where it did. **UNDO** and **REDO** step through your last changes (each drag, each
    run of - or +, RESET); **RESET** puts the layout back to its default; **DONE** (or Back)
    saves. GROUND and AIR switch between the race layout and Thumb zones' trick stick.
  - **Button Style** (SETTINGS > Touch Controls, or the launcher's Touch tab): **Actions** (the
    default) shows what each button does, with the pad's own button in a small badge so the game's
    "PRESS ✕" and trick lists still match: a pedal on ✕ (PEDAL), fists on ◻ and ○ (the left- and
    right-hand attacks), a brake lever on △ (BRAKE), which becomes a star in the air (△ there
    superstyles a trick), a bike hopping on R2 (HOP), which becomes two links in the air (R2 alone
    does no trick; it combines with L1, L2 or R1), a sliding wheel on L3 (SLIDE), arrowheads on R3
    (sprint), on the shoulders the trick each does alone (L1 One Hander: a hand, R1 One Legger: a
    boot, L2 Saran Wrap: a loop), a camera on SELECT, Pause on START and a handlebar on the
    stick. The pictures change with the rider in the air, never under a finger that is down.
    With Button Icons set to Xbox or Nintendo the badges use those names. At the smallest sizes a
    button too small for its picture shows its name. **Symbols** shows the PS2 symbols alone, as
    before. The idea comes from a community iOS fork (`bagasromadon/downhill-revived-ios-newbutton`);
    the pictures are our own.
  - **Tablets and iPads** (a screen 90 mm or more on its short side, about 7 inches and up) get
    their own defaults: the controls a quarter larger, nearer where your thumbs are when you hold
    the tablet by its sides. On every screen Pause and the camera button (START and SELECT in
    Classic pad) sit in a row above the right-hand buttons (the left-hand ones with Hand: Left),
    clear of the race HUD's top corner and of the camera cutout. The Size setting is a share of that. A layout you edited before
    this version keeps its places on every screen; RESET moves it to the new ones.

Known gaps:

- The 2-4 player split screens have not been checked in widescreen.
- Online races have been tested with two games (PC and Mac) and up to nine scripted players, not
  yet with ten people on ten PCs.
- The browser version (experimental) at <https://downhill-revived.pages.dev/play/> needs Chrome or
  Edge 137 or later and a fast PC; its online play is P2P races only (a page that can't reach the
  room's hub doesn't race). It reads your `.iso` in place in
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

**Android** (experimental, arm64, Android 10 or later with Vulkan):
`DownhillRevived-<version>-android-experimental.apk`. Open it on the phone and allow installs
from your browser or file manager when Android asks; the app is not on Google Play. The app opens
on its launcher, the same as the PC's: choose your `.iso` there (it is copied into the app once, a
few GB), change the settings that are fiddly in the game (resolution, frame rate, Performance mode,
Native VU, widescreen, the touch layout, hand, steering and size, your rider name), then press PLAY.
Its **Edit the touch layout** (Settings > Touch) opens the touch layout editor on its own; DONE
saves your layout and brings you back to the launcher. It works upright too; the game itself plays
sideways. To skip it, turn off Settings > Game > **Open
the launcher at every start**; SETTINGS in the game (Game tab, Launcher At Start) or a long press on
the icon (**Launcher**) bring it back. Install a new release's APK over the old one: your settings
and saves stay. If the game closes unexpectedly, the launcher says so at the next start and offers
**Share log** (see below). The release page's Android section has the details and the signing
certificate.

**iPhone and iPad** (experimental, iOS 17 or later): `DownhillRevived-<version>-ios-experimental.ipa`.
It is **unsigned**: Apple lets an app outside the App Store run only once it is signed for your
device, which you do while installing it with a sideloading tool on your computer or phone
(AltStore, SideStore or Sideloadly, with your own Apple ID; a free Apple ID's install runs for 7
days and then has to be refreshed by the tool, a paid developer account's for a year). The app is
not on the App Store. The app opens into the launcher, as on Android: choose your `.iso` there
(from Files, iCloud Drive or a USB drive); it is copied into the app once (a few GB); an `.iso` you
copied into Files > On My iPhone > Downhill Revived is moved into place instead. Then PLAY. Play
with the touch controls or a controller (MFi, Xbox, PlayStation); the button names follow what you
use last (touch until a controller is used). Settings, saves, the disc copy and `dhd.log` are in
that same Files folder. In the menus, a swipe in from the left edge is Back (iOS has no Back button);
the keyboard's bar has Cancel and Done. Leaving the app in an offline race pauses it. If the game closes
unexpectedly, the launcher says so at the next start and offers **Share log**. Install a new
release over the old one with the same tool: your files stay; deleting the app deletes them. What
is experimental: it is not yet measured on many devices, there is no tilt steering, and EXIT GAME
closes the app (the next start opens the launcher).

Then:

1. Start Downhill Revived (`DownhillRevived.exe` in the zip; `dhd.exe` opens it too) and press PLAY.
2. The first time, choose your disc image: your `.iso` of the USA disc. It is checked and
   remembered in `dhd.ini`; you won't be asked again unless the file moves or changes.

**A damaged or changed disc image.** The game also checks every file of your disc image against
the USA disc's, once per image (it reads the whole image in the background the first time, on a
phone for up to a minute or so; then `iso-check.txt` remembers the answer). If a file differs, the
main menu says so once, names the first file that differs and offers OK: the game still plays,
but it may close unexpectedly where it reads that file (a bad dump closed one player's game in
the Bike Shop), and online needs the USA disc's files. A new copy of your own disc fixes it.
`dhd.log` has the answer too (`disc check:`). The notice comes back only for another image, or
when this one changes; `disc-notice.txt`, beside `dhd.ini`, lists the images whose notice you
have seen. Nothing is sent anywhere.

**Starting straight into the game** (a Steam shortcut, a desktop shortcut): start the launcher with
`--play`. The game starts at once with your saved settings, without the launcher's window, and
the launcher closes when the game does (play time is still counted). The window opens instead
when something needs you: no disc image chosen yet, a disc image the game can't use, or Update
Now in the ONLINE menu. Updates follow `auto_update`: the check runs while you play, so the start
never waits for it; with `on` a newer release is installed after you quit, with `ask` the window
offers it the next time it opens.

- Windows: `DownhillRevived.exe --play` (a shortcut's Target, or Steam's launch options).
- Linux and Steam Deck: `DownhillRevived-<version>-linux-experimental.AppImage --play`.
- Mac: `open -a "Downhill Revived" --args --play`, or
  `"/Applications/Downhill Revived.app/Contents/MacOS/DownhillRevived" --play`.

Steam (a Steam Deck in Game Mode too): in Desktop Mode open Steam, Games > Add a Non-Steam Game to
My Library, pick the AppImage (or `DownhillRevived.exe` on Windows), then in the new entry's
Properties > Launch Options write `--play`. Choose your disc image once in the launcher (start it
without `--play`) before playing from Steam. `dhd.exe` started on its own still opens the launcher.

Files the game keeps in your data folder (the zip's folder, or the folder named above for an
installed copy):

| File | What |
|---|---|
| `dhd.ini` | Your settings, one `key = value` per line. The game adds your disc image; the REVIVED SETTINGS page (OPTIONS) writes the rest. |
| `saves\card1\` | Memory card 1, made at the first save. Copy this folder to back up your saves. |
| `dhd.log` | Messages from the last run. Attach it when you report a problem. |
| `dhd-previous.log` | The log of the run before. If you started the game again after a crash, the crash is in this one. |

On Android the logs are inside the app, where no file manager reaches them: after the game closes
unexpectedly the launcher offers **Share log** at the next start, its Help page has it at any time,
and so does a long press on the icon (**Share log**). Both open Android's share sheet with one text file; nothing is sent
unless you pick an app there.

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

- **Button icons** (REVIVED SETTINGS > Button Icons, the launcher's Settings > Controls): the game shows
  the buttons of your controller. Auto follows player 1's controller: Xbox pads (and a Steam Deck)
  get A, B, X, Y and LB, RB, LT, RT, Switch pads B, A, Y, X and L, R, ZL, ZR (by position, so the
  bottom button is always the one the game means), PlayStation pads and the keyboard the game's
  own symbols. Or choose PlayStation, Xbox or Nintendo.
- **Stick dead zone** (REVIVED SETTINGS > Dead Zone, Settings > Controls): how far a stick moves before
  the rider responds. Game is the PS2's own, about 36% of the travel; a smaller one steers sooner,
  a larger one ignores a worn stick that drifts. The whole travel past it is used, so full lock is
  still at the end. The launcher's Controls tab shows your stick and what the rider gets from it.

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

**Your own keys and buttons** (REVIVED SETTINGS > Controls in the game, the launcher's Settings >
Controls > Bindings): choose which key or controller button stands for each PlayStation button,
for the keyboard and for each player's controller (player 1 and player 2; every controller of a
player uses that player's layout). Each button has a main and an alternate. In the game, Cross on
a button's row waits up to five seconds for the main key or button (Esc cancels), Square clears
the main, and Triangle or Esc goes back. Changes are saved as you make them. The dimmed
'also' binding is the alternate, changed in the launcher. In the launcher, click a button's key to
change it, right-click to clear it; the game takes it at its next start. The buttons keep their
meaning (Cross pedals in a race and confirms in menus), only the key or button for them changes.

- A key or button that another PlayStation button had swaps: the new button takes it and the
  other one gets the new button's old key or button ("Swapped Cross and Square"). Given to an empty
  alternate, it just moves there.
- Cross, Triangle, START, R1 (it switches the REVIVED SETTINGS tabs) and the d-pad always keep a
  key or a button on each of the keyboard,
  Controller 1 and Controller 2, each on its own (the keyboard's keys don't count for a controller):
  a clear or a move that would take the last one away is refused ("Cross needs a button").
- Esc can't be changed: it always goes back in menus and pauses a race. Alt, Ctrl and the Windows
  keys stay your system's.
- Keys on the d-pad (the arrows by default) steer gradually while riding, whichever keys they are.
  The keyboard can also take the sticks' directions.
- A controller's sticks stay its own: **Swap Sticks** (the right stick steers) and an **Invert**
  for each axis of each stick.
- **Swap L1/L2** and **Swap R1/R2** (off by default) are for controllers that report a shoulder
  button as its trigger and the trigger as the shoulder: the two change places before anything
  else reads them. Your bindings stay as they are, and a button you press to bind one counts as the
  place it then has, so with a swap on you never need to rebind the four buttons by hand.
- The button icons (Button Icons) show the controller button you chose for each PlayStation
  button.
- **Reset Keyboard**, **Reset Controller 1** and **Reset Controller 2** (and the launcher tab's
  Reset) bring back the layouts above.

They are saved in `dhd.ini` as one line for each layout, with only what differs from the
defaults:

    keyboard = cross:j,space square:x
    pad1 = cross:east circle:south sticks:swap invert:ly l1l2:swap r1r2:swap

Each entry is `button:key` or `button:main,alternate` (`none` for nothing). Buttons: `cross`,
`circle`, `square`, `triangle`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `start`, `select`, `up`,
`down`, `left`, `right`, and on the keyboard `lstick_up`, `lstick_down`, `lstick_left`,
`lstick_right`, `rstick_up`, `rstick_down`, `rstick_left`, `rstick_right`. Keys by their place on a
US keyboard: `a` to `z`, `0` to `9`, `space`, `enter`, `tab`, `backspace`, `up`, `down`, `left`,
`right`, `lshift`, `rshift`, `f1` to `f12`, `insert`, `delete`, `home`, `end`, `pageup`,
`pagedown`, `minus`, `equals`, `lbracket`, `rbracket`, `backslash`, `semicolon`, `quote`,
`backquote`, `comma`, `period`, `slash`, `intlbackslash` (the key beside the left Shift on some
keyboards), `kp0` to `kp9`, `kp_plus`, `kp_minus`, `kp_multiply`, `kp_divide`, `kp_period`.
Controller buttons by their place: `south`, `east`, `west`, `north` (the face buttons: on a
PlayStation pad Cross, Circle, Square, Triangle; on an Xbox pad A, B, X, Y), `lb`, `rb`, `lt`,
`rt`, `ls`, `rs` (the sticks pressed), `back`, `start`, `dpad_up`, `dpad_down`, `dpad_left`,
`dpad_right`, `misc` (Share, Mic or Capture), `paddle_r1`, `paddle_l1`, `paddle_r2`, `paddle_l2`,
`touchpad`. A controller's line may add `sticks:swap`, `invert:` with `lx`, `ly`, `rx`, `ry`, and
`l1l2:swap` and `r1r2:swap`. The defaults are the tables above: the keyboard's keys, and on a controller the
button in the same place as on a PlayStation pad. A line the game can't read is skipped where it
can't be read (that button keeps its default) and dhd.log says why; a line that would leave the
keyboard or a controller without Cross, Triangle, START, R1 or a d-pad direction puts that one layout
back to its defaults. Players 3 and 4 use the default controller layout.

### Online

Online works out of the box: choose ONLINE in the main menu to create a public or private race,
or join one from the list or with its code. The game uses the project's official server,
`wss://dhd.skuza.app`. Every player in a room needs the same USA disc image and the same release
of the game: rooms of another release show as OTHER VERSION in the list, and a release the server
no longer takes gets "A new version is out" and Update Now in the ONLINE menu.

In a race the riders' data goes over Cloudflare's network, from the Cloudflare location nearest
to you (UDP). Where a network blocks UDP, the game falls back to an encrypted connection on port
443 by itself. In these rooms no other player ever gets your internet address.

START in an online race does not pause it: the room's menu (RESUME, BACK TO THE ROOM, END THE
RACE for the host, LEAVE THE ROOM) opens over the race, which goes on for everyone. While it is
open your rider gets no input and rolls on by itself. When you are the only player still racing
(everyone else finished or left), START pauses the race as offline, with OPTIONS too. On a phone
the touch controls' Pause button opens their online panel, which never pauses either.

MOSH PIT (ARENA), the last track in the room's track picker, is the game's Mosh Bowl online: a
two-minute round with the game's own rules and points per hit, then COMBAT RESULTS with the same
table on every game, and the room ranks the players by their scores (a normal race is always
ranked by time).

#### P2P races

ONLINE > **Create A P2P Race** makes a private room (never listed) with a 7-character code; others
join with Join With A Code. The room, its rules and the results run on the server as in any room,
but the race data goes directly between the players' games over WebRTC: one game, the hub (the
creator first; the next player when it leaves or its links fail), passes the others' data on, and
a player whose game can't reach the hub races over Cloudflare's network in the same room, bridged
by the hub. On a LAN or nearby this can be quicker than Cloudflare's network.

**Your address:** in a P2P room the games connect directly, so the other players can see your
network (IP) address, and the hub sees the address of everyone linked to it. The game says so on
the menu row and when the room opens. Play P2P races with people you trust. `dhd.log` shows a P2P
room's code only by its first two characters.

On Android, iPhone and iPad and in the browser, P2P races are the only online races so far.

A room's server (the lobby: who is in it, the next race, the start) runs in one of Cloudflare's
Durable Object cities, which are in North America, Europe, East and Southeast Asia and Australia
only. ONLINE's **Room Region** row picks where the rooms you create run: Auto (the server picks the
one nearest the Cloudflare location you reach) or EU West, EU East, NA East, NA West, Asia NE,
Asia SE or Oceania. The FPS & Ping counter (`perf_counter`) shows both numbers: `SERVER` is the
room's server and `RACE` the race network. From South America, Africa or the Middle East the
`SERVER` number is high wherever the room runs; races don't go through it.

Running your own server is optional. `online/server/` is the server (Cloudflare Workers), and
[online/server/README.md](../online/server/README.md) explains how to deploy it. Then put its
address on the `server =` line of `dhd.ini` (the game writes the file at the first start):

    server = wss://YOUR-SERVER-ADDRESS

An empty `server =` line means the official server.

#### What the official server keeps

To deal with cheaters and other abuse, the server keeps a short log of who joined which race:
the time, the room's code, your rider name, the game's version, your country (as Cloudflare
sees it), a scrambled form of your internet address (a keyed hash: the address itself is never
stored, and the hash can't be turned back into it) and your game's install id (a random number
the game makes once and keeps in `dhd.ini` as `install_id`; it says nothing about you or your
PC). Log entries are deleted after 14 days. A player who is banned is banned by that hash or
install id, not by name, until the ban ends (or until it is lifted, for one without an end).

No other player ever sees your address's hash or your install id, outside P2P rooms (above) no
other player sees your address, and the game doesn't write the install id into `dhd.log`. Deleting the `install_id` line makes the game
pick a new one. Players in a room see each other's rider names, ping and region (EU, NA...).

The server's operator can also turn online races off for a while; the ONLINE screen then says
so ("Online races are paused. Try again later.").

### Texture packs

Texture replacement packs made for PCSX2 work unchanged. Packs are made by their authors and are
not part of this project. Either put the pack's files in `textures\SCUS-97177\replacements`
beside `dhd.ini` (your data folder, see [Installing](#installing)), or name the pack's folder on the `texture_pack =` line of `dhd.ini` (for
example PCSX2's own `textures\SCUS-97177\replacements` folder in your Documents). REVIVED SETTINGS >
Texture Pack turns it off or on at the next start.

### Settings

OPTIONS starts with REVIVED SETTINGS (SETTINGS on Android). L1/R1 switch its Display,
Graphics, Controls, Online and Game tabs; the legend shows your bound inputs. Up/down choose
rows and left/right change them. Cross keeps changes and closes; Triangle undoes the settings
page's changes and closes (Android Back keeps them). Controls and Touch Controls save as you go.
A star marks a choice that applies at the next start. A host hides tabs it cannot offer.

The Graphics tab's **Performance** group makes the game lighter on slower PCs and phones:
Performance Mode ON turns off the three effects below it (the riders' shadows and the sun's glow,
the lens flare, the particles), OFF turns them all back on, and Custom shows a mix. Each can be
set alone. They apply at once, online too: only the picture changes, never the race.

**Native VU** (Graphics tab, on by default) runs the PS2's VU1 programs (the ones that place the
world, the lit objects and the shadows) on your graphics card instead of emulating them, which
gives more frames a second, most on a fast graphics card at a high resolution. It applies at once,
online too, and changes only the picture. If your graphics card can't run it, `dhd.log` says so
and the game goes on without it. If something in the picture looks wrong, turn it OFF (the game
then emulates those programs as before) and tell us with your `dhd.log` (in the browser version:
the lines under LOG).

`dhd.ini` in your data folder holds `key = value` lines; the game writes it, with comments, at the
first start. The REVIVED SETTINGS page writes most of them for you.

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
| `stick_dead_zone` | `game` | the sticks' dead zone: `game` (the PS2's own, about 36%) or a percentage such as `10` |
| `prompts` | `auto` | the button icons: `auto` (the controller's), `playstation`, `xbox` or `nintendo` |
| `names` | `small` | names above online riders: `off`, `small`, `medium` (original size), `large`; drawing only |
| `keyboard` | (empty) | your keys for the PlayStation buttons, see [Controls](#controls); empty: the table there |
| `pad1`, `pad2` | (empty) | player 1's and player 2's controller buttons and stick options, see [Controls](#controls); empty: by position |
| `texture_pack` | (empty) | a texture pack's folder; empty: `textures\SCUS-97177\replacements` beside `dhd.ini` |
| `textures` | `on` | `off` draws the game's own textures |
| `texture_cache_mb` | half the card's memory, at most 2048 | graphics memory for replacements |
| `auto_update` | `ask` | `ask`, `on` (update without asking) or `off` (never look) |
| `update_url` | (empty) | where to look for updates; empty: this project's GitHub releases |
| `server` | (empty) | the online server; empty: the official one |
| `font` | `sharp` | `original` draws the game's font as the PS2 does |
| `vsync` | `1` | `1` shows each picture without tearing, `0` sends it at once (may tear) |
| `perf_counter` | `0` | `1` shows FPS & Ping: frames a second and, online, `SERVER N MS` (the round trip to the room's server, which carries no race data) and `RACE N MS` (the round trip to Cloudflare's race network, where race data goes); in a race `RIDERS N MS` instead of `RACE`, how old the other riders' positions are when they arrive |
| `unlock_all` | `0` | `1`: everything reads as unlocked, without changing the save |
| `name` | (empty) | your rider name in online races |
| `shadows` | `1` | Performance: `0` turns off the riders' shadows and the sun's glow |
| `lens_flare` | `1` | Performance: `0` turns off the lens flare's ghosts (the sun's disc and halo stay) |
| `particles` | `1` | Performance: `0` turns off the particles (dust, dirt, spray, heat haze) |
| `native_vu` | `on` | Native VU: the VU1 programs run on the graphics card; `off` emulates them (for troubleshooting) |
| `room_region` | `auto` | where the rooms you create run (ONLINE's Room Region row): `auto` (the server picks by where you reach Cloudflare), `weur`, `eeur`, `enam`, `wnam`, `apac-ne`, `apac-se` or `oc` |

Each key has a `DHD_*` environment variable (`scale` is `DHD_SCALE`, `server` is
`DHD_NET_SERVER`; see [docs/BUILDING.md](BUILDING.md)), which wins over the file.

### System requirements

Every platform needs your own dump of the USA disc (SCUS-97177) as an `.iso` (about 2.4 GB) and
about 300 MB for the game. **Minimum** means about 60 fps at 1x-2x resolution; **recommended**
means 60 fps or more at 4x and up. Lines marked *estimate* are not yet measured on that hardware.

| | Minimum | Recommended |
|---|---|---|
| **Windows** | Windows 10/11 64-bit; a 4-thread CPU from about 2016 (Core i5 6th gen, Ryzen 3 1200; *estimate*); a Direct3D 12 or Vulkan GPU, integrated is fine (Intel UHD 620, AMD Vega 8; *estimate*); 4 GB RAM | a 6-core CPU (Ryzen 5 3600, Core i5 10th gen); GTX 1060 / RX 580 for 4x, RTX 2060 / RX 6600 for 8x (*estimate*); 8 GB RAM |
| **Mac** (experimental) | Apple silicon (M1 or later), macOS 11 or later, 8 GB | any M-series Mac |
| **Linux** (experimental) | 64-bit, glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora 36, Linux Mint 21 and later), a Vulkan driver (Mesa 22+, NVIDIA 470+), WebKitGTK 4.1 for the launcher's zip (the AppImage carries it); CPU and GPU as on Windows; Steam Deck works | as on Windows |
| **Android** (experimental) | Android 10 or later, arm64 with Vulkan; a Helio G80/G85 or Snapdragon 680 class chip (Mali-G52 MC2 GPU); 4 GB RAM; 3 GB free storage; **Performance Mode on** (REVIVED SETTINGS > Graphics) | Snapdragon 778G / 870, Dimensity 1080 or better |
| **Browser** (experimental) | desktop Chrome or Edge with WebGPU; a 6-core desktop CPU from about 2019 (the browser runs at about a third of native speed); online: P2P races only | a fast 8-core desktop |
| **iPhone / iPad** (experimental) | iOS 17 or later (an A12 chip or later); 3 GB free storage; *estimate*, not yet measured | a recent Pro iPhone or an M-series iPad (*estimate*) |

Measured: a Ryzen 5 7600 with an RTX 3070 runs about 250 fps at 8x; AMD integrated graphics hold
60 fps at 5x; an M1 MacBook Air runs over 100 fps; a Redmi 9 (Helio G80) about 50 fps at 2x and
about 63 with Performance Mode; a Mi 11i (Snapdragon 870) 59 fps at 2x.

Below the minimum the game may still start, but runs well under 60 fps: phones with a Helio
G35/P35/A22 (PowerVR GE8320), Snapdragon 4xx or Unisoc SC9863A, phones with 2-3 GB of RAM, and
PCs with 2 cores and old integrated graphics. Speed reports from such devices are closed with a
pointer here; crashes are fixed on any device.
