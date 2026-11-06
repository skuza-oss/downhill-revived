# 172: Fewer state changes per draw on the render thread

Status: in progress

Branch `native/vu-gs` (local only: never pushed or merged until the owner decides), after 168's
`22a5c2d`, started 2026-11-06. This file is the lane's plan while it runs.

## Goal and acceptance

Finding 168: with the native VU1 path on, the Mi 11i on the t612 profile at 2x is limited by the
render thread, and mostly by wgpu turning the recorded draws into Vulkan commands
(`encoder.finish` about 6.3 ms and `queue.submit` about 2.8 ms a picture, about 13 µs a draw), driven
by the state changes of every draw, not only the mesh draws: about 383 texture binds, 255-295
constants binds and 180-225 pipeline changes a race picture. Batching the mesh draws (168) cut
draws but not time.

Goal: cut the render thread's cost per picture for all draws, one code path for every platform:
- **Texture binds:** textures in arrays or atlases (by format and size class), the draw selecting
  its layer/region from its constants or vertex data, so consecutive draws with different
  textures share one bind group; or bindless-style descriptor indexing where wgpu offers it on
  every platform we ship (check Vulkan on Adreno/Mali/PowerVR, D3D12, Metal; fall back to arrays).
- **Constants:** looked up per draw from one storage buffer (an index per draw: a push constant or
  a vertex attribute) instead of a dynamic-offset rebind per draw.
- **Pipelines:** one shared layout for the mesh and GS draw pipelines (168's
  layout patch, kept outside the repository), fewer pipeline variants switched per picture
  where specialisation constants can become uniform branches without a GPU cost.
- Measure each step with `DHD_RSTAT`'s counters and the render thread's ms (encoder finish,
  submit) before keeping it.

Acceptance: pictures identical to before (or within finding 167's tolerance; say which) at 2x and
4x over finding 156's route, switch off and on; boot trace while iterating, race trace once at the
end; `check.py port`; the Mi 11i on t612 at 2x only (the owner's rule), switch on before/after, and
switch off before/after (the GS path's draws change too); desktop timing only if the owner's PC is
free and no other session uses the Mac.

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
