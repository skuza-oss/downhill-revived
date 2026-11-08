# 136: The resolution scale the GPU allows, said where the player picks it

Status: current
Checked: 2026-11-04

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

Done in this lane (before the 2026-11-02 change of process):

- Built (core and Windows game, `-j 4`/`-j 6`). The browser build (`build.py --web --no-core`) was
  stopped after 70 minutes in one clang compile, unfinished: the browser side is not built.
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

## Checks still for the lead (merged build)

1. Browser build (`python port/tools/build.py --web`): it compiles `gpu_rust_web_native.inc`
   (`n->scale_asked` in `native_rescale`) and loads the core with the new import
   `host_service_gs_scale` (answered 0 in `worker.js` and `run_headless.mjs`). A missing stub would
   show as an instantiate error naming that import.
2. Windows game, integrated GPU only (`DHD_WGPU_ADAPTER=integrated`), an isolated dhd.ini with
   `scale = 8`, `discord = off` (and `widescreen = 0`, `DHD_ASPECT = 4:3` for the same picture as
   here), env `DHD_SCALE` unset (else the row is locked "Set by DHD_SCALE"):
   - dhd.log: `gs: resolution scale 8 asked (DHD_SCALE)`, then
     `gs native: resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (graphics memory: ...)`,
     and `perf: render native scale=4`; `gs: GPUs` says the game runs on the AMD iGPU.
   - Keys from the title: X (title), Down, Down, X (OPTIONS), Up (to the last row, PC SETTINGS), X.
     Resolution row: `8x (running 4x: GPU memory)`, footer `This GPU's memory allows up to 4x`.
     Left once: `7x (runs at 4x: GPU memory) *`, footer `* Applies when the game starts again`;
     the row must stay inside the panel. Left three more: `4x *`. (Triangle puts 8 back.)
3. The same with `scale = window` in a 960x720 window: the log line `resolution scale 2x: window
   asked (720 lines, picture 448), this GPU allows up to 4x (...)`, the row `Match window (2x)`.
4. The RTX (default adapter) with `scale = 8`: `resolution scale 8x: 8x asked, this GPU allows up
   to 8x`, the row a plain `8x`, the default footer legend.
5. Launcher after run 2 (its data folder's dhd.log has that line): Settings > Graphics >
   Resolution's hint `Your GPU's memory allows up to 4x; 8x runs at 4x`, the list's `5x`-`8x` with
   `(runs at 4x)`; choosing 4x changes the hint to `Your GPU's memory allows up to 4x`. A data
   folder without dhd.log: the old hint, plain choices.
6. State traces boot and race against the shared next/0.7.7 reference build (boot already matched here).

## Not done

- The browser's page: its hosts answer 0 and set `DHD_SCALE_MAX` 8 themselves, so it marks nothing
  (its canvas limit lives in `gpu_rust_web_native.inc`); its log line is there.
- The limit's rule itself is unchanged (half of the reported dedicated memory, never under 4):
  shared-memory GPUs report little dedicated memory, and finding 130 measured true 6x/8x on this
  iGPU with the override, below 60 fps anyway. Raising the budget for UMA GPUs is a separate change.

## Integrated GPUs count shared memory (0.7.7)

The budget above was half the adapter's dedicated memory, which on an integrated GPU is only the
small slice the firmware reserves (485 MiB on a Ryzen 5 7600's Radeon), so 6x and 8x were refused
though such a GPU draws its frame and Z buffers in shared system memory and ran 8x without trouble
(finding 130's measurements with DHD_SCALE_MEMORY_MB). An integrated GPU (wgpu reports the adapter's
kind) with shared memory (DXGI's SharedSystemMemory) now counts dedicated plus half of shared for
the scale (texture packs keep their budget); a discrete card keeps its dedicated memory only,
however small (a review of 0.7.7 pointed out that DXGI reports shared memory for those too). On the Ryzen: `gs: integrated GPU: the resolution scale's
memory counts shared memory too (485 MiB dedicated, half of 15915 MiB shared)` and `resolution scale
8x: 8x asked, this GPU allows up to 8x`. Dedicated GPUs are unchanged. How fast a scale runs is the
player's choice; dhd.log's perf: lines say it.
