## Android (experimental)

`DownhillRevived-<version>-android-experimental.apk` is the game for Android phones and tablets.
It is experimental: recent phones hold 60 fps at 2x resolution, slower ones (and 4x) may not.
Play with the touch controls (Classic pad or Thumb zones, tilt steering if you like: OPTIONS >
SETTINGS > Controls tab > Touch Controls) or a controller.

**What you need**
- Any arm64 phone or tablet with Android 11 or later and Vulkan (most phones since 2018),
  older arm64 cores such as the Snapdragon 680's and Mali GPUs on Arm's newer drivers included.
- Your own disc image of Downhill Domination (USA, SCUS-97177) as an `.iso` file, like the PC
  version. No game data comes with the app.
- A few GB of free storage: the disc image is copied into the app once.

**Install**
1. Download the APK on the phone.
2. Open it. Android asks once to allow installs from your browser or file manager ("Install
   unknown apps"); allow it, then install. The app is not on Google Play.
3. Start Downhill Revived and choose your `.iso` when asked. The copy takes a minute or two.

**Updates.** The game checks once a day whether a newer release is out and says so at the top of
the screen; tap the line to open the release page, then install the new APK over the old one. Your
settings and saves stay. Nothing is downloaded by the game itself. If you installed a test build
before this release, uninstall it first: Android refuses an update signed by another key, and
uninstalling deletes the app's files (the disc copy and your saves).

**If the game closes unexpectedly**, it says so at the next start and offers **Share log**: send
the file to us (a GitHub issue or Discord) and we can see what happened. Long-press the app's icon
for **Share log** at any time, even when the game closes before anything shows. Nothing is sent
unless you pick an app in Android's share sheet.

**Not on phones yet.** Online play comes to phones later, once they run fast enough and the touch
controls are ready; the Android build has no ONLINE menu.

**Check the download.** `SHA256SUMS` lists the APK's SHA-256. The APK is signed with the
Downhill Revived release key, certificate SHA-256
`D7:61:3E:E0:F0:93:95:22:D8:3B:06:CE:8A:05:ED:9D:03:E4:A9:96:8E:EA:2F:9C:83:98:58:94:14:56:AE:1B`
(`apksigner verify --print-certs`).
