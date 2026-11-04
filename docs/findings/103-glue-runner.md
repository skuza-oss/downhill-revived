# 103: one copy of the GS glue's decisions, one process runner for the test tools

Status: current
Checked: 2026-11-04

2026-10-27, branch `desk/glue-runner` (from `desk/integrate`): two follow-ups of the architecture
review after finding 099. The aim, as there: one implementation of each decision, the old copies
deleted.

## 1. GS glue: the decisions both sides made alike

After finding 088's CL8 (the device ops, surfaces, the picture and the present drawing in
`port/gs/gpu_rust_shared.inc`), the x64 glue (`gpu_rust_native.inc`) and the browser's
(`port/web/gpu_rust_web_native.inc`) still each wrote three decisions. Each is now in one place:

| Decision | Now | Each side keeps |
|---|---|---|
| Scale limit and `DHD_SCALE=window` | `gpu_rust_shared.inc`: `NATIVE_SCALE_MAX` (8), `native_scale_hold` (called by `native_create_rust`), `native_match_window` | x64: `native_window_height` (the window), the card's memory and `DHD_SCALE_MAX` for PC SETTINGS. Browser: `native_window_height` (the canvas), `native_rescale` (now held to the same limit) |
| The display read | `gpu_rust_shared.inc`: `native_read_display` (size check, finish, discard, report) | `native_read_wait`: x64 none (the finish blocks); the browser `web_gpu_await` (JSPI), so the order stays begin, await, finish |
| The widescreen loading card's limit | `port/hle/widescreen_flat.h`: `widescreen_flat_limit` (menu and phase from guest RAM) | `host_widescreen_flat_limit` is one call to it in the core (`widescreen.c`) and in the browser's reactor |

### The scale limit's rule

Before: x64 matched the window up to `scale_limit` (8, lowered on a card with little memory, never
below 4); the browser matched the canvas up to a literal 4 (left from before finding 086's S1) while
its PC SETTINGS offered 8 and `native_rescale` held to 8.

Now one rule for both, `native_scale_hold(n, memory)`:
- the device's largest texture side over the largest surface (`NATIVE_SURFACE_MAX`, 2048 GS pixels,
  `native_wanted`'s cap): Direct3D 12 and desktop Vulkan give 16384 or more (8x), a WebGPU device
  at least 8192 (4x); the browser's device asks for its adapter's own limit (`web/init.rs`);
- then the memory rule, as before: the upscaled surfaces in half the dedicated memory (x64: DXGI's
  `DedicatedVideoMemory`), never below 4; unknown memory (0, the browser) leaves the texture limit;
- `DHD_SCALE` above the limit is held to it with a log line (`gs native: resolution scale N is more
  than this graphics device takes; M`), on both sides. The window or canvas match and the browser's
  live rescale stop at the limit.

The device's texture limit is new to C: `dhdr_device_max_texture()` in
`port/render/include/dhd_render_ext.h` (the post-freeze header; the v1 header and `abi.rs` stay as
frozen), one `#[no_mangle]` function in `port/render/src/device.rs`. The browser's reactor imports
it from the Rust module by name, as every `dhd_render` import (`graphics_common.js`).

On x64 nothing changes in practice (16384 or more on every supported device: 8, then the memory
rule). In the browser a window-matched scale can now reach the device's limit (up to 8) instead of
4.

Still open: the browser page's own Resolution list (`www/settings.js`, `DHD_SCALE_MAX: '8'`) is
written before the reactor has a device; a choice above a smaller device's limit is held by
`native_rescale`. `native_core.inc`'s `DHD_SCALE` parse and the browser's `native_rescale` parse the
variable alike; `native_core.inc` stayed as it was in this step.

## 2. One runner for the game and its helpers (`port/tools/owned_process.py`)

Before, each tool launched, timed out and stopped its processes its own way: `bench_race.py` (`kill`
on a timeout), `stutter_race.py` (WM_CLOSE by window name, `logman stop`, `kill`), `online_race.py`
(`taskkill /T /F`, with a stopping loop that killed the guest's process id for every process),
`sample_profile.py` (`kill`, plus a relaunched-child branch for the i686 build), and the comparison
and state-trace tools (`stop_verified`: a PowerShell walk over the process tree, checking each
executable path). Windows were found by process name (any `dhd.exe` or `dhd64.exe`).

Now `owned_process.start(args, ...)` (Popen's arguments, plus `close=`, `grace=`, `minimized=`) and
`owned_process.run(args, timeout=...)`:
- Windows: the process is created suspended in a new process group, put into a job object of its own
  (`KILL_ON_JOB_CLOSE`), then resumed (`NtResumeProcess`), so everything it starts is in the job from
  its start. Ending the tree terminates the job and waits until it is empty. The job's handle
  closing ends it too: a tool that is killed leaves nothing running.
- POSIX: the process leads its own session; ending the tree sends SIGTERM to its group, then SIGKILL.
  SIGTERM to the tool goes through its exit handlers.
- `stop()` (also the with-block's end, an error, Ctrl+C, and the tool's exit through `atexit`): the
  process's `close` once (the game: `Owned.close_windows`, WM_CLOSE so its logs flush; PresentMon: its
  ETW session's `logman stop`), up to `grace` seconds, then the whole tree.
- `minimize()`, `focus()`, `close_windows()`: the windows of the tree's own processes (the job's
  process ids), never by name.

Moved onto it: `bench_race.py`, `stutter_race.py`, `online_race.py`, `sample_profile.py`,
`compare_pcsx2.py` (its `stop` is gone), `pcsx2_retail.py`, `race_compare.py`, `rate_audit.py`,
`state_trace_run.py`, `test_fps_race.py` and the other tools that called `stop_verified`. Deleted:
`stop_verified`, `compare_pcsx2.stop`, `bench_race`'s `process_name` / `focus_game`,
`stutter_race`'s `close_game`, `sample_profile`'s `minimize_windows`, `child_of`, `--child` and its
Wow64 (i686) sampling.

`test_owned_process.py` (Python sleepers as the process tree):
- `run` with a 2 s timeout ends a three-level tree;
- a root that exits by itself: its children are still ended;
- an exception (KeyboardInterrupt) in the with-block;
- `close` called once, then the grace time, then the tree;
- a separate tool interrupted (Windows: CTRL_BREAK_EVENT made its Ctrl+C) leaves no process;
- a separate tool killed (Windows): the job's handle closing leaves no process;
- with `DHD_OWNED_GAME`: the game minimised (`DHD_RENDER=native`), its first pictures, stopped by
  WM_CLOSE.

## Checks (2026-10-27)

- Browser: `port/web/build.py` builds, `check_web_layout.py build/web --require-renderer` OK, and the
  reactor imports `dhdr_device_max_texture` from the Rust module. `web_gs_replay.mjs` on `gs-g+0.gs`:
  per-field VRAM hashes identical to the base's. It needed a harness fix first:
  `port/gs/replay_web.c` now gives `web_display` its `web_gs_mmio` (the reactor's register read,
  added for the threaded build in `host_graphics.c`, which the replay does not link; without it the
  page's stub returned a JS number for an i64 and the replay failed in both trees). Both trees'
  hashes differ from the stored web golden (recorded before), so that golden is out of date.
- x64: `build_host64.py` builds and links. Fixed in passing: the `spike_present` log line printed
  the 64-bit field with `%u` (clang's only warning in the glue).
- `test_owned_process.py`: 8 of 8 pass in 10.6 s, the real game included (one process, closed
  itself 0.1 s after WM_CLOSE, nothing left running).

Seen while checking, not changed here: started minimised with `DHD_SETTINGS=none` (the exact
renderer), the game ends at the main menu (field ~184, about 30 s) with `dhd_render: wgpu error in
dhdr_exact_submit: Out of Memory` (`GraphicsCommandList::close`, 0x8007000E) and exit 0xC0000409;
the base build does the same, with 18 GB of RAM and 6.8 GB of graphics memory free. With
`DHD_RENDER=native` the same start runs on (40 s, 2,100 fields). A minimised exact renderer presents
nothing; what it submits may then never be freed. 
(Fixed in finding 105.)
