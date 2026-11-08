# 029 - Round 21: the struct-indexing audit, VU1 from inside, the graphics worker (2026-10-15)

Status: partly superseded by 030 (DHD_MTVU opt-in)
Checked: 2026-11-04

The graphics worker of section 3 (`DHD_MTVU=1` here) became the default in finding 030 (9cc58bf8):
VIF1/VU1/GS run on worker threads unless `DHD_MTVU=0` (`port/hle/graphics_queue.c`). The worker
measurements below are the 32-bit D3D11 build's.

Goal: a race at 60 fps, paced. It ran at ~35 fps with VU1 at 60% of the frame (finding 028).

## 1. The struct-indexing audit

Finding 028 fixed `func_00131630`: retail reached `ends.after[i]` as `bp[2]` past `ends.before`,
and clang split the struct. This audit looked for the same pattern in the other game units.

### Method

1. `clang -Warray-bounds` over every unit found one irrelevant constant index (`00224CC8.c`).
2. The clang static analyzer's `security.ArrayBound` checker
   (`clang --analyze -analyzer-checker=security.ArrayBound`, port include paths) went through
   all 3797 units with no timeouts.
   - Positive control: it reports the unpatched `00131630.cpp` as "Out of bound access to
     memory after the end of 'ends.before'".
   - It reported 62 sites in 41 files.

### Triage

The failure mode needs memory the compiler owns: LLVM only splits or rearranges a **local**
aggregate, and only when its address doesn't escape to a call. So each site was classified:

| Kind | Sites | Verdict |
|---|---|---|
| Indexing through a pointer parameter or object field (`obj->x2500[i - 1]`, `map->e[lo]`, negative indices into a preceding field) | most | Safe: the compiler can't move memory it doesn't own, and the code does what retail does. |
| Extern globals (`D_xxxxxxxx`) | 15 | Safe: laid out by the data image at fixed addresses. |
| Locals whose address escapes to a call (`groups` in `00133DE0.cpp`, `hits` in `001357D8.c`, `event` in `00233670.cpp`) | 3 | Safe: an escaping alloca stays whole, and the accesses stay inside it. |
| Analyzer false positives (pointer subtraction, field-to-field walks in asm operands) | a few | Safe. |
| `func_00188890` (`00187438.c:773`) builds a task at `&archive[0x12F30]`, `archive` being `char[0xC90]` | 1 | **Real.** |

### The real hit

In retail's frame, `archive` is at sp+0, `name` at 0xC90, `buffer` at 0xCE0 (0x12240 bytes), and
`union Work w` at 0x12F20. So `archive[0x12F30]` is `w.pair.load`, after `w.pair.msg`. On the PC
the frame is laid out differently, and the task was built on whatever lay 0x12F30 bytes past
`archive`.

- Fix (3235607a): a port patch makes the store `load = &w.pair.load;`.
- No hit touches the shared-header files.
- Race dumps at GO+0, +2, +12 and +16 are identical to the reference: this load isn't on the
  race path.

## 2. VU1 from inside

### Tools

- The replay bench of finding 027: 32 race captures x 3000, bit-exact hash `6DB1972798276582`.
- Ablations: modified copies of `vu0_interp.c`, each removing one piece of work.
- The game's own profile: samples mapped to instructions inside one function.
- Code and data: 3196 VU pairs in 906 blocks, 27M blocks per bench run, ~4 pairs per block,
  no interpreted pairs in the bench.

### The bench, broken down (1.45 s)

| Part | Time | How measured |
|---|---|---|
| Bench harness (16 KB code compare, data copy) | 0.30 s | `ps2_vu1_run` returning at once |
| XGKICK into the GS | 0.10 s | the kick removed |
| Stall model (`pair_stalls`, component readiness) | ~0.04 s | removed (results unchanged) |
| Flags (MAC/status/clip ring) | ~0.35 s | removed (results change) |
| The rest: arithmetic, clamping, register traffic, dispatch | ~0.65 s | |

- So flags are ~30% of the pure VU1 time, and the stall model ~3%.
- Interpreted only, the same bench takes 2.4 s: the AOT is ~1.8x the interpreter.

### The answers to the questions asked

**Are the VF registers kept in SSE registers across a block?** No. Every operation loads its
operands from `cpu->vf` and stores its result back, along with four component-ready stamps.

**Is the AOT compiled at -O2?** Yes: `port/tools/link.py` compiles `vu0_interp.c` (which includes
the blocks) at `-O2 -msse2 -mfpmath=sse -ffp-contract=off`, with strict aliasing.

**Are the FMAC ops scalar per component?** No for VU1: they already use SSE (`_mm_add_ps`,
`_mm_mul_ps`) on clamped operands (`arithmetic4`). This is exact: MXCSR is set to round toward
zero with FTZ/DAZ for VU1, the clamp runs on the bits before the operation, and multiply and add
stay separate (`-ffp-contract=off`). The PCSX2 oracle checks it.

**Where does the frame go?**

- Each VU pair compiled to ~300 x86 instructions (1.1 KB of code). The 906 blocks made 3.6 MB.
- Inside a hot block the game's samples are flat: no instruction stands out. The cost is the
  instruction count.
- In the run loop (`_ps2_vu1_run`, 11.4% of the race), the samples sat on three dependent
  loads per block: the pair words, the resolved-block cache entry and the block's count.
- **The waste:** each block stored `u->unit = 1`, then re-read it after every store through
  `u->c` (which may alias `u`). So every pair carried the VU0 paths too: scalar lanes, stall
  checks and the scalar flag loop. An 8-pair block had 17 scalar `mulss`.

### Fixes (be03c15b), exact

1. The generated blocks restate the unit before each step
   (`if (u->unit != 1) __builtin_unreachable()`), and the VU0 paths drop out. The AOT code goes
   from 3.59 MB to 2.09 MB.
2. The run loop reads the pair words only when it interprets. It records one history entry per
   dispatch (the error message's last PCs) instead of one per pair.

Checks:

- PCSX2 VU1 oracle: 1584 cases, 32 entries, 0 failures.
- Bench hash unchanged.
- Race dumps at GO+0, +2, +12 and +16 identical to the reference.
- The image check prints MATCH.

Effect: the run loop's share of a race fell from 11.4% to 9.3%. The blocks' share is unchanged
(37.5% → 37.1%): the smaller code doesn't buy measurable speed, so instruction cache misses were
not the limit. The bench is unchanged too (1.40 s either way).

### What a VU1 2x needs

There is no cheap exact fix left that is big. The remaining work is spread across every
operation:

- the flag ring (~30%);
- register traffic through memory;
- the per-operation clamp of both operands;
- the per-block dispatch.

Halving VU1 means a real recompiler pass over each microprogram in `vu1_aot.py`:

1. Flag liveness across the whole microprogram, so only flags a later `FM*`/`FS*`/`FC*` or the
   program's end can see are computed. The end state stays exact, so the oracle keeps comparing
   it.
2. Static stall timing inside a block.
3. VF values kept in SSE registers within a block, with clamping skipped where the input is
   known finite.
4. Longer blocks across branches.

That is a design choice for the next round, not a tuning step.

## 3. The graphics worker (`DHD_MTVU=1`)

The existing worker runs VIF1, VU1, the GS and D3D on one thread. A temporary probe in
`graphics_queue.c` timed both sides over 5 s windows (race, paced):

| Measure | Race |
|---|---|
| Worker busy | 91% |
| Main thread waiting in `graphics_wait` | 75-81% |
| `graphics_wait` calls / 5 s | ~1900, about 60% with the worker already idle |
| Producer blocked on a full ring | 0.2% |

- The worker is the critical path.
- The fences are frequent but cheap: most find the worker idle.
- The overlap is already the EE (~20% of the frame) against everything else.

**Determinism.** Race dumps at GO+0, +2, +12 and +16 with `DHD_MTVU=1` are identical to the
inline reference. The fences make every guest-visible read wait for the worker:

- GS privileged registers and CSR;
- VIF1 registers;
- VU1 memory;
- store-image reads;
- vblank.

So completion points don't depend on host timing.

**The VU1/GS split.** Splitting the worker into a VU1+VIF thread and a GS+D3D thread would bound
the frame by VU1+VIF (~60% of today's frame, ~17-18 ms): about 50 fps at best, not 60. VU1 has
to get faster either way, so the split is not built this round.

## 4. Results

Measured as in finding 027 (presented frames per second, menu 20-40 s, race start +20..+80 s):

| Build | Mode | Menu fps | Race fps |
|---|---|---|---|
| round 20 (exe-defer) | paced | 27.2 / 29.6 | 35.8 / 34.9 |
| round 20 | wall | 32.8 / 30.4 | 27.8 / 27.9 |
| round 21 (r21a: audit patch, VU1 fixes) | paced | 30.1 / 30.1 | 35.4 / 35.5 |
| round 21 | wall | 33.2 / 33.3 | 28.1 / 27.4 |
| round 21 with `DHD_MTVU=1` (probe build, then r21a) | paced | 32.0 / 32.5 | 39.7 / 40.0 |

- Inline, round 21 is within noise of round 20 in the race (~+1%).
- Wall mode still can't pass 30 fps in a race.
- `DHD_MTVU=1` is +13% in the race and still opt-in. Finding 021 listed rendering faults under
  it (purple polygons, a crash); the dumps now match, but the rendering hasn't been checked
  against the inline output yet.

### Where a paced race frame goes now (r21a)

| Cost | Share |
|---|---|
| VU (VU1 ~52.6%: blocks 37.1%, run loop 9.3%, flag retire 3.1%, others; VU0 ~5.7%) | 58.3% |
| GS on the CPU | 13.6% |
| D3D11 waits (flush 4.5%, draw 2.8%, stage_pages 1.3%, others 0.5%) | 9.1% |
| VIF1 | 8.7% |
| ps2asm | 4.9% |
| Game code | 3.5% |
| Everything else | ~2% |

## Sources

- The clang static analyzer's `security.ArrayBound` checker; LLVM SROA: an alloca whose address
  escapes to a call is not split.
- PCSX2 `MTVU.cpp` (the copied-ring design, finding 021) and microVU `mVUsetFlags` (flag
  liveness), for the design discussion.
- The PCSX2 VU1 oracle (`port/tools/test_vu1_pcsx2.py --aot`).
