/* Shared allocator/optimizer invariant. Never use the adapter with another
   allocator unless it enforces this same committed minimum at instantiation. */
#ifndef DHD_HOST64_MEMORY_LAYOUT_H
#define DHD_HOST64_MEMORY_LAYOUT_H
#define HOST64_MIN_MEMORY_SIZE 0x70010000ull
/* The module's own memory (wasm-ld --global-base: its shadow stack, data, bss
   and heap) starts here, above the scratchpad. Only this range, the RAM views,
   and what the module or host commits explicitly (host_mem_commit: scratchpad
   and hardware windows; EE stack slots; the bridge arena) is committed. */
#define HOST64_MODULE_BASE 0x70100000ull
/* wasm-ld --stack-first places the shadow stack at [0, __stack_pointer) even
   with --global-base. host64_memory_commit_stack commits it above a reserved
   null/overflow page; it must end below the low RAM view (0x00400000). */
#define HOST64_NULL_GUARD HOST64_PAGE
/* The guest's page on every host: the largest OS page the port runs on (16 KiB: Apple silicon
   and some Android devices; 4 KiB elsewhere). Guest commits round to it and guard pages (the
   null page, each EE stack slot's lowest page) are this size, so what is committed and what
   faults is the same on every platform. */
#define HOST64_PAGE 0x4000u
/* iOS, wasm2c's bounds-checked layout (finding 258): the guest's memory is two parts at one base,
   [0, 0x3a000000) and [HOST64_HIGH_BASE, end), end at least HOST64_HIGH_END_MIN (the module's
   first 31 MiB above the scratchpad and the 64 KiB window above them); guest_memory.c places
   them, memory_access.h keeps accesses out of what lies between and above. Every layout switch
   tests HOST64_TWO_PARTS, which follows the memory-check mode; host64_selftest.py sets it to 0 to
   run its bounds-checked module on the 8 GiB layout its address lists describe (finding 262). */
#ifndef HOST64_TWO_PARTS
#if WASM_RT_MEMCHECK_BOUNDS_CHECK
#define HOST64_TWO_PARTS 1
#else
#define HOST64_TWO_PARTS 0
#endif
#endif
#define HOST64_HIGH_BASE 0x70000000ull
#define HOST64_HIGH_END_MIN 0x72010000ull
#endif
