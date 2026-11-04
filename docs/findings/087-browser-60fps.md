# 087: Browser speed toward 60 fps

Status: current
Supersedes: parts of 083
Checked: 2026-11-04

2026-10-25, branch `render/rust`. Shared context: the browser runs at about 25-30 fields/s on the
title and 17-25 in a race (finding 086 W1, W4); the game worker is CPU-bound. One section per task.

## BS1: plain IEEE floats in the graphics reactor

**Before.** `postlink.py` rewrote every float op of the graphics reactor (`graphics.wasm`: VIF1/VU1,
the GS front end, `host_graphics.c`, the Rust provider glue) into the exact helpers of
`port/web/float.c` / `double.c`: 1010 f32, 177 f64 and 3729 f32x4 ops. VU1 ran under the VU mode
(toward zero, FTZ, DAZ) through `web_vmul`/`web_vadd`/`web_vsub`, which emulate round-toward-zero in
binary64, two promotes and a demote per half (finding 086 W4: 22-37% of the game worker). The GS
front end already ran in IEEE mode (W4), where the helpers are a call, a load of the shared FP word
and the plain op.

**Changed.** The reactor's float ops stay plain wasm ops: f32/f64, and the f32x4 SIMD that simde
makes of VU1's SSE code under `-msimd128`. `build_graphics.py` no longer passes the reactor through
the float rewrite and builds it with `-DPS2_VU_IEEE`. Under that macro (reactor only):

- `vu0_interp.c` `operand4` does all of arithmetic4 (a denormal operand reads as its signed zero,
  exponent 255 as the largest finite magnitude): the DAZ the VU mode gave, plus the existing clamp.
  No optimizer barrier (there is no host rounding mode for constant folding to disagree with).
- FMAC results get the same clamp (`ieee_clamp4` in `fmac_flags4` and the native kernels'
  `native_result`): FTZ, and the largest finite value where round to nearest gives inf. MAC flags
  are computed on the clamped value, as natively.
- `ps2_vu1_run` and `graphics_gif_feed` no longer read or write the shared FP control word
  (0x07800000): nothing in the reactor reads it any more. (The `web_x_*` entry wrappers still save,
  set and restore it; they can go too once the reactor runs on its own worker.)

What is left different from x64: the rounding of finite VU1 results (nearest, not toward zero; the
last bit), and RN rounding up to FLT_MIN where toward zero would flush. VU0, the core module and
x64/i686 are untouched. `--exact-float` (build_graphics.py, and build.py passes it on) builds the
old exact reactor, e.g. for comparisons with the x64 pictures.

**Measured, not guessed: the special cases are live.** `--vu-ieee-probe` counts them
(`web_vu_ieee_probe`, in run_headless.mjs's report). Node, 4000 fields to the race start
(race_compare's NAV and pad): 79.8M denormal operand lanes and 2.7M exponent-255 operand lanes (VF
registers holding integer data); 52,912 FMAC result lanes that underflowed to a denormal and 280,089
that overflowed to inf. Without the clamps the zero flags would differ and infs would reach VU
memory and the GS, so both clamps stay. Their cost is a few integer SIMD ops per FMAC.

| | before | operand clamp only | final (operand and result clamps) |
|---|---|---|---|
| title (fields 300-750) | 21.3 | 35.1 | 42.6 |
| race (fields 3000-3500) | 17.4 | 23.7 | 23.1 |
| race (fields 3500-4300) | 21.2 | 28.8 | 27.8 |

The title gains most (VU1 was most of its time); in the race the core and the readback waits are a
larger share. Single runs on a shared PC: about 15% noise. `check_browser.mjs` with the race query
reached field 3945 in 119 s, with the race on screen.

Node (`run_headless.mjs`, no GPU) ran the 4000 fields at 38-44 fields/s with the probe builds (W4:
20-21 on the title before). Screenshots of the title and the race look as before. The optimized
reactor grows from 2.70 MB to 2.95 MB (inline SIMD and the clamps instead of calls).

## BS4: fewer WebGPU calls per field in the native renderer

**Measured how.** A scratch driver outside the repo (headless Chrome as `check_browser.mjs` drives
it) served `worker.js` with a prefix that wraps every method of the `GPU*` prototypes in the worker
and counts the calls (and `writeBuffer`'s bytes by buffer label), then read the counts per field
over field windows. Race: race_compare's `NAV` / `RACE_SYNC` / `RACE_PAD` / seed 81257, scale 2,
native renderer (the default). A second set of runs, without the counter, alternated the old and the
new renderer module (same core and reactor) and took a 15 s CPU profile of the game worker from
field 3600.

The calls were per draw: every draw set the viewport, the scissor, the blend constant (in the
browser that builds a JS dictionary too) and both bind groups again.

| WebGPU calls per field | title / menus (300-700) | race countdown (3000-3500) | race (3600-4300) |
|---|---|---|---|
| all, before | 340 | 2968 | 2641 |
| all, after | 181 | 1324 | 1140 |
| draw, before / after | 49 / 49 | 455 / 414 | 414 / 382 |
| setBindGroup, before / after | 98 / 74 | 904 / 620 | 820 / 550 |
| setViewport, setScissorRect, setBlendConstant, before | 48, 49, 48 | 451, 452, 451 | 409, 410, 409 |
| the same, after | 4, 5, 0 | 16, 17, 5 | 12, 13, 0 |
| writeBuffer, before / after | 2.9 / 2.0 | 20.9 / 19.6 | 11.5 / 10.5 |

**Changed** (`port/render/src/native/`, the native renderer only; the exact renderer, its
files and every locked file are untouched; x64 gets the same code):
- `render.rs`: the open pass remembers what it has set (viewport, scissor, blend constant,
  group 0's offset, group 1) and a draw sets only what differs. WebGPU keeps this state for the
  rest of a pass, across pipeline changes (all draw pipelines share `draw_layout`); a new pass
  starts empty.
- The blend constant is set only for a draw whose blend reads it (BLEND_FACTOR or
  INV_BLEND_FACTOR as a colour factor; the alpha factors are One and Zero).
- A draw whose constants are byte-equal to the previous draw's binds the same arena offset (no new
  copy, no new `setBindGroup(0)`).
- Draws are coalesced: a draw waits until the next call on the pass; a next draw that starts
  where it ends (whole triangles only) with nothing set in between extends it. draw(a..b) then
  draw(b..c) with the same state is draw(a..c): the same triangles in the same order. Every state
  call and the pass's end issue the waiting draw first. The per-command counts
  (`DhdrPictureResult`) are unchanged.
- `scanout.rs`: the scanout's bind group is kept while the display surface's view and the display
  texture stay the same (was one `createBindGroup` a field). `present.rs`: the present rectangle is
  written only when it changes (was one `writeBuffer` a field).

**What is drawn** stays the same: every change only drops a call that sets a value the pass already
has, or joins two draws that WebGPU would run back to back with the same state. A race screenshot
taken at field 4000 with the first two changes was identical to the old renderer's (0 of 1,240,668
game pixels differ); later screenshots caught other fields, so they only show the picture is right.

**Time.** In the worker profile the renderer's JS glue (`dhd_render.js`, the WebGPU calls under it
included) took 3.83% and 3.73% of the samples before, 3.34% and 3.42% after; without `writeBuffer`
1.35% / 1.29% before, 0.87% / 0.86% after. So about 0.4% of the worker's time, a third of the
per-call cost. Fields/s moved with the shared PC far more than that (title fields 400-1000 in three
alternating pairs: old 15.6, 21.1, 17.5; new 24.4, 21.9, 32.5; race 3000-3500: old 18.8, 18.1; new
17.2, 19.1): no change is measurable in fields/s.

**What is left.** `writeBuffer` is 2.4-2.6% of the worker, nearly all of it bytes, not calls: the
native vertices are about 4.0 MB a field on the title and 3.8-3.9 MB in the race (125,000 vertices
of 32 bytes). The C front end (`native_core.inc`) writes 3 vertices per triangle (strip triangles
are not shared) and 6 per sprite. Fewer bytes would need indexed draws or a smaller vertex, which
changes the picture ABI (`dhd_render.h`, `abi.rs`) or the shaders, all locked. The rest per draw is
`setBindGroup(1)` (the texture changes on almost every draw: 353-367 a field), group 0's offset
(197-253: the constants differ) and the draw; fewer of those needs the constants or textures indexed
in the shader (locked). In the race, the exact renderer's Z probe writes about 8-9 buffers a field
(`upload`, `primitives`, `states`, `bins`, `batches`, ~270 KB) and copies 5-11; that path is exact
and was left as it is.

Sources: WebGPU specification (render pass encoder state: bind groups, viewport, scissor and blend
constant persist for the pass and survive `setPipeline`; draw ordering), wgpu 29.0.3
`src/backend/webgpu.rs` (each call is one web-sys call; `set_blend_constant` builds a
`GPUColorDict`; dynamic offsets go through a `Uint32Array` view), Chrome DevTools CPU profiler.

## BS2: the graphics side in its own Web Worker

**Before.** The browser ran everything in one worker: the core called the graphics reactor
(`graphics.wasm`: VIF1/VU1, the GS front end, the Rust renderer) synchronously through
`www/thunks.js`, so the EE, VU1 and the GS took turns on one thread. Natively they run at once:
`port/hle/graphics_queue.c` (the VIF1/GIF worker) and `gs_queue.inc` (the GS/D3D worker) copy each
packet into a ring and the EE waits only where it reads graphics state.

**Changed.** `python port/web/build.py <core> --renderer rust --threads` builds a two-worker
browser: the core in the game's worker (`www/worker.js`), the reactor and the Rust renderer, which
owns the WebGPU device and the canvas, in `www/graphics.js`, on one shared `WebAssembly.Memory`.
`worker.js` picks the mode from the core module (a shared memory means threaded), so the page and
`check_browser.mjs` are the same for both. Without `--threads` the build and the page are the
one-worker build as before.

- **The queue** (`port/web/web_queue.h`, `web_queue.c`: `graphics_queue.c`'s ring on wasm's
  atomics). The core's side is a small module of its own, `build/web/gqueue.wasm` (no libc, no
  data, no stack; it imports only `env.memory`), whose `web_x_*` exports the core's graphics
  imports link to through the same call_indirect thunks as before. In program order it queues:
  - VIF1/GIF data (`host_service_graphics_feed`, `host_service_vif_feed`): copied, not waited for;
  - the 16 GS/VU1 calls: the core waits for the 13 that return a value or write guest memory
    (register and CSR reads, readbacks, snapshots, the trace, `gs_reset`); `gs_vblank`,
    `gs_priv_write` and `gs_trace_begin` are queued without a wait;
  - `host_service_graphics_wait` (every place the native EE waits for its workers: the vsync, the
    mapped VU1/VIF1/GIF/GS registers through `ps2_graphics_sync`, `sceGsSyncPath`, the Z probe)
    waits until the ring is empty;
  - per field: the field number (`web_frame`: the reactor's `kernel_vsync_count` and its
    `host_wall_seconds`), the vsync, and every `host_putenv` (the reactor's environment).
  The reactor's side, `web_q_run`, runs the packets in that order, so the graphics state sees the
  call sequence of the one-worker build exactly: what does not wait natively does not wait here,
  and nothing that waits natively is skipped.
- **The vsync** is queued, not waited for: `web_display` (the native renderer draws the field) and
  `web_present` run in the graphics worker while the core goes on to the next field, until its
  next graphics wait. The GS register MMIO pages that `web_display` reads (0x12000000 + 16 i) are
  copied into the vsync packet when it is queued (`web_gs_mmio`), since the core goes on writing
  them. After the present the graphics worker ends its task, so the canvas commits.
- **The VU1 image check** (`web_vu_ready`: the core loads the microcode image into guest RAM) is
  made by the core's side when a feed is queued and travels with the packet: the reactor loads
  the image at the same packet as in the one-worker build.
- **Waits.** The core spins briefly, then blocks in `memory.atomic.wait32` (a dedicated worker
  may). The graphics worker spins briefly, then returns to JS, which waits in
  `Atomics.waitAsync` on the ring's write position, so its event loop (WebGPU's map callbacks,
  the canvas commit) keeps running. A readback inside `web_q_run` suspends it through JSPI, as
  before. Each side announces its sleep and reads the other's position again before it sleeps,
  and the other side notifies after publishing (graphics_queue.c's handshake). If the graphics
  worker fails it sets a flag; a waiting core then traps instead of hanging.
- **Memory.** The core module that `build_wasm.py` links is not touched (it is the x64 build's
  wasm2c input; its offset scan is unchanged): `port/web/threads.py` sets the shared flag on the
  browser's copy after `postlink.py`, on the reactor's memory import and on the Rust module's.
  Plain loads and stores are valid on a shared memory; each module is still instantiated once, so
  its active data segments are applied once. The maximum stays 2 GiB.
  New fixed places in the web host's scratch region, next to the core's FP control word
  (0x07800000):
  - 0x07800040: the reactor's own FP control word (two threads cannot share one). Since BS1 only
    `--exact-float` reads it (its float helpers are built against it, `WEB_CSR_AT`, and VU1's VU
    mode switches it). `web_q_init` sets IEEE mode and it stays there at rest, so the threaded
    reactor's calls no longer save, set and restore it, as BS1 anticipated; the one-worker
    build's wrappers still do (the word there is the core's).
  - 0x07960000: the top of the reactor's float/memory helper stack (`postlink.py
    helper_stack`; the core's is 0x07920000).
  - 0x07A00000: the queue's control block; 0x07A01000: the scratch line for `web_q_env`. The ring
    (8 MiB) is in the reactor's bss.
- **JS.** `TextDecoder` refuses views of a `SharedArrayBuffer`: both workers decode copies
  (`graphics_common.js`). wasm-bindgen's glue renewed its cached `Uint8Array`/`Uint32Array` views
  only when the buffer was detached, which a shared buffer never is: `threads.py` makes it renew
  them when `memory.buffer` changes (what wasm-bindgen emits for shared memories).
- **Node.** `run_headless.mjs` runs a threaded build the same way, with the reactor in a worker
  thread (`headless_graphics.mjs`).
- **Not changed.** x64 and i686 (no file they build); the one-worker browser build (its reactor
  only gains `web_field` and the `web_gs_mmio` indirection, which read the same values).

**Open: `check_web_layout.py` (locked) refuses the threaded build.** Rule M2 forbids shared
memories, which the threaded build needs by design. All the other rules (D1, D2, R1-R5, M1, M3) pass
on it, and the one-worker build passes all of them. The checker also does not know the new fixed
places above or `gqueue.wasm` (no data, no stack, only the memory import).

**Checks.**
- State trace, `run_headless.mjs --fields 600`, the threaded build against the one-worker build
  of the same commit, before the merge of BS1/BS4 (render/rust 12025b835) and after it:
  `trace_columns.py` PASS both times, all 13 columns identical (`gs_stream`, `gs_regs`,
  `gs_vram` and `cpu` included). The VU1 counters match too (630390 native kernel runs;
  29384262 steps before BS1, 29384116 with it, in both modes).
- The one-worker build: `check_web_layout.py --require-renderer` OK; `run_headless.mjs` and
  `check_browser.mjs` run as before. `gpu_js_web.c` (the JS provider, for `web_gs_mmio`):
  syntax-checked with the reactor's flags.

**Results.** Headless Chrome through `check_browser.mjs` (race: race_compare's NAV, pad and seed;
scale 2, the native renderer), fields/s over the same field windows, the two builds alternated on
the shared PC (single runs: about 15% noise). Title windows count only the title scene's fields (see
below).

| fields/s | one worker (before) | two workers (after) |
|---|---|---|
| **without BS1/BS4** (12025b835), three pairs: title (300-1000) | 21.0, 20.3, 22.2 | 24.0, 22.4 (45 fields), 26.4 |
| race countdown (3000-3500) | 17.4, 17.2, 19.8 | 26.4, 31.2, 25.8 |
| race (3500-4300) | 20.9, 20.6, 13.9 | 31.7, 38.6, 30.4 |
| **with BS1 and BS4** (render/rust 78cc9d6c0), one pair, fresh profiles: title (300-1000) | 30.5 | 50.5 |
| race countdown (3000-3500) | 25.7 | 40.2 |
| race (3500-4300) | 33.1 | 56.1 |

Without BS1 the threads gain about 15% on the title, which the graphics worker bounded (VU1 under
the exact float helpers), and 1.5-2x in the race. With BS1's plain floats the gain is about 1.65x on
both (the race reached 59.7 fields/s at its last status, field 4415). Readbacks (851 by field 4300)
and the pictures are as in the one-worker runs.

**What is left.**
- VU1 and the GS front end still share one worker; natively they are two threads. Splitting them
  needs the reactor twice on one memory, which means a threads build of the reactor (wasi-libc's
  `wasm32-wasip1-threads`: a stack and TLS per instance, a locked malloc, passive data segments)
  and the `gs_queue.inc` ring between the two.
- Every waited call is a round trip (about 10-50 us when the other side sleeps). The vsync's
  CSR read waits for the whole field, as natively, so the core cannot run further ahead than that.
- With the default Chrome profile (`build/web/chrome-profile`, kept between runs), the idle title
  flow differed between runs in builds of both kinds: in some runs the menus moved on and a race
  loaded during fields 300-1000. The title numbers count only the title scene's fields (45,000 to
  60,000 draws per field). The merged pair used a fresh `--user-data` per run and stayed on
  "PRESS X TO START".

Sources: `port/hle/graphics_queue.c` and `gs_queue.inc` (the native rings and their handshake), the
WebAssembly threads proposal (shared memories, `memory.atomic.wait32`/`notify`; a shared memory
needs a maximum, an import must match the shared flag), the HTML/ECMAScript `Atomics.waitAsync` and
the Encoding standard's `TextDecoder.decode` (not `[AllowShared]`), WebGPU's
`GPUQueue.writeBuffer`/`writeTexture` (`AllowSharedBufferSource`), wasm-bindgen's glue for shared
memories (views renewed on a new buffer), and Emscripten's `UTF8ToString` (decodes a copy of a
shared view).

## BS3: cheaper core memory access, compiler and optimiser settings

**What the memory checks were.** The core module (`dhd_core.wasm`, the same module wasm2c turns into
the x64 build) uses guest addresses as wasm addresses. EE RAM lives at 0x08000000; the game also
reaches it through the low view (0x00400000..0x01FFFFFF) and the uncached windows (0x2xxxxxxx,
0x3xxxxxxx). Natively the OS maps those windows onto the same pages (`port/host64/memory_access.h`).
A browser can't alias pages, so `port/web/postlink.py` (web only) rewrote every core access whose
address it couldn't prove canonical into a call to a per-shape wrapper (`web_access_N`). The wrapper
tests the base against the canonical intervals and does the native access, or else calls
`memory.c`'s checked fold (`web_prepare`/`web_finish`). The checks exist for address translation
only. They are not a sandbox: wasm's own bounds checks already confine the module. Debugging uses
`--debug-memory`.

**What the race does with them.** A counting build of the wrappers, headless to field 3800 with
race_compare's NAV, made 10.9 G wrapper calls (about 2.9 M per field). 98.7% were canonical, 1.07%
were low view and 0.26% were in the 0x38000000 window. Accesses to 0x2xxxxxxx were negligible
(1566). The 8.2% of worker time was the call plus the test, not the fold: `web_prepare` and
`web_finish` together took 0.5%.

**Changed (`port/web/postlink.py`, browser only; on by default, `--no-inline-memory` turns it
off).**
- Each access that isn't a proven literal now tests its base inline against the two alias
  intervals, adjusted for its memarg offset and width (the exact complement of the old canonical
  intervals). It is the original native instruction when canonical, so it keeps the native memarg
  and OOB trap. Only an alias base calls the wrapper. Each function gets at most a few scratch
  locals for this (the base and the stored value).
- An access whose offset alone puts every base beyond the alias windows (offset ≥ 0x3a000000,
  the core's indexed C data) stays native without any test: 24,514 accesses.
- The wrappers now fold a span that lies wholly inside the low view (+0x08000000) or the
  0x38000000 window (-0x30000000) with one native access at `a + offset + delta`. Every other
  alias address keeps the complete checked helper (crossings, bulk, the other windows).
- `--debug-memory` checks the inline path with `web_assert_unfolded` and the new folds with
  `web_assert_folded` (`memory.c`).
- Guest pointer values, the address translation and the stored values are unchanged. The x64
  core module and build are untouched (`build_wasm.py` is not changed; the core input to
  postlink is the same `dhd_core.wasm`). The GS reactor (postlinked with `memory=False`) comes
  out byte-identical. Any core variant that goes through `postlink.rewrite` (a shared-memory link,
  for example) gets the same lowering.
- Cost: the optimised core grows from 7.55 MB to 12.3 MB (raw).

**Results.** Fields/s were measured in headless Chrome on the shared PC, so they are rough.

| | before | after |
|---|---|---|
| race profile, core memory adaptation (self) | 8.7% | 0.2% |
| race profile, core (self, now including the inline tests) | 23.5% | 25.7% |
| race 3500-4400, Chrome fields/s (one run each) | 24.0 | 25.8 |
| title 400-1000, Chrome fields/s (2 interleaved pairs) | 26.1, 25.7 | 27.9, 27.0 |
| race 3600-4000, Node, main-thread CPU ms per field (2 interleaved pairs) | 38.5, 43.1 | 33.6, 37.3 |
| memory fixture loop (check_memory.mjs), folded/raw | 27x (finding 083) | 4.3x |

**Compiler and optimiser settings (measured, nothing kept).** Main-thread CPU ms per field in
Node (`process.threadCpuUsage`), interleaved runs; the PC was shared, so runs of the same build
varied by up to 25%.
- GS reactor clang `-O3`, `-O3 -flto` and `-O2 -flto` against `-O2`, and wasm-opt `-O4` and
  `-O3 --converge` against `-O3`: the title's CPU per field ranged 34-44 ms for every variant,
  with no consistent difference (lowest run: `-O2` 34.1, `-O3 -flto` 34.2). LTO grows the module
  by 60 KB. BS1 is changing the reactor's float lowering, which is most of its time, so this
  is worth measuring again after that.
- Core wasm-opt `-O4` against `-O3` (on the new postlinked core): race 34.2 / 40.8 against
  33.6 / 37.3 ms per field. No gain, and `-O4` takes 3 minutes instead of 1. The core's clang `-O2`
  is shared with the x64 module. A browser-only `-O3` core needs a second full core build, and the
  `-O4` result suggests there is little left there.
- Rust renderer: 0.3% of the worker's time in the race profile. Its release profile is already
  `opt-level = 3`, `codegen-units = 1`, thin LTO, plus wasm-opt `-O3`, and `Cargo.toml` is locked.
  Nothing to gain here.
- `-ffp-contract=off` and the exact float helpers are unchanged everywhere.

**Checks.**
- `node port/web/run_headless.mjs --fields 600` (state trace) with the old and the new core:
  `trace_columns.py` PASS, all 13 columns identical (`cpu` and `ram` included).
- `build_memory_test.py`, then `check_memory.mjs` and `check_fast_memory.mjs`: PASS. The
  shared-stack omission control still fails its leaf canary, as it must. The fixture postlinked
  with `debug_memory=True` also passes `check_memory.mjs`, through both certificates.
- `python port/web/build.py build/wasm/dhd_core.wasm --renderer rust` and
  `check_web_layout.py build/web --require-renderer`: OK.
- `check_browser.mjs` with NAV to field 4400: race picture (rider at the start, lens flare).

## WO1: browser PC settings

2026-10-26, branch `rr/WO1` (from main after the 64-bit/browser merge). The browser's OPTIONS > PC
SETTINGS showed only Display (`BROWSER_ROWS = 'DHD_WINDOW'`). Display is gone; the page now shows
the rows a web player can use:

| row | variable | values | default | applies |
|---|---|---|---|---|
| Resolution | `DHD_SCALE` | 1x-8x, Window (the canvas's match, up to 4x) | 2x | at once |
| Scaling | `DHD_PRESENT_FILTER` | Smooth, Sharp | Smooth | at once |
| Widescreen | `DHD_WIDESCREEN` | OFF, ON | OFF | page loaded again |
| Font | `DHD_FONT` | Original, Sharp | Sharp | page loaded again |
| Frame Rate | `DHD_FPS` | 60, 120, 144, Monitor | 60 | page loaded again |

Left out: Display, Window Size, Picture (the canvas has one shape, below), Texture Pack (none in the
browser), Skip Movies (the browser never plays them: `host_service_host_skip_movies` is 1) and
Vibration (`host_service_host_pad_vibrate` is a no-op here; the Gamepad API's `vibrationActuator`
could drive it later). There is no sound latency row in `pc_settings.c`. The defaults are the core's
own fallbacks; the settings persist in IndexedDB as before (`settings.js`, `host_setting_save`).

**Core (`port/hle/pc_settings.c`).** Two more host variables beside `DHD_PC_SETTINGS_ROWS`:
`DHD_PC_SETTINGS_LIVE` (rows the host applies at once besides the built-in live ones: the row then
sets its variable at once) and `DHD_PC_SETTINGS_RESTART` (the footer of a row that waits for the
next start). The row list match moved into `listed()`. x64 sets none of these, so its page is
unchanged (same rows, same live rows, same footer). The browser sets `DHD_SCALE` live and the footer
"* Applies when the page is loaded again"; worker.js also sets `DHD_SCALE_MAX=8` (the reactor's
`NATIVE_SCALE_MAX`) so the Resolution row lists 1x-8x as on x64.

**Resolution while the game runs.** x64 does not change the scale live (the row waits for the next
start; `n->scale` and every surface's scale are fixed once drawn). In the browser the row sets
`DHD_SCALE`; worker.js (one worker) or graphics.js (threaded; the change arrives in the queue's
environment message, in order with the graphics calls) calls the reactor's new export
`web_env_changed` for `DHD_SCALE`, `DHD_PRESENT_FILTER` and `DHD_ASPECT`. The next `web_display`
reads the environment again, applies the present options before the picture
(`web_native_present_options`) and, after the picture, `native_rescale` (`gpu_rust_web_native.inc`):
every scaled surface (bw >= `NATIVE_SCALED_BW`) starts over. Rust drops its texture (a
`dhdr_native_surface` call with the other kind resets a surface; the next picture asks for every
surface again, with the right kind at the new scale, so the frozen ABI is unchanged), and the pages
those surfaces owned are imported from GS memory at their next claim. What only the GPU had drawn
there is lost for at most one picture (the game draws its frame every field). `native_core.inc` and
the Rust crate are untouched. Scaling was already live in the core but
`host_service_gs_present_options_apply` was a no-op in worker.js; the same path makes it work.

**Frame rate.** The worker paced at a fixed 1/59.94 s. It now paces at the core's
`port_frame_period` (`host_pacing_config`, called at each vsync: 1001/60000 s at 60, 1/N above), and
`host_display_hz` (Monitor) is the display's refresh, which main.js measures before Start (median of
30 `requestAnimationFrame` intervals; 60 when no frames come within a second). fps.c keeps 60-144
and 60 online (the browser has no online play). What is missing: the browser core runs unpaced on
its frame clock (`DHD_FRAME_CLOCK=1`), so fps.c's wall-clock mode (`clock_interval`, which on x64
lengthens a step when a frame is late) is off. Turning the wall mode on would need a real
`host_wall_seconds` in worker.js (today field/60, kept for repeatable runs) and a core switch in
`kernel_init`.

**Page.** Display's fullscreen plumbing (the stored mode at Start, the `display` message) is gone; a
double click on the picture toggles fullscreen. With widescreen on (the stored setting, read at each
Start), the canvas is 16:9 (1708x960), else 4:3 (1280x960), so the wide picture fills it.

**Checks.** Build only: `build_wasm.py -j 2`, both web builds (threaded and `--no-threads`) with
`check_web_layout.py build/web`, and the x64 `package_release.py`. No browser or game runs. 
