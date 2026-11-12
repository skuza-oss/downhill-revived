# 219: three checks that passed when they should fail

Status: current

Branch `fix/check-tools-false-pass` from `main` c5f839a2, started 2026-11-12. This file is the
lane's plan while it runs and its finding when it merges. Merged into `main` on 2026-11-12
(3073462c) and checked there the same day ("Checked on the merged main" below).

## Goal and acceptance

An outside review of the 0.8.0 candidate (findings 3, 4 and 5 of its game/host report) found three
checks that can say OK when the thing they guard is broken. This lane makes them stricter and
proves each with a test that shows the old false pass now fails. Tools and their tests only: no
game code, no patches.

1. `port/tools/stack_escapes.py --check` passed when a unit's `-gstabs` compile failed (the unit was
   a `WARN` and dropped out of the analysis).
2. A reviewed `fixed` overrun passed with any patch on its unit (`check_fixed` only tested that the
   unit was a key of `patches.PATCHES`).
3. `port/tools/options_walk.py` printed `ok` for a row whose picture did not change after Cross.

Acceptance: each tool stricter, each with a test that shows the old false pass now failing;
`python tools/check.py port` and `check.py docs` green; `docs/TESTING.md` updated.

## Progress

- [x] 2026-11-12: finding reserved (dad957f1)
- [x] 1 compile failures fail `--check` (d4142ee9)
- [x] 2 fixed entries proven on the patched source (db018855)
- [x] 3 OPTIONS walk needs evidence per row (0e3e933f)
- [x] `check.py port` and `check.py docs` on the last commit (Result)

## Decisions

**1. Compile failures.** `--check` fails on a unit whose retail text does not compile with `-gstabs`
unless `port/tools/stack_escapes_compile_failures.txt` lists it with a reason, and on a listed unit
that compiles (or is gone), so the list cannot rot. The list is empty: 3815 of 3815 units compile
today. Report-only runs still warn. A failure of a *patched* text is a different case (below).

**2. Fixed lines.** The proof is the analysis itself, not a marker. A unit with port patches is also
compiled as the port compiles it (`port_source.apply_patches`, what `build_wasm.py` uses). The
function frames, locals and the escapes of their addresses come from that text; what a callee does
with its argument still comes from the retail text. (The first version analysed the patched text for
everything and lost a real read: a port patch rewrites `func_00221AB8`, so its summary no longer
showed the 64-byte read, and `func_001AE358`'s fix looked proven by absence. Summaries stay on the
retail code, the code the game runs.) A `fixed` line is true when, for that function and callee, the
patched analysis shows no overrun (another local's own `safe` line aside) and at least one escape
that fits (`fits`, or an `unbounded` row whose measured range lies inside the local, or for a
`sprintf` a call whose longest text fits): an escape the tool no longer sees proves nothing. The unit
named in the reason must be the function's unit, and it must have been analysed patched.
- Of 294 units with port patches, 246 build with the matching compiler (SN cc1) and 48 do not (the
  patches call port-only functions, C++ references in C code, asm constraints of the port's own
  helpers). Those 48 are analysed unpatched, as before; none holds a reviewed fix (the ten fixed
  functions' units all build). A `fixed` line on such a unit fails ("analysed unpatched, so the fix
  is not proven"): no assertion fallback was needed. A unit that stops building after a patch
  edit is not an error (the same code was analysed before this lane); the summary line of
  `--check` prints the count of such units.
- Not done: listing the 48 units with a reason each. A new port patch that calls a port function in a
  C++ unit would then fail `check.py port` until the list was edited, for a unit that carries no
  reviewed fix; the retail text stays analysed for it.

**3. OPTIONS walk.** Run on the four scenarios (below), every row opens something: a page of its own
or, for LOAD GAME DATA, SAVE GAME DATA and LOAD REPLAY, a box that asks or says "not found", which
Triangle cancels. So no row acts in place and no side-effect checks were needed (nothing is saved or
loaded by the walk; the earned card is made by `make_earned_card`, its own step). The evidence per
row: the picture differs from the OPTIONS list by more than 6 grey levels (crop mean; real values
19.6 to 58.8), is the same a second later (stable, at most 2.0), differs from every other row's
picture, and the list is back after Triangle (at most 1.6). Distinctness uses the fraction of whole
picture pixels (grey, 480x360) that differ by more than 40 levels, with a tolerance of the two
pages' own wobble plus 0.0002: the list crop cannot tell the three boxes apart (their crop
difference is 1.6 to 5.9; their pixel fraction 0.0026 to 0.0096; the same static page twice 0.0).
Before Cross the list picture must differ from the previous row's by more than 0.002 of the pixels
(real: 0.007 to 0.021), or a dropped Down (Cross would act on the wrong row) fails the walk.
A row with no change gets one retry (after a late capture), printed as `retry:`; a second miss
fails with the row's name and what should have shown.

## Surprises

- The retry is not hypothetical. On a fresh card SAVE GAME DATA's first Cross shows nothing in all
  three fresh-card runs (off-fresh, on-fresh twice) and the second opens "Overwrite Game Data?"; on an
  earned card it never needs one. The old walk passed that row either way. Whether the game ignores
  the first press after the LOAD box was cancelled or the posted key is lost was not chased: the
  retry covers both and the output says so.
- A patch that rewrites a callee hides its accesses from callers (above): analysing patched text
  needs the retail summaries.
- The analysis over the patched text alone first raised "unbounded" from 616 to 850: calls the
  patches redirect to port functions have no summary. With retail summaries the counts are 2754 fits,
  636 unbounded (retail 2749 and 616; the difference is locals the patches add).
- Background shell jobs of this session lost their redirected output (an empty log); long runs went
  in the foreground or were waited with an until-loop.

## Checked on the merged main (2026-11-12)

On `main` 3073462c, by the lead's verifier.

- `python tools/check.py port -j 6`: OK, with `test_options_walk` 7.4 s, `stack_escapes` (`--check`)
  45.0 s, `test_stack_gate` 154.4 s. `check.py server` OK.
- `options_walk.py --scenario off-fresh` on this main and on the 524948ba reference build: both walk
  every row with no fault, and both print the same `retry:` for SAVE GAME DATA.

**SAVE GAME DATA's first Cross on a fresh card is taken: it saves.** On an empty card there is
nothing to overwrite, so the game saves at once without asking. A scratch probe (one hidden-desktop
game per case, `DHD_LOG=1` for the memory card calls, LOAD GAME DATA opened and cancelled first as
the walk does, then Cross on SAVE GAME DATA): right after the first Cross the card's four files
appear (`BASCUS-97177`, `icon.sys`, `listicon.ico`, `pathfile.dat`) with 70 memory card log lines
(the directory read finds nothing, then the writes); the OPTIONS list fades out while it saves (the
picture 22 to 33 grey levels from the list at 0.4 s and 1.5 s) and is back by 3 s (1.3 to 2.8),
which is when the walk looks, so it sees "no change". The second Cross then finds the save (the
directory read returns 1) and asks "Overwrite Game Data?", the walk's evidence. The same with a
0.5 s hold and 3 s on the row before the press, and on the 524948ba build: the game's own
behaviour, not a dropped key, and old. So the fresh-card scenarios write a save to their scratch
card (Decisions 3 says nothing is saved: SAVE GAME DATA's first Cross does), and the row passes on
its retry's box. Not changed here; the walk could take the faded list or the card's new files as
that row's evidence on a fresh card instead of a second Cross.

## Result

Lane result, 2026-11-12. Merged and checked (above).

| Check | Before | Now | Proof |
|---|---|---|---|
| `stack_escapes.py --check`, a unit that fails to compile | WARN, exit 0 | ERROR unless listed with a reason in `stack_escapes_compile_failures.txt`; a listed unit that compiles fails; the list is empty (3815 of 3815 compile) | `test_stack_escapes_gate.py`: a made-up broken unit: error unlisted, pass listed, error for a stale line, warning only in report mode |
| a reviewed `fixed` overrun | passed with any patch on the unit | needs no overrun and an escape that fits in the analysis of the patched source | the same test removes each fix in turn from a copy of the patch table (a harmless edit stays in the unit): all ten fixed functions (the nine overruns, 001F8CA8 and 0013C6A8 counted apart) fail with "the patched unit still overruns", while the old predicate still passes for every one |
| `options_walk.py`, a row whose picture did not change | `ok` | failure after one retry, with the row's name | `test_options_walk.py` (scripted stand-in): normal pass; withheld or dropped Cross, dropped Down, one picture for two rows, an unsettled page, no way back: each fails. Real game: `--withhold-cross 5` fails on POSTERS and passes VIDEO CLIPS and PICKUP EDITOR; the four scenarios pass |

What main has: nothing flagged. `stack_escapes.py --check` is OK on c5f839a2 with the stricter
rules (all ten fixes are present in the patched source); the OPTIONS walk passes in all four
scenarios (every row opens, settles, differs from the others, and the list comes back, no fault in
any log); it reports one retry per fresh-card scenario for SAVE GAME DATA (Surprises).

Cost: `check.py port` on this branch, warm builds, 6 jobs on a PC that other workers use: the
stack step `stack_escapes --check` 14 s (13.5 to 14.4 s; the cold cache about 90 s as before, the 246
patched units add a second compile pass of about 15 s once), `test_stack_gate` 157 s (the made-up unit and
ten full analyses, each about 11 s), `test_options_walk` 7 s. The four-scenario OPTIONS walk takes
about 10 minutes now (the evidence waits; it is not in `check.py`).

Not checked: the 48 patched units the matching compiler cannot build are analysed unpatched, so a
new overrun that a patch there introduces is not seen (as before); the 616 unbounded escapes are as
unreviewed as before; the walk's distinctness uses picture differences, not an OCR of the page, so
two rows that really show near-identical pages could be told apart wrongly or not at all (the three
boxes differ by 0.0026 to 0.0096 of the pixels, a duplicate by 0.0); `redzone_walk.py` calls the
walk and takes its exit status as before and was not run; Android and the browser were not touched.
