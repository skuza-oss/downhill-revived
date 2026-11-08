# NONMATCHING C for the last unmatched functions (2026-10-21)

Status: current
Checked: 2026-11-04

The 26 functions left `failed` in the queue never got a byte-exact match. 25 now have readable,
behaviour-equivalent C next to their original asm. The default build still uses the asm and
matches retail. The 26th, func_00220DA0, is hand-written asm and stays asm.

## Convention

```c
#ifdef NONMATCHING
/* Non-matching: <what differs>; best diff N/M words; notes: build-try/<dir>/FUNC.notes.md */
<declarations the function needs (extern only), then the function>
#else
INCLUDE_ASM("asm/nonmatchings/text", FUNC);
#endif
```

No convention existed in the repo before this, so this one is new; docs/WORKER.md ("NONMATCHING C") documents it.
Because the `INCLUDE_ASM` line stays, queue.py, progress.py, report.py and promote.py still
count the function as asm. A block never defines data, so the NONMATCHING build takes all data
from the asm the same way the default build does. Seven functions sat in existing multi-function
units. The other 18 got new single-function units (`/* UNIT start next */`), and in the default
build those compile to the same bytes through INCLUDE_ASM.

func_00220DA0 is listed in `config/handwritten.txt` (status `handwritten`). It sums `pmulth` halves
read back with `pmfhi`/`pmflo` using two trapping `add`s (0x220EF8, 0x220F7C). gcc emits `addu`,
and the source rules don't allow the MMI moves as ordinary inline asm.

## NONMATCHING build

`python tools/configure.py --nonmatching && python -m ninja -f build-nonmatching/build.ninja`.
This adds `-DNONMATCHING` to every unit, uses its own build dir and doesn't reuse C objects from
the default build. Each piece goes at its retail address, or right after the previous piece if
that one grew (`. = MAX(., offset)`; sections use `MAX(vram, ALIGN(16))`). There is no check step.
Two checks of the change:
- the default `build.ninja` and `dhd.ld` are byte-identical to before;
- a NONMATCHING build before any block existed gave `MATCH` on all eight sections.

Result with all 25 blocks: it links, and each of the 25 symbols comes from its C object (no
`.NON_MATCHING` asm marker in any of those objects). The address and size of all eight sections
match retail. Five sections are byte-identical: .vutext, .gcc_except_table, .sdata, .sbss and .bss.
Differences:
- .text: 33,996 words. Three functions grew (001B40A0 +8, 0020C530 +8, 0022E9A0 +0x10), so
  these ranges moved until a smaller function or alignment padding absorbed the growth: 0x1B45F8–0x1C1C00 +8, 0x20C918–0x20E018 +8, 0x22F040–0x23AE78 +16,
  0x23AEB8–0x23D1F0 +8. Every `jal` into a moved function therefore differs.
- .data: 37 words of function pointers.
- .rodata: 678 words of jump tables that point into the moved code.

The functions that grew reach small data with `lui`/`%lo` instead of `$gp`, because the data is
`extern` and not defined in their unit. The game wasn't run (other agents use the PC).

## Review

Each function started from the closest earlier candidate (`build-try/cand-*`, notes in the queue).
It was then checked against the retail asm by hand: every load and store, constant, branch
condition, loop bound, call and each argument register, return value and float op. Each block was
compiled unwrapped with `try_func` (word diff below), and every differing word had to be
explained as register naming, scheduling or an equivalent instruction choice.

None of the candidates had a logic error. Fixes made while preparing the blocks:
- func_001A1990: dropped a 4th argument to func_0023D650 (the callee reads only a0–a2).
- func_001FB6E0: the candidate defined `__clz_tab` (D_00297138); it is now `extern`.
- func_0020C530: the candidate defined D_0029DA10, which 0020C9F0.c owns; it is now `extern`.
- func_00213658: a tied asm output that the asm never wrote became a plain input.
- Several candidates' helpers that existed only to steer codegen were removed where that kept
  the code clear. Some remain (`do{}while(0)` scopes, small inline helpers) where removing them
  made the diff much larger. Each one keeps the same behaviour, and the file says so in a comment.

Two functions with structural diffs (0020E018, 001D2428) got a second, independent adversarial
review, which also found them exact.

| Function | File | Diff (words) | Size built/retail | Confidence | Why |
|---|---|---|---|---|---|
| func_0010D838 | 0010D838.c | 8/431 | 0x6BC/0x6BC | high | two registers swapped at the head of one branch |
| func_00119AC8 | 00119AC8.c | 12/208 | 0x340/0x340 | high | entry-block FP registers only; leftover words before the entry kept as .text data (as in 001E3CD0.c) |
| func_0011A438 | 0011A438.c | 65/67 | 0xFC/0x10C | high | rewritten: walks one Rider pointer where retail recomputes riders[i]; all 6 stores and 4 compares checked |
| func_0014C800 | 00148F60.cpp | 14/270 (aligned) | 0x434/0x438 | high | s1/s3 swapped, one copy dropped |
| func_001A1990 | 001A1990.c | 88 rows (aligned) | 0x4EC/0x4F4 | high | index product computed once, strings via extern symbols; loops and format calls traced |
| func_001A3598 | 001A3598.c | 25/216 | 0x360/0x360 | high | loop hoists a stack address instead of the stride constants |
| func_001B40A0 | 001B40A0.c | 33/342 (with local sdata) | 0x560/0x558 | high | branch/load order and register swaps; extern sdata costs 2 lui |
| func_001D1C58 | 001C5040.c | 14/347 | 0x56C/0x56C | high | one register copy, operand order of two adds |
| func_001D2428 | 001C5040.c | 208/292 | 0x490/0x490 | high | register allocation (rows/i spill reversed); every path traced twice (independent review) |
| func_001DC548 | 001DC548.c | 94/186 | 0x2E8/0x2E8 | high | register naming and spill scheduling, same branch structure |
| func_001FB6E0 | 001FB6E0.c | 73 rows (aligned) | 0x6D4/0x6EC | high | libgcc 64-bit division in the form of the matched 001FBDD8.c; constant forms and register allocation differ |
| func_00204F38 | 00202E00.cpp | 8/286 | 0x478/0x478 | high | sched2 load order, one movz |
| func_0020C530 | 0020C530.c | 9 rows (aligned) | 0x3F0/0x3E8 | high | f2/f3 swapped, extern data via lui/%lo |
| func_0020E018 | 0020E018.c | 225 rows (aligned) | 0x408/0x468 | high | retail spills queue pointers and counters to stack slots, the C keeps them in registers; every path traced twice (independent review) |
| func_00213658 | 00213658.c | 121/363 (before the asm-operand cleanup) | 0x59C/0x5AC | high | FP registers, scratch slots, delay-slot fill; the cleanup only removed pointer copies |
| func_00213C08 | 00213C08.c | 20/594 | 0x948/0x948 | high | scheduling, register naming, mtlo/madd address form |
| func_002173C0 | 00215B68.c | 54/241 | 0x3C4/0x3C4 | high | obj/index in s1/s2 swapped, two loads reordered |
| func_00225CE8 | 00225CE8.c | 8/50 | 0xC8/0xC8 | high | two register pairs swapped |
| func_0022B258 | 00228BE8.c | 5/187 | 0x2EC/0x2EC | high | one s1/s2 swap |
| func_0022E9A0 | 0022E9A0.c | 37/423 (with local sdata) | 0x6AC/0x69C | high | same instructions reordered in the frustum setup; extern data costs 2 lui + 2 nop |
| func_00233F00 | 00233F00.cpp | 194/450 | 0x708/0x708 | high | register allocation and spills only; aligned diff read in full, vtable slot +0x28 checked |
| func_00238740 | 00238740.c | 72/107 | 0x1AC/0x1AC | high | saved vs temp registers change the slt/movz forms and store order; all 12 stored values traced |
| func_002388F0 | 002388F0.c | 37/94 | 0x178/0x178 | high | two return-0 exits merged, scheduling |
| func_0023E288 | 0023E288.c | 20/335 | 0x53C/0x53C | high | register naming in the per-group table setup |
| func_0023F138 | 0023E7C8.c | 44/325 | 0x514/0x514 | high | ring index register, mask setup order, one commutative mul.s |
| func_00220DA0 | config/handwritten.txt | – | – | – | hand-written asm (see above), no C |

"Confidence" covers semantics only. It rests on static review plus each compiled listing; nothing
was executed. A word diff can't be measured exactly when the size differs, so those rows give the
number of aligned rows that differ (`tools/adiff.py`).

## Next

- A worker retrying one of these can start from its block: unwrap it as the candidate.
- If the NONMATCHING image is ever run (PCSX2), the moved ranges above are what to watch. All
  references there are relocated symbols, so moving them is expected to be safe.

Sources: candidates and notes from earlier workers (build-try/cand-*), docs/WORKER.md helper
forms, findings 013/014. No outside code.
