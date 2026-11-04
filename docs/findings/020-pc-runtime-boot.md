# 020 — PC runtime: memory, kernel, IOP, DMA and VU0 (boot, disc, input step)

Status: partly superseded by 088 (the 32-bit memory map, relaunch, Windows fibers, the then-missing parts)
Checked: 2026-11-04

Now: the core is a wasm32 module on a 64-bit host (finding 082): guest RAM and its mirrors are
`port/host64/guest_memory*.c` (no relaunch, no fixed host addresses), PS2 threads are the shared
fiber module `port/host64/fiber.c` (finding 099), pads come through SDL3 (finding 059), and the
memory card, multitap, sound and renderer all exist (findings 040, 022, 021, 085). The SIF, CDVD,
IOP file, DMA, VIF, VU0 and C++ vtable rules below still hold.

The PC build (then `build/port/link/dhd.exe`, today `dhd64.exe`) runs the game's own `main` on a native replacement of
the PS2 system (`port/hle/`) and the emulated register file of the asm runtime
(`port/runtime/`). This finding records how each part works and why.

## Memory (the 32-bit build's map; retired, see the note above)

- PS2 RAM (32 MB) is one shared memory block, mapped at host `0x08000000` (cached), and at
  `0x28000000` and `0x38000000` for the game's uncached (`| 0x20000000`) and uncached-accelerated
  (`| 0x30000000`) pointers. A DMA address (`pointer & 0x0FFFFFFF`) is then the host pointer.
- A second view of the same RAM sits at the PS2 addresses themselves, from `0x00400000` up
  (plus the two mirrors). It's needed because the game's data files hold absolute PS2 pointers:
  NGP models and texture files are linked to load at `0xA00000` and are relocated by
  `(where loaded − 0xA00000)`. Windows keeps `0x100000–0x400000` busy even in a suspended
  process, so RAM can't be mapped at its own addresses outright. On the first run, `main`
  starts the program again suspended, reserves `0x00400000–0x02000000` in the child with
  `VirtualAllocEx`, and ties the child to a kill-on-close job. The child releases the
  reservation and maps the view there (`memory.c`, `host.c host_relaunch_reserving`).
- The scratchpad (`0x70000000`), the EE hardware registers (`0x10000000`, `0x12000000`) and VU
  memory (`0x11000000–0x1100FFFF`) are plain memory at their PS2 addresses. The game writes VU0
  data memory through `0x11004000` (e.g. `func_0021CB98`), so `ps2.vu0_mem` and `ps2.vu0_code`
  point there.
- `.rdata` is writable: some data the C declares `const` is written by the game.
- `data.py` keeps a `.word` as a number when splat took text for a pointer: the word is
  unaligned, or ends a run of printable bytes after another such word ("m0:\0" had become a
  pointer). Words in the data that point a few instructions into an asm function are code
  entries. Splat merged that function with the tail of its neighbour, which happens in the
  vtables of the C++ runtime stubs at `0x2617C0–0x262760`. The rule: aligned, right after the
  alignment `nop`, or after a return/jump and opening a stack frame. `recomp.data_entries` finds
  them (50), and each one is recompiled as a function of its own.

## Kernel, threads, IOP

- `kernel.c`: PS2 threads are fibers (then Windows fibers; now `port/host64/fiber.c`). One runs at a time, and switches happen where the
  PS2 kernel makes them: blocking, a higher priority thread becoming ready, an interrupt
  handler. Interrupts (VBLANK, DMAC) are delivered at kernel entries. `boot.c` does crt0's work
  and calls `main`.
- `sif.c`: SIF RPC. A bind finds a native server, and a call runs it at once and fills the
  receive buffer as the IOP reply would.
- `cdvd.c`: cdvdfsv's servers read the user's own disc image (`DHD_ISO`). `sceCdSearchFile` gets a
  300-byte packet: the 36-byte `sceCdlFILE` (`lsn, size, name[16], date[8], flag`) followed by
  the path at `0x24`. The reply is copied from the packet's start.
- `iop.c`:
  - The SDK's file calls (`sceOpen/Close/Lseek/Read/Write`, `func_0025DA60…0025E300`) are native.
    `cdrom0:` files go to the disc image; fd 1/2 go to stderr.
  - The game's sound driver (server `0x123456`) receives commands batched in a 4 KB buffer (fno
    `0x4D`): word 0 is the count, then `{short cmd, short len, data padded to 4}`. The reply is
    one result word per command, framed by `0xFFFFFFFF`.
  - Command `0x38` is a disc read `{lsn, sectors, EE address}`. It clears the busy word
    `D_004CD380[0]` and puts the error in `[4]`. The game loads most files through it.
  - Other sound commands were acknowledged then (no audio yet; now `sound.c`, finding 022).
- The game's debug prints (empty functions in retail) go to the log with `DHD_LOG=1`
  (`patches.py DEBUG_PRINT`, `host_safe_vprint` checks each `%s` pointer).

## DMA, VIF, VU0

- `dma.c`: at each kernel entry, channels with `CHCR.STR` set are run and ended (STR cleared,
  then the DMAC interrupt). Scratchpad channels copy. VIF0/VIF1 transfers are walked in normal or
  source-chain mode (cnt/next/ref/refs/refe/call/ret/end, TTE sends each tag's upper half), and
  the words go to `vif.c`. GIF transfers were dropped until the renderer existed (now PATH3 to
  `port/gs`, finding 021).
  `sceGsSyncPath` (`func_002433A8`) is native.
- `vif.c`: VIF codes. MPG loads micro memory. UNPACK fills VU data memory, with the CL/WL
  cycles, the mask, the row/column registers and the offset/difference modes. MSCAL/MSCNT start
  a VU0 program. The game uploads its VU0 microcode this way: the chain at `0x26CF80` (finding
  019).
- `vu0_interp.c`: VU0 micro mode for every `vcallms` entry with no native version. The game uses
  16 entries, but only 3 are native (finding 016); 13 more are used by asm-only functions such as
  the scene walk. Arithmetic is host float, like the macro-mode runtime. Timing is modelled where
  results depend on it:
  - a vf register written by an FMAC or a load is ready 4 cycles after issue, and readers stall;
  - MAC, status and clip flags reach flag reads 4 cycles after issue, counting stalls;
  - Q is written when the divider finishes (7, or 13 for RSQRT).
  On the retail microcode, the interpreter matches the three native programs bit for bit (6,000
  random cases, `test_runtime.py` with `DHD_VUTEXT=assets/vutext.bin`), including 0x0c0, whose
  result depends on the flag timing.
- VU0 control registers vi16–31 (`cfc2`/`ctc2`) are kept in `ps2.vu0_ctrl`. Status, MAC and clip
  are written back after each microprogram.

## C++ virtual calls (gcc 2.95)

g++ 2.95 puts the vtable pointer after a class's data members, not at offset 0. Each vtable slot
`k+1` is `{short delta, short index, void *fn}`. Clang would lay classes out its own way, so
`transform.py gcc_virtuals` rewrites each `virtual` method as an inline dispatcher through
`ps2_vptr` (`port/include/port.h` `PS2_VCALL`) and appends `void *ps2_vptr;` as the last member.

- The game's 125 C++ units have 126 polymorphic classes, none with a base class.
- They contain no constructors and no virtual function definitions.
- The vptr offset is checked against the asm for 116 classes.

## Calls that pass values only in registers

Matching C can rely on MIPS register state that a C call doesn't express. On the EE the code
works; on the PC the value is garbage. `port/tools/passthrough.py` lists three kinds:
1. A call with no arguments to a function that takes some: the callee reads the caller's
   `$a0..$a3`. These are written out in `src` (56 sites, still matching).
2. A `void` function whose result a caller uses: `$v0` still holds its last call's result.
   These became real `return` statements (26 units, still matching). Two units that gcc 2.95
   schedules differently with a `return` get PC-only patches (`patches.py`).
3. A call with fewer arguments than the definition takes. A wrapper such as
   `func_0018E6B8(obj)` forwards `$a1/$a2` to `func_0018D898` untouched. The checker counts
   126 such sites. In 24 of them the callee never reads the missing registers; for the rest, the
   callee's asm decides.

## Other host details

- C++ is built with `-fno-strict-return` and C with `-Wno-return-mismatch`. gcc 2.95 accepted
  functions that fall off their end and bare `return;` in non-void functions, and clang would
  otherwise turn the first into a trap.
- EE `long` is 64-bit: `transform.py` turns `long` into `long long` and `L`/`UL` literals into
  `LL`/`ULL`.
- Variadic functions recompiled from asm take `...` and copy the variable arguments into the
  remaining argument registers and stack slots.

## Running

```
DHD_ISO=<your USA disc image> build/port/link/dhd.exe
```

Optional environment variables:
- `DHD_LOG=1`: system and game log.
- `DHD_DUMP=N`: thread state after N seconds.
- `DHD_PEEK=addr,len`: memory, printed with that dump and after a crash.

State on 2026-10-13:
- The game boots, loads the loading-screen resources (`MCSTART.NGP/RTX/TEX`), relocates them,
  and runs its first scene setup with VU0.
- It keeps running its first screen (no crash in 28 s); kind-3 register passing is fixed in `src`
  (58 sites, still matching).
- Controllers: `pad.c` (padman servers, pad area refreshed each vsync; then XInput or the
  keyboard, now SDL3 and the keyboard, finding 059).
- Not written yet then: memory card, multitap, sound, and VU1/GS (the renderer). All exist now.
