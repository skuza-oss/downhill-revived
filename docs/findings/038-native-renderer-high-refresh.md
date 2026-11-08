# Native renderer and interpolated high-refresh presentation (round 29)

Status: partly superseded by 088 (the D3D11 native renderer, off by default; gate_native_renderer.py)
Checked: 2026-11-04

Now: the native renderer is the player default (dhd.ini `renderer = native`,
`port/render/settings/src/table.rs`; Rust, `port/gs/native_core.inc`, findings 085, 086), and the
exact one stays the reference (`DHD_RENDER=exact`; a run without dhd.ini still gets exact).
`DHD_INTERP` still exists, a developer variable, off by default (`port/gs/native_core.inc`); the
user rejected in-between pictures (finding 049), so it is not offered to players. The D3D11 code and
`gate_native_renderer.py` are gone (finding 088); state traces are the check now (finding 081).

Round 29: the native renderer (`DHD_RENDER=native`) and interpolated presentation (`DHD_INTERP=1`)
were committed, off by default. The exact GS backend stayed the default, the reference and the
referee. `port/tools/gate_native_renderer.py` passes: exact and native arms of one exe match at
all six EE moments, every readback record matches, and both match the r27-defined-native
reference at GO+0/2/12/16. User direction for this round: pictures need not be bit-identical to
PCSX2 any more; only game state (simulation, riders, timers, RNG, everything the EE reads) must
stay exact.

## Step 1: cleanup

The uncommitted build31-43 experiments were dropped (diffs archived under
`build/port/archive-r29`, ignored) and the round-27/28 accepted renderer committed. Relinked on
the accepted r28 object basis, the cleaned tree passes `gate_hw_renderer.py`.

## Step 2: a fair benchmark

`port/tools/bench_race.py` times one recorded race, virtual GS fields 6000-7000, unpaced
(`DHD_FRAME_CLOCK=1`), no diagnostics, `DHD_BENCH_EXIT=1`. Candidates run interleaved, three runs
each; the summary gives the median, spread, median p99 and median worst-1% field time. Windows
slows a background window a lot, so the tool keeps the game window in front every half second
(restore, Alt tap, `SetForegroundWindow`) and records each run's focused share; a run on a busy
PC or out of focus is not a measurement. With the native renderer it also reads the
`bench interp:` line: pictures displayed per second over the same window and the time between
presents (p50, p99, mean of the longest 1%, max).

Ryzen 5 7600, RTX 3070, idle PC, every run 100% focused. Unpaced (fields per second the game can
run, `DHD_FRAME_CLOCK=1`):

| Build | Median fields/s | Spread | p99 field | Worst 1% field |
| --- | ---: | ---: | ---: | ---: |
| build31 fast preview (baseline) | 68.88 | 0.53 | 22.05 ms | 26.14 ms |
| cleaned accepted tree | 66.09 | 0.99 | 22.33 ms | 25.37 ms |
| native renderer (native15) | 75.42 (+14.1%) | 2.81 | 17.06 ms | 17.88 ms |

A first unfocused attempt on a busy PC (55-68 fields/s, spreads up to 11) is discarded.

## Step 3: the native renderer

`port/gs/gpu_native.inc`, selected by `DHD_RENDER=native` (the exact backend is the fallback and
the reference).

- The GS worker records each field as a command list (draws, local copies, uploads) of native
  vertices and state: a real D3D11 depth buffer, hardware texture sampling with a decoded-texture
  cache, fixed-function blending, real render targets per GS frame buffer. Blend, alpha test,
  fog, depth test and write masks are mapped; approximations count in the profile line
  (`blend_approximate`, `date_ignored`, unsupported texture reasons). A render thread draws and
  presents the list at vsync; the EE never waits for the GPU.
- The source-alpha blend with a ZERO or ONE destination factor premultiplies in the shader
  (flag 512): GS alpha 0x80 means 1.0, and D3D's source factor would clamp the doubled range
  (the sun halo was too dim).
- **What the EE reads stays exact.** The game reads a 4x4 PSMZ32 patch of the Z buffer under each
  lens flare (`sceGsExecStoreImage` from `func_00228328`) and uses it the next frame. The exact
  backend runs on its own device: every Z write under a watched probe rectangle is logged and
  replayed exactly, clipped to the probe, in order (out-of-order replays mask colour writes,
  except a colour write into the Z memory itself). Probe pieces are mapped page by page.
- The probe can sit at the Z buffer's bottom edge (rows 221-224 of a 224-row buffer at page 0),
  so row 224 lies in the next buffer's memory: sky colour in frame buffer 56, darkened by the
  rider-shadow render target (page 508, from page 510, a downsample of frame buffers 56/112). Z
  and colour block layouts swap page halves there (`ox ^ 32`, `oy ^ 16`; PCSX2
  `GSLocalMemory` block tables). While the probe is within 32 rows of the bottom edge the exact
  backend draws whole frames: every render target an exactly drawn primitive samples joins the
  exact set, and the four rows past the Z buffer (the band) are drawn exactly. Two seconds after
  the probe leaves the edge these exact frames stop. Always-on, they cost 59.1 fields/s with
  200 ms worst 1%; lazily, they cost nothing measurable outside those moments.

### The last divergence was a port ABI bug, not the renderer

With all readbacks matching, native still differed from exact at GO+3600/4500 (two native runs
matched each other). EE dumps bisected the first difference to GO+738: a rider float 1 ulp apart
(`func_001238B8` storing 0xBEA7891B instead of 0xBEA7891C). Host state at the function's entry
(hardware breakpoint, FXSAVE dump) showed MXCSR equal but the x87 stack holding seven values in
native and none in exact. A per-kernel-boundary tag-word check found one value leaking at field
3946 in both modes; a scan of the linked exe for calls to x87-returning functions without a pop
found `func_00174590` and `func_00235F38` calling `fabsf` without a prototype. Retail (gcc 2.95)
emits `abs.s`, but clang called the CRT as an implicit `int fabsf()`: the caller compared EAX
and never popped ST0. Once the stack filled, later float returns overflowed it, and whether that
happened depended on where host code (fiber switches, exception handling, D3D) emptied the
stack. `port/patches.py` now gives both units `__builtin_fabsf`; the rebuilt objects were checked
to match the basis code before the patch. With the fix the native gate matches at all moments
and the early moments still match the reference.

Tools (ignored, `build/port/archive-r29`): `bisect_ee.py` (exact and native arms with extra dump
moments), `diag_kernel.py` (per-field kernel-boundary trace, MXCSR and x87 checks, sub-field EE
dumps), `diag_watch.py` (hardware write watchpoints and execution breakpoints with FXSAVE
dumps), `x87_leaks.py` (unpopped x87 return scan of a linked exe).

## Step 4: interpolated pictures (DHD_INTERP=1)

The simulation stays at 59.94 Hz on the virtual frame clock. The render thread keeps the two
latest fields and draws a picture per display refresh (`Present(1)`), or per `DHD_INTERP_HZ`
tick (a high-resolution waitable timer, `Present(0)`). Vertices are paired per segment: the
primitives of one GIF tag (a VU1 batch, a piece of a mesh) under one draw state. Each segment of
the newer field takes the older field's next unused segment (looking at most 256 ahead) with
the same vertex count and the same state hash; paired vertices are blended in x, y and z at
`t = (now - newer.time) / period`. Pairs whose vertices jump more than 96 pixels, unpaired
segments (particles, flare, HUD, pieces whose clipping changed) and periods outside 4-100 ms
(pauses, loading) show the newer field as is.

The first version paired whole draw commands and included the render target in the state. Draw
commands merge many VU1 batches, so clipping anywhere changed their counts, and the game draws
alternate fields into its two frame buffers (pages 56 and 112), so the main scene never paired:
8.6% of vertices, the rest moving at 60 Hz in 120/144 pictures. Per segment and without the
target, 83% of race vertices pair (held 16%, jumped 0.5%). `DHD_INTERP_TRACE=1` prints the
pairing every 120 fields. The recorded native command lists
are re-submitted; no game code runs again. Each field is shown at least once (one new field per
picture), so every field's uploads and copies reach the surfaces. Latency is one field.

Paced at 59.94 Hz (`DHD_FRAME_CLOCK=0`), fields 6000-7000, native16, 3 interleaved runs:

| Mode | Fields/s (game) | Pictures/s | Present p50 | Present p99 | Present worst 1% | Game worst 1% |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| native, no interpolation | 59.94 | 59.93 | 16.68 ms | 17.11 ms | 18.01 ms | 17.90 ms |
| interpolated, 120 Hz | 59.94 | 119.99 | 8.33 ms | 8.89 ms | 8.98 ms | 18.01 ms |
| interpolated, 144 Hz | 59.21 (spread 9.68) | 143.69 | 6.95 ms | 7.16 ms | 7.85 ms | 37.80 ms |

Over the window 79% of vertices pair, 20% are held, 0.9% jump. The race dumps still match with
interpolation (gate with `DHD_INTERP=1 DHD_INTERP_HZ=144`: MATCH, 1,144 readbacks).

At 144 Hz the game thread stalled in two of three runs (one field took 221 ms, another 3.3 s)
while pictures kept coming (no present interval over 14 ms); the earlier 144 Hz series showed the
same once (3.9 s) and 120 Hz never did. The render thread is not the one stuck; the cause is not
found yet (suspects: the exact backend's whole-frame and readback work competing for the GPU with
144 pictures per second). 120 Hz is the recommended setting until then.

Artifacts: judged from the pairing counters only, not yet by eye at speed. Held pieces (particles,
the flare, HUD, mesh pieces whose clipping changed this field) move at 60 Hz inside a 120/144 Hz
picture, so they can look less smooth than the scenery around them; a piece that changes from
paired to held snaps to the newest field.

## Launchers

`build/port/fast-preview/Play.ps1 -Native` (native renderer), `-Interp` (native plus
interpolation at the display refresh), `-Hz N` (a fixed picture rate), and
`Play-native-120.cmd`.

## Left

- The 144 Hz game-thread stalls (above).
- Interpolation pairs GIF-tag pieces by order, state and vertex count; pieces whose clipping or
  level of detail changed, particles and the HUD are held (about 20% of vertices).
- The pictures have not been checked by eye at 120/144 Hz yet.
- The native renderer still approximates destination-alpha tests and some blend modes (counted).
- Exact whole frames near the flare edge cost a few milliseconds per field while active.

## Sources

- PCSX2 `GSLocalMemory` / `GSTables` block and column tables (Z versus colour block layout).
- PCSX2 `GSState`/`GSRendererHW` for the GS draw model (blending, alpha test, scissor, SCANMSK).
- Microsoft docs: `IDXGISwapChain::Present` sync interval, flip model presentation,
  `CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`.
- Microsoft x86 calling convention: float and double results return in ST0 and the caller pops.
