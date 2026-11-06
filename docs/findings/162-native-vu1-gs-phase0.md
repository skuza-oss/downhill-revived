# 162: Native VU1 and GS rendering, phase 0: what the game draws and how

Status: in progress

Branch `native/vu-gs` from `main` 9ecb8e1, started 2026-11-06. **A local branch**: the owner keeps
the native VU1/GS work off `main` until it is decided (not merged, not pushed). This file is the
lane's plan while it runs.

## Goal and acceptance

The owner's direction (2026-11-05/06): keep the decompiled game code; replace what is still
emulated for graphics (VU1 microprograms, VIF/DMA, the GS command stream and its renderer front
end) with native GPU rendering, built at the packet level first: the game keeps building its
VIF/DMA packets, a translator turns them into GPU draws whose shaders do each VU1 program's maths.
Finding 156 measured the gain: VU + GS are about half of the CPU work on every machine; on
the low-end phone profile removing both takes the race start from about 60 to 80 fps.

Phase 0 is measurement and inventory only, no renderer change. It answers, with numbers:

1. **The game thread's hot functions**, named (the decomp's names or addresses with what they do)
   and grouped by system (physics, AI, camera, scene traversal, **packet building**: the code that
   writes VIF/DMA/GIF packets for VU1 and the GS, sound, other), per scene (title, shop, race
   start, mid-race) on Windows and the Mi 11i (finding 156's routes and runners). How many ms of
   the game thread a deeper hook (before packet building) would also remove.
2. **The VU1 programs**: every microprogram entry the game uploads and runs, what each does
   (skinning, terrain, sky, particles, shadows, HUD/2D, ...), its share of VU time per scene, its
   inputs (the packet layout: unpacks, matrices, lights, flags, which VU0/EE data it reads) and
   outputs (GIF packets: primitive types, registers, texture use). Which programs cover 90 % of
   the time.
3. **The GS features the game uses** per scene: primitive types, texture formats and CLUTs,
   alpha/blend/test modes, depth use, render-to-texture and framebuffer reads (the flare's Z read,
   local-to-local copies, feedback effects), PATH1/2/3 mixes, anything a native path must
   reproduce or replace.
4. **The translation plan**: per program and feature, how it maps to a GPU draw (vertex shader for
   the VU1 maths, the native renderer's fragment path, what stays emulated longest), the
   order to build it (biggest gain first), and how each step is checked (pictures against the
   current renderer and PCSX2, state traces).

Acceptance: the tables and lists above in this finding, the method to repeat them, and a phase 1
proposal (which program first, the prototype's scope and its measurement). Tools that are cheap
and worth keeping may be committed on this branch; nothing changes the game's state or output.

## Progress

- [ ] 2026-11-06: finding reserved on the local branch

## Decisions

## Surprises

## Result
