# 125: Installers for the first install (Windows setup, macOS DMG, Linux AppImage)

Players asked for something that installs like other games, and a player could not get the Linux
zip running on a Steam Deck. From the release after 0.7.5 every platform has an installer next to
its zip. The installers are only for the first install: the launcher keeps updating from the
release zips (one update path, `update.rs`), so the zips stay as release assets. The AppImage is
the exception: it can't be updated file by file, so it replaces itself with the release's
AppImage (below).

User decisions (2026-10-31): installers for the first install only; installed copies keep the
player's files in the system's app-data folder, zips beside the game as before; the uninstaller
never deletes them; no code signing for now; scope Windows installer, macOS DMG, Linux AppImage.

## Where the player's files go

`port/launcher/src-tauri/src/game.rs` decides the one data folder (dhd.ini, saves/, dhd.log, the
update's work folder) and passes it to the game (`DHD_SETTINGS`, `DHD_MEMCARD_DIR`,
`DHD_DATA_DIR`). It now picks the system's app-data folder when the copy is installed:

| Copy | Installed when | Data folder |
|---|---|---|
| Windows setup | `installed.txt` is beside the launcher (the setup writes it, the uninstaller deletes it, updates never touch it) | `%APPDATA%\Downhill Revived` |
| macOS app | it is an app bundle (as before) | `~/Library/Application Support/Downhill Revived` |
| Linux AppImage | `APPIMAGE` is set and the launcher is inside `APPDIR` (the AppImage runtime sets both) | `$XDG_DATA_HOME/downhill-revived` or `~/.local/share/downhill-revived` |
| Zips | never | the game's folder (or app data when it can't be written, as before) |

The `APPDIR` condition keeps a launcher started from a terminal that inherited an AppImage's
variables from treating itself as one.

## Windows: Inno Setup, per user

`port/release/installer/windows.iss`, compiled by `package_release.py` through
`port/tools/installers.py`; `port/tools/innosetup.py` fetches Inno Setup 7.1.1 (pinned SHA-256,
Authenticode signature checked) and installs it portable (`/PORTABLE=1`) into
`DHD_DEVCACHES/innosetup`.

Why Inno Setup and not Tauri's NSIS bundler:
- `PrivilegesRequired=lowest` with `{autopf}` is exactly `%LOCALAPPDATA%\Programs\<app>`, per user,
  no administrator; Tauri's `currentUser` mode installs to `%LOCALAPPDATA%\<productName>`, and
  moving it means overriding its template.
- The uninstaller has to remove what later updates put in the folder (a newer release's files,
  `*.old`) while keeping the player's files. Inno's `[Code]` section does that in a few lines
  (`RemoveUpdateLeftovers`: every plain file at the top except the update.rs KEEP names; folders
  are the player's and stay). Tauri's uninstaller deletes the files it logged, plus an optional
  "delete app data" step we must never take.
- The script compiles the folder package_release has already checked, byte for byte. Tauri's
  bundler would run its own build/bundle step around the launcher and download its NSIS and plugins
  into the user's cache on the system drive.
- One small, readable script that is easy to review; Inno's licence is permissive (notice in
  THIRD-PARTY-NOTICES.txt).

The setup: Start menu shortcut, optional desktop shortcut (checked by default), Apps & features
entry (AppId `{930C9A04-B028-4A3A-8027-E06E5D50C889}`, also in game.rs `UNINSTALL_KEY`),
`CloseApplications=yes` so a running launcher or game is closed first, x64 Windows 10+.
After a launcher update of an installed copy, `update.rs` writes the new version into the entry's
`DisplayVersion` (`reg add`, no window), so Apps & features shows what is installed.

Unsigned: SmartScreen shows "Windows protected your PC" for a new file; More info > Run anyway.
Said plainly in README, PLAYING and README.txt.

## macOS: DMG

`installers.mac_dmg`, on the Mac after the app is signed ad hoc as before: the app copied with
`ditto` (keeps the signature) next to an `Applications` link, `hdiutil create -format UDZO`. The
check mounts the DMG read-only, compares every file of its app with the signed one, verifies the
signature on the mounted copy and refuses anything else on the volume.

The update's bundle swap works from `/Applications` as before: the app dragged there belongs to
the player, so renaming it to `.app.old` and moving the new one in needs no password. Started
from the mounted DMG, macOS translocates it and the update stops with update.rs's existing "move it
to Applications" message.

## Linux: AppImage (Steam Deck)

Why the zip fails on a Steam Deck: the launcher's window is WebKitGTK 4.1, which SteamOS doesn't
ship (and its root file system is read-only, so a player can't add it). The AppImage carries it.

`installers.linux_appimage`, on the Ubuntu 22.04 build system:
1. `tauri bundle --bundles appimage` on the checked launcher (`bundleMediaFramework` off). Tauri runs
   linuxdeploy with its GTK plugin: WebKitGTK, GTK and their libraries are copied from the build
   system, WebKit's helper processes are found relative to the AppImage, and the launcher's RUNPATH
   points at `usr/lib`.
2. Into a copy of that AppDir: the game exactly as checked (`usr/bin/dhd`; linuxdeploy's copy
   would have had its RUNPATH rewritten, and the game uses no bundled library: it loads the system's
   Vulkan, display and sound libraries as the zip does), the three text files in
   `usr/share/doc/downhill-revived/`, and `BUNDLED-LIBRARIES.txt`: every bundled file with its
   Ubuntu package, version and source package (`dpkg -S`, `dpkg-query`), with each package's
   `copyright` file in `usr/share/doc/<package>/`.
3. Checks on every file: the retail-bytes scan; other server hosts; the builder-folder and server
   strings in our files and every text file; every ELF's glibc symbol versions no newer than 2.35
   (package_release's `LINUX_GLIBC`); `build_launcher.problems` and the full symbol check on the
   launcher; a bundled file that no Ubuntu package owns stops the package.
4. Packed again with linuxdeploy's AppImage plugin, unpacked with `--appimage-extract` and compared
   file by file with the checked AppDir.
5. latest.json gets `platforms.linux-appimage` (url, sha256, size).

Updates: `update::platform()` is `linux-appimage` when the launcher runs from an AppImage, so it
reads that entry (or, from GitHub's release JSON, the `DownhillRevived-*-linux*.AppImage` asset),
downloads the new AppImage into the data folder's `dhd-update/`, checks its SHA-256, makes it
executable, renames the running image `.old` (a running AppImage can be renamed: its mount holds
the file, not the name), moves the new one in, and starts the launcher again from `$APPIMAGE`
(`Place::launcher`). `clean_up` deletes the `.old` image at the next start. Older launchers ignore
the new platform key.

## Release checks

`package_release.py` makes the installer after the zip unless `--no-installer`:
- Windows: `installed.txt` goes through the same checks as the package's files; the setup is
  built only from a folder of exactly those files; the setup file goes through the server,
  builder-folder and retail-bytes scans, and the absolute-path scan, where only paths that Inno
  Setup's own pinned program files hold are allowed (its developer's `C:\Coding\Is\...` paths and
  code bytes that spell a drive path) plus package_release's chance-bytes rule for the compressed
  payload.
- macOS and Linux: as above.

One trap found on the way: the builder-name check (`FORBIDDEN` holds the account name) refused
README.txt for the word "administrator", this machine's account name; the text says "admin
rights" now.

## AppImage traps found on the way

- **The relaunch after an update ran the old launcher.** The launcher started the new AppImage from its own
  process, which carries the AppImage runtime's state for the *old* image: its `APPDIR`, `APPIMAGE`,
  `ARGV0`, `OWD`, the GTK hook's paths into the mount, `PATH`/`LD_LIBRARY_PATH` entries in it, fd 1023 (the
  runtime's handle on its mount) and one end of the pipe that keeps the mount alive. The new image's
  runtime found the old mount through the inherited fd 1023 and started the old launcher from it, and the
  old mount stayed until that launcher closed. `Place::relaunch` now drops the image's variables (folder
  lists such as `PATH` lose only the mount's entries: without `PATH` the runtime finds no `fusermount`)
  and marks every open file above stderr close-on-exec (the old launcher exits right after).
- **appimagetool downloaded the AppImage runtime** (type2-runtime's continuous build) on every pack: a
  failed download stopped the package and the runtime could change between runs. It is pinned now
  (release 20251108, SHA-256 checked, cached in `DHD_DEVCACHES/appimage-runtime`, `LDAI_RUNTIME_FILE`).
- **Two release checks were quadratic or slow on 240 MB of libraries**: the regex for other servers' host names
  (tens of minutes on one library; now a literal search walking back, same results) and scan_retail
  rebuilding the ELF's window table per file (now built once). A package run takes minutes again.
- **The retail-bytes scan found the C library's digit string** `0123456789abcdefghijklmnopqrstuvwxyz`
  between zeros in the bundled JavaScriptCore and Kerberos libraries, as in the game's own C library at
  0x281D80; `scan_retail.PUBLIC` now covers the whole string (it held the hexadecimal part).
- Tauri patches the launcher binary in `target/release` with its bundle type before bundling; the
  AppImage's launcher is checked as it is in the image (`build_launcher.problems`, the symbol and
  library checks), not compared with the zip's.

## How it was checked

- Launcher: `cargo test` (installed marker, AppImage place, AppImage update, asset names, the relaunch
  environment).
- **Windows** (this PC), the setup built from the 0.7.5 files and again from this branch's build:
  silent install into a scratch folder puts every file byte for byte, makes the Start menu and desktop
  shortcuts and the Apps & features entry (the launcher's `reg add` of DisplayVersion works on it); the
  installed launcher, started on a hidden desktop, writes dhd.ini to `%APPDATA%\Downhill Revived` and
  nothing into the install folder; after a simulated update (a newer file, a `.old` launcher, a dhd.ini
  and `saves\` put in the folder) the silent uninstaller removes the installed files and the update
  leftovers, keeps dhd.ini and `saves\`, and leaves app data alone. package_release's setup checks pass.
- **macOS** (the M1 Air): package_release makes the zip and the DMG, mounts the DMG, compares every file
  of its app with the signed one and verifies the signature. The app dragged from the DMG into
  `/Applications` updated itself through the launcher (auto_update on, a local manifest naming the mac
  zip as 9.9.9): the bundle was swapped in place in 3 s, the restarted launcher asked for updates
  again, `.app.old` and `dhd-update/` were gone, the signature still verified. The test app was removed.
- **Linux** (WSL Ubuntu 22.04, the build system; launcher windows on a private Xvfb display): the
  AppImage passes package_release (172 libraries from 112 Ubuntu packages listed with licences, the
  unpacked image equal to the checked AppDir, glibc 2.34 at most). Started through FUSE, the launcher
  opens its window and writes dhd.ini to `~/.local/share/downhill-revived`, nothing beside the image.
  The game, in the AppImage's environment (the GTK hook's variables set as the launcher passes them),
  ran the race recipe headless through the menus to GO and raced 7 minutes without a fault. The
  launcher's own update (a local manifest's `linux-appimage` entry) replaced the image in 4-6 s, mode
  755; after the relaunch no runtime of the old image is left, one mount, the launcher runs from the
  new image with its window up.
- Steam Deck controls (static check): the game's SDL3 3.4.18 has the HIDAPI Steam Deck driver, its
  Linux joystick back end and built-in mappings for "Steam Deck", "Steam Deck Controller" and "Steam
  Virtual Gamepad" (what Steam Input presents to a game in Game Mode).

## Not checked / open

- No real Steam Deck, and no controller at all on Linux: WSL has no `/dev/input`. The Deck's built-in
  controls, Game Mode (gamescope) and SteamOS's FUSE are untested.
- The AppImage on a distribution other than Ubuntu 22.04, and on Wayland without XWayland.
- A real download of the installers through a browser: SmartScreen's prompt for the unsigned setup and
  Gatekeeper's for a quarantined DMG are described in the docs, not exercised (the tests installed
  local files).
- The Windows installer into its default folder (`%LOCALAPPDATA%\Programs\Downhill Revived`): the tests
  installed with `/DIR=` into a scratch folder on another drive (the system drive is nearly full); `{autopf}` with `PrivilegesRequired=lowest` is
  that folder by Inno Setup's documentation. Installing over a newer release, and the installer's
  CloseApplications with the game running, untested.
- The installers of this branch are 0.7.5-versioned test builds; the release builds come after the
  version bump, on each platform.
- Licence question for the release: the AppImage redistributes Ubuntu's binaries (LGPL and GPL ones
  among them) with their copyright files and a pointer to Ubuntu's source packages; whether to also
  host those sources is the user's call.
