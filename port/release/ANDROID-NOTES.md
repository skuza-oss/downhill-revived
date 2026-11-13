## Android (experimental)

`DownhillRevived-<version>-android-experimental.apk` is the game for Android phones and tablets.
It is experimental: recent phones hold 60 fps at 2x resolution, slower ones (and 4x) may not.
On a slower phone, OPTIONS > SETTINGS > Graphics tab > Performance Mode turns off the riders'
shadows and the sun's glow, the lens flare and the particles, which helps the most. Native VU (the
same tab, on by default) lets the phone's graphics chip run part of the PS2's work, which is
faster; if something in the picture looks wrong, turn it OFF and send us the log.
Play with the touch controls (Classic pad or Thumb zones; steer with the stick, by tilting the
phone or with a D-pad: OPTIONS > SETTINGS > Controls tab > Touch Controls; Button Style shows
what each button does (Actions, the default) or the PS2 symbols (Symbols); Edit Layout moves and
sizes each button, 25 % to 600 %, with undo; tablets get their own larger defaults) or a controller. A
**SKIP** button ends the flight over the track before a race, the movies and the title's demo. The
game's cheat codes are typed on the touch pause menu's **CODES** page.

**What you need**
- Any arm64 phone or tablet with Android 10 or later and Vulkan (most phones since 2018),
  older arm64 cores such as the Snapdragon 680's and Mali GPUs on Arm's newer drivers included.
- Your own disc image of Downhill Domination (USA, SCUS-97177) as an `.iso` file, like the PC
  version. No game data comes with the app.
- A few GB of free storage: the disc image is copied into the app once.

**Install**
1. Download the APK on the phone.
2. Open it. Android asks once to allow installs from your browser or file manager ("Install
   unknown apps"); allow it, then install. The app is not on Google Play.
3. Start Downhill Revived: its launcher opens (the PC launcher's look, upright or sideways).
   Choose your `.iso` there; the copy takes a minute or two. Then PLAY.

**The launcher** opens before the game at every start: PLAY, your disc, the settings that are
fiddly in the game (resolution, frame rate, Performance mode, Native VU, widescreen, the touch
layout, hand, steering and size, Edit the touch layout), your rider name, updates and Help (Share
log, links, licences). They are the same settings as SETTINGS in the game. To start the game
directly, turn off Settings > Game > Open the launcher at every start; turn it back on in the
game's SETTINGS (Game tab, Launcher At Start) or long-press the icon and pick **Launcher**.

**Updates.** The launcher checks whether a newer release is out and says so above PLAY (a game
started directly checks once a day and says so at the top of the screen); open the release page
from there, then install the new APK over the old one. Your
settings and saves stay. Nothing is downloaded by the game itself. If you installed a test build
before this release, uninstall it first: Android refuses an update signed by another key, and
uninstalling deletes the app's files (the disc copy and your saves).

**If the game closes unexpectedly**, the launcher says so at the next start and offers **Share
log**: send the file to us (a GitHub issue or Discord) and we can see what happened. The
launcher's Help page has Share log at any time, and so does a long press on the app's icon, even
when the game closes before anything shows. Nothing is sent
unless you pick an app in Android's share sheet.

**Online: P2P races only.** ONLINE in the main menu offers Create A P2P Race and Join With A Code
(a 7-character code from a friend's P2P race, on a phone or a PC). The race data goes directly
between the players' games, so **the other players in a P2P room can see your network (IP)
address**; the game says so. Play them with people you trust. In an online race the pause
button opens the room's menu over the running race (the race does not stop for the others).
Public races, Quick Race and the normal private races are not on phones yet.

**Check the download.** `SHA256SUMS` lists the APK's SHA-256. The APK is signed with the
Downhill Revived release key, certificate SHA-256
`D7:61:3E:E0:F0:93:95:22:D8:3B:06:CE:8A:05:ED:9D:03:E4:A9:96:8E:EA:2F:9C:83:98:58:94:14:56:AE:1B`
(`apksigner verify --print-certs`).
