# Simulation and rendering at different rates

Status: rejected

A 2026-10-15 investigation of drawing extra, interpolated pictures between 60 Hz
game fields (render-only replays of the engine's draw calls) on the 32-bit build. Never shipped.
The user rejected in-between pictures (finding 049). High frame rates came instead from real game
steps: the frame-rate unlock (finding 067) and the delta-time clock (finding 139, `port/hle/fps.c`,
`port/hle/kernel.c`). The separate `DHD_INTERP` presentation (finding 038) still exists as a
developer variable, off by default and not offered to players.

## Why render-only extra frames are hard in this engine (still true)
- `func_00215B68` (prepare) and `func_002161B8` (finish), in `src/unsorted/00215B68.c`, are not
  pure render functions: they set scratchpad packet headers, global packet pointers and view
  matrices, call the per-view callback `D_0029DA94`, walk geometry, move packet cursors and kick
  DMA. Running them twice consumes flags twice and reuses in-flight packets.
- Scene walks (`002167C0` over `D_004D0318`, `00216980` over `D_004D0338`, `SceneNode` in
  `include/scene.h`: resource at +0, position +0x10, matrices +0x40/+0xC0) clear visibility bits
  in `node->x5`. Node pointer + resource is a candidate identity, but allocation reuse needs a
  generation rule.
- Whole-EE-memory rollback does not work: the workers legitimately write that memory meanwhile.
- The lens flare's store-image readback, HUD integer clocks and particle birth/death/RNG must
  advance once per game step; extra pictures need their own colour/depth/output and must not issue
  readbacks.
- Doubling VBLANK, pad polls, `sound_tick` or the integer game delta changes game behaviour.

## What was built and measured (all removed)
- A private snapshot/transport layer replaying a selected DMA chunk into an independent GS: in
  the game, 8 chunks / 2,374 words, 356 GPU primitives, every readable EE page unchanged. It
  proved isolation, not a usable interpolated frame.
- Many batch builds of the render-only path ran 600 display changes at 49-59 fps (best 59.27);
  120 was never reached, and visual correctness was never reviewed.

Full text: `git show 860ab6b:docs/findings/036-high-refresh-gameplay.md`
