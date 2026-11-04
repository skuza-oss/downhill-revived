# 131: The draw pass from first principles: where an iGPU frame's time goes, and what is left

Status: current
Checked: 2026-11-04

2026-11-01/02. Result: the iGPU holds 60 fps at 5x (was 4x); at every scale its GPU time per frame
is 22-24% lower (4x 12.02 -> 9.11 ms, 8x 45.13 -> 35.30 ms uncapped), with the game unchanged and
the picture unchanged except for one-step rounding on DX12 (section "The DX12 picture change").

## 1. The cost model

### A new counter: DHD_GPU_STATS

Finding 130's pass timestamps say that the draw passes are ~90% of the frame, not which draws.
`DHD_GPU_STATS=N` (`port/render/src/native/gpu_stats.rs`, diagnostics, off by default) wraps
every draw of every N-th picture in a pipeline-statistics query (fragment shader invocations:
pixels actually shaded, after early depth rejection) and two in-pass timestamps, and logs per kind
of draw (shader path from the draw's flags, texel scale, pipeline variant, depth state, blending,
whether it writes alpha): draws, Mpixels shaded, ms and ns per pixel, and per picture the passes,
pipeline switches and distinct pipelines. wgpu has pipeline statistics on Vulkan and Metal, not
DX12 (there: times only), so the pixel counts below are from the iGPU on Vulkan, which finding 130
measured within a few percent of DX12 at 8x. In-pass timestamps stop the GPU overlapping draws:
the times are shares, not the frame's real time.

### What a race frame is (base build, iGPU, Vulkan, inside the GO window)

| | 4x (2048x1792 = 3.67 MP) | 8x (4096x3584 = 14.7 MP) |
|---|---:|---:|
| pixels shaded per frame | 13.3-15.1 MP | 52.9-58.9 MP |
| overdraw (shaded / target) | 3.6-4.1 | 3.6-4.0 |
| draw calls (after merging) | 354-422 | same |
| pipeline sets / distinct pipelines | 80-109 / 21-24 | same |
| render passes | 12-16 | same |

By kind (4x; three sampled windows; 8x has the same shares):

| kind | Mpix/frame | share of draw time | ns/pixel |
|---|---:|---:|---:|
| textured, bilinear, fog, dual-source blend (As), Z GEQUAL no write, alpha not written | 7.0-8.4 | 50-65% | 0.82-0.91 |
| textured, bilinear, fog, opaque, Z GEQUAL + write | 1.5-1.8 | 10-17% | 0.78-1.07 |
| untextured sprite, Z ALWAYS + write (the frame's clear) | 1.84 | 4-5% | 0.27-0.29 |
| everything else (HUD, alpha-tested foliage, sprites, ...) | ~2 | ~20% | |

So per frame the GPU shades about four screens of pixels, two of them in one kind: the terrain's
blended second layer over the opaque first (the game draws the slopes in two passes). The game's
own draws fix the overdraw (blended layers cannot be skipped); what the renderer controls is the
cost per shaded pixel.

### The floor and the shader's parts

The clear is the floor for a pixel that only rasterises, tests and writes: 0.27 ns/pixel. The main
kind cost three times that. Its shader was the uber-shader: every draw ran `native_pixel` with the
path chosen at run time from the uniform (texture or not, TFX, TCC, bilinear, fog, alpha test and
its failing pass, premultiply, sprite, replacement, four CLAMP modes per axis), with the registers
of the worst path, every uniform branch, a `discard` (which turns early depth into late depth on
depth-writing draws) and a bounds test with `textureDimensions` around each of the bilinear's four
loads.

What the per-pixel work must be at minimum, exactly as the shader defines the picture: four texel
loads (the GS bilinear is not the hardware's; finding 130 and the shader's own maths), three
vector mixes, the TFX multiply and floor, the fog mix and floor, the blend in the ROP (colour read
and write), the depth test (read). Measured parts of the main kind (after step 2.1, 4x, Vulkan,
local experiment builds that change the picture, never committed):

| variant of the main kind | ns/pixel |
|---|---:|
| as committed in step 2.1 | 0.58 |
| point instead of bilinear (1 load, no mix) | 0.35 |
| no fog | 0.54 |
| without the `round()` of step 2.1 | 0.50 |
| one textureGather of packed texels instead of 4 loads (prototype) | 0.46 |
| no texture at all (raster, depth test, blend) | 0.24 |

The bilinear's four loads and mixes are the largest part (~0.23), then the shader's fixed
overheads; fog and rounding are small. Bandwidth (colour read/write, depth read, ~12 B per blended
pixel) is inside the 0.24 floor.

Outside the draw pass at 8x: scanout ~3.4 ms (a 117 MB copy of the picture into the display
texture, at ~34 GB/s: bandwidth-bound already) and present ~1.3 ms (minifying the 4096x3584 picture
into the window, cache-bound). Conversions 0.06 ms.

## 2. Candidates, estimates, results

| candidate | estimate | result |
|---|---|---|
| 2.1 specialise each draw's pipeline on its state (no uber-shader, no discard where no alpha test) | large: ~40% of draw per pixel | done |
| 2.2 drop texel bounds tests that always pass / make them branch-free | ~20% of the main kind | dropping them: not repeatable on AMD; branch-free: no gain |
| 2.3 decoded textures as R32Uint, bilinear as one textureGather | ~20% of draw | done: 4x 87.4 -> 105.1 fps |
| single-source blending for As draws that do not write alpha | colour exports 2 -> 1 | measured, no gain: rejected |
| skip the scanout copy (present from the surface) | ~10% at 8x | not exact (filter edges, interlace, readback): not done |
| naga's runtime bounds checks off (trusted module) | small | none on DX12 (HLSL Load is already safe; naga adds array checks only): not done |
| narrower depth (D24) or colour formats | bandwidth | not exact for the GS's Z: not done |
| load/store ops, pass merging | small on an immediate-mode GPU (12-16 passes) | not done |

### 2.1 Specialised draw pipelines (commit "Native renderer: draw pipelines specialised ...")

Each draw's pipeline carries its state as an override constant (`native_spec`: flags, CLAMP modes,
alpha test, each with a "given" bit). Untextured draws carry no texture state and draws without
the alpha test no test, so a race uses ~30 distinct pipelines instead of ~21. New pipelines are
built on a worker thread (`dhd-pipelines`); until one is ready its draws use the generic
pipeline, which draws the same picture. Built at first use instead, they cost 95-264 ms frames
(one right after GO); with the worker the run's worst frames are the base build's (boot and menu
loads). `DHD_NATIVE_SPEC=0` turns specialisation off, `=sync` builds at first use (replays and
checks, so every draw runs the specialised shader). The browser builds them at first use.

The committed version also skipped the bounds test where every texel is inside the texture
(`inside`); 2.2 replaces that.

| 4x DX12 uncapped | fps | GPU ms | draw ms |
|---|---:|---:|---:|
| base | 77.41 | 11.808 | 10.646 |
| 2.1 as committed | 111.41 | 8.592 | 7.556 |

| 8x DX12 uncapped | fps | GPU ms | draw ms |
|---|---:|---:|---:|
| base | 22.08 | 44.504 | 39.785 |
| 2.1 as committed | 28.50 | 34.507 | 29.709 |

### Exactness: compilers fuse, and `round()`

Long captures (below) showed that removing branches lets shader compilers fuse differently:
`texel()` returned `t.rgb * 255.0`; computed alone that product is exactly the byte (f32(v/255)*255
rounds to v for all 256 v), but fused into the bilinear mix's add (FMA) the exact product,
v +- 8e-6, reaches a `floor()` and flips a pixel by one. `texel()` now rounds the product (and
step 2.3 reads decoded textures as integers, which needs no rounding). With it the generic and
specialised shaders give byte-identical pictures on FXC and on Vulkan.

### The DX12 picture change from `round()`: for a decision before release

`round()` changes what base drew wherever base's compiler had fused that product: the base value
was the fused, non-integer one, the new one the integer the maths means. Every difference is
exactly one step in one or more channels (largest channel difference 1 in every capture below);
no other kind of difference was found. Pixels changed per 20-field capture (1x 4.6 M, 3x 41.3 M,
4x 73.4 M, 8x 293.6 M pixels per capture; captures frame-000 .. frame-005: three menus or loads,
three race):

| GPU, compiler | 1x | 3x | 4x | 8x (race captures) |
|---|---|---|---|---|
| RTX, Vulkan | 0 | 0 | 0 | 0 |
| RTX, DX12 FXC (developer builds) | 0-4 | 8 | 12-42 | 100-138 |
| RTX, DX12 DXC (packaged builds) | 8-27,186 | 500 | 304-433,898 | 1,098-7,990 |
| iGPU, DX12 FXC | 0-4 | 10 | 22-56 | 80-90 |
| iGPU, DX12 DXC | 2-24 | 58 | 124-266 | 474-798 |
| iGPU, Vulkan (same AMD compiler as DX12) | see below | 10 | 22-56 | 80-90 |

The RTX rows are step 2.1's generic pipelines against base (the only picture change there is the
`round()`); the iGPU rows the final build's generic pipelines (2.2 and 2.3 add only integer and
select changes). The largest case, 433,898 pixels (0.59%) at 4x on the RTX with DXC, is the
menu capture frame-000 (a full-screen bilinear-filtered textured background); the race captures
change 304-2,114 pixels (0.003%) at 4x there. At 1x on the iGPU's Vulkan the counts mix with the
run-to-run variation below. VRAM (what the game reads) is identical in every run.

DXC (fast math, no `-Gis`) also reassociates other float maths by code shape, so under DXC the
specialised pipelines differ from the generic ones in a few pixels: final build, iGPU 4-22 per
capture at 3x/4x, 42-60 at 8x; RTX 0-86 at 1x-4x, 136-306 at 8x; by one step, except a few
pixels of the 4x menu capture on the RTX by two. FXC and Vulkan: none.

Options: keep it (DX12 then draws the integer bytes, as Vulkan always did; on the default packaged
path, DXC, about 0.003% of race pixels and up to 0.6% of a menu's change by one), or drop
`round()`, which keeps base's DX12 rounding only for the generic pipelines: the specialised ones
then fuse differently again (step 2.1 before `round()` was added: FXC 2-8 pixels in 1-3 fields per
capture, RTX Vulkan up to 9 fields per capture), so no option leaves DX12's pictures byte-identical
to base with specialisation on.

### The checks and their captures

New captures (`DHD_GS_CAPTURE`, base build, fields 900, 1700, 2500 (menus, loading) and 3300,
4100, 4900 (race), 20 fields each) replayed with `replay.exe` builds of base, and of the new code
with `DHD_NATIVE_SPEC=0` (generic) and `=sync` (specialised), on the RTX and the iGPU, Vulkan,
DX12/FXC and DX12/DXC, at scales 1, 4 (all six captures), 8 (the three race captures) and 3 (one):
every field's picture and VRAM compared (`build/gpu2/matrix.py`, per pixel). The frozen oracle's
native golden (finding 130's, RTX, Vulkan, g+3600 at 1/2/4x) compared with the unmodified
`gs_golden.py compare replay-native`: identical for specialised, generic and default runs.
`test_gs_wgpu.py`: PASS. `test_native_transfers.py` (dual/no-dual, `--presented`, `--stress`):
PASS, zero RGB and VRAM differences. Renderer unit tests (with a new one for `spec`) and the six
pipeline-key tests pass. (`gs_golden.py compare test-gpu` refuses: the installed golden is for an
older NVIDIA driver, as finding 130 found for the AMD one; no gate was loosened.)

### The AMD iGPU is not repeatable: no application race found

The first matrix (step 2.1 as committed, with `inside`) showed, on the iGPU at 1x only, specialised
pictures differing from generic in a few pixels by up to 120. So each configuration was replayed
several times against itself (`build/gpu2/repeat.py`: N runs of one capture, fields differing
from run 1; captures frame-003 .. frame-005, 1x, DX12 FXC unless noted). Earlier series:

| build, iGPU | runs | runs with differing fields |
|---|---|---|
| base, Vulkan | 6, 6, 5, 6, 6 | every run after the first: 1-6 fields |
| base, DX12 | 6, then 12 + 12 | none (0 of 36) |
| base, DX12, frame-005, a later series | 8 | 7 of 7 after the first |
| step 2.1 generic (`DHD_NATIVE_SPEC=0`) | 6 + 6 | none |
| step 2.1 specialised (all parts) | 6 + 6 | 2 of 5 and 1 of 5 after the first |
| step 2.1 without `inside` (flags, CLAMP, alpha test) | 8 + 8, then 12 + 12 | 2 of 40, one field each |
| step 2.2 (branch-free) | 10 + 10 | 1 of 18 |
| step 2.3 (gather), frame-004 / frame-005 | 10 + 10 | 1 of 9 / 9 of 9 |

Final series, one process at a time, 10 runs each (runs 2-10 against run 1):

| capture | base | final, specialised | final, generic |
|---|---|---|---|
| frame-003 (race start) | 0 of 9 | 0 of 9 | 0 of 9 |
| frame-004 (race) | 0 of 9 | 1 of 9 | 0 of 9 |
| frame-005 (race) | 1 of 9 | 9 of 9 (1-3 fields) | 0 of 9 |

The differences are a few pixels near the rider (pairs of rows, the interlaced field), often one
channel, by 1-16 and sometimes much more (green 2 against 84). The RTX was repeatable in every
configuration (its generic and specialised pictures were identical everywhere, which needs that).

Bisect of step 2.1's parts (6 runs each, frame-004, a temporary mask on `spec`, not committed):
flags, CLAMP modes, alpha test, `inside`, each alone: repeatable; flags plus CLAMP, flags plus
alpha test: repeatable; flags plus `inside`: 5 of 5 runs after the first differed.

Which draw: with a debug replay that draws only commands lo..hi of each picture (local, not
committed), bisecting the prefix on frame-005 (final build, 5 runs per step): draws 1-21 never
varied, 1-22 varied in 4 of 4 runs; draw 22 alone varies too (1 of 5, 4 of 7 with depth test
ALWAYS). Draw 22 is alpha-tested foliage-like geometry: textured, TCC, fog, bilinear, alpha test,
a 64 x 64 decoded texture with CLAMP repeat, Z GEQUAL with Z writes, no blending.

The first explanation written here (fragments landing out of primitive order) was wrong: D3D12
and Vulkan guarantee that colour and depth output are applied in primitive order, and the tests
below show the same primitive winning every time. Variation that grows with shader speed and load
usually means the application races: a shader reading what its pass writes, an unfenced copy, a
buffer rewritten while the GPU reads it. Each was tested on the iGPU (DX12 FXC, final build,
frame-005 unless noted; debug builds, none committed):

| question | test | result |
|---|---|---|
| do the GPU's inputs differ between runs? | hash of every picture's vertices, indices, commands with their constants, upload pixels, surfaces, display, and every decoded texture's bytes; 4 runs | identical (the converts' hash differs only by a texture pointer) |
| does the CPU change them while the picture renders? | the same hashes again just before the upload at the picture's end; 6 runs x 20 pictures | never changed |
| wgpu validation (DX12 debug layer, `DHD_WGPU_DEBUG`) | 4 + 10 runs | no message; still varies (1 of 9) |
| does draw 22 sample its target or depth, or a copy or conversion of them? | its command | no: a decoded texture of its own, written once by `write_texture` when made |
| late texture upload? | wait for the GPU after each decoded texture's upload | still varies (as many runs) |
| pictures overlapping (a buffer rewritten while the last picture reads it)? | wait for the GPU after every submit | still varies (4 of 29; base on Vulkan as before) |
| draw 22 in a render pass of its own | end the pass before and after it | still varies (2 of 9) |
| anything without draws? | all draw commands skipped (uploads, copies, conversions, scanout run) | never varies (0 of 9) |
| draw 22 alone, depth test ALWAYS, no blending | other draws skipped | still varies |
| which primitive wins? | draw 22 alone coloured by its primitive (`@builtin(primitive_index)`), 31 runs; by provoking vertex, 18 | always the same primitive at every pixel |
| what differs? | draw 22 alone writing its texel / vertex colour / fog factor and uniforms / interpolated s, t, q | only the interpolated texture coordinates vary: same primitive and pixel, `t` off by a few bits or, in some pixels, a value giving `fract(uv.y * 16) = 0` |
| any draw reading past what the picture wrote? | every index against the vertex count, every draw's range against the indices; 120 pictures of six captures | none |
| buffer reuse? | new vertex, index, uniform and upload buffers every picture | DX12: 0 of 29 against 6 of 29 (control) in one session, but the control fell to 0 of 29 later; Vulkan (base varies every run): large zero-filled new buffers 2 of 7 against 7 of 7, exact-size new buffers 9 of 9 (4-7 fields against 7-10); a ring of three sets: no change |

How often it happens follows the machine, not the code path: on DX12 the final build's frame-005
differed in 9 of 9 runs while other lanes compiled, in 6 of 29 and then 0 of 29 as the PC went
quiet, 1 of 11 under three CPU busy loops; memory-copy load slowed each replay twenty-fold. Extra
GPU work per picture (zero-filling large new buffers) makes it rarer; new buffers without it barely
change it. No access the application makes reads data that differs between runs, the inputs are
the same, the winning primitive is the same, and the value that differs is the attribute the GPU
interpolates for that primitive at that pixel. The RTX never varies, and base varies on the same
iGPU too (rarely on DX12, every run on Vulkan, frame-004: 7-10 fields). So no application race was
found; what remains points at the AMD driver or hardware (driver 32.0.21045.16010 / Vulkan
26.10.1) delivering a wrong interpolated attribute under load. Not fixed; neither a ring nor new
buffers per picture removes it, so neither is kept. A report to AMD would need a small standalone
repro (one alpha-tested draw replayed in a loop); not done.

Decision: `inside` goes (step 2.2): with it DX12 varied in most runs where base rarely did. Step
2.3 is kept: its variation is the same behaviour in the same few pixels, its gain is 20%, and no
draw path of it reads anything uncertain.

### 2.2 Bounds tests (commit "texel bounds tests stay, without a branch")

Without `inside` most of step 2.1's gain went: 4x 88.37 fps, GPU 10.417 ms (a first run during
other GPU load, 76.82 / 12.496, was repeated), against 111.41 / 8.592 with it. Testing against the
uniform's tex_size instead of `textureDimensions`: 88.51 / 10.225, the same. The branch-free form
(load from the clamped coordinate, select zero outside, the same values) was meant to let a
bilinear's four loads issue together, but it measured the same: 4x 87.36 fps / 10.609 ms (draw
9.424), 8x 24.02 / 41.116 (draw 36.305). So the cost is the test's ALU, not branching or the size
query: on two CUs about ten instructions per shaded pixel cost about 0.5 ms at 4x (15 M pixels),
and four bounds tests are about forty. The branch-free form is kept only because step 2.3 builds
on it (same speed, same values); the commit's real content is dropping `inside`.

### 2.3 Decoded textures as R32Uint, bilinear by one gather (commit "decoded textures as ...")

A decoded texture (the CPU-decoded ones: `Tex::create`) becomes R32Uint holding the same RGBA
bytes, so a texel is an exact integer without a unorm conversion or `round()`, and a 2x2
footprint is one `textureGather` (component 0 of an R32Uint texture is the whole texel) instead of
four loads. Draws reading one take `_u` entry points (`native_pixel_u`, `native_pixel_blend_u`,
`native_pixel_dual_u`: pipeline key bit 29, `TEXELS`, with a texels bind-group layout: uint
texture, non-filtering sampler). WGSL forbids two resources at one binding in one entry point, so
the float and uint paths are separate entry points over a shared `combine()`; `shade()` was split
into `fetch` / `fetch_u` and `combine` with the same expressions. The gather is used when the
texture is exactly tex_size at offset 0 and unit scale, CLAMP repeat or clamp, and the
footprint's wrapped texels are neighbours (`next == at + 1`); its coordinate is their shared
corner, `(at + 1) / tex_size`, exact for the power-of-two GS sizes, so the hardware's footprint is
exactly those texels. Otherwise four branch-free loads. The conversion pass reads its base
texture as integers and stores `v / 255`, which the unorm store turns back into `v`.

Prototype (gather of the float texture's first channel with an integer unpack: the cost, not
the picture): main kind 0.58 -> 0.46 ns/pixel, draw time -20% at 4x. The gather replaces the four
bounds-tested loads of most bilinear draws, so it also recovers most of what dropping `inside` cost:

| DX12 FXC uncapped | 4x fps | 4x GPU ms | 4x draw ms | 8x fps | 8x GPU ms | 8x draw ms |
|---|---:|---:|---:|---:|---:|---:|
| base | 77.41 | 11.808 | 10.646 | 22.08 | 44.504 | 39.785 |
| 2.1 with `inside` (not kept) | 111.41 | 8.592 | 7.556 | 28.50 | 34.507 | 29.709 |
| 2.2 branch-free, no `inside` | 87.36 | 10.609 | 9.424 | 24.02 | 41.116 | 36.305 |
| 2.3 gather (final) | 105.11 | 8.922 | 7.807 | 27.71 | 35.500 | 30.572 |

### The final build's picture checks

The frozen oracle's g+3600 replay (`dhd-oracle/gs-golden-base`, verified by its manifest) against
base and against the final build's specialised pipelines on the iGPU, DX12 FXC and Vulkan, 1x, 2x
and 4x: byte-identical (both fields and VRAM). The replay matrix, final build, iGPU: VRAM
identical everywhere; generic against specialised identical under FXC and Vulkan at 3x, 4x and
8x in every capture, and at 1x except the captures that vary between runs anyway (frame-004 and
frame-005 under FXC, frame-003 .. frame-005 under Vulkan); under DXC the reassociation above.
Base against the final build: the `round()` table above. RTX: the replay matrix's rows (Vulkan:
base, generic and specialised byte-identical in every capture at 1x, 3x, 4x, 8x; FXC and DXC: the
`round()` table, generic equal to specialised under FXC); the oracle golden (`gs_golden.py
compare replay-native`, g+3600 at 1/2/4x) PASS for specialised, generic and default runs;
`test_gs_wgpu.py` PASS; `test_native_transfers.py` plain, `--presented`, `--stress`: PASS, zero RGB
and VRAM differences between dual-source and no-dual. Unit and pipeline-key tests pass.

The game: only the renderer changed (`port/render`, `port/gs/native*.wgsl`; the build-tool
commits are cherry-picks from `next/0.7.7`), the core (`dhd_core.wasm`) is not rebuilt by these
commits, so no state trace was needed; what the game reads (VRAM) is identical in every replay.

### Rejected: single-source blending for As draws without alpha writes

The main kind uses dual-source blending (As/128 as the second output) but its write mask drops
alpha, so `native_pixel_blend` (RGB with As/128 as its own alpha, the no-dual path's) gives the
same RGB blend. 4x: 109.97 fps / 8.500 ms against 111.41 / 8.592; 8x: 29.01 / 33.998 against
28.50 / 34.507: within run-to-run spread. Colour exports are not the limit here. Not kept.

## 3. Final table

Before = `next/0.7.7` with the DHD_GPU_STATS commit (the renderer of finding 130); after = this
branch (step 2.3's build; the later commits change only build tools and documents). Ryzen 5 7600
iGPU, DX12 with FXC, the recipe at the top; RTX 3070 at 8x as a regression check. Each "after" and
"before" point ran back to back.

| GPU | Scale | Build | FPS paced | 1% low | GPU ms paced | FPS uncapped | 1% low | GPU ms uncapped | draw ms uncapped |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| integrated | 1 | before | 59.94 | 38.07 | 1.878 | 254.95 | 101.36 | 1.897 | 1.587 |
| integrated | 1 | after | 59.95 | 45.86 | 1.390 | 275.84 | 135.36 | 1.439 | 1.110 |
| integrated | 2 | before | 59.67 | 36.30 | 4.116 | 221.80 | 111.40 | 3.700 | 3.319 |
| integrated | 2 | after | 59.95 | 46.80 | 3.008 | 251.97 | 114.73 | 2.836 | 2.406 |
| integrated | 4 | before | 59.95 | 47.52 | 12.541 | 76.48 | 51.20 | 12.018 | 10.789 |
| integrated | 4 | after | 59.98 | 53.28 | 9.631 | 102.55 | 61.63 | 9.106 | 7.910 |
| integrated | 5 | before | 51.67 | 16.63 | 18.059 | 52.97 | 22.78 | 17.918 | 16.127 |
| integrated | 5 | after | 59.97 | 50.79 | 14.412 | 69.98 | 32.17 | 13.793 | 12.016 |
| integrated | 6 | before | 37.74 | 27.44 | 25.583 | 36.90 | 25.26 | 25.992 | 23.227 |
| integrated | 6 | after | 49.50 | 36.66 | 19.793 | 49.49 | 37.46 | 19.783 | 17.033 |
| integrated | 8 | before | 21.48 | 12.26 | 45.313 | 21.54 | 10.59 | 45.126 | 40.422 |
| integrated | 8 | after | 27.47 | 8.60 | 35.309 | 27.96 | 20.98 | 35.301 | 30.385 |
| discrete | 8 | before | 59.09 | 23.84 | 4.601 | 283.83 | 144.52 | 1.923 | 1.628 |
| discrete | 8 | after | 59.02 | 23.30 | 4.600 | 282.06 | 145.70 | 1.681 | 1.395 |

DXC (the compiler packaged builds use; `dxcompiler.dll` beside the executable,
`DHD_WGPU_COMPILER=dxc`), iGPU uncapped:

| DXC control, iGPU uncapped | Scale | FPS | 1% low | GPU ms | draw ms |
|---|---:|---:|---:|---:|---:|
| before | 4 | 71.12 | 46.15 | 12.938 | 11.685 |
| after | 4 | 103.95 | 63.72 | 8.929 | 7.691 |
| before | 8 | 20.03 | 15.24 | 49.103 | 44.129 |
| after | 8 | 28.59 | 21.37 | 34.482 | 29.395 |

The highest scale holding the 60 target (59.94 fps, paced) on the iGPU: **5x** after (59.97 fps,
1% low 50.79, GPU 14.41 ms), **4x** before (5x before: 51.67 fps, GPU 18.06 ms, GPU-bound). 6x after
is 49.5 fps (GPU 19.8 ms: the draw pass alone is over the 16.7 ms budget). The RTX at 8x is CPU- and
pacing-bound either way (about 283 fps uncapped); its GPU time falls from 1.92 to 1.68 ms.

How the runs were kept apart from other work: four other lanes built and ran games on this PC.
Before every timed run `build/gpu2/measure.py` waited until no clang, clang-cl, rustc, cargo, cl,
link, wasm2c, node, wrangler, dhd*.exe or DownhillRevived.exe process ran (checked every 2 minutes;
10 of 32 runs waited, up to 20 minutes), and during the run it checked every 10 s for such
processes appearing. Runs during which another lane's game or compile appeared were repeated:
both 8x points of base and final, both RTX 8x paced, both DXC 8x points, the final 6x paced (another
game started 10 s in: 26.06 fps, 33.1 ms GPU; repeated: 49.50). In the table, base 8x paced and
uncapped and the final 8x uncapped still had compiles overlapping on their repeat (the first and
second base 8x runs agree within 1 fps; the point is GPU-bound), and the final 6x paced had a
node process (a local room server) start. Two single stalls were repeated as in finding 130: the
final 4x paced (one 414 ms frame: 59.05 fps, 1% low 21.65; repeated: 59.98 / 53.28) and the final
5x paced (frames of 792 and 266 ms, every other frame at most 18.4 ms: 57.83 / 12.80; repeated:
59.97 / 50.79). The earlier single measurements in sections 2.1-2.3 were taken on other days'
loads and are superseded by this table where they differ (4x after: 105.11 there, 102.55 here).
All first runs are kept beside the results (`build/gpu2/runs-set-aside`).

## 4. What is left

Against the cost model of section 1, per shaded pixel of the main kind (blended terrain, bilinear,
fog; iGPU, Vulkan, 4x): the floor is ~0.24 ns (raster, depth test, blend: the clear's cost); the
uber-shader was ~0.85; with the gather ~0.46 (the prototype's figure; not re-measured per kind).
What the remaining ~0.2 ns is, from the experiment builds: the bilinear's mixes and the per-texel
wrap logic (now one gather, but CLAMP handling and the neighbour test stay per pixel), the fog mix
and floors, and the TFX multiply. The texel fetch itself is now one instruction. Taking the rest
down further needs changes that are not exact:

- the hardware's bilinear filter instead of the GS's (rounding differs; finding 130 rejected
  arithmetic shortcuts of this kind);
- half precision for the colour maths (the floors would land differently);
- skipping draws the GS draws (overdraw is ~4 and set by the game: the blended terrain layers
  cannot be skipped, and a depth pre-pass does not help blended draws).

Outside the draw pass at 8x: scanout ~3.5 ms (a full-picture copy into the display texture,
bandwidth-bound) and present ~1.3 ms (minifying 4096 x 3584 into the window). Presenting straight
from the surface would remove the scanout copy for the common display mode but needs the
interlace, offset and readback paths reworked and checked; a cheaper present (a downsample before
the filtered blit) changes the presented picture's filtering. Neither was done. 6x at 60 would need
the draw pass ~15% faster again plus most of the scanout.

Open: the AMD run-to-run variation (section 2) and DXC's reassociation; the paced 1% lows still
show isolated wall-clock stalls of 0.3-0.8 s on the iGPU in some runs (finding 130 saw the same);
their cause needs CPU-side profiling.

## Switches added

- `DHD_GPU_STATS=N` (diagnostics): per-draw-kind GPU time and pixels shaded, every N-th picture.
- `DHD_NATIVE_SPEC=0` (no specialised pipelines: the generic shader, as before), `=sync` (built at
  first use instead of on the worker thread: replays and picture checks).
