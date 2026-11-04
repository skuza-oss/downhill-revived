# 012: C idioms that steer SN ee-gcc 2.95.3 (from sibling PS2 decomps)

Status: current
Checked: 2026-11-04

Our compiler (SN BUILD 1.36) is the one used by OpenRAC's Ratchet & Clank decomps. Their notes
list C-level levers for the near misses that register-allocation and scheduling leave behind.
Condensed and paraphrased here; see the sources for their examples.

Sources: [rac3-uya-decomp Matching Patterns](https://github.com/OpenRAC/rac3-uya-decomp/blob/main/docs/wiki/Matching-Patterns.md),
[rac1-decomp WORKFLOW](https://github.com/OpenRAC/rac1-decomp/blob/main/docs/WORKFLOW.md) and
[DECOMP_PROGRESS](https://github.com/OpenRAC/rac1-decomp/blob/main/docs/DECOMP_PROGRESS.md).

## Register allocation
- Priority ~ refs * log2(refs) / live length (`tools/regalloc.py` prints it). Change what outranks
  or overlaps the value you want to move.
- A callee declared `void` vs `int` changes the `$v0`/`$v1` choice after the call, also for a
  block-local constant several insns later (func_0019EE98: `func_0017BE38` void gives `li $v0, 1`,
  while `func_00236C60` must stay `int` for `lui $v1` after it). Try each callee near a v0/v1
  mismatch both ways.
- Block-local temporaries per case/arm instead of one shared variable; one counter per loop.
- Reuse the incoming parameter as the working pointer to keep it in `$a0`.
- Register ties: duplicate the code that follows an if/else into both arms (gcc merges it back,
  but the allocation follows the original shape).
- `k = C; k *= t;` ties the output to operand 1.
- `addiu $v0, $s2, X; sw ..($v0); move $s1, $v0`: an address passed into an inline function
  (the argument temp is used first, then copied to the parameter's saved register). Loads the
  helper does itself (`root = path->root;` inside it) land after that copy, unlike loads passed
  in as arguments (func_0016F198).
- `.greg` "N preferences: R" comes from `set_preference`: `(set t (op x ..))` where x dies gives x
  a preference for t's hard register, and copies pass it on. `slot = &list->slots[i]` (slot in
  `$s0`) after a scan loop pulls `i` into `$s0`. If later loops share that `i`, loop.c reuses it as
  the reversed counter (`i = n`), so `n` inherits `$s0` too. Separate counters cut the chain
  (func_001DFC08, not matched).
- A global read once into a local (`players = D_x[0];`) is kept in a register and reused, where a
  second `D_x[0]` would reload it.

## Store order
- The compiler rotates stores: for retail order A, B, C, D try writing B, C, D, A.
- Chained assignment `a = b = f()` gives retail's store order in some cases.
- Reading a global right after storing it changes the store schedule.
- "Assign first, then test": `p->f = t; if (a < t) p->f = a;`

## Delay slots
- reorg never moves a `volatile` access into a delay slot.
- A store written before the last call fills that call's delay slot.
- A callee's parameter order changes which argument setup lands in the `jal` slot.

## CSE, combine, %hi/%lo
- `n = h->count++;` keeps a copy of the old value; split `w = e->b; w -= 6;` keeps a repeated load.
- Index first in an add: `(i << 2) + (int)base`.
- Stop reassociation with a temporary: `t = off + 1; off = x + t;`
- One `char *` local per block that reads a global gives "%hi kept, %lo rebuilt".
- Two C names on one symbol (an `__asm__("D_x")` alias with another type) when one global is
  addressed two different ways.
- An unsized `extern T X[];` is never small data (always `lui`); a sized one can be `$gp`.
- Retail recomputes an expression (two `abs.s` of one value) that cse would share: reassign
  the operand variable in between, even to the same value (`bc = obj->xBC;`). That bumps its
  register tick and drops the cached expression. Use the builtin (`extern float fabsf(float);`),
  not an inline wrapper: the wrapper's return copy keeps a second register holding the old
  result (func_001737F0).
- `D.f.q = v` (128-bit field) stores at `0x80(base)`; `*(u128 *)D.f.f = v` loads `sym+0x80`
  into its own register (`sq 0(reg)`), and later `D.g` accesses become `reg - 0x80` through cse
  related values (`addiu $a1, $v1, -0x80`) (func_001DFC08).
- Several members of one global struct (`&D.pos`, `D.flag`) make cse keep `&D` in a saved
  register and add offsets; retail code that reaches them through their own splat symbols
  (`lui/%lo(D_x)`) wants separate externs (`asm("D_x")` aliases) (func_001D4E80).
- `world = &D; list = &world->x240;` gives `lui/addiu D` then `addiu +0x240` (offsets added to the
  base register); `list = &D.x240` gives one `%lo(D+0x240)` constant.
- An inline accessor `static inline World *GetWorld(void) { return &D; }` used at several sites
  keeps `%hi(D)` in a saved register and rebuilds `addiu %lo` per use, then `+offset`; a direct
  `D.field` folds into one `%lo(D+off)` (func_0010F8E8, func_0019EE98).
- Inline parameters are copied into fresh pseudos unless the argument is a constant; an
  `unsigned char` parameter adds an `andi 0xFF` at the copy. An argument containing a call makes
  gcc compute the other arguments first; a comma expression inside the argument list moves them.
- `x % n` always expands into a temporary that is then copied to the destination; local-alloc
  cannot tie that temporary to a variable that lives on.
- `total + (size - 1)` is reassociated to `(total + size) - 1`; `t = size - 1; total + t` keeps
  retail's `addiu` order.
- Two identical `i * size` mults survive cse when the first goes straight into a variable that is
  then reassigned: `off = i * sizeof(E); off += (int)&D;` (expand uses the assignment target for
  the first operand of a `+`, and the reassignment drops cse's entry). Array refs and
  `ptr + i` (pointer first after `pointer_int_sum`) always share one mult (func_001979B0,
  func_001A42D8, func_001A40B8: the natural source is still unknown).
- r5900 `mult` + `add` fuse into one madd pattern in combine (`(plus (mult a b) c)`, split
  again after reload into `mult` and `addu`), but not when that add is the first use of `c` after
  the insn that set it (a LOG_LINK from the add back to `c`'s set). Toys: `b = (int)T; a = i*20;
  a += b;` stays unfused; with `h(b);` in between it fuses. This matters for scheduling: an
  unfused add waits for the mult latency, so sched1 can put an argument copy `$a2 = c` before
  it and local-alloc can no longer tie `c` to `$a2` (func_001979B0, still open).

## Aliasing
- No `-fstrict-aliasing`: a float store and a pointer load conflict unless the alias analysis
  finds different bases. A pointer variable assigned more than once (one `Entry *e` used as a
  loop iterator and again later) has no known base, so `e->x8` is reloaded after every store
  to a local `Vec` (func_001979B0). Single-assignment pointers derived from a global keep the
  symbol as base and the load is shared.
- flow deletes a store to a stack slot overwritten later in the same block only if no read in
  between may alias it; a read through such an unknown-base pointer keeps both stores.
- sched1 hoists a load above a store when both use the same base register with different
  offsets; with the store written first in C there is no way to keep retail's order without
  changing the base (func_00137B70).
- sched1 lets an insn that sets a pseudo living across calls move before a call in the same
  block (only pseudos that do not cross calls depend on the last call); the delay slot then gets
  the nearest single-insn candidate. Macro stores to absolute addresses (`lui $at` + `sw`) are
  never put in a delay slot (func_00215B68).

## Division traps
- `div`/`divu` emit `beql div,$zero,1f; break 7` (`div_trap`). mips.md prints nothing for a trap
  that is anti-dependent on an earlier trap when its operand 1 is `$zero`: two divisions by the
  same register in one block print one `break`. Inside a loop, loop.c hoists the trap's zero into
  a register (`daddu $t4, $zero, $zero` in the preheader), then both traps print. A `goto` loop
  (no loop notes) keeps `$zero`. Retail's func_0023BF20 has one hoisted zero and one suppressed
  trap in the same loop (not reproduced yet).

## Loop hoisting
- Name hoisted invariants as locals in retail's init order before the loop
  (`i = 0; three = 3; sent = &sym;`). A constant kept in a saved register is such a local.
- A dead second assignment inside the loop keeps a constant from being hoisted.
- Index arrays (`e[i].t`) rather than walking a pointer, so strength reduction matches.
- `do { } while (0)` adds loop depth to reference counts (changes priorities) and acts as a
  scheduling barrier.
- Whether loop.c hoists an invariant depends on the loop's real-insn count (`.loop` dump:
  "Insn N: regno R (life L), savings S not desirable" vs "moved to"); the second loop pass
  (-frerun-loop-opt) recounts after cse2. Inner loops written as hand strength-reduced pointer
  walks shrink the outer loop, so an invariant retail recomputes per iteration (a `%lo`
  rebuilt from a kept `%hi`) gets hoisted; plain indexed inner loops (`for (j = 1; j < 43;
  j++) a[j]`) keep the count up and give the same inner code (func_001688C8).
- Retail with two pointer walks over one array (`s1 = &e[0]`, `s0 = &e[0].f`, each with its own
  `addiu 0x380`, initialised after the loop's entry test) is an indexed loop with a sub-struct
  pointer: `rider = &arr[i]; launch = &rider->launch; launch->a = ..; f(rider);`. Two DEST_REG
  givs never combine (`combine_givs_p` only folds a DEST_ADDR giv into another), so both get
  reduced. Stores straight through `rider->launch.a` are DEST_ADDR givs and fold into `rider`
  (`sw 0xA0($s1)`). Pre-test init (`p = arr; for (...) p++`) puts the inits before the
  `blez` instead (func_0019EE98).
- Two arrays read with one index (`a[i]`, `b[i]`): loop.c combines both address givs into the
  shared `4*i + base` giv, so retail's single pointer `p = base + 0xE0` with the other array at
  `-0x10(p)` needs both reads indexed off one pointer (`((float *)kinds)[i - 4]`, `kinds[i]`).
  Combine ties go to the later insn (`.loop` dump: "Sorted combine statistics"). To keep the
  pointer init as two `addiu` (`%lo(sym)`, then `+0xE0`), compute it inside the loop
  (`w = &D; kinds = w->kind;`): cse doesn't fold it there and loop.c hoists it (func_001D4E80).
- gcse PRE inserts its own `%hi(sym)` at the end of the loop preheader for uses after the loop
  instead of reusing a `%hi` computed earlier in that block (an extra register plus a `move`).
  Reading the global inside the loop makes PRE hoist one `%hi` for all uses (func_001D4E80).
- `do { } while (!done)` vs `done = 0; while (!done) { }`: same code, but when a call ends the block
  right before a do-loop label, gcse appends a `(use (const_int 0))` there. Sched1 issues it like a
  real insn and it can push a hoisted invariant behind the call (func_0020D958: the delay slot of
  the call before the loop). `tools/passdump.py --track '\(use 0x0\)'` shows it.

## Switch, cross-jumping
- Each case ending `x = i + 1; break;` keeps its own `addiu` before the branch; writing
  `next = i + 1; joint = cmd;` (the store last) stops jump.c from cross-jumping that case into the
  `default:` block. Without a `default:` the variable is uninitialized on that path and becomes
  live from function entry (a saved register).
- Two cases with the same tail: jump.c keeps the copy in the earlier case and makes the later one
  branch back. For retail's forward branch, write the shared tail once in the later case and
  `goto` it from the earlier one (func_00215140, not matched).
- When retail's if/else chains of calls merge in odd ways (one branch jumping to another
  branch's bare `jal` with arguments set before the jump), check whether a common tail after the
  chains was written once per branch. `{...chain...; tail;} else {...chain...; tail;}` gives
  jump.c different jumps to cross-jump than one shared tail after both branches
  (func_0014CC38: the `x2C0`/`x2E0` stores repeated in both axis branches).
- jump.c (gcc 2.95.3 source) rules that decide early-return layout:
  - "if (foo) bar; else break;" swap (not in the first round): for `condjump L1; range1; jump L2;
    L1: range2; jump X; barrier; L2:` where L1 has one use and L2 is the next label after L1, the
    condition is inverted and the two ranges swapped. A `return 0;` before a body that ends in
    `return x;` gets moved after the body this way (func_002388F0: write `if (ok) { body; return
    x; } return 0;` to keep retail's `bnez body; b end` order).
  - Cross-jumping (jump2, after reload) walks jumps in insn order; for an unconditional jump it
    first tries the code falling into the target label (minimum 1 matching insn), then other
    jumps to the label (minimum 2). A mismatch at a conditional jump that jumps around this
    jump lowers the minimum by one, so two `v0 = 0; jump ret` blocks right after two
    conditional jumps merge: the earlier one is redirected to the later one.
- gcc 2.95 does not reorder blocks: code retail places between two cases (a loop's "found" block
  after the next case) is lexically there in the source, e.g. as a label.
- Loop rolling (stmt.c `expand_end_loop`): if a loop's first insns (up to 30) end in a
  conditional jump to the loop exit, they move to the bottom and the loop is entered with
  `b test`. `for (i = 0; i < n && !found; )` gives one folded test before the loop and both
  tests at the bottom (func_0023BF20).
- The rolling scan goes past the first exit: it keeps the *last* jump to the loop's end label
  (conditional, or a `break`) seen while the count of insns + jumps from the loop top is <= 30
  (calls don't count), and moves everything from the loop top up to that jump to the bottom.
  A second `break` a few insns later moves too much. A `goto` to a label right after the loop
  is not the end label, so the scan ignores it: `for (;;) { if (hit) { ..; if (ok) { ..;
  break; } restore; } else { ..; goto out; } hit = next(); } out:` gives retail's `b test;
  restore; b next; nohit; next; test; body` (func_00157BE8).
- Two `if (x >= 0)` blocks in a row are one block for cse1 (it follows the jump path), so
  expressions in both are shared before jump threading merges them.

## min.s / max.s
- Retail `min.s`/`max.s` in a call's delay slot are real insns, not asm: reorg never fills a
  delay slot with an asm. Without -ffast-math, gcc 2.95's fold doesn't turn `a < b ? a : b`
  into MIN_EXPR for floats, so C can't produce them. g++'s `a <? b` / `a >? b` can. Retail code
  with them sits in a C++ unit; if the surrounding C unit compiles differently as C++, split
  the unit at the function (func_0013B5E8; as C++, func_0013A270's struct copy changed).
- `(set lo (minus t c))` reuses t's register only if t is block-local: a `t` assigned both in a
  loop and after it is global, so local-alloc gives the result the constant's register. Use a
  separate variable per block (func_0013B5E8).

## Booleans
- `if (r < 0) return 0; return 1;` gives `slti`/`xori`; `return r >= 0;` gives `nor`/`srl`.
- Bind a comparison to a local before shifting it (`(x != 0) << k` folds into a branch otherwise).
