# 004 — How ps2eeas addresses small data

Status: current
Checked: 2026-11-04

Date: 2026-10-01.

## ps2eeas small-data addressing (corrects finding 003's `.extern` hoist)
Measured: for `lw $2,sym` / `sw $2,sym` (cc1's macro form under -G8) ps2eeas uses a single
`$gp`-relative instruction if it already knows `sym` is small; if not, it expands to
`lui`+`lw` — **except in a delay slot** (`.set nomacro`), where it must emit one instruction
and uses `$gp`. cc1 writes `.extern sym, size` at the end of the file, so for an extern
variable the size is unknown at every use.

So in retail:
- `$gp` access outside a delay slot ⇒ the variable was **defined earlier in the same TU**
  (define it in the unit, claim its `.sdata`/`.sbss` range, use its retail initial value);
- `lui` access to a small scalar, or `$gp` only in delay slots ⇒ extern / defined later.

The earlier hoist made all externs `$gp` and broke the second case (func_00187F48); it is
reverted. This is TU evidence: functions that `$gp`-access a variable outside slots share a
TU with (and follow) its definition.

## Other notes
- 128-bit by-value args (names packed into one GPR via pcpyld/dsll/ori) match with
  `typedef unsigned int u128 __attribute__((mode(TI)))` constants.
- gcc 2.95 vtables: 8-byte entries (delta at +0, pfn at +4), first virtual at +8.
- try_func --quiet still prints ninja progress; harmless "Assembler messages:" lines.
