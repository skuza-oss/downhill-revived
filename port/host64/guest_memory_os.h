/* OS backend for guest_memory.c (shared policy). Each function is small and
   policy-free: which regions are RAM mirrors and what stays committed is
   decided by the shared file. Backends: guest_memory_win32.c (Windows 10
   1803+), guest_memory_posix.c (Linux and other POSIX hosts). */
#ifndef DHD_HOST64_GUEST_MEMORY_OS_H
#define DHD_HOST64_GUEST_MEMORY_OS_H
#include <stdint.h>
#include "memory_layout.h"

#if HOST64_TWO_PARTS
#include "guest_place.h"
/* The bounds-checked layout (iOS, finding 258; POSIX only): guest_memory.c places the guest's two
   parts itself. The process's free address ranges, in a malloc'd array (*out; the caller frees
   it); their count. */
unsigned os_free_ranges(GuestRange **out);
/* Reserves [at, at + size) inaccessible, exactly there and only if it is free (never over another
   mapping). Nonzero on success. */
int os_reserve_at(uintptr_t at, uint64_t size);
/* Readies the 32 MiB RAM backing store. Nonzero on success. */
int os_backing_open(void);
#else
/* Reserves 8 GiB of inaccessible address space and readies the 32 MiB RAM
   backing store. Returns the base, or 0 on failure. */
uintptr_t os_reserve(void);
#endif
/* Prepares [at, at + size) for mapping (Windows placeholders; no-op on POSIX).
   Returns nonzero on success. */
int os_split(uintptr_t at, uint64_t size);
/* Aliases the RAM backing store at [file_offset, file_offset + size). */
int os_map_ram(uintptr_t at, uint64_t size, uint64_t file_offset);
/* Marks [at, at + size) as a private inaccessible reservation. */
int os_private(uintptr_t at, uint64_t size);
/* Commits [at, at + size) read/write, preserving existing contents. */
int os_commit(uintptr_t at, uint64_t size);
/* Returns [at, at + size) to the reserved state (contents lost). */
void os_decommit(const void *at, uint64_t size);
/* Removes one mapping installed by os_map_ram / os_private. */
int os_unmap_view(void *at, uint64_t size);
int os_release(void *at, uint64_t size);
/* Closes the RAM backing store. */
void os_backing_close(void);
/* Installs the report-and-exit fault reporter; nonzero on success. */
int os_fault_install(void);
void os_fault_remove(void);
/* From the shared file: whether a host address is in the guest's reservation (the fault reporter's
   test, safe in a signal handler). */
int host64_memory_owns(uintptr_t host);
/* Logs what (with the OS error) and aborts. */
void os_fail(const char *what);
#endif
