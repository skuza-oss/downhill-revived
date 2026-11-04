# Frame critical path investigation

Status: history

A 2026-10-15 round on the retired 32-bit build's compute rasterizer (an HLSL
compute shader over swizzled GS memory, D3D11). It took the late race from 44.98 to 62.9 fps
uncapped and 59.93 paced on the RTX 3070. That renderer is gone; the Rust renderer (findings 085,
088) carries its shader rules into WGSL (`port/gs/raster.wgsl`, finding 084). Finding 033 is the
direction this round led to.

## What it found
- Late race (wall 120-150 s): 44.31 fps; GPU render intervals 22.5 ms, raster dispatches
  20.3 ms a frame (~325 dispatches, 22,148 primitives). Scanout, copy and Present were small
  (<0.1 ms). So the **shader** was the target, not VU compiler trials or the readback waits.
- Profiling (GPU timestamps, per-field counters) cost nothing measurable in one pair (fields
  6000-7000: 22.3295 vs 22.3290 s).

## What it changed (carried into the WGSL shaders)
Three shader options, each exact, default on since this round, each with its own off switch
(`DHD_GS_ADDRESS_SHIFT=0`, `DHD_GS_EARLY_DEPTH=0`, `DHD_GS_EXCLUSIVE_WORD_STORE=0`; still read by
`port/gs/gpu_rust.c` `raster_flags`): address shifts, early depth guarded by an independence
proof, and plain masked word stores (not compare/exchange) for CT32/CT24/Z32/Z24 when scissor and
stride prove one word per pixel and colour/depth pages are disjoint; packed formats and local
copies keep atomics.

## Results and limits
- Accepted build, fields 6000-7000: 62.90 fps uncapped, 59.93 paced; but uncapped p95 17.46 ms,
  p99 21.65 ms, 166 of 1,000 intervals over the 16.683 ms budget. Average fps is not a deadline.
- The first full gate failed at GO+60/+75 s (words 0x2D5CC4 and 0x4C9B94, the sound command
  buffer): undefined native producer bytes, fixed in finding 034, not excluded.
- 120 fps would need both raster and geometry (VIF ~13 ms CPU a field then) cut; see 033, 037.

Full text: `git show 860ab6b:docs/findings/032-frame-critical-path.md`
