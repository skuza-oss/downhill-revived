/* Native wasm2c runtime. One module and one cooperative EE scheduler per OS thread. */
#ifndef HOST64_CORE_HOST_H
#define HOST64_CORE_HOST_H
#include <stdint.h>
#include "../host/host_api.h"
extern uintptr_t guest_base;
/* Bind the exported mutable __stack_pointer before creating any EE fibers. */
void host64_fiber_bind(uint32_t *stack_pointer);
void host64_fiber_shutdown(void);
uint32_t host64_fiber_stack_top(HostFiber fiber);
int host64_fiber_id(void);  /* the running EE fiber, unchecked (diagnostics) */
/* Commits guest [at, at + size) (guest_memory.c); everything else stays reserved. */
int host64_memory_commit(uint64_t at, uint64_t size);
/* Whether guest [at, at + size) is inside the memory's size and its reservation. */
int host64_memory_reserved(uint64_t at, uint64_t size);
/* Whether a host address lies in the guest's reservation; *at its guest address (either part's
   base on iOS, finding 267). Plain arithmetic, safe in a signal handler. */
int host64_memory_owns(uintptr_t host);
int host64_memory_guest(uintptr_t host, uint64_t *at);
/* iOS (finding 258): ends the process as a fault of the guest at address does (guest_memory_posix.c). */
void host64_memory_wild(uint64_t address) __attribute__((noreturn, cold));
/* After instantiation: the root shadow stack below the initial stack pointer,
   when the module keeps it below HOST64_MODULE_BASE. 1 on success. */
int host64_memory_commit_stack(uint32_t stack_pointer);
#endif
