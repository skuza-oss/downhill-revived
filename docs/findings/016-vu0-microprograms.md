# 016 — VU0 microprograms and the inline asm runtime

Status: partly superseded by 044 (lockstep as the reason for bit-exact VU0)
Checked: 2026-11-04

The programs and their timing are current (`port/runtime/vu0_micro.c`). The
reason once given for bit-identical results, lockstep multiplayer, is gone: online races are state
sync (finding 044). Exactness is kept for the state traces and the PCSX2 checks. Today the other
VU0 entries run compiled (`port/tools/vu0_aot.py`, finding 090 SP1), not only interpreted.

The game's inline asm starts three VU0 microprograms with `vcallms`. The microcode is in the ELF,
uploaded by two VIF MPG packets: micro 0x000–0x7FF and 0x800–0xD4F. They are the last pair of
the three MPG pairs in the file. (The other two pairs fill 0x000–0xFFF and don't look like VU0
code: at 0x978 they hold unrelated FMAC/load code.) To locate the packets, scan for VIF code
0x4A (MPG) with a micro address below 0x200, then disassemble from the `vcallms` address until
the E bit plus one.

| addr  | callers | what it does |
|-------|---------|--------------|
| 0x978 | ~10 sin/cos sites (00221F50, 00222040, 00222E80, 00227DD0, 00228BE8, …) | vf14 = sin(vf14), vf15 = cos(vf14), per field. Reduces to [0, 2π) by whole turns (`vftoi0` truncation, float 2π), folds to [-π/2, π/2] with min/max, then an odd degree-9 polynomial: c3 = 0xBE2AAAA4, c5 = 0x3C08873E, c7 = 0xB94FB21F, c9 = 0x362E9C14. |
| 0x830 | 00222140 | `bal vi15` into the sin/cos at 0x980, then builds rotation rows vf10..vf12 (xyz only) from the products of the sines and cosines. |
| 0x0c0 | 00130010 | Sphere against two boxes: the point vf5.xyz + vf4.xyz goes through 3×3 matrices vf7..9 and vf1..3 and is offset by ±vf5.w. The result is sign bits in vi2/vi5 (read with `cfc2`), plus vf10.x += (A+r).z. |

Flag timing matters for 0x0c0. An `fmand`/`fsand` sees an FMAC instruction's MAC/status flags
4 cycles after that instruction issues, counting stall cycles. MAX/MINI/FTOI/ITOF don't set
flags. With any other latency, two of the four reads would see the same instruction, which
makes no sense. With 4 they read the four offset results in order.

`port/runtime/vu0_micro.c` holds the native versions. Each follows the microcode operation by
operation: same constants, same rounding order, no fused multiply-add. So on every PC they
produce the same bits (then wanted for lockstep multiplayer; now for the state traces and the
PCSX2 comparisons). They differ from the PS2 only where the
VU's own float rules differ: it rounds toward zero and has no denormals, infinities or NaNs.
Large angles keep the microcode's reduction error (about |a| × 4e-8).

Runtime state at this point (`python port/tools/test_runtime.py`): all 462 distinct game asm
templates parse, none use unsupported operations, and the semantic tests pass, including these
three programs.
