# 086: Fixes from a play test of the Rust renderer

Status: current
Supersedes: parts of 045
Checked: 2026-11-04

2026-10-25, branch `render/rust`. One section per task.

## W1: Browser on the native (upscaled) renderer

The browser now draws with the native renderer, the same one x64 uses: the shared C front end
`port/gs/native_core.inc` and the Rust module `port/render/src/native`. Only the glue is new:
`port/web/gpu_rust_web_native.inc`, modelled on `port/gs/gpu_rust_native.inc`. Native is the
default. The exact renderer stays available for checks:

- `?env.DHD_RENDER=exact` selects it.
- `?env.DHD_SCALE=N` sets the resolution scale. The default is 2. It is capped at 4
  (`native_core.inc`'s default `NATIVE_SCALE_MAX`); S1's 8x is x64 only.
- The `js` (no-Rust) build is exact only.

### How it works

- **One thread.** The glue defines `NATIVE_INLINE`, so `gs_native_vsync` draws a field inline:
  the browser has no render thread, and `DHD_INTERP` is not used.
  - `native_present` only marks the picture as pending.
  - `web_display` returns the picture's size when one is pending.
  - `web_present(canvas_w, canvas_h)` then calls `native_show`, which does
    `dhdr_native_present` and `dhdr_surface_present`.
  - JavaScript's pacing stays as it was.
- **Reads that need the GPU** (the Z probe and the display read) are split into begin,
  `env.web_gpu_await` (JSPI, the same as exact uses), then finish. On wasm32,
  `native/scanout.rs` maps the readback through `web::wait::map_read`.
- **The native renderer is created at the core's first `gs_reset`**, in `web_gpu_reattach`, not
  at `web_gpu_enable`, which runs before the core's `main`. By `gs_reset` the settings are loaded.
  - worker.js's WASI environment is now built from the live `env` object, which the core's
    `host_putenv` updates.
  - The reactor calls wasi-libc's `__wasilibc_deinitialize_environ()` before it reads
    `DHD_RENDER`, `DHD_SCALE` and `DHD_WIDESCREEN_ASPECT`, so it sees the current values.
- **The Z probe in the browser.** The browser's GS reads are not deferred: `host_callback_fault_guard`
  returns 0 in wasm, so `graphics.c` reads through `gs_read_image` and then `g->sync_range`. As a
  result, `native_stage_pages` (the bookkeeping that copies the native Z back into the exact GS
  memory) never ran, and the race's probe read stale depth. `native_sync_range` now stages the
  pages first, then calls the exact `sync_range`.
  - It does this only for a fresh local-to-host read (TRXDIR 1) of whole 8 KiB pages, so CLUT and
    texture syncs keep their old path.
  - Readbacks now happen about once per field in the countdown and the early race, as in exact.
- **WGSL that Chrome rejects.** Tint (Chrome) refuses a shift mixed with `&`, `|` or `^`
  without parentheses: "mixing '>>' and '&' requires parenthesis". It does so at
  `native.wgsl` lines 71, 72, 104, 258 and 259.
  - naga accepts these lines with C precedence: a shift's operands are additive expressions,
    and a shift binds tighter than `&`, `^` and `|`.
  - `native.wgsl` is locked, so on wasm32 `native/init.rs` gives the browser a copy with the
    shifts in parentheses (`parenthesize_shifts`). The copy refuses (and falls back to the
    unchanged source) whenever an operand touches `+ - * / %`, where precedence could matter.
  - A unit test checks that naga parses the original and the parenthesized copy to the same
    module (spans removed), for the plain and the dual-source variants. A negative control
    (`>> (8u & 255u)`) must parse differently.
  - Validated in Chrome: the parenthesized shader compiles with 0 errors.
- **Device.** The browser has no dual-source blending, so the native renderer uses its existing
  no-dual path: a draw that blends on As writes its alpha in a second draw.
  - The web device now requests the adapter's `max_texture_dimension_2d`, not the 8192
    default, so larger scales can get their surfaces.
  - Browser error scopes are not popped, because that would need an await. A WebGPU error is
    fatal through the uncaptured-error handler.
- **Not in the browser.**
  - No texture packs: `texpack_*` are stubs.
  - The sharp font is built: `build_graphics.py` compiles `port/gs/font_sharp.c` through a small
    `windows.h` shim (strcasecmp, no-op SRW locks), written to `build/web/font_sharp.c`.
  - The widescreen aspect reads `DHD_WIDESCREEN_ASPECT` and the game's own flat-2D flag in
    guest RAM, as on x64.
- **i686** is not touched: no file it builds changed, and `pc_settings.c` is unchanged, so it
  offers nothing new.

### Measurements

The build was `python port/web/build.py <core> --renderer rust`. Fields per second are from the page
status. The race uses race_compare.py's input (DHD_NAV, DHD_RACE_SYNC, DHD_RACE_PAD,
DHD_RACE_SEED=81257).

| Run (scale 2) | Title, fields 300-750 | Race, fields 3000-3500 (countdown, probe reads) | Race, fields 3500-4300 |
|---|---|---|---|
| native | 31.7 (check_browser: about 28) | 17.0 | 24.9 |
| exact | 26.8 (check_browser: about 22-25) | 19.2 | 25.0 |

An earlier iteration build gave the same picture:

| Mode | Title | Race 3000-3500 | Race 3500-4300 |
|---|---|---|---|
| native | 23.9 | 17.5 | 20.8 |
| exact | 20.6 | 14.5 | 20.1 |

- Native is faster on the title screen and about equal in the race, at twice the resolution.
- The race is CPU-bound in the worker, not in the GPU. W4 is the speed task.
- The native race picture matches exact (sun flare included).
  - `check-native.png` and `check-exact.png` are from check_browser.mjs;
  - `final-native-race-f*.png` and `final-exact-race-f*.png` are from the race runs;
  - `native-scale4-wide-f600.png` is a run at `DHD_SCALE=4`.

### Checks

- The layout checks pass: `check_web_layout.py build/web`, with and without `--require-renderer`.
- `check_rust_lib.py build/web/renderer/dhd_render_bg.wasm` passes.
- `run_headless.mjs --fields 300 --trace-off` exits 0 at 55 fields/s.
- `check_browser.mjs` reaches field 600, once on native and once with `--query env.DHD_RENDER=exact`.
- `rustgs.py test -- --lib native::init::tests` passes, and the x64 `rustgs.py build` builds.

### Sources

- naga 29.0.3, `src/front/wgsl/parse/mod.rs`, for the WGSL expression grammar it implements:
  shift operands are additive expressions, and a shift is accepted next to bitwise operators.
- The WGSL specification, section "Operator Precedence and Associativity". Mixing a shift with
  `&`, `|` or `^` needs parentheses, and Tint enforces this. Chrome's error text was
  "mixing '>>' and '&' requires parenthesis".
- wasi-libc, `libc-bottom-half/headers/public/wasi/libc-environ.h`, for
  `__wasilibc_deinitialize_environ` (environ is read again on the next `getenv`).
- wgpu 29.0.3, `Limits` and the WebGPU backend, for the adapter limits it can request.

## W2: browser settings are saved

**Found.** In the browser nothing written to dhd.ini lasted past a reload: `worker.js` had
`host_service_host_setting_save`, `host_service_host_setting_locked` and
`host_service_host_game_options_read` as stubs that returned 0, `host_service_host_settings_load`
did nothing, and the worker's environment fixed `DHD_SETTINGS=none`. That last one also turned off
the game's own options line (`game_options.c` returns early when `DHD_SETTINGS` is `none`), so the
audio and controller options were lost too. Natively the settings page shows every row, but in the
browser most rows did nothing: Window Size, Picture and Scaling (the browser has no `present.c`),
Skip Movies (the browser plays no movies and `host_skip_movies` is always 1), Vibration (pad
vibration is a no-op), Texture Pack (no packs), Font (no `font_sharp.c` in the reactor), Frame Rate
(the worker paces at 59.94 fields/s) and Widescreen (the canvas is 4:3). Resolution did nothing
either, since the browser renderer does not read `DHD_SCALE` yet.

**Changed.**
- `port/web/www/settings.js` (new): the browser's dhd.ini. Its text is kept in IndexedDB
  (database `downhill-revived-settings`, store `ini`), under the name `DHD_SETTINGS` gives: `dhd.ini`
  for players, while `none` or `0` means nothing is read or saved. It uses the same rules as
  `host.c`: the same keys and the variables they set, quoted values, comments, and "a variable
  already set wins" (so the page's `?env.NAME` overrides the file and the worker's fixed
  variables stay as they are). A save rewrites the key's first line and keeps the other lines,
  as `settings_save` does. The `iso` key is never stored or applied: the disc stays the player's
  own file, picked at each start, and is never kept in the browser.
- `port/web/www/worker.js`: the defaults are now `DHD_SETTINGS=dhd.ini` and
  `DHD_PC_SETTINGS_ROWS=DHD_WINDOW`. The stored settings are loaded before the core and the
  reactor are instantiated, because the reactor's WASI environment is copied when it is
  instantiated. `host_setting_save`, `host_setting_locked` (true for a variable the page set
  with `?env.`) and `host_game_options_read` now go to settings.js. `host_service_gs_window_apply`
  sends the Display value to the page.
- `port/hle/pc_settings.c`: a host that can do only some rows lists their variables in
  `DHD_PC_SETTINGS_ROWS` (comma-separated). When it is unset, all rows are shown, so native
  x64 and i686 are unchanged. The browser lists only `DHD_WINDOW`. When the browser renderer
  reads `DHD_SCALE` (task W1), `DHD_SCALE` only has to be added to `BROWSER_ROWS` in settings.js.
- `port/web/www/main.js`: Display controls the page's fullscreen. Borderless and Fullscreen put
  the canvas in fullscreen (`requestFullscreen`; the UA's fullscreen style keeps the 4:3 shape),
  and Window exits fullscreen. Browsers allow fullscreen only just after a click or key press. A
  key press in the PC SETTINGS page reaches the worker within a field, well inside Chrome's
  transient-activation window. The Start click also counts, because the page reads the stored
  Display value before the click. At any other time the request is refused and logged, and the
  page stays as it is.
- Tests: `?iso=test` now passes `DHD_SETTINGS=none` unless the URL sets it. Headless runs therefore
  read and save no settings, like the native tools' `DHD_SETTINGS=none` (finding 056), and the
  shared `build/web/chrome-profile` holds no settings that could change a later run.
  `check_browser.mjs` gains `--user-data DIR` (Chrome profile), `--then-query Q [--then-field F]`
  (load the page again once `--field` is reached) and `--eval EXPR` (printed at the end).

**Check** (headless Chrome, a fresh profile in `build/w2/chrome-settings`, the USA ISO; core built
with `build_wasm.py -j 2 --no-wasm2c`, then `build.py`):

```
node port/web/check_browser.mjs --user-data build/w2/chrome-settings --field 900 \
  --query "env.DHD_SETTINGS=w2test.ini&env.DHD_NAV=0:triangle|cross,15:down,15:down,15:cross,2:up,2:cross,13:right,13:cross" \
  --then-query "env.DHD_SETTINGS=w2test.ini" --then-field 300 --eval "<IndexedDB get of w2test.ini>"
```

- First load: OPTIONS > PC SETTINGS opens (menu 13). Its first row is now Display (natively it
  is Resolution). Right gives `pc settings: Display: borderless`, and Cross closes the page.
- Reload: `settings: 1 from w2test.ini in this browser`, and the page's Display is `borderless`
  (`window.dhdDisplay`). The stored text is the header plus `window = borderless`.
- `run_headless.mjs --fields 300 --trace-off` (Node): 300 of 300 fields, exit 0.
- An i686 `-fsyntax-only` compile of `pc_settings.c` shows only the warnings that were already
  there.

Sources: the Fullscreen API and its transient-activation rule
(https://developer.mozilla.org/en-US/docs/Web/API/Element/requestFullscreen,
https://developer.mozilla.org/en-US/docs/Glossary/Transient_activation), IndexedDB
(https://developer.mozilla.org/en-US/docs/Web/API/IndexedDB_API), and the existing memory-card store
in `vfs.js` used as the model.

## W3: browser "wobble" graphics

**Cause: (a) only.** The browser draws through the exact renderer at the PS2's own resolution, and
the exact renderer keeps the game's per-field half-line draw offset. The browser core computes the
same geometry as x64 (no (b)), and the present path adds no motion of its own (no (c)). The fix is
the move to the native renderer (task W1), which brings finding 060's correction with it. W3 changes
no renderer, core or web code.

**What the game does (finding 060).** In INT+FFMD mode (SMODE2 = 3) the game alternates OFY by 8/16
of a framebuffer line every field. The framebuffer is half height (224 lines for 448), so scanout
doubles each line. Half a framebuffer line is one output line, and sprite rounding often turns it
into a whole framebuffer line, which is two output lines. On a TV the interlaced fields cancel it;
on a progressive picture the scene moves up and down every field. Finding 060 fixed this in the
native renderer only: `native_record` (shared `port/gs/native_core.inc`) removes the half-line OFY
on screen-sized INT+FFMD targets before rounding. The exact renderer must stay bit-exact, so it
still draws the offset. Correcting the exact scanout by field parity would be wrong, because PATH1
and PATH3 draws in the same field can have opposite phases (finding 060's table).

The browser only has `web_display` + `web_present`: the exact scanout, stretched over the canvas
with nearest-pixel sampling. At the browser's ~25 fields/s each field stays on screen for more than
two vsyncs, so the bob runs at about 12 Hz, which is easy to see.

**Evidence 1: the browser core equals x64, 786 fields into a race.** Same race recipe on both
(`state_trace_run.py race`: race_compare NAV, `DHD_RACE_SYNC=1`, seed 81257, the race pad), 4000
fields, GO at field 3214 on both:
- x64: `dhd64.exe` from gate D2's Rust arm (this branch at `e9e74f108`, sha256 `534f1d86…`),
  `state_trace_run.py race --name x64-race --exe … --fields 4000`: 4000 rows in 181 s.
- Browser core: gate B4's `build/web` modules (core `db573f3b…` from input `9eef6cbb…`, the
  default `--renderer rust` reactor), `node port/web/run_headless.mjs --fields 4000` with the same
  env: 4000 rows in 310 s (12.9 traced fields/s), exit 0.
- `trace_columns.py x64-race.tsv web-race.tsv`: **vu0, gs_stream, gs_regs, gs_vram identical in
  all 4000 fields**; menu, phase, clock, scratch, cpu and code identical as well. Every GS packet,
  every GS register, VU0/VU1 memory and the 4 MB software VRAM are the same, so the vertices the
  browser draws are the x64 core's. The software float layer, VU1 AOT and VIF in the web core do not
  move geometry.

New tool `port/tools/field_bob.py`: mean RGB error of each field against the next one (adjacent) and
against the one after that (same parity), plus the best vertical shift per pair.

| Renderer, region | adjacent | same parity | best shift per pair |
|---|---:|---:|---|
| exact, speedometer (427,352,75,80) | 14.562 | 1.320 | +2,-2 repeating |
| native, speedometer (855,704,150,160) | 0.908 | 1.403 | all 0 |
| exact, whole picture | 8.696 | 10.210 | mostly 0 (the world moves) |
| native, whole picture | 8.374 | 10.947 | all 0 |

The speedometer is a still HUD element. In the exact picture it moves by two output lines every
field and returns on the next one, and the same-parity error is small. The native picture holds it
still. This is finding 060's race measurement ("speedometer 12.3757 before, 0.0674 after"), now on
the exact renderer.

**For W1 (and the gate).** The browser gets the correction for free if its native path records draws
through `native_core.inc`'s `native_record`, as x64 does. It reads SMODE2 from `gs`'s privileged
registers, which `web_display` copies from the MMIO page every vsync, as `present.c` does. To check
after the switch: capture consecutive browser fields during a race and run `field_bob.py` on a still
HUD element. Adjacent and same-parity errors should both be small, with every shift 0.

**Separate finding, not a graphics problem: the browser core's sound and timing differ from x64
at the race countdown.** The same pair of traces differs in ram and hw from field 2954 (race anchor
c, the countdown) and in sound from field 2955. `DHD_STATE_DUMP=2954,2955` on both and
`state_diff.py` show:
- hw: one word, `0x10000800` (T1_COUNT): x64 `0x2209`, web `0x2205`, at 2954 only;
- ram: 44 words, almost all in the sound engine's list at `0x004C9B00..0x004C9BB4` and its links at
  `0x004CBB18..0x004CBB58`. The same sound entries (`E_WIND_H…`, `UP_DUM_SUER09`, …) are stored in
  a different order, and some counters at `0x0029E3E0` differ (`0x358` against `0x35C`);
- the PCM is equal at 2954 and differs at 2955 (785 of 1602 samples);
- GS, VU and the CPU registers stay equal through field 4000.

So at the countdown the web core starts the race sounds in a different order, and a Timer 1 read
lands 4 counts apart. This does not cause the wobble and W3 does not fix it. Candidates for whoever
takes it: the remaining unlowered f64 operations (finding 083, "Float modes": 27 packed f64
arithmetic and 31 packed promotions; Timer 1 already showed an f64 rounding difference once, at
boot), or a difference in when the host services the countdown's sound and stream work between the
native host and `run_headless.mjs`/`worker.js`.

Only `port/tools/field_bob.py` (new) and this finding.

Sources: finding 060 (the native correction and its PCSX2 research: `GSPCRTCRegs::SetRects` halves
the read height in INT+FFMD; `GSRenderer::Merge` adds the field-dependent interlace offset),
findings 081-083 (the state trace protocol and the web core's float layer).

## S1: render resolution up to 8x and new default settings

- Resolution 2x.
- Display Borderless.
- Window Size: the monitor's highest resolution.
- Skip Movies ON.

**Resolution up to 8x (x64 Rust native renderer only).**
- `native_core.inc` now clamps DHD_SCALE to `NATIVE_SCALE_MAX`, and logs a line when it does
  (`gs: resolution scale 12 is more than this renderer draws; 8`).
  - The value defaults to 4, so the C backends (i686 D3D11 and wgpu-native) behave as before.
  - `gpu_rust_native.inc` sets it to 8.
- Texture size limit:
  - `native_wanted` caps a surface at 2048 GS pixels a side, so 8x needs textures of at most
    16384 texels a side.
  - Direct3D 12 guarantees 16384 (`D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION`). Desktop Vulkan drivers
    report 16384 or 32768, and the device is made with the adapter's own limits (`device.rs`).
  - If a device refuses a size anyway, the Rust side (`new_texture` in `native/surfaces.rs`) already
    returns an error, which is logged, and keeps the old texture. Nothing crashes.
- Graphics memory limit:
  - `native_init` (`gpu_rust_native.inc`) lowers the largest scale on a card with little memory.
  - Its estimate is 16 MB of upscaled surfaces at 1x, growing with the scale squared. That must fit
    in half of DXGI's `DedicatedVideoMemory`, the same source texpack.c uses for its budget.
  - It never goes below 4, so no card loses a scale it had before. When it lowers the setting it
    logs a line.
- Window scale: `DHD_SCALE=window` now matches the window up to that limit, not up to 4.
- The settings page:
  - The renderer publishes its limit as `DHD_SCALE_MAX`.
  - `pc_settings.c` builds Resolution as 1x up to that limit, then Window.
  - Without the Rust native renderer (the i686 build, the browser, exact mode) the variable is
    unset, so the list stays 1x-4x + Window.
- dhd.ini's template comment now says 1-8 (1-4 on the 32-bit game).

**Defaults.**
- `host.c` `player_defaults`, the template written to a new dhd.ini:
  - `skip_movies` is now 1.
  - Scale 2x and borderless were already the defaults.
- The window size default is the monitor's highest resolution:
  - `host.c` sets `DHD_WINDOW_WIDTH`/`HEIGHT` to `monitor`, but only when dhd.ini has neither a
    `width` nor a `height` line and the environment sets neither.
  - On the SDL thread, before the window is made, `present.c` (`monitor_size`) reads the primary
    display's modes through SDL. It takes `SDL_GetFullscreenDisplayModes`, which lists the largest
    first, and falls back to `SDL_GetDesktopDisplayMode`.
  - It puts the size into both variables as numbers. Everything after that (gs_window_apply, the
    settings page) reads plain numbers.
  - It also publishes the size as `DHD_MONITOR_MODE`. Window Size's list gains that entry when it
    is not already one of the five sizes.
- `widescreen.c` runs before the window exists. With a windowed `monitor` size it takes the
  monitor's shape (`host_monitor_aspect`), not 896x672's.
- As before, these defaults apply only when there is a dhd.ini:
  - The game writes one at the first start (the disc image line), and the relaunched process reads
    it.
  - The tools run with `DHD_SETTINGS=none`, so they keep the old defaults. No harness changed.
  - A line in dhd.ini, or a DHD_* variable, still wins.

**This PC's monitor.** SDL's highest mode is 2560x1440, but the desktop runs at 1920x1080 (96 dpi).
- The default Window Size is therefore 2560x1440.
- The borderless window still covers the desktop: 1920x1080, from SDL's display bounds.
- A windowed window at the default size would be larger than this desktop.
- SDL has no call for the monitor's preferred (native) mode; NVIDIA DSR modes would also count as
  "highest". If the windowed default should be the desktop size instead, the change is one line in
  `monitor_mode`.

**Checks.**
- Build: x64 `package_x64_test.py --impl rust` (cargo and build_host64 with 2 jobs), using a wasm
  core rebuilt from this tree (`pc_settings.c` and `widescreen.c` are module code).
- i686: `host.c`, `pc_settings.c`, `widescreen.c`, `present.c`, `gpu.c` and `gpu_wgpu.c` compile
  for i686-pc-windows-msvc with link.py's flags.
- Runs used a separate hidden desktop (`run_on_desktop.py`), so the window presents. The dhd.ini held
  only the iso line, with no DHD_* settings:
  - Log: `gs: window size 2560x1440, the monitor's highest resolution`, `gs: SDL window 1920x1080`
    (borderless), `gs: resolution scale 2`, and `DHD_SKIP_MOVIES: bypassing startup movie sequence`.
  - With `skip_movies = 0` in dhd.ini, the movies play (`movie: mov/dhsceapr.pss`).
- PC SETTINGS, reached with DHD_NAV:
  - The page shows Display Borderless, Window Size 2560x1440 and Skip Movies ON.
  - Pressing left on Resolution steps 2, 1, window, 8, 7, 6, 5, 4, ...
  - dhd.ini gets `scale = ...`.
- Races at 8x, using the bench recipe's race route, flat clock and borderless, on the RTX 3070
  (driver 617.42). The PC was shared, and the GPU was 57-65% busy before each run:

| Run | fields/s (3800-4400) | p99 ms | presented |
|---|---|---|---|
| 8x, DX12 | 91.6 | 15.8 | 4039 of 4069 |
| 8x, Vulkan (`DHD_WGPU_BACKEND=vulkan`; the log shows `wgpu_hal::vulkan`) | 89.0 | 20.5 | 4054 of 4084 |
| 2x, DX12, for comparison | 88.8 | 16.3 | |

  - At 8x the race is still CPU-bound on this card: it runs as fast as at 2x, well above 60.
  - Total GPU memory in use rose by about 0.7 GB during the 8x race.
- `DHD_SCALE=12` logs the clamp and runs at 8x.

Sources: Direct3D 12 hardware limits (`D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION` = 16384, d3d12.h); the
SDL3 3.4.18 headers (`SDL_GetFullscreenDisplayModes`: "sorted largest to smallest",
`SDL_GetDesktopDisplayMode`); wgpu's `Limits::max_texture_dimension_2d`.

## F1: title-screen flicker at 120/144/Monitor fps, FXC on WARP

### Flicker

With Frame Rate set above 60 (`DHD_FPS=120`), the screen before "PRESS X TO START" flickered for a
while and then settled. The old C renderer did the same. At 60 it was fine.

Per-field diffs flagged the fields that alternate A/B/A/B.

**What it is.** The game draws its loading picture into both display buffers (DISPFB 0x38 and 0x70),
then stops redrawing it and only flips between them for about 0.6 s. Texture-pack replacements load
asynchronously on two loader threads (texpack.c); until a replacement is READY, the original texture
is drawn. At 120 the two last draws are about 8 ms apart, so one buffer often got the original
texture and the other the replacement. The flip then showed the two in turn until the game drew a
new picture. At 60 the draws are 16 ms apart, and the replacement was nearly always ready for both.

- It depends on timing: 2 of 4 unfixed runs at 120 flickered.
- Without the pack nothing flickers.
- It isn't interpolation: the Frame Rate setting runs real simulation steps (finding 067) and
  doesn't enable `DHD_INTERP`. Aspect switching and movies were ruled out too.

**The fix (port/gs/texpack.c, shared by i686, the C wgpu path and the Rust path).**

- Outside a live race, a draw whose replacement is still queued now waits for it, with a
  condition variable the loaders signal.
- The wait is bounded at 200 ms in all per field (`PACK_WAIT_MS`). After that the original is
  drawn, as before.
- In a live race (screen `D_0029DED8 == 9`, phase `D_0043F348` 1..4) there is no wait, so the
  race never stalls on disk.
- `texpack_report` now counts the waits, their total time, and the ones that timed out.

PCSX2 takes the same approach for menus and loading screens: replacements load asynchronously and an
optional preload means the swap happens before the texture is shown. Here the wait happens only
while the game isn't racing.

**Checks.**

- At 120 with the pack, 0 of 4 fixed runs flicker (2 of 4 unfixed runs did).
- At 60, the captures of the fixed build match the unfixed ones.
- The route to the race (field 8100) at 120 with the pack runs with no hang.
- The exact referees aren't touched. They run without a texture pack, so they never reach the
  new wait (the renderer tests link texpack stubs).
- texpack.c still compiles for i686 (clang `--target=i686-pc-windows-msvc`).

### FXC on WARP

On WARP, DXC compiles take tens of minutes and FXC takes seconds (finding 085, gate J1 open item).
`port/render/src/device.rs` now checks the adapter after selection. On a Dx12 software adapter
(`DeviceType::Cpu`, or vendor 0x1414 device 0x8C), it rebuilds the instance with `Dx12Compiler::Fxc`
and logs that it did. Hardware adapters keep DXC when `dxcompiler.dll` is present, and
`Gpu::options` is unchanged.

The C wgpu-native path (gpu_wgpu.c) is unchanged; the fix lives in the crate. The crate's
`exact_warp_dxc` smoke test now runs FXC on WARP. It still passes bit-exact.

**Checks.**

| Check | Result |
| --- | --- |
| `test_gs_wgpu --impl rust --arch x86_64 --only gpu --skip-hardware`, `dxcompiler.dll` absent | PASS (3 m 05) |
| Same, `dxcompiler.dll` present | PASS (2 m 33) |
| `test_gpu.exe` direct on WARP, DLL present | logs the FXC switch, 23 s, VRAM identical |
| `cargo test device_smoke` | 12 of 12 |

### Sources

- Finding 085 (Rust renderer, J1 WARP/DXC open item); 067 (real high-fps steps); 057 (texture packs); 024
  (PCSX2 comparison and field captures).
- PCSX2 texture replacement: `GSTextureReplacements.cpp`, asynchronous loading and the
  "Load Textures Asynchronously" and "Precache Textures" options (PCSX2 wiki, Texture
  Replacement).
- wgpu `Dx12Compiler` docs (`Fxc`, `DynamicDxc`) and `AdapterInfo`/`DeviceType`. Microsoft
  docs on WARP (vendor 0x1414, device 0x8C) and on `SleepConditionVariableCS`.

## Flicker at 144 (second look)

It showed on the title before "PRESS X TO START" and on the screens after choosing rider and bike.
F1's cause is real, but it is a separate and rarer case.

**Cause.** The front end's screen fade is a timer callback that runs once a field, not part of the
frame. `func_001F79D8(value, frames)` registers `func_001F7840` in the game's timer pool
(`D_00759780`). Each run moves the fade alpha (`*D_0029DED4`) one field toward its target and queues
the full-screen fade sprite (`func_002103D8(D_00513B00, 0, D_004C67C0)`) in view 0's 2D list. The
game empties those lists every frame (`func_002102A8`). The pool (`func_00236C60`) runs a callback
only once the integer field clock (`func_00238F48`) reaches its due field. At 144 most steps advance
less than a field (finding 067). On those steps the callback didn't run, so the frame was drawn
without the fade. During a fade-in or fade-out, about every other picture flashed at full brightness
until the fade ended. At 60 every step is a whole field, so this never happens. The training hint
box (`func_002626B8` -> `func_001E41A0`, game mode 11) works the same way: a timer callback that
draws into the per-frame lists.

**Why F1 missed it.** F1's per-field captures read each picture back on the game thread, and that
thread waits for the renderer. Each step then took longer than 1/144 s, so the paced clock advanced
a whole field every step and 144 ran like 60.

**Repro.**

- A scratch dump on the render thread (not committed) saved each picture as it was
  presented, without making the game thread wait.
- `DHD_FRAME_CLOCK=1` (the virtual clock) makes runs repeatable.
- Route: memory-card dialog, title, menus, rider and bike, up to field 2700; widescreen,
  pictures at 256x144.
- A field counts as a flash when its mean luminance is more than 5 above or below both of
  its neighbours.

| Run | Unfixed | Fixed |
| --- | --- | --- |
| 144, paced, no pack | 185 flash fields: title fade 318-493 (83), after rider and bike 2425-2505 (61) and 2535-2589 (41) | 1 (field 318, the cut from the loading picture to the title) |
| 144, paced, PCSX2 pack | 64 (63 of them in 2398-2480, after rider and bike) | 1 (318); no A/B/A fields |
| 144, virtual clock | 175 (318-581) | 1 (318) |
| 60, virtual clock, to field 2000 | 1 (field 230) | all 1851 pictures identical to the unfixed build |

With the fix, the title fade-in moves one step per field and holds that value between fields.

**Fix (port/patches_fps.py: func_00236C60 and func_001E41A0).** On a step that doesn't advance the
field clock, the pool queues the draw again for the callback due next field. The draw is the one its
last run left, and no state changes:

- for the fade, the same fade sprite, if the fade is active and its alpha isn't 0;
- for the hint, a draw-only pass of `func_001E41A0` (`port_timer_redraw`) that draws what its
  last run drew in the same field.

The fade's callback is identified by what it draws: it is the first callback whose run queues the
`D_004C67C0` sprite. Its address can't be compared with `func_001F7840`, because `func_001F79D8` is
recompiled MIPS and stores the wasm core's thunk (`func_001F7840__as__v`,
`build/wasm/abi_thunks.tsv`). The hint's callback is the one whose context is the hint object
`D_004C5640`. All of this sits behind `port_fps_high()`, so 60 runs the old code. The hint part was
compiled but not run, because training isn't on the repro route. The race's own fade
(`func_0020CDF0`) already runs on every step.

It still covers a real case with the pack (the two display buffers holding different versions of a
texture while the loading picture flips). Outside races it costs nothing, so it stays.

## W4: browser speed

**Measured how.** Headless Chrome, driven over the DevTools protocol as `check_browser.mjs` does,
with a CPU profile of the game worker (`--profile F S`, 500 us sampling, 15 s). Title: fields
500-1000 (`?iso=test`). Race: fields 3700-4400 with race_compare's `NAV` (`env.DHD_NAV`); the rider
stands at the start, as in finding 083's round 2. A scratch copy of `check_browser.mjs` also read
the CPU seconds of Chrome's renderer processes (the page and its game worker) every 2 s, giving CPU
ms per field, which the shared PC disturbs less than fields/s. Core module: P0-8's `dhd_core.wasm`
(unchanged); renderer: the Rust module from gate B4.

**Where the time goes (before).** Self time; `port/web/profile.py` categories, then the top items.

| | title | race start |
|---|---|---|
| GS reactor (VU1 dispatch, translated blocks, VIF, GS front end) | 40.2% | 26.9% |
| GS float layer (float.c/double.c helpers) | 37.4% | 22.0% |
| core (EE) | 9.3% | 21.5% |
| core memory adaptation (postlink's mirror checks) | 3.8% | 7.9% |
| core float layer | 1.3% | 4.0% |
| JS (writeBuffer, idle, runtime) | 7.5% | 16.7% (12.8% idle) |

- Title: `web_vmul` 15.8%, `run_micro` 10.2% (the VU1 dispatch loop with the native geometry
  kernels inlined), `web_vadd` 9.3%, `draw` 6.6%, `writeBuffer` 6.0%, `ps2_vu0_run` 4.0%,
  `aot_block_173` 3.9%, `vif_feed` 3.9%, `web_dmul` 3.5%, `web_dsub` 3.0%, `gs_feed` 2.9%,
  `gs_write` 2.6%, `web_ddiv` 1.9%, `__multi3` 1.1%.
- Race: idle 12.8% (the worker waits for readbacks: one or more maps per field, about 3 ms each, finding
  085), `web_vmul` 9.3%, `run_micro` 6.9%, `web_vadd` 5.6%, `ps2_vu0_run` 4.4%, `draw` 3.5%,
  `writeBuffer` 3.0%, `web_dmul` 1.9%, `web_dsub` 1.8%.
- The worker is the bottleneck: one core busy, and Node without any GPU work runs the title at
  the same 20-21 fields/s. Message passing and JS-wasm crossings are below 1%.

**Found: the GS front end ran in the VU's rounding mode.** `web_dmul`, `web_dsub`, `web_ddiv` and
`__multi3` (8.4% of the title, 5.3% of the race) were all called from `draw`
(`port/gs/gpu_exact.inc`), in its double-precision depth plane. On the web, VU1's XGKICK calls
`graphics_xgkick` and then `gs_feed` straight from the VU1 run, under the VU mode (0xFFC0: toward
zero, FTZ, DAZ), so double.c took its slow exact round-toward-zero paths (a 128-bit product through
`__multi3`). Natively, `gs_feed` runs on the GS worker thread, and `gs_queue.inc`'s `gs_consume`
sets `(csr | 0x1F80) & ~(63 | 0xE040)` first: round to nearest. So the browser's live game also
computed the depth plane in a mode that native never uses. This is the open item of plan section 8
(the "VU1 XGKICK depth plane under RTZ", B6, never run). A scratch test of the depth-plane formula
through double.c's helpers, toward zero against to nearest, on 1M random triangles: the gradients
differ in 75% of them (last bits).

**Changed.** `port/web/host_graphics.c`: `graphics_gif_feed` (every GS packet on the web, PATH1
XGKICK included) sets the native GS worker's FP mode around `gs_feed` and restores the caller's. The
front end now runs as natively, and its float ops take the helpers' IEEE path (a plain op). The GS
replays (`web_gs_replay.mjs`) never had the problem: they build `gs.c` without postlink, in IEEE
mode.

**Results.**

| | before | after |
|---|---|---|
| title, fields/s (3 interleaved pairs) | 20.7, 19.3, 19.5 | 20.7, 21.5, 17.6 |
| title, CPU ms per field (same runs) | 49.5, 47.4, 53.6 (mean 50.2) | 49.4, 44.2, 50.6 (mean 48.1) |
| race start, fields/s | 23.1 | 25.3 |
| race start, CPU ms per field | 40.3 | 37.0 |
| race profile, GS float layer | 22.0% | 19.1% (`web_dmul`/`dsub`/`ddiv`/`__multi3` gone) |

That is about 4% less CPU per field on the title and 8% in the race, within the noise of the title's
fields/s. The game still runs at 20-25 fields/s; 60 is far off.

**Tried and dropped: VU1's f32x4 RZ arithmetic inlined.** The VU1 float work (`web_vmul`,
`web_vadd`, `web_vsub`: about 27% of the title) is `_mm_mul_ps`/`_mm_add_ps`/`_mm_sub_ps` in
`vu0_interp.c` and the `vu1_native_*.h` kernels, which postlink turns into helper calls. A web-only
header replaced them with float.c's helper logic inlined (first its fast path, then a branch-free
version that also handles zero lanes, the DAZ flush, rz()'s flush and clamp, and sum()'s
far-exponent rule). It was bit-identical to the helpers in 120M vector results, in both modes
(normals, near-equal exponents, underflow, overflow, zeros, denormals, inf, NaN), and the test
catches each of 5 deliberate mistakes. It removed the helper calls but not the time: in Node the
title stayed at 20-21 fields/s, and `run_micro` grew by what the helpers lost. A microbenchmark
explains this: a dependent mul+add costs 1.2 ns plain, 9.8-11 ns through the helpers, and 9.6-12 ns
inlined or as a leaner path. The call is not the cost; the binary64 work is (two promotes, the op, a
mask and a demote per half, about 17 cycles of latency). This method is at its floor, and the change
was not kept (it also grew the reactor by 1 MB).

**What is left, biggest first (not done here).**
1. VU1 and GS share the EE's worker. Natively they run on their own threads (`vu_queue`,
   `gs_queue`). In the browser the reactor is 60-75% of the worker's time, so moving VIF1/VU1 (and
   the GS) to a second worker on the shared memory, fed through a ring as `gs_queue.inc` does, is
   the one change that can approach 2x. It needs a shared-memory core link and Atomics waits for
   readbacks, so it is a project of its own.
2. Readback waits, 12.8% of the race: task W1 (the native renderer) reads back less.
4. Core memory adaptation, 8-9% of the race: the range checks in postlink's access wrappers.

**Checks run.**
- `node port/web/run_headless.mjs --fields 600` (state trace) with the old and the new reactor:
  all 13 columns identical to each other and to `baseline-gates/x64-boot/pre.tsv` (the native x64
  core), `cpu` included (`trace_columns.py`: PASS).
- `python -I port/web/check_web_layout.py build/web --require-renderer`: OK (core, graphics,
  renderer).
- The reactor built by `build_graphics.py` from this tree has the same SHA-256 as the measured
  one (`aff158b5…`).
- The title and the race (picture, lens flare) in headless Chrome, to fields 1000 and 4400.
- No locked file, referee or golden changed. The exact referees do not link `host_graphics.c`.

Sources: `port/hle/gs_queue.inc` (native GS worker FP mode), `port/web/float.c`,
`port/web/double.c`, `port/web/postlink.py`, findings 083 and 085, the WebAssembly SIMD spec
(promote/demote are exact conversions with round-to-nearest; no rounding-mode control), and V8's
DevTools CPU profiler.

## Gate X64

## Frame-time spikes at 144

The report: at Frame Rate 144 (x64 Rust build, scale 8, RTX 3070, 1920x1080 borderless, G-SYNC
monitor, RTSS) the frame time spikes at a steady interval; at 60 it doesn't.

**Period and cause.** About 7 s. The just-in-time pacing (finding 052) put pictures on an exact
1/144 s grid of the QueryPerformanceCounter clock. The monitor's "144 Hz" mode is 143855/1000 Hz
(QueryDisplayConfig), and DWM's vblanks come 69513.3 QPC units apart (143.857 Hz on the QPC clock).
The grid ran 0.143 pictures/s faster than the display, so every 1 / 0.143 = 7 s one refresh got two
pictures. On a fixed-refresh or composed display that drops one; with G-SYNC at its top rate plus
V-Sync, the second present waits a refresh, which is the spike RTSS shows. "Monitor" (143, the whole
hertz) ran slower than the display instead: one repeated picture every 1.2 s. 60 has no beat.
Nothing else at 144 runs on that period. The renderer's own frame time is about 1 ms. The EE
thread's spikes in the race are irregular and also come at 60.

**Measuring it on a hidden desktop.** A window on its own desktop (`run_on_desktop.py`) is not tied
to DWM's vblanks, so the display's side is modelled. `DHD_PACING_LOG` now logs its clock's QPC
origin (`pacing: log seconds = QueryPerformanceCounter / f - x`). Each present time is then placed
on that vblank grid (fit to 23-75 us), and each refresh's pictures are counted (a picture shows at
the next vblank). Settings: native renderer, wide, `DHD_FPS=144`.

**Fix (`port/hle/host.c`).** Above 60 pictures a second, when the requested rate is within 2% of the
primary display's refresh (or a whole fraction of it), a picture lasts n refreshes. The refresh
period comes from the mode's exact rate, refined from DWM's vblanks to the QPC clock. DWM's
`qpcRefreshPeriod` is noisy and falls back to 144.000, so it isn't used. The period gets 20 ppm on
top, so with VRR the pictures stay just under the top rate. While DWM's vblanks lie on one fixed
grid, each due also moves a quarter of the way, at most 1% of a refresh, towards half a refresh
after a vblank. Only an on-time picture after an on-time one may come that 1% early. A late picture
(the CPU behind) and the one after it wait a whole period. Without that, late pictures came every
6.81 ms in the race and drifted across refreshes (29 refreshes with 2 pictures in 195 s). 60 a
second and online keep their 59.94 grid, game speed doesn't change (the step follows wall time), and
i686 is unchanged. `DHD_PACE_LOCK=0` turns it off. The log says `pacing: 144 pictures/s follow the
display's refresh, 143.8549 Hz (1 per picture)`. It applies with the default Immediate (tearing)
surface and with `DHD_TEARING=0` (Mailbox) alike.

**Numbers** (same `dhd64.exe`, `DHD_PACE_LOCK=0` for "before"; final exe sha256 `4d2ca55f…`):

| run | present p50 | refreshes with 2 pictures |
| --- | --- | --- |
| title, scale 8, before | 6.945 ms | 11 in 65 s: at 7.7, 14.9, 22.1, 29.3, 36.5, 43.7, 51.0, 58.9, 66.1 s (7.15-7.35 s apart), two more next to the 51.0 one; phase sweeps the whole refresh |
| title, scale 2, before | 6.945 ms | one every 7.6-10 s |
| race, scale 8, before | 7.66 ms (busier PC) | at 20.2, 27.8, 78.2, 86.1 s (7.6, 7.9 s apart, where the race keeps up) |
| title, scale 8, after | 6.951 ms (p99 6.952) | 0 in 95 s; phase 0.50-0.72 |
| title, scale 2, after (1% slack build) | 6.951 ms | 0 in 62 s; phase 0.50-0.57 |
| race, scale 8, after | 6.952 ms | 0 in 195 s (16471 pictures); late pictures at least 6.951 ms apart |

Some refreshes in the scale-8 race get no picture (about 8% after the fix). Those are the race being
CPU-limited on a PC in use, not a period. The title run after the fix has one 125 ms gap at 52.4 s:
both threads stopped together, so it was the PC, not the pacing.

Sources: Microsoft's documentation of `DwmGetCompositionTimingInfo` / `DWM_TIMING_INFO` and
`QueryDisplayConfig` / `DISPLAYCONFIG_PATH_TARGET_INFO.refreshRate`; finding 052 (the just-in-time
pacing). 
