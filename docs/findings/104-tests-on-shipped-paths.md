# 104: test programs on the shipped paths

Status: current
Checked: 2026-11-04

2026-10-28. The port's test programs used to compile for 32-bit Windows (pctarget.py's
i686-pc-windows-itanium, build.py's objects, clang-cl -m32), years after players stopped running
that build (finding 088). They now run their code where the game runs it.

## How

`port/tools/testprog.py` is the one place with the compiler policy:

- `guest(name, sources)`: C that runs inside the core. Compiled for wasm32 with build_wasm.py's
  flags (SIMDe for the x86 intrinsic headers), linked with wasi-libc and the core's own
  `runtime/wasi_adapter.c`, translated by wasm2c and built for the 64-bit host with
  `port/host64/test_guest.c`. The C library reaches the host as in the core: the adapter turns
  WASI calls into host_api.h services (console, files, environment, clock, exit), which
  test_guest.c gives, with the float control word and the program's arguments. A trap exits 134.
  `memory=` sizes the guest for tests that use the core's fixed addresses (PS2 RAM at
  0x08000000, the registers at 0x10000000).
- `game_unit(rel, out)`: one src/ unit compiled as build_wasm.py compiles it (ABI fixes, port
  patches, transform.py, the EE float IR rewrites).
- `host(name, sources)`: host code (port/host, port/gs, the host side of port/hle) for x86_64
  with the 64-bit build's float flags. `low=True` is for programs that mix guest units with host
  ones and pass host pointers as guest addresses: they link below 2 GiB with guest_base 0
  (`port/host64/test_low.c`), Windows only.
- `run(exe)`: runs a program, raising unless it exits as expected (negative controls pass
  `expect=`).

`abi_check.py` audits the calls as the wasm32 core compiles them (wasi-sdk's clang). Its report is
identical to the i686 one: 3815 units, 19477 calls, 475 mismatches, 82 reviewed, 226 gameplay, 0
sites to reorder.

## Expected values

Values that came from an i686 run were derived again from an independent source:

- test_inflate_status: the retail asm returns the decoder's $v0 for block types 0-2 and 2 for the
  invalid type 3 (the delay slot at 00241F50). i686 had left the type itself (3) in EAX and
  port/patches.py copied that; the patch now returns 2, as the PS2 does.
- test_wasm_postlink, test_wasm_retail_names: every word comes from the probe's C and
  wasm_postlink.py's adapter rules, written out.
- test_data: the converted data and fixups as data.convert writes them for the core (no label
  prefix, wasm32's `.long` pointers).
- test_ps2asm: the native VU0 microprograms against the interpreter running the retail
  microcode (6000 cases, bit for bit), now inside the guest.
- The float, rider, NPC, particle and packet tests already compared with the EE's values
  (ps2float.h after PCSX2's FPU, the retail layouts): unchanged.

## Bugs the move found

- `wasi_adapter.c`: a guest `fflush` stopped at the host's FILE buffer, because the adapter never
  called host_file_flush. A file the game wrote could not be read while it ran (sound_debug.h's
  WAV header, published every tick for killed runs). fd_write now flushes; on i686 the C
  library's buffer was the only one too.
- sound_debug.h published DHD_SND_REPORT through a temporary file and rename. The core's files
  have no rename (wasi_adapter.c returns ENOTSUP), so the report never appeared. It is now
  rewritten in place; a reader can see it half written.
- port/patches.py's inflate fix (above).
- Stale tests on desk/integrate, fixed: test_data (data.convert's fixups), test_sif (the multitap
  RPC servers), test_game_options (the host_service_* settings calls), test_gamepad (SDL on the
  main thread, sdl_host_main), test_vif (host_service_graphics_wait), test_text_input
  (host_pacing_period), test_shared_scalars, test_defined_producers, test_fps.
- hle.h's RETAIL gave the i686 underscore and build_wasm.py rewrote it for the core; the header
  now says what the core uses.

## Retired

- test_npc_float_return: the x87 stack balance after discarded float returns. The core has no
  x87; a mismatched return type traps instead (test_wasm_postlink, test_virtual_scale).
- build.py (the i686 objects of the game units) and pctarget.py (the i686 target values).
- The x86 FP-trap setup (unmasked MXCSR exceptions) of the float tests: wasm has no float traps.
  The exact value checks still reject an Inf/NaN where the EE saturates.
- The i686 ISA, ThinLTO and PGO variants of the VU1 PCSX2 comparison and of test_huft.

## Lost coverage and open points

- The ABI fixture program (test_abi.prepare's) is not run: fix_abi_calls repairs a call by
  casting the callee to its definition's type, and wasm32 turns a direct call through such a cast
  into a trapping stub (wasm-ld's signature_mismatch). port/abi_calls.json has no sites today; a
  site added there would trap in the core. The audit and the rewrites are still checked as text.
- test_ps2asm_fast runs the game's two float modes (to nearest, toward zero, each with and
  without FTZ/DAZ). The round up and down modes the i686 test also set mean nothing to
  wasm2c's code: LLVM's wasm back end assumes round to nearest (ps2_vu_madd gives inf under them).
- test_integer_abi no longer checks `sizeof(long double)`: it is 16 in wasm32 and 8 on the EE,
  and no game unit uses long double.
- The VU0 PCSX2 oracles run on the x64 host, not in the guest (the guest runs the same C through
  SIMDe). VU0 captures hold the core's 32-bit ps2_cpu; vu0_capture.h reads that layout on x64.
- Host test programs with `low=True` are Windows only.
- Some x64 test scripts still call clang-cl themselves (net/build_tests.py, host64_selftest.py,
  host64_dispatch_test.py, harness_negative.py, test_native_vertex.py,
  test_retired_snapshot.py, test_gs_wgpu.py).
- build_wasm.py still assembles the data image as i686 COFF.
