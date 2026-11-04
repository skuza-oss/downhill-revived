# 019 — Graphics pipeline: engine draw paths and VU1 microprograms

Status: partly superseded by 090 (one native routine per VU1 entry; meshes as vertex shaders)
Checked: 2026-11-04

The engine paths, `.vutext` contents and entry points are current. Of the
recommended design, the GS layer exists (`port/gs/`, Rust renderer, findings 021, 085); VU1 runs
compiled blocks (`port/tools/vu1_aot.py`) plus a few native kernels (`vu1_native_clip.py`,
`port/runtime/vu1_native_*.h`; findings 030, 066, 090), not one native routine per entry; the GPU
fast paths of step 3 (meshes as vertex shaders) were not built. The game does read GS memory back:
the lens flare's Z probe (findings 028, 097, 148).

Study for the native PC renderer (plan step 7). Sources: our decompiled engine (US
`SCUS_971.77`), our disassembly of `.vutext` (`port/tools/vudis.py`) and PCSX2's source as the
hardware reference (paths below).

## Hardware paths in one picture

```
 EE (game + engine C)                                        GS
   scene walk  func_00216E28 ─┐
   VU0 transform/cull         ├─► frame VIF1 DMA chain ─► VIF1 ─► VU1 programs ─XGKICK─► PATH1 ─┐
     func_0021B8A8 (asm)      │   (double buffered,           (overlay A or B             │
   particle renderers         │    D_00528900[D_0052890C])     at VU1 0x0000)              ├─► draw
     0x224xxx-0x227xxx ───────┘                                                             │
   2D / setup / flips ─────────────────────────────► GIF DMA (PATH3) ──────────────────────┘
   GS privileged regs (display) ───────────────────► 0x12000000 (func_0020B938, func_0021F000)
   FMV ─► IPU (0x0023Bxxx, libmpeg)
```

- **The frame chain (PATH1).** `func_00218720` renders one view (its last argument is the view
  index, for split screen). It points two packet heads into scratchpad (`0x70000300`,
  `0x70000290`), fills the VU1 program table (`func_0022F600`), walks the scene
  (`func_00216E28`) and runs `func_0021B8A8`. Then `func_0021E9A8` kicks VIF1 DMA (D1,
  0x10009000) in chain mode from `D_00528900[D_0052890C]`. `func_00216DD0` and `func_00218038`
  also start D1 directly.
- **`func_0021B8A8` is the core of the 3D renderer.** It is hand-written asm, now recompiled
  (finding 018). It walks the scene nodes (the node kinds are in the PAL notes) with VU0 macro
  code: matrix stacks, box/frustum tests, and `vcallms` to the VU0 microprograms 0x0, 0x2E0,
  0x5E8, 0x7B8, 0x808, 0x8D0, 0xB40, 0xBD0, 0xC50 and 0xCD0. It copies each visible mesh's
  precompiled VIF chain (from the level `.NGP`) into the frame chain, together with its
  matrices. Game objects (riders, bikes, payload kinds > 0x3E8) are drawn through a callback the
  game stores at scratchpad `0x700001D4`.
- **Particles and effects** (emitter objects of types `0x8002`-`0x800D`, dispatched in
  `func_00228BE8`). They build their own VIF packets with `func_002249E0` (entry address
  `| 0x14000000` = `MSCAL`) and upload overlay A in their chain (`func_00224B88`).
- **PATH3 (GIF DMA, D2 0x1000A000).** Used for GS setup (`func_00223BB0`, `func_002240A8` sends
  the GS init packets `D_00263BC0`/`D_00263C00`), display double-buffer packets
  (`func_0021F0A0`), loading-screen packet chains (`func_00210460`) and two game-side senders
  (`0019C868`, `001EB1D8`). `func_002433A8` is `sceGsSyncPath` (it polls D1/D2 CHCR, VIF1_STAT
  and GIF_STAT).

## `.vutext` contents

`.vutext` (0x263BC0-0x26DD00) is not just microcode. It holds GS init data, a zeroed buffer at
0x263C00, data tables (0x2694A8 is a matrix written at run time, 0x2694D0 a grey colour table)
and three DMA chains that upload code with VIF `MPG`:

| Chain | Tag | Uploads | Contents |
|---|---|---|---|
| 0x265C00 | `ret`, 0x38C qw | VU1 0x0000-0x3858, 1803 instructions | **overlay A**: particles/effects, 16 `XGKICK` |
| 0x269C50 | `ret`, 0x332 qw | VU1 0x0000-0x3268, 1613 instructions | **overlay B**: world meshes, screen effects, 4 `XGKICK` |
| 0x26CF80 | `cnt`, 0xD6 qw | VU0 microcode, 426 instructions | the VU0 programs of finding 016 |

Both VU1 overlays load at address 0, so they replace each other. Every view's chain starts with
a DMA `call` to overlay B (`func_00215B68`), and each particle batch's chain calls overlay A.
The 20 `XGKICK` sites match the count the PAL project reported, so the microcode is very likely
identical across regions.

### Entry points (all of them, from the `MSCAL`/`MSCALF` words the EE writes)

| Overlay | Entry | Started by |
|---|---|---|
| A | 0x0050 | `00224F58` |
| A | 0x02D0 | `00225458` |
| A | 0x0850 | `00227C60` (the instruction at 0x850 also serves as the delay slot of the program ending at 0x848) |
| A | 0x0E10 | `002268E0` |
| A | 0x1010 | `002256F0` |
| A | 0x1240 | `00225A40` |
| A | 0x17C8 | `00226B90` |
| A | 0x1F50 | `00226D08` |
| A | 0x22C8 | `00226FA8` (emitter types 0x8007/0x8008) |
| A | 0x26D8 | `00226588` |
| A | 0x29C0 | `002370E0` (`func_00237918`) |
| A | 0x2E48 | `00227518` |
| A | 0x30F8 | `00227840` |
| B | 0x0078 0x0280 0x06F0 0x07F0 0x0948 0x0A90 0x0C20 0x1050 0x12F0 0x1430 0x1638 0x17E0 0x2738 0x28D0 0x2B10 0x2C20 | the 16-entry `MSCALF` table at scratchpad 0x70000120 (`func_0022F600`), most likely one per mesh/vertex format; 0x0C20 is the terrain vertex program the PAL notes describe (3-qword records, `FTOI4`, ADC flags from `CLIP`) |
| B | 0x0050 | `0021D250`, `0021E4F0`, `0021E820`: full-screen passes (`func_0021E728` builds textured sprites with bilinear filtering, e.g. a blur/glow) |
| B | 0x2DC8, 0x30A0 | `0021D250` |

That gives 32 VU1 programs. Each is a small, fixed routine: unpacked vertices in VU memory in,
GIF packets out to the GS.

## What the native renderer needs (recommended design)

1. **GS layer, `port/gs/` (first, and testable without the game).** It consumes GIF packets
   (PACKED, REGLIST, IMAGE) from PATH1 and PATH3 and turns them into native GPU draws:
   - Register state: PRIM, RGBAQ, ST/UV, XYZ2/XYZF2, TEX0/TEX1/CLAMP, ALPHA, TEST, FRAME, ZBUF,
     XYOFFSET, SCISSOR, FOG/FOGCOL, TEXA, PABE/FBA, COLCLAMP, DTHE.
   - Vertex kicks are batched into draw calls.
   - The ALPHA equation `(A-B)*C+D` maps to GPU blend state, TEST to alpha and depth state.
   - Texture uploads (BITBLTBUF/TRXPOS/TRXREG/TRXDIR) go into an emulated 4 MB local memory with
     the GS swizzle (reference: PCSX2 `pcsx2/GS/GSLocalMemory.cpp`). They are decoded into GPU
     textures by TEX0 and CLUT state (`pcsx2/GS/Renderers/HW/GSTextureCache.cpp`,
     `pcsx2/GS/GSClut.cpp`).
   - Render targets are native and can run at a higher resolution. The overlay B screen passes
     sample the frame buffer, so a target-as-texture path is required, as in PCSX2's texture
     cache.
   - Register layout and semantics: `pcsx2/GS/GSRegs.h` and `pcsx2/GS/GSState.cpp`.
   - **Testing:** PCSX2 GS dumps (`.gs`: register state, the starting VRAM, then GIF transfers
     per path and vsyncs; `pcsx2/GS/GSDump.h`, `pcsx2/GSDumpReplayer.cpp`) captured from the real
     game can be replayed into this layer and compared against PCSX2's own output, frame by
     frame, before our build boots.
2. **VIF1 + VU1 programs.** At the D1 kick (`func_0021E9A8`, `func_00216DD0`, `func_00218038`)
   the port walks the DMA chain:
   - DMA tags: cnt/next/ref/refe/call/ret/end.
   - VIF codes: UNPACK with STCYCL/STMASK/STROW/STCOL/OFFSET/BASE, `MPG` (records which overlay
     is loaded), `MSCAL`/`MSCALF`/`MSCNT`, DIRECT/DIRECTHL to the GS layer. Reference:
     `pcsx2/Vif_Unpack.cpp`.
   - VU1 data memory (16 KB) is emulated.
   - Each `MSCAL` runs the **native implementation of that entry point** (32 of them), which
     reads VU memory and emits GIF packets to the GS layer.
   - A small VU1 interpreter built from the `vudis.py` tables serves as the test oracle: native
     program == interpreted microcode on captured VU memory, bit for bit where the program is
     integer/flag logic.
3. **GPU fast paths (after it all draws correctly).**
   - Static level meshes: the `.NGP` VIF chains never change, so their decoded vertices can live
     in GPU buffers keyed by chain address. Program 0x0C20 and its siblings then become vertex
     shaders (transform, `FTOI4`, clip, ADC strip restarts).
   - Skinned riders become vertex-shader skinning.
   - Particles stay CPU-built or move to instancing.
4. **Prerequisite on the EE side.** `func_0021B8A8` needs the VU0 microprograms listed above as
   native code (finding 016 has four of twelve), or a VU0 micro interpreter. Without them the
   scene walk can't run.

## Tools

- `python port/tools/vudis.py` walks the three upload chains and writes
  `build/port/vu1/<chain>.txt`: VIF log, then the disassembly by VU address, with labels at
  branch targets, `[E]` ends, `LOI` immediates. Independent code; the encoding follows Sony's
  VU manual and PCSX2's `pcsx2/DebugTools/DisVUmicro.h`.

## Open questions

- Which mesh format each of the 16 overlay-B table slots is (it needs the mesh leaves' slot
  index; per-program study).
- What `func_0019C868`/`func_001EB1D8` send over PATH3 (game-side 2D, likely the HUD or the
  poster maker).
- Whether the game reads the frame buffer back to the EE (none found so far). Answered later:
  yes, a small Z rectangle every race frame for the lens flare (`sceGsExecStoreImage`,
  findings 027, 028).
