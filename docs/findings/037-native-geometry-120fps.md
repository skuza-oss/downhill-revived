# Native geometry CPU investigation toward 120 fps

Status: rejected

2026-10-15 candidates on the 32-bit build to cut VU1 geometry CPU time, all
compile-time opt-in and default off, none accepted. The 64-bit build's VU1 work replaced them:
AOT blocks and native kernels (`port/tools/vu1_aot.py`, `vu1_native_clip.py`,
`port/runtime/vu1_native_*.h`), findings 046, 066 and 090 (VU1R, OP3).

## Measurement that framed it (hardware bridge build, race, 2,674 fields)
VIF worker CPU 13.36 ms/field (busy 12.17), GS worker 8.40; timed VU execution 7.83 ms/field plus
1.42 ms of GIF callbacks (overlapping, not additive). The top programs: B0078/1040 595.8 calls
and 2.61 ms a field, B0078/1CB0 0.71, B1638/1040 0.61, B17E0/1040 0.54: the first four 4.47 ms.

## Candidates (none adopted)
- **FMAC result clamp:** a bounded clamp of FMAC results in the shared native arithmetic helper;
  replay-exact on its call sites, never live-gated or measured whole-game.
- **DAZ operands** (`DHD_VU1_NATIVE_FMAC_OPERANDS`): MXCSR DAZ in place of the operand
  sanitizers of `native_add/sub/mul/madd` (exponent-255 saturation kept explicit); exact on
  4,760,896 differential cases and 64 captures, never live-gated or timed. Precise SSE2, no FMA,
  no contraction stay required for exact VU arithmetic.
- **Selector-3 static mesh decoding** and a private transform/two-field bridge: foundations for
  game-level mesh submission (finding 036), never exercised in a real pass.
- The rejected B0078 prototype of round 25 (finding 030) stayed rejected.
- The clipping program touches 28 of VF1..VF31 (VF18/19/25 absent): a union-based spill
  elimination would save little.

Full text: `git show 860ab6b:docs/findings/037-native-geometry-120fps.md`
