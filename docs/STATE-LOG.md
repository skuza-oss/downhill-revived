# State log

Dated entries, newest first: what a lane or round changed and how it was checked. What is true
now is in `docs/STATE.md`; the details are in the findings it names. Older round logs (2026-10-12
to 2026-10-25) are in the findings and in `git show v0.3.1:docs/STATE.md`.

Add an entry when a lane merges: date, finding, one paragraph. Do not copy values that live in
code (versions, MIN_VERSION, defaults): name the file instead.

2026-11-06 (finding 171, issue #58): a lost GPU device (a driver reset after a GPU hang, DXGI_ERROR_DEVICE_REMOVED on Windows) no longer leaves a frozen picture with the sound going on: the game logs the reason (on D3D12 the removed reason; with DHD_GPU_DRED=1 where the GPU stopped), shows a message to update the graphics driver and exits. The device is not recreated and there is no automatic fallback to Vulkan. DHD_TEST_DEVICE_LOST=N tests it.

2026-11-06 (findings 157, 158, 161; the 0.7.12 release review): two review rounds fixed puppets that no longer interpolated (every snapshot cleared their history), a peer able to close another's connection through the early-event queue, ban and limit messages shown as "connection closed", the room's SFU setup and teardown when a racer leaves or fails (a 25 s setup deadline, retried cleanup), the TURN channel renewal (240 s, inside the 5-minute permission), R1 kept bound on every device (it switches REVIVED SETTINGS' tabs), and the launcher's and game's binding waits. A PC-Mac race passed both ways, also with the Mac forced onto TURN over TLS. `MIN_VERSION` in `online/server/wrangler.jsonc` now matches protocol 3 (0.7.11 can't race with this release); the server deploys with the release.

2026-11-06 (finding 158): online race frames travel over Cloudflare Realtime SFU instead of the room's WebSocket: each player connects to the nearest Cloudflare location over UDP, a lost snapshot is skipped instead of holding the others back, and a player whose UDP is blocked falls back to Cloudflare TURN over TLS on 443. No player receives another's address, SDP or TURN credentials. Snapshots go out 30 times a second and the puppets' delay adapts to each sender. On one PC the median snapshot age in a race went from 71-90 ms (the 0.7.11 relay through the official server) to 19-45 ms (33-54 ms through TURN). Protocol 3: the release that ships it raises `MIN_VERSION`; the deploy needs the Worker secrets in `online/server/README.md`, and a real race between two machines is still to run.

2026-11-06 (finding 161): the online server can now refuse abuse without a game update. A kill switch in the moderation database (`node tools/moderate.mjs online off`) pauses new rooms and joins; limits per connection source and per member stop floods; joins are logged for 14 days with a keyed hash of the address (never the address) and an install id, and bans by either are checked before a room is reached. The operator uses `online/server/tools/moderate.mjs` with their own login; there is no admin endpoint, and no player ever receives another's address, hash or install id (tested). The install id is in dhd.ini from the next release; the deploy's one-time setup is in `online/server/README.md`.

2026-11-06 (finding 165): the build refuses a patch whose text lies in code the port never compiles (NONMATCHING functions run their recompiled assembly). It found two: the online spectate resume (after any pause the spectator's camera stayed held: fixed, the hook now runs) and a harmless timer edit. Also fixed: an online race that closes while the pause menu is open now returns to the room after RESUME instead of running on until the next START.

2026-11-06 (finding 166): online rooms offer Salt City MX (up to 6 riders) and Salt City TD (up to 8): the original game has no split-screen levels for them, but online every PC loads the one-player level. 25 tracks now; Dual Slalom (a one-on-one career event with 2 start places) stays out. No server change.

2026-11-06 (finding 163): PC SETTINGS is now REVIVED SETTINGS, first in OPTIONS, with tabs (Display, Graphics, Controls, Online, Game) switched with L1/R1. The in-game Controls page edits the main binding only (Cross: press the new key or button; Square clears; changes save at once; the alternate shows dimmed and is set in the launcher). New setting `names`: online player names Off / Small (default, smaller than before) / Medium (the old size) / Large.

2026-11-06 (finding 159, issue #54): the touch controls never showed in a career continued from a save: that route starts the race without the GET READY screen the port took as "a race". One test, `port_race_running` (the race's own world update sets it, the menus clear it), now serves the touch controls, the port's grounded-stall recovery (which also never ran in continued careers), dhd.log's clock lines and Discord presence. New-game races unchanged (state traces equal).

2026-11-06 (finding 160): the blocks of other pictures in the first seconds after start (and before a race) are gone: the game resets the GS there, which on a PS2 also turns the display off until the next screen is set up, and the port kept showing the framebuffer the game was loading textures into. A GS reset now clears the display registers, the native renderer shows black with no display circuit on, and a movie's last picture stays until the game changes the display, as the PS2 shows. Checked against a retail PCSX2 boot field by field.

2026-11-06 (finding 157): keys and controller buttons can be rebound, in the game (PC SETTINGS > Controls: press the key or button) and in the launcher (Settings > Controls > Bindings): a main and an alternate per PS2 button, a layout for the keyboard and for each of two controllers, swap sticks and invert per stick axis. dhd.ini `keyboard`, `pad1`, `pad2` hold only the changes; the defaults are the old layouts (state traces equal). Esc always backs out and pauses; a change that would leave a player without Cross, Triangle, START or a d-pad direction is refused.

2026-11-05 (finding 154): the rider was missing from the bike shop (inventory) on phones, with a dark wedge in his place: widescreen at shapes wider than about 2:1 moved the shop panel's corner behind the camera, so the panel was drawn mirrored over him. Phones are 20:9 with widescreen on; 21:9 and 32:9 PC monitors had it too. Past 16:9 the corner keeps the 16:9 patch's value; 16:9 and narrower are unchanged. Checked on the Mi 11i and on Windows at 20:9, 21:9 and 32:9.

2026-11-05 (finding 155): the Android build targets ARMv8.0 (`-march=armv8-a -moutline-atomics`, was ARMv8.2): phones with Cortex-A53/A73 cores such as the Snapdragon 680 closed at start with an illegal instruction (issues #45, #51). Atomics still use LSE where the core has it.

2026-11-05 (finding 153): phones with a Mali GPU on Arm's r54 driver (Galaxy A07, Redmi with a Unisoc UMS9230E; issue #47) closed at startup with the native renderer: the driver's shader compiler crashes on a texture passed into a shader function. Each texture now has its own load function; the pictures are byte-identical, and a test checks that no shader passes a texture or sampler. Confirmed by a tester's Mali phone.

2026-11-05 (finding 152): Android players can send a crash log without a computer. After a crash the next start shows "Downhill Revived closed unexpectedly last time" with the reason Android recorded (crashed, the graphics driver crashed, ran out of memory, stopped responding), Share log (Android's share sheet, one text file with the phone, Android's record and tombstone summary, and the logs) and Play; the icon's long-press has Share log too. The POSIX crash handler now drains its report into dhd.log, can probe the stack safely, and on Android hands the crash on to the system's crash dump (tombstone, CRASH_NATIVE). Every platform keeps the run before as dhd-previous.log. Checked on the Mi 11i with a game-thread crash at start.

2026-11-05 (finding 151): faster builds on every platform. The core build remembers the recompiler's signatures, the objects' signatures and its key's file hashes (a one-line game change 388 -> 224 s CPU, 6,748 -> 832 processes); the object cache prunes once a day (it walked 460,000 entries for 30-65 s after every build with a miss); a split module's wasm2c C is cut into content-defined chunks with headers of their own, so a change recompiles the chunks it touches (Mac: one-line changes 42-81 -> 23 s, a new port function 110 -> 22 s; Android one-line game change 215 -> 120 s CPU); the module's C is reused when its inputs did not change (Mac rebuild with no change 11.5 -> 2.0 s); SDL3's download and libraries are shared by checkouts (Windows 186 -> 0.3 s); the browser's wasm-opt passes are cached (no change 455 -> 9.5 s CPU). The same source gives the same core (sha256); the Windows executable is identical apart from its link stamp; boot 600 and race 5000 state traces match, also for a chunked x64 build. Windows keeps one `core.c` by default (`build.py -- --num-outputs 8` for a development loop).

2026-11-04 (finding 150, PR #39): the launcher uses native macOS window controls over its title bar; the HTML minimize and close buttons are hidden on Mac. Windows and Linux keep their custom controls. Checked by the contributor on macOS (cargo check, close/minimize/restore/drag by hand).

2026-11-04 (finding 149): on the throttled phone a bilinear's texel footprint is one gather everywhere (conversions too) and the alpha test runs before the colour (4x GPU -2.1 ms a frame), the GS and render threads stay on the little cores when there are only two big ones, a swapchain view is kept instead of a new framebuffer every frame (render thread -3.1 ms), and two GS loops store less; 2x race 55.2 -> 59.5 fps at the race start (61.6 with PGO: 60 held throughout with PGO), window 66.1 -> 70.1; 4x race start 43.2 -> 45.9 (GPU-bound), the rest 57.6-63.2 (was 51.5-56.4), window 55.4 -> 62.1.

2026-11-04 (finding 148): the slow race interval is the race start, where the flare's Z read made the game wait for VU1 every frame; queued behind the drawing, plus three GS/fill changes, the throttled phone's race start went 31.5 -> 55.8 fps at 2x (58.8 with PGO) and 31.4 -> 42.9 at 4x (GPU-bound); 2x holds 60 except the race start, 4x does not.

2026-11-04 (finding 147): unlocked phone window 3400-5800 at FPS=240 ends before GO (6321); actual race slow intervals are ~31.5 fps without PGO at 2x/4x, 36.1 with PGO at 4x. Matrix-clamp hoisting passed correctness but gained nothing and was rejected; 60 fps is not reached.

2026-11-03 (review fix, finding 137): the name rule let any player with the same name take a member over, host
included. Now the server sends each member a random rejoin token in its welcome, the game sends it
back in its rejoin's hello, and only the token takes a member back (at once, no 4 s wait); a
same-name hello without it is a new member. Needs the next release's client and MIN_VERSION.

2026-11-03 (finding 142): race replays, the attract demo and the time trial's ghost work again on
the delta-time clock, at every rate. The recorder is an input recording the replay re-simulates
and checks (the player's z every 60 samples), so an offline race now runs on a field grid: one
sample per field of race clock (60 a race second at every rate, the format and the seven-minute
buffer unchanged), the step chosen when a field ends as a code in 1/120 field in the sample's
step byte, the pad read when a field ends, one-field steps from the load to GO; a replay forces
the recorded codes and paces its pictures by them. Headless race + replay (`replay_check.py`):
exact for all ten riders at 45 (frame clock) and paced 144 with uneven frames. The attract demo's
camera cuts and two-minute end count fields; the ghost samples every 8 fields at every rate.
State trace unchanged. Replays saved on the original clock (PS2, port up to 0.7.7) do not play on
the delta-time clock: since the review fixes they say so over the countdown and end at GO.

2026-11-03 (review fixes, findings 139 and 142): about twenty units read the integer steps a
second as their time step (1/rate: the rigid-body integrator, the contact solver); the delta-time
clock truncated it, 59 at paced 60, so bodies moved 1.2-1.6 % too far online, in the menus and in
the attract demo. They now read the exact step (`port_step_seconds` / `port_step_rate`). The
pacing grids (`pace()`, the slow stretch) re-anchor when the frame period changes (a 9.7 s wait
at the load after 144 fps, 0.8 s on a replay's step change). The item list timer's flag clear
and 60 mark fire on two-field steps. Old-clock replays explain themselves. The headless browser
runner answers the touch service.

2026-11-03 (finding 143): the next release can ship an experimental Android APK. `build_apk.py
--release` builds it not debuggable, without R8, signed with a release key the environment names
(RSA 4096, certificate SHA-256 D7:61:3E:E0:...:AE:1B, kept by the maintainer outside the
repository) and checks it with apksigner and aapt; `package_release.py --platform android` puts it
in the release folder with the licence texts inside, a `latest.json` `android` entry and
`SHA256SUMS`. A phone has no launcher, so the game checks once a day (the server's `/v1/latest`,
then GitHub's latest.json; the HTTPS client follows redirects now) and shows one line, tap to open
the release page; nothing is downloaded. Online play is offered on Windows, macOS and Linux only:
one host switch (`host_service_net_offered`) hides ONLINE and refuses every connection on Android
and in the browser. Desktop state traces unchanged.
Review fixes (finding 143, "Review fixes"): the first start checks and copies on a worker thread
(a cloud document no longer freezes the screen; a streamed document is copied first, then
checked), a short copy fails (the source's size, else the ISO volume's), each copy owns its
temporary file; the release check takes only the release key's certificate
(`--new-signing-key` for a deliberate change); a stale server answer no longer hides a newer
GitHub release from the update notice. Second round (finding 143): the update page can't leave
the repository by dot segments or backslashes (game, phone, server; the launcher's rule), and
`build_apk.py --check` refuses a wrong package in words. Finding 139: the item list's timer rests
at 0, not -1, on a two-field step.

2026-11-02 (finding 132): the online room names tracks as the game does (retail's event names
read from the player's data image, e.g. "Mt. Midoule FR", "Mosh Pit (Arena)") everywhere it
shows one, and the track picker shows the selected track's loading-page picture, read from the
player's disc at run time (nothing in the repository; no generated art needed). Checked on the
hidden desktop: all 23 picker views, a race afterwards; the lead's checks are in the finding.

2026-11-02 (finding 133, issue #19): Mt. Liddellroch's ambience is no longer too loud. 989snd's END
grain (type 24) stops its sound and releases its voices (retail IOP 0x5A60 -> 0xFE04); the port only
ended the program, so `E_TREE_WHOOSH`'s wind loop, started at every tree passed, never stopped (35
copies at once a minute into the race; PCSX2: 5). Sound records' solo flag and instance limits
(byte +5, flags 8/0x10/0x20; 0x4A34, 0xF34C, 0x10038) now apply as in retail. Against a PCSX2
recording of the same race (a capture hook in the comparison adapter): Liddellroch FR race 0-60 s
-24.71 dBFS PCSX2, -22.54 before, -25.18 after; ambient voice power +13.3 dB -> +3.4 dB (most of
the rest is samples shared with rider sounds); Mt. Midoule FR -25.03 PCSX2, -24.77 before, -25.02.
`DHD_SND_VOICE_TRACE` dumps each sounding voice for such comparisons. State traces: the END fix
changes only the PCM column; with the limits the race trace's RAM also differs, in the sound queue
and handle words only (the game keeps the refused plays' replies).

2026-11-02 (finding 135): rooms are made near their creator and everyone can see where they are.
`/v1/rooms/new` gives the room's Durable Object a location hint from the creator's continent and
country (`online/server/src/region.ts`), the room learns its own colo from a Cloudflare trace, the
welcome and a new `where` message say where the player and the room are, and the public list
carries each room's region in the asker's region's order (0.7.6's Quick Race then prefers a near
room too). dhd.log gets `net:` lines: on joining (this PC's Cloudflare location, country, the
room's location), every 30 s in a room or race and on leaving (own ping median and worst, every
other player's ping and region; another player's country is never sent). The list rows and the room's header show the region
(`PLAYERS 2/10 - EU SERVER`). The list's ENDS IN / NEXT IN / RACING times no longer count from the
epoch before a room is joined (0.7.6 showed ENDS IN 29893031:32): the list carries the server's
`now`. Server tests 73/73; two games on hidden desktops logged every line.

2026-11-02 (finding 136): the resolution scale the GPU allows is no longer hidden. dhd.log has
one line with the scale in use, `capped` when it is under the one asked, the limit and its reason
with the figures (`resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (graphics
memory: ...)`), Match window too. PC SETTINGS offers every scale and marks the ones over the
limit (`8x (running 4x: GPU memory)`, footer `This GPU's memory allows up to 4x`); the launcher
reads the last run's line from dhd.log and says the same under Resolution (`Your GPU's memory
allows up to 4x; 8x runs at 4x`, choices `8x (runs at 4x)`). The cap's rule is unchanged. Boot
state trace unchanged.

2026-11-02 (finding 134, issues #20 and #27): controllers. A stick dead zone setting
(`stick_dead_zone`: Game, the PS2's own, about 36% of the travel, by default; or 5-50%, the travel
past it spread over the bytes that steer in proportion, `port/hle/stick.h`), in PC SETTINGS and a
new Controls tab in the launcher with a live picture of the controller's sticks and what the rider
gets. Button icons (`prompts`: Auto from player 1's controller, PlayStation, Xbox, Nintendo): the
game's face-button icons drawn as our own dark discs with the letter in the game's font, L1/R1/L2/R2
as LB/RB/LT/RT (L/R/ZL/ZR), at draw time in the text packet (`port/hle/prompts.c`). `DownhillRevived
--play` starts the game at once without the launcher's window (Steam shortcuts, the Steam Deck).
Boot state trace with default settings equal to next/0.7.7's.

2026-11-02 (finding 131): the native renderer's draw pass from first principles. A diagnostic,
`DHD_GPU_STATS`, gives GPU time and pixels shaded per kind of draw: a race frame shades about four
screens of pixels, two of them one blended terrain kind, at three times the cost per pixel of a
plain fill. Each draw now gets a pipeline specialised for its state (built on a worker thread; the
generic one draws until it is ready), texel bounds tests stay but cost what they must, and decoded
textures are R32Uint texels read by one gather per bilinear footprint. The Ryzen 5 7600 iGPU now
holds 60 fps paced at 5x (was 4x); GPU time per frame is 22-24% lower at every scale (4x uncapped
76.5 -> 102.6 fps, 12.02 -> 9.11 ms; 8x 21.5 -> 28.0 fps, 45.13 -> 35.30 ms; DXC alike). The RTX at
8x is unchanged (CPU-bound; GPU 1.92 -> 1.68 ms). Oracle goldens, the replay matrix on both GPUs
and APIs, `test_gs_wgpu.py` and the native-transfer tests pass; VRAM is identical everywhere. One
picture change remains for a decision: a `round()` that makes the generic and specialised shaders
agree draws some pixels one step differently on DX12 than before (none on Vulkan; DXC up to 0.6% of
a menu picture's pixels at 4x, about 0.003% of race pixels). The AMD iGPU varies a few pixels from
run to run (also before; no application race found, finding 131 lists the checks).

2026-11-02 (finding 137, issue #30): no more clones in online rooms. A game whose connection
dropped rejoins with the same hello; the server gave a member back by name only when that member
had no socket, but with a dead connection its old socket stays open on the server for minutes, so
the rejoin became a second member (and a host's rights stayed with the dead copy). Now a hello
takes back the same-named member whose socket has been quiet 4 s (waiting up to that when it spoke
just before), a 1006 drop outside a race keeps the member 20 s for its rejoin, and a socket silent
20 s is dropped. Server only: deploying it fixes 0.7.7. Server tests 81/81.

2026-11-02 (finding 138): the launcher's update check has a backup. When GitHub's releases API fails
(no connection to api.github.com, its 60-an-hour limit, 5xx, a bad answer), the launcher asks the
online server's new `GET /v1/latest`. That endpoint serves the newest release's `latest.json`,
fetched from GitHub's release download, kept fresh for 5 minutes, with the last good copy kept for
when GitHub fails, and checked to name only this repository's release assets. No release step.
A player's own `update_url` works as before, without a backup. The bottom bar now says what went
wrong in one line ("No internet connection.", "GitHub is limiting update checks from your
connection; try again in an hour.", "Couldn't reach GitHub or the backup server (your connection
timed out)...", "Update failed: the download couldn't reach GitHub (...)"). The full errors go to
the new `launcher.log`. Server tests 88/88, launcher tests 27 pass. The endpoint needs a server
deploy.

2026-11-02 (finding 139): one delta-time game clock. Every paced frame, at every frame rate (60
included) and online, the game steps by the real time since the last frame, between one step at
the selected rate and two fields, smoothed so the steps add up to wall time; no debt, no
fast-forward. It replaces the fixed 1/60 step at 60 (slow motion below 60 fps), the one-field cap
above 60 with its repaid debt (the speed ramps), the online extra fields and `DHD_PACE=wall`. The
mixer gets wall time, so the sound keeps its speed too. Slow PC made on the RTX desktop
(`DHD_SLOW_FRAME`): at 45 fps the game went from x0.75 to x0.999 at FPS 60 and 120, the worst
second at 85 fps from x0.87-x1.13 to x0.97-x1.04, the sound's time-stretching from 57 to 5-6 s a
minute. Two-field steps broke 12 of finding 067's per-frame counters (the post-race countdown never
ended, the action lock stuck, ...) and made riders "airborne" on every bump (at 30 steps a second
nobody finished the scripted race): fixed. The race at 30-144 steps a second now finishes with no
trend against 60 (player -7..+6%, AI mean -0.5..+4.1%: the race's own chaos, finding 067).
Deterministic runs keep the original clock: the state trace is unchanged. V-Sync keeps three swapchain buffers, so a missed refresh no longer halves 120 to 60.
Ghosts/replays stay off on the delta-time clock, now at 60 too (finding 142 brings them back).

2026-11-01 (finding 128): online rooms keep Ready across track changes (TRACK CHANGED line), MOSH
only by the host's pick, and the shop's bike, jersey and bike-colour pages online (issue 18).
Server tests 64/64; two local games on hidden desktops: each saw the other's choice in its log.

2026-11-01 (finding 129): every player's dhd.log says how fast the game ran and what held it back
(`port/gs/perf_log.c`, on by default, `DHD_PERF_LOG=0` off): at start the GPUs with their kind and
the one used, the present mode, and a `perf: render` line (scale, picture and window size, rate,
pacing, V-Sync, refresh, GPU timer); every 30 s of race or menus in front, and per kind at exit, a
`perf:` line with fps against the requested, 1% low, frames over 1.2x the budget, each thread's
CPU and busy ms per field, the render thread's wait in the present, the GPU's ms per frame (one
frame in 8 timed by DHD_GPU_TIMES' queries) and `limit=gpu|cpu:<thread>|vsync|none`. The graphics
queue's wait counters now count by default. Boot state trace unchanged; 1x uncapped 243.3 -> 241.1
fields/s (one run each). Windows only built and run; no lines in the browser.

2026-11-01 (finding 130): Windows `DHD_WGPU_ADAPTER=integrated|discrete` selects hardware through
DXGI's power/performance preference order, with the player default unchanged. The Ryzen 5 7600
iGPU holds the nominal 60 fps target at 4x, up from baseline 3x. The dedicated-VRAM guard can
clamp high requests; `DHD_SCALE_MEMORY_MB` supplies a logged developer estimate for UMA measurements.
Exact bounded integer scanout, duplicated-row reuse and power-of-two texture pipeline specialisation
reduce 4x uncapped FXC GPU time from 18.81 to 11.73 ms (50.52 to 78.48 fps, fresh controls).
The full matrix's true 8x improves from 13.79 to 22.19 fps, GPU 71.80 to 43.81 ms; 5x/6x/8x
remain below 60. Final Vulkan is slower at 2x and effectively tied at 8x. Renderer/host GS tests,
oracle replays, strict fixed-stream 8x comparisons on both GPUs/APIs and boot traces pass.
Finding 130 contains the final paced/unlocked tables, 1% lows and pass splits; wall-time stalls
remain, and the RTX-connected monitor's transport cost is not isolated. Player defaults unchanged.

2026-10-31 (finding 124): PERU no longer logs "vu1: microprogram at 0x1F50 didn't end" (0.7.4,
0.7.5: about a million interpreted VU1 steps in a frame after earlier races). The weather object's
four per-view batch records were never initialised, so a zero-density emitter left an old particle
float there, which the microprogram read as thousands of particles (PCSX2 runs the same input as
long). A `port/patches.py` patch clears them in `func_00226D08`; with stale pool data seeded, a
PERU state trace of the fixed build equals the unfixed build's clean run from the constructor on.
Opt-in VU1 diagnostics: `DHD_VU_CAPTURE_FAILURE`, `DHD_VU_CAPTURE_ENTRY`, `DHD_VU_CAPTURE_EE`.

2026-10-28 (finding 105): the exact renderer no longer runs out of GPU memory while minimised
(0.6.4 died at the menu after 30-60 s, 0xC0000409): each frame waits for the previous frame's
GPU work (`Exact::frame_end`), and an out-of-memory device error now ends with a log line, an
error box and exit code 1. 5 minutes minimised through menus and a race: memory flat.

2026-10-26 (finding 090, SP3): native GS vertex conversion and texture-page key iteration
cost less CPU; one x64 race A/B measured GS busy 4.571 -> 3.807 ms/field.
Exact, native and interpolated replay goldens and all 198 transfer golden items match;
x64 packaging and the Rust browser build/layout pass. Exact/probe paths and game core unchanged.

2026-10-26 (finding 090, SP6): fixed a retired-queue observation race that could wait after
completion; added opt-in per-field cycles and host/GPU stall attribution. One race pair observed p99 73.803 -> 17.763 ms (not an isolated speedup estimate), with unchanged-counter
timeouts 2,443 -> 513. All 4,500 race state-trace fields match with the existing mask; x64 packaging,
Rust browser build/layout and GS FIFO stress pass. Details and the legacy VIF fixture link limit
are in finding 090. No game core or GPU operation changed.

2026-10-26 (finding 090, SP7): x64 core and packaging pass. Two race pairs retain padded queue counters
and the 512-pause retired/full-ring spin policy: 104.548 -> 141.226 and
121.783 -> 124.349 fields/s. EE spin falls in both; second-pair p99 worsens
21.869 -> 24.024 ms. Run-to-run variability prevents an isolated speedup claim.
Full busy/spin/park tables and limitations are in the SP7 section.

2026-10-26 (finding 090, FPS1): at 240 the two counters that stayed per-update now
advance per field behind `port_fps_high()`: the particle/effects frame clock
`D_0029DBAC` (dust, snow and spray waves, glow phases) and the sound/skat queue age
`obj->x60C`; the rate audit (`DHD_RATE_AUDIT`, off by default) now measures them at a quarter
of their 60 Hz travel per update (0.250 and 1.002 against 1.000 and 4.004 before), which is
time-based motion at 240. Boot 600- and race 5000-field state traces against the base build
match in every column with the mask unchanged (3); x64 packaging, Rust browser build and layout
pass.
Measured words, attribution and the audit's false positives are in the FPS1 section.

2026-10-21 (finding 074): the 25 functions not matched yet have behaviour-equivalent C under
`#ifdef NONMATCHING` next to their `INCLUDE_ASM` (18 new single-function units). The default build
keeps the asm and still matches; `python tools/configure.py --nonmatching` builds the C into
build-nonmatching/ (links, not checked; sizes and the moved ranges are in the finding).
func_00220DA0 is now listed in config/handwritten.txt (trapping adds, pmfhi/pmflo).

2026-10-11: six more matches (00137300, 00137B70, 00148F60, 001554C0, 00171B28,
00228BE8), including two translation-unit merges. The rebuilt image matches all eight
checked retail sections.

2026-10-06: `func_00134CC8` and `func_00240358` promoted after exact function and
whole-image checks. The latter required integration into its existing full translation unit.
Finding 009 distinguishes an assigned-function implementation from a matching assembly stub,
and records serialization of report generation with source promotion. A fresh full-image check
matched all eight checked sections.
