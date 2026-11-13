/* Guest linear memory: one 8 GiB reservation with aliased RAM views. The
   policy here (views, commit-on-demand, fault-on-uncommitted) is identical on
   every host; the OS mapping lives in guest_memory_win32.c (Windows 10 1803+)
   or guest_memory_posix.c (Linux and other POSIX hosts), behind
   guest_memory_os.h. do NOT link wasm-rt-mem-impl.c.
   Six RAM views share one section (three complete views and three low aliases
   starting at section offset 4 MiB). Of the rest, only the module's own memory
   (HOST64_MODULE_BASE up to its size) is committed here; the scratchpad and
   hardware windows (host_mem_commit), the EE stack slots and the bridge arena
   are committed by their owners. Everything else stays reserved, and an access
   to it ends the process with the guest address (the OS fault reporter), never
   a silent zero. [4 GiB,8 GiB) stays inaccessible even if memory grows to
   wasm32's limit. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "memory_layout.h"
#include "guest_memory_os.h"

uintptr_t guest_base;
static wasm_rt_memory_t *owner;
#if WASM_RT_MEMCHECK_BOUNDS_CHECK
/* iOS without the extended-virtual-addressing entitlement: wasm2c checks bounds
   at each access, so the reservation covers the 2 GiB maximum memory plus one
   64 KiB guard window. */
#define GUEST_MAX_PAGES 32768u
static const uint64_t edges[] = {0,0x00400000,0x02000000,0x08000000,0x0a000000,
    0x20400000,0x22000000,0x28000000,0x2a000000,0x30400000,0x32000000,
    0x38000000,0x3a000000,0x80000000ull,0x80010000ull};
#else
#define GUEST_MAX_PAGES 65536u
static const uint64_t edges[] = {0,0x00400000,0x02000000,0x08000000,0x0a000000,
    0x20400000,0x22000000,0x28000000,0x2a000000,0x30400000,0x32000000,
    0x38000000,0x3a000000,0x100000000ull,0x200000000ull};
#endif
enum { REGIONS=sizeof edges/sizeof edges[0]-1 };
static int mirror(unsigned i) { return i==1 || i==3 || i==5 || i==7 || i==9 || i==11; }
static uint64_t section_offset(unsigned i) { return edges[i]&0x01ffffff; }
static int commit_range(uint64_t lo, uint64_t hi) {
    for (unsigned i=0;i<REGIONS-1;i++) {
        uint64_t a=lo>edges[i]?lo:edges[i], b=hi<edges[i+1]?hi:edges[i+1];
        if (!mirror(i) && a<b && !os_commit(guest_base+a,b-a)) return 0;
    }
    return 1;
}
/* Commits [at, at + size) outside the RAM views, which are always mapped.
   1 on success; 0 beyond the memory's current size. */
int host64_memory_commit(uint64_t at,uint64_t size) {
    if (!owner || at+size>owner->size || at+size<at) return 0;
    uint64_t lo=at&~(uint64_t)(HOST64_PAGE-1), hi=(at+size+HOST64_PAGE-1)&~(uint64_t)(HOST64_PAGE-1);
    return commit_range(lo,hi);
}
int host64_memory_commit_stack(uint32_t sp) {
    if (sp>HOST64_MODULE_BASE) return 1;  /* inside the module's committed memory */
    return sp>HOST64_NULL_GUARD && sp<=0x00400000 &&
        host64_memory_commit(HOST64_NULL_GUARD,sp-HOST64_NULL_GUARD);
}
void wasm_rt_allocate_memory(wasm_rt_memory_t *m,uint64_t initial,uint64_t max,
                             bool is64,uint32_t page_size) {
    if (owner || is64 || page_size!=65536 || initial<HOST64_MIN_MEMORY_SIZE/65536 ||
        initial>max || max>GUEST_MAX_PAGES || initial*65536<=HOST64_MODULE_BASE)
        os_fail("unsupported module memory layout");
    guest_base=os_reserve();
    if (!guest_base) os_fail("reserve guest memory");
    for (unsigned i=0;i<REGIONS-1;i++)
        if (!os_split(guest_base+edges[i],edges[i+1]-edges[i])) os_fail("split reservation");
    for (unsigned i=0;i<REGIONS-1;i++) {
        if (mirror(i)) {
            if (!os_map_ram(guest_base+edges[i],edges[i+1]-edges[i],section_offset(i)))
                os_fail("RAM mirror");
        } else if (!os_private(guest_base+edges[i],edges[i+1]-edges[i]))
            os_fail("private reservation");
    }
    if (!commit_range(HOST64_MODULE_BASE,initial*65536)) os_fail("module commit");
    if (!os_fault_install()) os_fail("fault reporter");
    m->data=(uint8_t *)guest_base; m->size=initial*65536; m->data_end=m->data+m->size;
    m->pages=initial; m->max_pages=max; m->is64=false; m->page_size=page_size; owner=m;
}
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *m,uint64_t pages) {
    if (m!=owner || pages>m->max_pages-m->pages) return UINT32_MAX;
    uint64_t old=m->pages, end=(old+pages)*65536;
    if (!commit_range(m->size,end)) {
        /* Growth starts beyond all mirror windows. Roll back partial commits. */
        os_decommit(m->data+m->size,end-m->size);
        return UINT32_MAX;
    }
    m->pages+=pages; m->size=end; m->data_end=m->data+end; return old;
}
void wasm_rt_free_memory(wasm_rt_memory_t *m) {
    if (m!=owner) os_fail("free of non-owner");
    for (unsigned i=0;i<REGIONS;i++) {
        void *p=(void *)(guest_base+edges[i]);
        if (mirror(i)) {
            if (!os_unmap_view(p,edges[i+1]-edges[i])) os_fail("unmap RAM");
        } else if (!os_release(p,edges[i+1]-edges[i])) os_fail("release reservation");
    }
    os_backing_close();
    guest_base=0; owner=NULL;
    os_fault_remove();
    m->data=m->data_end=NULL; m->size=m->pages=0;
}
