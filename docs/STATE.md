# State

What is true now, per area, and where to read more. The dated log of what each lane changed is
`docs/STATE-LOG.md`; the findings index is `docs/findings/README.md` (read its area map, then the
current findings it names). This file names files instead of copying values that live in code:
the version is `port/include/version.h`, the oldest release the server accepts is `MIN_VERSION`
in `online/server/wrangler.jsonc`, player defaults are `port/render/settings/src/table.rs`.

## The project

- **Matching decompilation** of Downhill Domination (PS2, USA `SCUS_971.77`): readable C/C++ that
  rebuilds the retail executable byte for byte. PCSX2 is the behavioural reference.
- **Downhill Revived**, the PC port built from that source: `port/`, applied at build time, so
  `src/` keeps matching. Released for Windows, with macOS, Linux and Android as experimental
  builds (`port/release/`, the GitHub releases).

## Matching decomp: finishing

- Compiler: SN ProDG 3.01 (cc1 "2.95.3 SN BUILD 1.36" + `ps2eeas`), `-O2 -G8 -fopt-stack`; C++
  `-fno-exceptions` (findings 002, 003). A few files differ: SCE ee-gcc 2.9 (`-sce29`) and
  `-fno-opt-stack` / `-O0` clusters (finding 010, `docs/WORKER.md`).
- Build: `python tools/configure.py && python -m ninja`, checked by `tools/check_image.py`
  (finding 003). Workflow: `tools/queue.py`, `try_func.py`, `promote.py` (findings 007, 009);
  contributors read `docs/WORKER.md`.
- Progress: `python tools/report.py` (objdiff, `progress/report.json`, the README table). On
  2026-10-12 game 98.17 % and engine 92.61 % of code matched; the queue's last functions have
  behaviour-equivalent C under `#ifdef NONMATCHING` (finding 074; `configure.py --nonmatching`).
- Library regions (libgcc, the SCE SDK, libc) are identified (`config/libmatch.txt`) and stay
  assembly. Hand-written asm (`config/handwritten.txt`) stays `.s`; `.vutext`/DVP overlays stay
  binary.
- Open: what the `engine` region 0x201000-0x243000 is (engine/middleware, same toolchain); real
  TU boundaries (merge single-function units when `$gp` data or `.rodata` ownership shows them;
  func_001F24B8 and func_001C8380 have matching C waiting for their TU); turning shared structs
  into classes.

## The port: how it is built

- **One portable core:** `port/tools/build_wasm.py` compiles every `src/` unit (rewritten by
  `port/tools/transform.py` with the patches of `port/patches*.py`; a patch in code the port
  doesn't compile is refused, finding 165), the asm-only functions
  recompiled to C (`recomp.py`), the module-side runtime and HLE and the data image into one
  wasm32 module, `build/wasm/dhd_core.wasm` (findings 081, 082).
- **One host build script:** `port/tools/build_host64.py --target win-x64|linux-x64|macos-arm64|
  android-arm64` (wasm2c, the 64-bit host `port/host64` + `port/host`, the Rust library; finding
  099). `port/tools/build.py` runs core + host in one command. Setup: `docs/BUILDING.md`; what
  each step costs and what is cached: its "Build times" section (finding 151).
- **Browser:** `port/web/build.py build/wasm/dhd_core.wasm` (findings 083, 087).
- **Android:** `port/android/build_apk.py` (debug, `--release` with the release key named by the
  `DHD_ANDROID_*` variables; findings 091, 143). Android 10 (API 29) and later: calls from newer
  APIs are chosen at run time (finding 191).
- **Launcher:** `port/launcher` (Tauri; settings, rider name, updates, disc choice; finding 093;
  native window buttons on macOS, finding 150).
- **Releases:** `port/tools/package_release.py` builds, checks and packages each platform
  (`docs/BUILDING.md`, "Releases"; the `release` skill).
- Retired, do not bring back: the 32-bit i686 build, the Direct3D 11 renderer, the C wgpu-native
  renderer, the browser's JS renderer, the game's own updater and start window (findings 088, 093).

## The port: how it runs the game

- `port/hle/` replaces the PS2 system: EE kernel (threads as fibers, `port/host64/fiber.c`),
  crt0, SIF RPC, disc and file I/O, DMA, VIF0/VIF1, timers, pads, memory card, the sound driver
  (989snd replaced by `sound.c`, findings 022, 133), movies (`movie.c`, libmpeg2, finding 055).
- The game data image is read at start from the player's `SCUS_971.77` (SHA-1 checked,
  `game_data.c`); the executable holds only the words the port changes (finding 053).
- Inline asm runs on an emulated EE/FPU/VU0 register file (`port/runtime/ps2asm.c`,
  `ps2asm_aot.py`); EE float rounding follows PCSX2's (finding 026). VU0 microprograms are native
  or compiled (`vu0_micro.c`, `vu0_aot.py`); VU1 runs compiled blocks plus native kernels
  (`vu1_aot.py`, `vu1_native_clip.py`), checked against PCSX2's interpreter (findings 030, 090),
  and, with Native VU (the players' default on every platform, the browser included: findings
  198, 199), the translated batch loops as GPU shaders instead (findings 164, 167, 194). The
  emulated path for those loops remains as the fallback and the setting's off until it is deleted
  (root AGENTS.md).
- GS: GS memory and registers in C (`port/gs/`); the Rust renderer (`port/render`, wgpu: DX12 or
  Vulkan on Windows, Metal on Apple, Vulkan on Linux and Android, WebGPU in the browser) behind
  the shared C front ends `gpu_exact.inc` (exact: the PS2's pixels, the reference) and
  `native_core.inc` (native, upscaled, the player default). Findings 084-086, 088, 131. A lost GPU
  device ends the game with a message box instead of a frozen picture (desktop and Android; tried on
  Direct3D 12; finding 171). Dual-source blending is off on Intel GPUs (`device::dual_source`;
  `DHD_WGPU_DUAL_SOURCE=0|1` overrides; finding 188): an Ice Lake GPU hung on it (issue #58).
  Upscaled bilinear sprites interpolate at output resolution (foliage and particles); only
  nearest-filtered sprites retain GS-pixel snapping (finding 196). On Vulkan with timeline
  semaphores every submit gives each semaphore a value (binary waits 0; vendored wgpu-hal
  `vulkan/semaphore_list.rs`, finding 201), and dhd.log says which way submits signal.
- **The game clock:** one delta-time clock at every frame rate, 60 included and online: each
  paced frame steps by the real time since the last, between one step at the selected rate and two
  fields (finding 139, `port/hle/fps.c`, `port/hle/kernel.c`). Deterministic runs keep the
  original clock, so state traces are unchanged. Replays, the attract demo and ghosts run on a
  field grid (finding 142). The track's actors (helicopters, birds, animals, walkers, crowds) step
  by game time too (`port/patches_fps.py`, finding 193); a few steering loops are still up to
  25 % off at 240 (issue #76).
- Host services: one code path on SDL3 for every platform (finding 099): `host_sdl.c` (files,
  clock, frame waits, display, threads, the error box), `sdl_host.c` (SDL's loop on the main
  thread, the game on its own thread), `present.c` (the window), `sound_out_sdl.c` (one sound
  output), controllers through SDL only (findings 059, 134; Swap L1/L2 and R1/R2 per controller,
  finding 189), touch controls on phones (`port/hle/touch.c`, finding 141; the game's codes from
  the touch pause menu, finding 189).
- dhd.log on every platform, with `perf:` lines on speed and what limits it (finding 129) and
  `net:` lines online (finding 135). What the game can detect goes in the log. Each start keeps
  the one before as dhd-previous.log; on Android the log says how the run before ended (Android's
  exit record and tombstone; on Android 10 only the game's own crash report, finding 191), and a
  crash dialog and the icon's shortcut share it (finding 152).

## What players get

- REVIVED SETTINGS at the top of the game's options, with Display, Graphics, Controls, Online
  and Game tabs (finding 163), saved to dhd.ini (findings 064, 101): resolution scale
  (capped by GPU memory and said so, finding 136), window mode, widescreen with the HUD anchored,
  no 4:3 screens left and VU1 sprites at their 4:3 shape (findings 056, 140, 192), frame rate 60-240 or Monitor (findings 067, 139),
  V-Sync, texture packs in PCSX2's format (finding 057), the sharp font (finding 058), a
  Performance group (shadows and sun glow, lens flare, particles; finding 183), Native VU
  (on by default: VU1 programs as GPU shaders, falls back to the emulated VU when
  the GPU can't; findings 164, 167, 194, 198, 199), movies,
  vibration, stick dead zone and button icons (finding 134; also over a texture pack's font,
  finding 200), the keys and controller buttons for
  the PS2 pad's buttons, per player, in the game and the launcher (Controls, finding 157), FPS &
  Ping counter (findings 120, 169), online name size (finding 163), Unlock All (finding 095;
  applied at once, finding 200),
  EXIT GAME. dhd.ini has one implementation, the Rust crate `dhd_settings`
  (`port/render/settings`), used by the game, the launcher and the browser.
- The launcher holds settings, rider name and updates on every desktop platform, decides the one
  data folder (`DHD_SETTINGS`, `DHD_MEMCARD_DIR`, `DHD_DATA_DIR`), updates from `latest.json`
  with a backup through the server's `/v1/latest` (findings 093, 125, 138). A phone has no
  launcher: the game checks once a day and shows a notice (finding 143).
- Discord Rich Presence on the desktop (finding 106). The mouse works in the port's pages and the
  game's popups (finding 092).

## Online play

- SFU rooms (public, private, Quick Race) on Windows, macOS and Linux; P2P rooms there and on
  Android and in the browser, which offer nothing else (`host_service_net_offered`, findings 143,
  180, 186).
- **State sync, never lockstep:** each PC runs its own one-player race at its own frame rate and
  shows the others, and on guests the host's AI riders, as puppet riders moved from snapshots
  (`port/hle/puppet.c`, findings 044, 047, 114). Hits are events; each game times its own finish.
  Track pickups are shared events (finding 144); a player who leaves becomes an AI rider.
- Server: `online/server/` (Cloudflare Workers + Durable Objects) at `wss://dhd.skuza.app`, the
  game's default (dhd.ini `server =` or `DHD_NET_SERVER` override it). Rooms of up to 10
  (finding 123) that live across races (findings 118, 119), 25 tracks with Salt City MX and TD
  (finding 166) listed in Super Career order (finding 190), Ready for the next race and MOSH by
  the host's pick (finding 128), rooms placed where their creator picks (ONLINE's Room Region row,
  dhd.ini `room_region`) or, on Auto, by the Cloudflare location the creator reached, from measured
  round trips (findings 135, 181), rejoin by token
  (finding 137); a room connection lost in a race no longer ends it: the game races on over the
  SFU and rejoins, the seat is kept 20 s (finding 175). A room runs at most 5 Realtime calls at
  once and a timed-out call is an unknown outcome, not a failure (finding 182). A deploy
  (`online/server/tools/deploy.mjs`: a watched 10 % canary, then 100 % or a rollback) restarts
  rooms for the new code only in the lobby or at results; games rejoin in about 2 s (finding 184).
  Protocol: `docs/online-protocol.md`; steps: `online/server/README.md`.
- Abuse guard (finding 161): a kill switch in the moderation database (`moderate.mjs online
  off`), limits per address, caps per member, and
  bans by address hash or install id with a 14-day moderation log in D1, worked from
  `online/server/tools/moderate.mjs` (no admin endpoint). In an SFU room no player gets another's address, its
  hash or install id.
- P2P rooms (findings 180, 186): the creator's choice beside SFU rooms, private, by code (7
  characters). A server room like any other (its rules, results, rejoin, region) whose race frames
  travel over WebRTC data channels in a star around one member, the hub, which the room picks (the
  creator, the next one when it leaves or can't do its work) and which forwards the others'
  frames; a member who can't reach it directly races over the room's SFU, bridged by the hub. The
  game warns that the room's players can see your network address. One send thread and one star
  router (`port/render/star`) for the native games and the browser page. Windows and macOS tested; Android
  builds it but was not run; the browser joins and races on the star (no SFU fallback there yet).
- Race frames (findings 158, 177): over Cloudflare Realtime SFU DataChannels, STATE unordered with
  no retransmissions and EVENT reliable (Cloudflare forwards STATE ordered and reliable anyway; the
  client's vendored sctp-proto delivers it unordered, finding 173). One Realtime session per member
  per room stay (finding 177): made when the member joins, kept across races with its
  publications and subscriptions (a member who sits a race out loses its subscriptions at that
  race's start and gets them back at its next grid); a race's missing pairs are subscribed at the
  grid's lock, during the countdown, so a connected room is released as soon as its games
  acknowledge the start. The room passes each member only its own SFU answer, channels and TURN
  credentials, never a session id. If UDP doesn't connect in 2 s, Cloudflare TURN over TLS on 443
  with credentials minted for that member, renewed between races once 35 minutes old. A racer
  whose Realtime never connects leaves the race 10 s after its start message and the others race
  on (finding 175); failed subscription closes are retried and a rejoin's channel list replaces
  recycled channel ids (finding 179). A session on which the room closed subscriptions its game
  never had has dead stream ids on Cloudflare's side (finding 187): the room replaces it once that
  costs no race anything. A game whose race link brings nothing from the others 8 s after its
  release tells the room, which gives it a new session in its race or takes it out of the race;
  the host's start moment also goes over the room, and a game with no moment 75 s after loading
  leaves the race instead of starting alone.
- Protocol 4 (0.7.13, finding 169): a PC's riders of one tick go out bundled in STATE messages of
  at most 1,000 bytes (one datagram), stamped with their moment on the server clock (the PONG
  carries the server's time); each player's own rider 60 times a second, AI riders 30. Puppets are
  drawn at the present: the newest snapshot carried forward by its velocity and angular velocity,
  corrections fading in. Hits are
  decided on the puppet as drawn. In a room the FPS & Ping counter shows "SERVER N MS", the round
  trip to the room's server, and "RACE N MS", the round trip to Cloudflare's race network (the SFU
  association's ICE checks); in a race the second line is "RIDERS N MS", the other riders'
  snapshot delay (findings 169, 181).
- Client: `port/net/` over `port/net/ws.h`, the Rust crate `dhd_net` (`port/render/net`:
  tungstenite over rustls for the lobby, str0m for the SFU with a vendored RustCrypto adapter,
  its own TURN client; webpki roots, no OS certificate store).
- Online needs the USA disc's game files (a hash of every ISO 9660 file, `dhd_disc`; finding 098).
- Releases with online or pacing changes need a real race between two different machines both
  ways (single-PC tests have hidden bugs before).

## Checks and tools

- One entry point: `python tools/check.py docs|quick|port|matching|server` (root AGENTS.md,
  "Done means"). Docs alone: `python tools/check_docs.py`.
- Unit tests: `python port/tools/test_runtime.py` (after `build_wasm.py`); test programs run where
  the game runs their code (finding 104). Renderer: `test_gs_wgpu.py`, replay goldens recorded
  locally (`gs_golden.py`), `test_native_transfers.py`. Game state: per-field state traces
  (`port/hle/state_trace.c`, `state_trace_run.py`, `state_diff.py`), `test_fps_race.py`,
  `rate_drift.py`, `slow_race.py`, `replay_check.py`. Server: `npm test` in `online/server`.
  Online: `port/net/build_tests.py`, `port/tools/online_race.py`. Retail comparisons with PCSX2:
  `compare_pcsx2.py`, `race_compare.py` (finding 024). Everything, with setup variables:
  `docs/TESTING.md`.

## Open

- Phones: on the low-end profile (Realme RMX3930 (Unisoc T612), reproduced on a throttled Mi 11i)
  2x races hold 60 with the profile-guided build; finding 195 added 5.6-17 % over the route
  (t612 2x 64.4 -> 68.0 fields/s, 4x 55.5 -> 64.5; more with Native VU on) and finding 197 another
  7 % with Native VU off (race start 61.5 -> 65.9, the route 67.7 -> 72.4; on: +1 %); the release
  APK needs a profile recorded with its file-name keys (`docs/BUILDING.md`; finding 197's is
  `dhd-197.profdata`). 4x does not hold 60 at the race start (GPU-bound). Findings 127, 146-149,
  195, 197 say what is left (197: the VU thread alone limits now); finding 156 says where a
  frame's time goes on the phone and the desktops.
- PowerVR GE8320 phones start since finding 170 and no longer close in the Bike Shop (finding 176:
  a blend that keeps the colour is drawn unblended, a pipeline the driver refuses skips its draws);
  not yet confirmed by the reporters (issues #56, #60).
- The PowerVR GE8322 (driver 1.18) closed at the first picture's submit (finding 201: wait values
  the driver reads unchecked); fixed by the Vulkan submit rule above, not yet confirmed by the
  reporter (issue #82).
- The Mali-G52 (Redmi 9) at 2x: about 49.5 fields/s with every effect on (finding 174's
  barriers), about 60 with the shadows, sun glow, lens flare and fog cut by a diagnostic build;
  Performance Mode (finding 183) cuts those but fog. 60 at 2x with every effect on is not reached.
- Native VU is the players' default (finding 198), and it is slower where the GPU limits: the
  Redmi 9 at 2x 50.6 -> 48.7 fields/s over the route (the race 47.3 -> 44.8), the integrated
  Radeon at 4x 122.1 -> 118.2; it gains where the CPU limits (finding 194). No driver that refuses
  it has run it. The emulated path is deleted once players' logs show no fallbacks.
- On Mali GPUs a picture stays in one encoder (finding 198): finding 195's encoder cut lost the
  device on the Redmi 9's Mali-G52 in every full route, so 0.7.15 is expected to lose it there
  (fixed in 0.7.16).
  Why the driver fails, and other Malis, are not known.
- Browser speed is below 60 in races (finding 087).
- High-rate handling differs a little from 60 by the race's own chaos, with no trend (finding
  139, after finding 067's limits).
- Rich Presence on macOS and Linux is built but not run against Discord there (finding 106).
