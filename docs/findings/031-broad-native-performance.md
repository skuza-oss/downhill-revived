# Round 26: waits and broad native performance

Status: history

A 2026-10-15 round on the retired 32-bit D3D11 build (RTX 3070): paced race
47.0 → 47.8 fps, late race still ~44 fps, 60 not reached. The D3D11 renderer, its flip-model
present and the i686 compiler options are gone (finding 088); speed work continued in findings
032, 066, 089, 090. One fix here is still in the port: the Huffman table bytes (below).

## Kept: deterministic Huffman tables (`func_00240CB8`, huft_build)
Retail copies the whole 8-byte local entry (instructions 0x2412B0..0x2412BC) after storing only a
16-bit leaf at sp+0xA4, so its bytes 2-3 and the union's bytes 6-7 carried host stack bytes into
guest RAM (stale pointer halves, not decoder inputs; PCSX2's capture shows retail's own leftovers,
0x008C0001 against the port's 0x088C0001). A port-only patch zeroes the padding, initialises the
entry and clears the union before each leaf; layout and child pointers unchanged.
`port/tools/test_huft.py` checks it with poisoned stacks.

## Measured and rejected (do not retry without a new reason)
- **The late-race readback wait** (one 64-byte Z StoreImage per frame, the lens flare's probe):
  Map + settle fence ≈ 17.3 ms per probe. A diagnostic that returned zeros without waiting
  (`DHD_ZPROBE_NOWAIT`) gained nothing (late race 43.4 vs 43.3 fps): the wait just moved. Probe
  issued in field N is consumed in N+1, while the GPU is still on the N+1 interval.
- **Blocking GS-owner rendezvous** at a packet boundary: exact, but 41.1 vs 47.1 fps (it stopped
  newer draw submission).
- **Small per-page staging:** exact, 47.0 vs 47.0. No gain.
- **Compiler/ISA:** SSE4.1 helped menus (57.7 vs 55.3) but not the race (46.9); VU-only PGO no
  gain; **AVX2, game -O2, recompiled-asm -O2 and ThinLTO changed EE state** (guest-word
  differences in the strict comparison) and were rejected. Finding 069 cites this.
- **Flip-model present:** kept then (47.8 vs 47.0), retired with D3D11.
- **64-bit move:** estimated then at 4-8+ engineer-weeks; done later as the wasm32 core (findings
  081, 082).

## Open then, settled later
An intermittent menu picture difference with 12,540 texture uploads at field 600 (387 in a good
run) pointed at texture-submission state, not timing; no cause was found in this round.

Full text: `git show 860ab6b:docs/findings/031-broad-native-performance.md`
