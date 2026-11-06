# Findings index

Read this index first. Every finding has one `Status:` line under its title:

| Status | What it means for you |
|---|---|
| `current` | Describes the code as it is. Trust it, and still check the code it names. |
| `partly superseded by NNN (<part>)` | The named part was replaced (read NNN for it); the rest, and any "Still true" section, is current. |
| `superseded by NNN` | The design was replaced. Do not follow it; read NNN. |
| `history` | Done or dropped; true when written, not needed to work on the code today. |
| `rejected` | Tried or researched and declined. Read it so you don't retry the same idea blindly. |
| `in progress` | A lane's plan, reserved on its first commit; becomes `current` at merge. |

`Checked:` is the day the finding was last compared with the code. History-only findings were
slimmed to a summary; each ends with the `git show` command for its full text. The table at the
bottom is generated: `python tools/findings_index.py` (and `tools/check_docs.py` fails when it is
out of date). New findings: copy `TEMPLATE.md` to the next free number on the lane's first commit.

## Where to start, by area

| Area | Read first (current) | Older, for history only |
|---|---|---|
| Matching decomp | 002 compiler, 003 build, 004 small data, 012 C idioms, 013/014 asm forms, 074 NONMATCHING, `docs/WORKER.md` | 001 |
| Port core (wasm32 core, 64-bit host) | 081 plan, 082 host runtime, 088 one 64-bit build, 099 one code path, 104 tests | 015, 017, 018, 020 (32-bit parts) |
| Game clock and pacing | 139 delta-time clock, 142 replays on it, 052 just-in-time frames, 026 vblank wait | 027, 036, 049 (survey still true), 067 |
| Renderer (GS, Rust/wgpu) | 021 GS core, 084 WGSL, 085/086 Rust renderer, 088 CL2, 131 draw pass, 045/136 scale | 025, 031-033, 035, 038 |
| VU0/VU1 | 016, 019, 030, 090 (VU1R, OP3, SP1) | 027, 029, 037 |
| Audio and movies | 022, 133, 055 | |
| Online (state sync) | 044 puppets, 047 race protocol, 114 any frame rate, 118/119 rooms, 123, 128, 135, 137, 144, `docs/online-protocol.md` | 039-042 (lockstep parts) |
| Menus, settings, input | 050, 064, 092, 101/102 settings, 059/134 controllers, 141 touch, 056/140 widescreen | 065 (WM_CHAR part) |
| Launcher, releases, installers | 093, 125, 138, 143, 098 disc check | 053, 054, 070 |
| Speed (PC) | 089, 090, 129 perf log, 130/131 iGPU | 031, 032, 046, 048, 051, 066, 069 |
| Android and phones | 091, 107, 108, 126/127 low-end profile, 143, 145, 146-149 | |
| Browser | 083, 087, 102 | |

## Numbers not in this folder

005, 006, 008, 011, 071, 073 and 076-080 were never used (or were drafts that did not land).
075 and 113 have no file: they were branch numbers of 081 and 139, and code cited them until
2026-11-04; the last rows of the table map them.

## All findings

<!-- index:begin -->
| No. | Finding | Status | Read instead |
|---|---|---|---|
| 001 | [Compiler identification (superseded)](001-compiler-identification.md) | superseded | [002](002-compiler-sn-prodg.md) |
| 002 | [Compiler identified: SN ProDG 3.01 (ee-gcc 2.95.3 SN BUILD 1.36 + ps2eeas)](002-compiler-sn-prodg.md) | current |  |
| 003 | [Build system: retail image from asm + C, byte-identical](003-build-system.md) | current |  |
| 004 | [How ps2eeas addresses small data](004-ps2eeas-small-data.md) | current |  |
| 007 | [Quick check before the full trial build](007-quick-check.md) | current |  |
| 009 | [Assigned-function validation and shared build serialization](009-assigned-function-and-shared-build.md) | current |  |
| 010 | [Some engine code was built with SCE ee-gcc 2.9; -fno-opt-stack files](010-sce-gcc-and-flags.md) | current |  |
| 012 | [C idioms that steer SN ee-gcc 2.95.3 (from sibling PS2 decomps)](012-sn-gcc-matching-levers.md) | current |  |
| 013 | [the game's `Abs` is a volatile `abs.s` asm helper](013-volatile-abs-helper.md) | current |  |
| 014 | [libgcc's `umul_ppmm` is longlong.h inline asm](014-libgcc-umul-ppmm.md) | current |  |
| 015 | [PC port, starting point and plan](015-pc-port-plan.md) | partly superseded | [088](088-cleanup.md) for the 32-bit host of step 1; the renderer choice of step 7 |
| 016 | [VU0 microprograms and the inline asm runtime](016-vu0-microprograms.md) | partly superseded | [044](044-puppet-riders-state-sync.md) for lockstep as the reason for bit-exact VU0 |
| 017 | [PC data conversion and link census](017-pc-data-and-link-census.md) | history |  |
| 018 | [Static recompilation of the remaining MIPS functions](018-static-recompilation.md) | partly superseded | [088](088-cleanup.md) for the i686 link it ran in |
| 019 | [Graphics pipeline: engine draw paths and VU1 microprograms](019-graphics-pipeline.md) | partly superseded | [090](090-speed.md) for one native routine per VU1 entry; meshes as vertex shaders |
| 020 | [PC runtime: memory, kernel, IOP, DMA and VU0 (boot, disc, input step)](020-pc-runtime-boot.md) | partly superseded | [088](088-cleanup.md) for the 32-bit memory map, relaunch, Windows fibers, the then-missing parts |
| 021 | [Native renderer](021-native-renderer.md) | partly superseded | [088](088-cleanup.md) for the D3D11 window and present, i686/x87 details |
| 022 | [Audio: 989snd on libsd, native on the PC](022-audio.md) | partly superseded | [099](099-one-code-path.md) for WASAPI output; the open lists of the rounds |
| 023 | [Shared animation and vector headers](023-shared-headers.md) | partly superseded | [088](088-cleanup.md) for x87 return checks of the i686 build |
| 024 | [Automatic PCSX2 rendering comparisons](024-pcsx2-compare.md) | partly superseded | [088](088-cleanup.md) for i686 build steps, build_compare_port.py and link.py |
| 025 | [PCSX2's GS as the port's renderer (research)](025-pcsx2-gs-backend.md) | rejected |  |
| 026 | [Frame pacing: the vblank wait and the virtual clock (2026-10-14)](026-frame-pacing.md) | current |  |
| 027 | [Real-time speed: round 19 (2026-10-15)](027-real-time-speed.md) | history |  |
| 028 | [Round 20: the late-race crash and the GPU readback wait (2026-10-15)](028-crash-and-readback.md) | current |  |
| 029 | [Round 21: the struct-indexing audit, VU1 from inside, the graphics worker (2026-10-15)](029-vu1-profile-and-audit.md) | partly superseded | [030](030-vu1-recompiler.md) for DHD_MTVU opt-in |
| 030 | [Round 22: VU1 recompiler experiments (2026-10-15)](030-vu1-recompiler.md) | current |  |
| 031 | [Round 26: waits and broad native performance](031-broad-native-performance.md) | history |  |
| 032 | [Frame critical path investigation](032-frame-critical-path.md) | history |  |
| 033 | [Native renderer direction](033-native-renderer-direction.md) | history |  |
| 034 | [Late native-state nondeterminism](034-late-native-uninitialized-inputs.md) | current |  |
| 035 | [Opt-in hardware coverage bridge](035-hardware-raster-bridge.md) | history |  |
| 036 | [Simulation and rendering at different rates](036-high-refresh-gameplay.md) | rejected |  |
| 037 | [Native geometry CPU investigation toward 120 fps](037-native-geometry-120fps.md) | rejected |  |
| 038 | [Native renderer and interpolated high-refresh presentation (round 29)](038-native-renderer-high-refresh.md) | partly superseded | [088](088-cleanup.md) for the D3D11 native renderer, off by default; gate_native_renderer.py |
| 039 | [Online server (lobbies and the race relay)](039-online-server.md) | partly superseded | [044](044-puppet-riders-state-sync.md) for INPUT/HASH lockstep relay, input delay, iso SHA-256 |
| 040 | [Online four-player race: what the game already does, and what the port must add](040-online-four-player.md) | partly superseded | [044](044-puppet-riders-state-sync.md) for the lockstep split-screen race plan |
| 041 | [Online races in the port: four pads, full-screen view, launcher (first design: input lo...](041-online-integration.md) | partly superseded | [044](044-puppet-riders-state-sync.md) for the input-lockstep race path, the friends' launcher |
| 042 | [Online client: lobby and WebSocket transport (and the dropped lockstep core)](042-online-client.md) | partly superseded | [044](044-puppet-riders-state-sync.md) for the lockstep core; the WinHTTP transport, now Rust dhd_net in 099 |
| 043 | [Round 30: sound stutter, native flicker, exact-frame cost, VU1 load](043-round30-sound-flicker-speed.md) | partly superseded | [088](088-cleanup.md) for gate/build tools, the "still to do" list |
| 044 | [Puppet riders: state-sync online races](044-puppet-riders-state-sync.md) | current |  |
| 045 | [Round 31: resolution scale for the native renderer](045-native-resolution-scale.md) | partly superseded | [086](086-rust-renderer-play-fixes.md) for scale 1x-4x, file names, the gate tool |
| 046 | [Round 32: VU1 thread speed (native 07F0 kernel, VIF stream unpack)](046-vu1-thread-speed.md) | current |  |
| 047 | [Online races with friends: start, riders, grid, hits, finish, leaving, launcher](047-online-friends-race.md) | partly superseded | [118](118-online-flow-074.md) for the boot-time lobby, scripted menus, the friends' zip, the 30 fps limit |
| 048 | [Round 33: game logic overlapping the drawing (deferred vsync), measured and not shipped](048-deferred-vsync-ee-overlap.md) | partly superseded | [090](090-speed.md) for the revert: deferred vsync shipped in 090 HO1 |
| 049 | [Real frames above 60: the game's frame clock (research)](049-real-high-frame-rate.md) | partly superseded | [139](139-delta-time-clock.md) for the verdict and plan for real 120; the 60-only launcher |
| 050 | [PC settings and online screens in the game's own menus (research), first-start disc prompt](050-native-menus.md) | partly superseded | [093](093-launcher.md) for the first-start disc prompt, Launcher.ps1 |
| 051 | [Round 34: where a race field's CPU time goes, and the first fixes](051-cpu-critical-path.md) | current |  |
| 052 | [Frame pacing and input latency (round 38)](052-frame-pacing-latency.md) | partly superseded | [139](139-delta-time-clock.md) for the fixed 59.94 Hz grid at 60; D3D11 present details |
| 053 | [The first public release: reverb from the disc, a reproducible build, the zip (round 39)](053-public-release.md) | partly superseded | [088](088-cleanup.md) for the 32-bit release build, its zip and checks |
| 054 | [Online start gates, the version gate and the auto-updater (round 40, 0.2.0)](054-online-versioning-updater.md) | partly superseded | [093](093-launcher.md) for the game's own updater |
| 055 | [The movies play (round 40)](055-movies.md) | partly superseded | [140](140-widescreen-everywhere.md) for movies shown at 4:3 |
| 056 | [Widescreen HUD at any shape, player defaults, EXIT GAME](056-widescreen-hud-defaults.md) | partly superseded | [101](101-one-settings-implementation.md) for player_defaults in host.c, the disc prompt, 4:3 cards; since 154 the bike shop corner past 16:9 |
| 057 | [Round 40: texture replacement packs (upscale mods) in PCSX2's format](057-texture-packs.md) | current |  |
| 058 | [the game's font drawn sharp at high resolutions](058-sharp-font.md) | current |  |
| 059 | [keyboard play and generic controllers (SDL3)](059-keyboard-generic-pads.md) | partly superseded | [099](099-one-code-path.md) for XInput/raw-HID fallback, the i686 SDL build |
| 060 | [Native progressive field jitter](060-menu-jitter.md) | current |  |
| 061 | [Pickup flare investigation (round 45)](061-pickup-flare.md) | current |  |
| 062 | [Data fixup audit and reference refresh (round 46)](062-data-fixup-audit.md) | history |  |
| 063 | [Visual sweep (round 48)](063-visual-sweep.md) | current |  |
| 064 | [Player settings across restarts](064-settings-persist.md) | current |  |
| 065 | [Keyboard text entry and online selection flow](065-keyboard-text-and-lobby.md) | partly superseded | [099](099-one-code-path.md) for the WM_CHAR text path |
| 066 | [CPU speed toward 144 real steps/s](066-cpu-speed-144.md) | partly superseded | [090](090-speed.md) for VU0 AOT opt-in; the i686 build |
| 067 | [Real offline simulation steps (experimental; equivalence not established)](067-fps-unlock.md) | partly superseded | [139](139-delta-time-clock.md) for the fixed 60 clock, the 144 cap and debt, online forced to 60, replays off |
| 068 | [Visual sweep corrections (round 51)](068-visual-sweep-fixes.md) | current |  |
| 069 | [Independent performance review of the combined CPU/FPS build](069-perf-review-astra.md) | current |  |
| 070 | [the start window and the shader cache](070-start-window.md) | history |  |
| 072 | [Level starts, online restarts and widescreen (2026-10-18)](072-level-start-and-rematch.md) | current |  |
| 074 | [NONMATCHING C for the last unmatched functions (2026-10-21)](074-nonmatching-c.md) | current |  |
| 081 | [A portable 64-bit core (plan)](081-portable-core.md) | partly superseded | [088](088-cleanup.md) for the phase status; the 32-bit description is the then-state |
| 082 | [Independent wasm2c x64 core host runtime](082-host64-runtime.md) | partly superseded | [099](099-one-code-path.md) for "unproven" status, per-OS build scripts |
| 083 | [Direct wasm core in Node (first web-route proof)](083-web-core.md) | partly superseded | [087](087-browser-60fps.md) for the i686 core link, the JS renderer |
| 084 | [The wgpu renderer — moving the GS from D3D11 to WebGPU](084-wgpu-renderer.md) | partly superseded | [085](085-rust-renderer.md) for the C wgpu-native backend, gpu_wgpu.c |
| 085 | [The Rust (wgpu) GS renderer: spike report](085-rust-renderer.md) | current |  |
| 086 | [Fixes from a play test of the Rust renderer](086-rust-renderer-play-fixes.md) | current |  |
| 087 | [Browser speed toward 60 fps](087-browser-60fps.md) | current |  |
| 088 | [Cleanup: one 64-bit build on the Rust renderer](088-cleanup.md) | current |  |
| 089 | [Where the time goes (x64 and browser, race)](089-profile.md) | current |  |
| 090 | [Game speed round 1 (x64 to 144 fps)](090-speed.md) | current |  |
| 091 | [Android arm64 build (task AND1)](091-android.md) | partly superseded | [143](143-android-release.md) for the known gaps: touch, online, release key; the first speeds; since 155 also -march |
| 092 | [online flow, centred menu layer, mouse (UI1)](092-ui.md) | partly superseded | [128](128-room-ready-bikes.md) for bike and outfit online "not done" |
| 093 | [the player launcher (DownhillRevived.exe)](093-launcher.md) | current |  |
| 094 | [Crash after a Super Career finish that does not advance (issue #8)](094-career-finish-crash.md) | current |  |
| 095 | [PC SETTINGS "Unlock All" reads, never writes](095-unlock-all.md) | current |  |
| 096 | [Sky mosaic from high jumps (issue #2), native renderer](096-sky-mosaic.md) | current |  |
| 097 | [the lens flare's Z probe caused the race stutter (issue #2)](097-lens-flare-probe-stutter.md) | current |  |
| 098 | [a player couldn't create a public room (the disc image), and the log said nothing](098-online-disc-image-refusal.md) | current |  |
| 099 | [one code path for Windows, macOS, Linux and Android](099-one-code-path.md) | current |  |
| 100 | [Other releases: what supporting the European disc (and others) would take](100-other-releases.md) | current |  |
| 101 | [dhd.ini, one implementation (dhd_settings)](101-one-settings-implementation.md) | current |  |
| 102 | [dhd.ini in the browser through dhd_settings; the rider name's rule in the crate](102-browser-settings-crate.md) | current |  |
| 103 | [one copy of the GS glue's decisions, one process runner for the test tools](103-glue-runner.md) | current |  |
| 104 | [test programs on the shipped paths](104-tests-on-shipped-paths.md) | current |  |
| 105 | [the exact renderer ran out of GPU memory while minimised](105-exact-minimised-oom.md) | current |  |
| 106 | [Discord Rich Presence](106-discord-rich-presence.md) | current |  |
| 107 | [Current Android profile versus PCSX2](107-android-profile-vs-pcsx2.md) | current |  |
| 108 | [Where a 120 field/s frame goes on the phone](108-phone-120-measure.md) | current |  |
| 109 | [Vulkan framebuffer reuse across encoder resets](109-vulkan-framebuffer-cache.md) | current |  |
| 110 | [VU1 toward 120 fields/s on the phone: lean clipping frame and AOT readiness skip](110-vu1-phone120.md) | current |  |
| 111 | [GPU submission overlap and same-field native Z probes](111-gpu-overlap-zprobe.md) | current |  |
| 112 | [Fewer native render passes](112-fewer-render-passes.md) | current |  |
| 114 | [online races at the player's frame rate](114-online-high-fps.md) | current |  |
| 115 | [The race room, laid out (layout B)](115-race-room.md) | partly superseded | [132](132-track-picker.md) for no track thumbnails |
| 116 | [Track thumbnails for the race room (research, not built)](116-track-thumbnails.md) | partly superseded | [132](132-track-picker.md) for "not built": 132 built it |
| 117 | [The race room's CHEATS setting, and where the game reads cheat codes](117-room-cheats.md) | current |  |
| 118 | [The 0.7.4 online flow in the game: room rules, back to the room, quick race](118-online-flow-074.md) | current |  |
| 119 | [Session rooms (0.7.4)](119-session-rooms.md) | current |  |
| 120 | [The FPS & Ping counter](120-perf-counter.md) | current |  |
| 121 | [Online crash when a remote rider throws a bottle](121-puppet-bottle-crash.md) | current |  |
| 122 | [Watch the rest of a room race after finishing](122-online-spectate.md) | current |  |
| 123 | [Ten players in an online room](123-online-ten-players.md) | current |  |
| 124 | [PERU weather inherits particle floats as VU1 loop counts](124-peru-vu1-loop.md) | current |  |
| 125 | [Installers for the first install (Windows setup, macOS DMG, Linux AppImage)](125-installers.md) | current |  |
| 126 | [Android bench, 2026-10-31 — the low-end phone (Realme RMX3930, Unisoc T612)](126-android-bench-1031.md) | current |  |
| 127 | [The Mi 11i throttled to a Unisoc T612: where a 1x field goes, and 39.6 -> 55 fields/s](127-phone-t612-throttle.md) | current |  |
| 128 | [Ready for the next race, MOSH by choice only, bikes and jerseys online](128-room-ready-bikes.md) | current |  |
| 129 | [perf: lines, how fast the game ran and what held it back, in every dhd.log](129-perf-log.md) | current |  |
| 130 | [Integrated GPU performance](130-igpu-performance.md) | current |  |
| 131 | [The draw pass from first principles: where an iGPU frame's time goes, and what is left](131-gpu-first-principles.md) | current |  |
| 132 | [Track picker names and disc previews](132-track-picker.md) | partly superseded | [166](166-online-more-tracks.md) for the 23-track list: 25 since 166 |
| 133 | [Ambient sound too loud on Mt. Liddellroch (issue #19)](133-ambient-sound.md) | current |  |
| 134 | [controllers: stick dead zone, button icons, starting without the launcher](134-controllers.md) | current |  |
| 135 | [where the player and the room are, in dhd.log and in the room list, and rooms made near...](135-net-region.md) | current |  |
| 136 | [The resolution scale the GPU allows, said where the player picks it](136-scale-cap.md) | current |  |
| 137 | [rejoining players no longer clone themselves in online rooms](137-rejoin-clones.md) | current |  |
| 138 | [the update check falls back to our server, and its errors say what went wrong](138-update-check-fallback.md) | current |  |
| 139 | [one delta-time game clock](139-delta-time-clock.md) | current |  |
| 140 | [Widescreen everywhere: no 4:3 screens left](140-widescreen-everywhere.md) | current |  |
| 141 | [touch controls for phones](141-touch-controls.md) | current |  |
| 142 | [race replays, the attract demo and the ghost on the delta-time clock](142-delta-time-replays.md) | current |  |
| 143 | [the Android APK as a release asset (release key, update notice, no online on phones)](143-android-release.md) | current |  |
| 144 | [Shared online track pickups](144-shared-online-pickups.md) | current |  |
| 145 | [Android black screen on an Adreno 710 (the gralloc lock in the surface format query)](145-android-gralloc-format-hang.md) | current |  |
| 146 | [The throttled Mi 11i at 2x and 4x paced 60: where a race field goes, and what moved it](146-phone-60.md) | current |  |
| 147 | [Unlocked phone measurements must reach the race](147-phone-race-unlocked.md) | current |  |
| 148 | [The race start was a serial chain: the flare's Z read made the game wait for VU1](148-phone-race-start.md) | current |  |
| 149 | [The throttled phone at 4x: one gather per bilinear texel footprint; the render thread's...](149-phone-gpu-texels-and-render-thread.md) | current |  |
| 150 | [Native macOS launcher window controls](150-macos-launcher-window-controls.md) | current |  |
| 151 | [Faster builds on every platform](151-build-speed.md) | current |  |
| 152 | [Crash logs players can keep and share (Android first)](152-crash-log-sharing.md) | current |  |
| 153 | [The native renderer's pipelines on Mali's r54p1 shader compiler](153-mali-shader-compiler-crash.md) | current |  |
| 154 | [The rider missing from the inventory screen on phones](154-menu-rider-missing-on-phones.md) | current |  |
| 155 | [The Android build for every arm64 core (ARMv8.0)](155-android-armv8-baseline.md) | current |  |
| 156 | [Where a frame's time goes, before replacing the VU1 and GS emulation](156-profile-vu1-gs.md) | current |  |
| 157 | [Keyboard and controller rebinding, in the launcher and in the game](157-control-rebinding.md) | current |  |
| 158 | [race frames over Cloudflare Realtime SFU, and puppets with less delay](158-online-sfu-transport.md) | current |  |
| 159 | [Touch controls gone after loading a career save](159-touch-gone-after-save-load.md) | current |  |
| 160 | [Garbage blocks on screen in the first seconds after start](160-boot-garbage-frames.md) | current |  |
| 161 | [online abuse guard: kill switch, rate limits, bans and a short moderation log](161-online-abuse-guard.md) | current |  |
| 163 | [REVIVED SETTINGS: tabs, a clearer Controls page, online name size](163-revived-settings.md) | current |  |
| 165 | [no port patch in code the port does not compile](165-dead-patch-guard.md) | current |  |
| 166 | [Online races on the tracks without a split-screen level (Salt City MX and TD)](166-online-more-tracks.md) | current |  |
| 075 | no file: the portable-core plan was numbered 075 on its branch and merged as 081 | renumbered | see 081 |
| 113 | no file: the delta-time clock work was numbered 113 on its branch; the mixer's VBLANK time is in 139 | renumbered | see 139 |
<!-- index:end -->
