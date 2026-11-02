# 136: The resolution scale the GPU allows, said where the player picks it

Why: on integrated GPUs the native renderer lowers the resolution scale to fit its graphics-memory
budget, and said so only in one dhd.log line. A player who picked 8x got 4x and nothing in the game
or the launcher told them (40-50 fps "at 8x" that was really 4x). Finding 130's first 6x run was
capped to 4x the same way. The cap is right; hiding it was not.

Branch `fix/scale-cap` (from local `next/0.7.7`).

## The cap (unchanged)

`native_scale_hold` (`port/gs/gpu_rust_shared.inc`): the largest scale is 8, lowered so that every
surface fits the device's largest texture (`dhdr_device_max_texture` / 2048 GS pixels) and the
upscaled frame and Z buffers (16 MiB at 1x, growing with the scale squared) fit in half of the
dedicated graphics memory (DXGI's `DedicatedVideoMemory`; 0 = unknown, no memory limit). Memory
never takes it below 4. `DHD_SCALE_MEMORY_MB` (finding 130) still overrides the memory figure for
developer measurements. On this PC's Ryzen 5 7600 iGPU Windows reports 485 MiB: the budget is
242 MiB, 5x needs 400 MiB, so the limit is 4x.

## What the game says now

dhd.log: one line once the scale in use is known (the first picture, where `DHD_SCALE=window` is
resolved too), with the scale used, `capped` when it is less than the one asked for, what was
asked, the limit and its reason with the figures:

    gs: resolution scale 8 asked (DHD_SCALE)
    gs native: resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (graphics memory: 8x needs 1024 MiB, over the 242 MiB budget, half of the 485 MiB the GPU has)
    gs native: resolution scale 2x: window asked (720 lines, picture 448), this GPU allows up to 4x (graphics memory: 5x needs 400 MiB, over the 242 MiB budget, half of the 485 MiB the GPU has)

Without a limit the line ends at `this GPU allows up to 8x`; a texture limit says
`(largest texture: N pixels a side)`. It replaces the old `is more than this graphics device takes`
line and the window match's own line. The install-time line printed the setting after the cap
(`gs: resolution scale 4 (DHD_SCALE)` while dhd.ini said 8); it now prints what was asked. The
`perf: render` line's `scale=` already was the scale in use (`n->scale`): `scale=4` in the 8x run.
The browser prints the line again after each change of the setting (its `native_rescale`).

The game: the renderer already published the limit in `DHD_SCALE_MAX` for PC SETTINGS; it now
also sets `DHD_SCALE_MAX_REASON` (`memory` or `texture`, unset without a limit), and a new host
service, `host_service_gs_scale(asked)`, answers the scale in use (asked 0) or the one asked for
(asked 1: the window's match before the limit), 0 before the first picture or without the native
renderer. The render thread writes it once at the first picture; PC SETTINGS reads it when it
draws the row (a volatile word: the page opens long after). The browser's hosts answer 0
(`worker.js`, `run_headless.mjs`), so its page is as before.

## PC SETTINGS > Resolution

Before, the row hid the scales over the limit (on this iGPU it offered 1x-4x, and a dhd.ini 8 showed
as a bare "8"). Now with the native renderer it offers every scale; the ones over the limit are
marked, not hidden (the limit follows the memory Windows reports now, which a BIOS setting, a
driver or another GPU changes). Without the native renderer it still offers 1x-4x as before.

- a scale over the limit, running: `8x (running 4x: GPU memory)`
- one chosen for the next start: `7x (runs at 4x: GPU memory) *`
- Match window: `Match window (2x)`, and `(4x: GPU memory)` when the limit held it below its match
- the footer on that row, while nothing waits for the next start: `This GPU's memory allows up to 4x`
  (`This GPU's texture size allows up to Nx` for a texture limit)

The first try said `graphics memory` as in the brief; the pending row with its `*` then reached the
edge of the 640-unit view (the panel grows with its widest row, and can't grow past it), so the row
says `GPU memory` (dhd.log keeps `graphics memory`).

## The launcher

The launcher can't make the game's GPU device, so it reads what the game found: the last
`gs native: resolution scale` line of the data folder's dhd.log (the first 4 MiB),
`port/launcher/src-tauri/src/scale.rs`, into the snapshot as `scale: { used, asked, limit, reason }`.
Chosen over a value saved in dhd.ini: nothing new is written to the player's settings file, the
render thread writes no file, the line carries the whole answer (window included), and it is
always the last run's GPU. A fresh install (no dhd.log) or a run without the native renderer
shows nothing new. The snapshot is read again when the game exits.

Graphics > Resolution, with a report whose limit is under 8:
- hint: `Your GPU's memory allows up to 4x; 8x runs at 4x` (8x chosen), `Your GPU's memory allows up
  to 4x` (a scale within it), `Match window ran at 2x last time; your GPU's memory allows up to 4x`
  (window chosen and the last run matched the window);
- the choices over the limit read `6x (runs at 4x)`, `8x (runs at 4x)`.
Without a limit the hint stays the old one (and a window run says what it ran at).

## Checks

- Built (core and Windows game, `-j 4`/`-j 6`); the browser build (`build.py --web --no-core`).
- Hidden desktop, `DHD_WGPU_ADAPTER=integrated` (the log: `the game runs on AMD Radeon(TM) Graphics
  (integrated); a discrete GPU is there but not used`), an isolated dhd.ini with `scale = 8`, then
  `scale = window` (960x720 window): the lines above; the row and footer captured in
  `build/checks/scale8/pc-settings-8x.png`, `build/checks/scale8/pending-7x.png` and
  `build/checks/scale-window/pc-settings-window.png` (not committed). The RTX wasn't used.
- Launcher: `cargo test` 19 passed (two new: the lines exactly as this run printed them, and the
  last line of a log); the hint and choice words evaluated from `app.js` for a fresh install, the
  8x run, the window run and an uncapped run. The real launcher window wasn't opened (no computer
  use; WebView clicks on a hidden desktop are not reliable).
- `dhd_settings` tests pass. `tests/web_corpus.json` was stale on `next/0.7.7` (it lacked
  `DHD_PERF_COUNTER`, the FPS & Ping row of fe950ad): re-blessed in its own commit.
- State trace `boot`: all 600 rows equal to the `next/0.7.7` reference build's (and to this
  branch's own unchanged build).

## Not done

- The browser's page: its hosts answer 0 and set `DHD_SCALE_MAX` 8 themselves, so it marks nothing
  (its canvas limit lives in `gpu_rust_web_native.inc`); its log line is there.
- The limit's rule itself is unchanged (half of the reported dedicated memory, never under 4):
  shared-memory GPUs report little dedicated memory, and finding 130 measured true 6x/8x on this
  iGPU with the override, below 60 fps anyway. Raising the budget for UMA GPUs is a separate change.
