## iPhone and iPad (experimental)

`DownhillRevived-<version>-ios-experimental.ipa` is the game for iPhone and iPad. It is
experimental: it has been run on few devices so far, so we can't yet say which ones hold 60 fps;
on a slower device OPTIONS > SETTINGS > Graphics tab > Performance Mode helps the most, and a
lower resolution scale (the same tab) helps too. Play with the touch controls (Classic pad or
Thumb zones; steer with the stick, by tilting the device or with a D-pad: OPTIONS > SETTINGS >
Controls tab > Touch Controls; Button Style shows the PS2 symbols (Symbols, the default)
or what each button does (Actions); Edit Layout moves and sizes each button, 25 % to 600 %, with undo;
iPads get their own larger defaults) or a controller (MFi, Xbox, PlayStation). A **SKIP** button ends
the flight over the track before a race, the movies and the title's demo. The game's cheat codes
are typed on the touch pause menu's **CODES** page. The iOS port was first made by Gustavo Ribeiro
(`ribeirogustav`).

**What you need**
- An iPhone or iPad with iOS 17 or later.
- Your own disc image of Downhill Domination (USA, SCUS-97177) as an `.iso` file, like the PC
  version. No game data comes with the app.
- A few GB of free storage: the disc image is copied into the app once.
- A computer (Windows or Mac) or AltStore/SideStore on the device, and an Apple ID, to sign the
  app while you install it.

**Install.** The IPA is **unsigned**: iOS runs an app from outside the App Store only once it is
signed for your device, and the sideloading tool does that with your own Apple ID.
1. Install a sideloading tool: AltStore or SideStore (on the device, with a helper on a computer
   the first time) or Sideloadly (on a computer, the device on a cable).
2. Open the IPA with it and sign in with your Apple ID when it asks. With a free Apple ID the app
   runs for 7 days, then the tool has to refresh it (AltStore and SideStore do that by themselves);
   with a paid Apple developer account, for a year. On iOS 16 and later, turn on Developer Mode
   once (Settings > Privacy & Security) when iOS asks for it.
3. Start Downhill Revived: it opens into the launcher (the same one as on Android and the PC).
   Choose your `.iso` there (from Files, iCloud Drive or a USB drive): it is checked and copied
   into the app once, which takes a minute or two. An `.iso` you copied into Files > On My iPhone >
   Downhill Revived can be chosen there too (it is moved into place, not copied). Then PLAY.

**The launcher** opens at every start: PLAY, the settings that are hard to change in the game
(graphics, touch, the rider name, online), the update line (a newer release is shown with a link
to its page; you install it with your tool), Help with **Share log**. Turn off **Open the launcher
at every start** to go straight into the game; SETTINGS in the game (Game tab, Launcher At Start)
brings it back. The launcher speaks English, Brazilian Portuguese, Spanish, French, German, Italian, Japanese and Simplified Chinese (machine-translated, corrections welcome): it follows the device's language, or pick one in Settings > Game > Language; the game's own menus stay in English. EXIT GAME closes the app; the next start opens the launcher again. If the game
closed unexpectedly, the launcher says so at the next start and offers Share log.

**Going back.** iOS has no Back button: in the menus swipe in from the screen's left edge (it is
the page's BACK), or tap BACK in the line at the bottom. When the keyboard is up for a room code or
a name, the bar above it has Cancel and Done.

**Leaving the app in a race** (the home screen, a call): the game waits and the sound stops; when
you come back an offline race is paused (the game's own pause menu) and goes on from where it
was. Online the race does not pause for the others: your rider stops while the app is away, and
iOS may close the room's connection when the app stays away long (more than a few seconds).

**Updates.** Install the new release's IPA over the old one with the same tool: your settings,
saves and the disc copy stay. Deleting the app deletes them.

**Your files** (Files > On My iPhone (or iPad) > Downhill Revived): `dhd.ini` (the settings),
`saves`, the disc copy and `dhd.log`. If the game closes unexpectedly, send us the log (the
launcher's Share log, or `dhd.log` from that folder) in a GitHub issue or on Discord.

**iPad**: the game is landscape. If your iPad stays upright, the game shows turned at the whole
screen's size: turn the iPad. In a Stage Manager or Split View window the game fills the window.

**Online: P2P races only.** ONLINE in the main menu offers Create A P2P Race and Join With A Code
(a 7-character code from a friend's P2P race, on a phone or a PC). The race data goes directly
between the players' games, so **the other players in a P2P room can see your network (IP)
address**; the game says so. Play them with people you trust. Public races, Quick Race and the
normal private races are not on phones yet.

**Check the download.** `SHA256SUMS` lists the IPA's SHA-256. The IPA carries no signature of ours;
the signature your tool adds is your own.
