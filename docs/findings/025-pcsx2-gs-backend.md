# 025: PCSX2's GS as the port's renderer (research)

Status: rejected

A 2026-10-14 proposal to replace the port's own GS renderer with PCSX2's GS
behind `DHD_GS=pcsx2|native`. Not built. Decision: the port keeps its own renderer (today the Rust
wgpu renderer, findings 085, 088); PCSX2's GS is the **referee**: it replays the port's own GS
captures and is compared against (`compare_pcsx2.py --mode gs-replay`, finding 024).

## The measurement that settled it
- PCSX2 v2.8.2 (`fd9d310c…`, the comparison adapter's tree, kept locally in
  `build/port/compare-pcsx2-src`) replayed the port's capture of countdown +120
  (`compare_pcsx2.py --mode gs-replay --from-dumps`): **MAE 0.0, block SSIM 1.0, 0 % of pixels
  over threshold** against the port's own render of the same stream. PCSX2's GS drew the riders
  inside out exactly as port/gs did.
- The cause was upstream of any GS: about 220 rotation matrices in EE RAM (the camera's among
  them) had [3][3] = 0 instead of 1, so rider Z was 0xFFFBD7..0xFFFC00 against retail's
  0x8F3..0x1EF2D9. A hardware write watch put the bad store in the inline-asm interpreter
  (`port/runtime/ps2asm.c`, an `sq` of a GPR; finding 024 round 4). The sky and floor holes of the
  same frame replayed identically too.
- Lesson kept: before blaming a renderer, replay the same GS stream through PCSX2. If the pictures
  agree, the bug is in the stream (EE, VU1, ABI), not the renderer.

## Options weighed (all dropped)
- **(a) 64-bit PCSX2 helper process** fed by a shared-memory ring of the capture records
  (Transfer, VSync, ReadFIFO2, Registers; `GSDumpReplayer`'s fake CPU), port headless. Workable,
  but it means building most of PCSX2 (VMManager, MTGS, Host) and keeping GS→EE readback
  (ReadFIFO2: 24 records in 6 fields at countdown +120 in retail) served by port/gs. It would not
  have fixed the artefacts above.
- **(b) PCSX2 GS built 32-bit:** not realistic (x64-only GSVector intrinsics, x64 JIT; PCSX2
  dropped 32-bit in 1.7).
- **(c) Move the port to 64-bit:** judged the largest change then; it was done later for other
  reasons (findings 081, 082, 088), with the port's own renderer.

Full text: `git show 860ab6b:docs/findings/025-pcsx2-gs-backend.md`
