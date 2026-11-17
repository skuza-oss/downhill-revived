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
   wasm32's limit.
   iOS (the bounds-checked layout) has no room for that: see below. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "memory_layout.h"
#include "guest_memory_os.h"

uintptr_t guest_base;
static wasm_rt_memory_t *owner;
#if HOST64_TWO_PARTS
/* iOS without the extended-virtual-addressing entitlement (finding 224): wasm2c's bounds-checked
   layout, memory at most 2 GiB. iOS caps such an app's address space, and on a device with 3 GB or
   less there is no 2 GiB hole at all (finding 258): the system's shared cache takes
   [0x180000000, 0x300000000) and the app's space ends at 0x358000000. So the guest is reserved in
   two parts (guest_place.h): [0, 0x3a000000), the RAM views and everything below them, at
   guest_base, and [0x70000000, limit), the scratchpad, the HLE window and the module's own memory,
   with a 64 KiB window above, at a host address of its own (finding 267: on the iPhone XR the two
   rarely fit 1792 MiB apart): guest address a of the high part is at host64_high_base + a. Region
   GAP, [0x3a000000, 0x70000000), is not the guest's: nothing is ever reserved for it. The parts are
   placed at the process's start (reserve_early), before the launcher, WebKit and Metal take the low
   addresses; limit is as high as the free ranges allow (at most the 2 GiB maximum) and the memory
   grows up to it. memory_access.h's adapters, GUEST() and the bridge pick the part's base by the
   address and end the process for an access outside both parts (host64_memory_wild). */
#include "guest_place.h"
#define GUEST_MAX_PAGES 32768u
#define GAP 12u
#define WINDOW 0x10000ull
static uint64_t edges[] = {0,0x00400000,0x02000000,0x08000000,0x0a000000,
    0x20400000,0x22000000,0x28000000,0x2a000000,0x30400000,0x32000000,
    0x38000000,0x3a000000,HOST64_HIGH_BASE,0x80000000ull,0x80000000ull+WINDOW};
uint64_t host64_guest_end = UINT64_MAX;   /* the high part's end once reserved (memory_access.h) */
uint8_t *host64_high_base;    /* host address of guest 0 for the high part: its host start - HOST64_HIGH_BASE */
/* The host address of guest address at (the part it lies in decides the base). */
static uintptr_t host_of(uint64_t at) { return (at>=HOST64_HIGH_BASE ? (uintptr_t)host64_high_base : guest_base)+at; }
#else
static uintptr_t host_of(uint64_t at) { return guest_base+at; }
#define GUEST_MAX_PAGES 65536u
#define GAP (~0u)
static const uint64_t edges[] = {0,0x00400000,0x02000000,0x08000000,0x0a000000,
    0x20400000,0x22000000,0x28000000,0x2a000000,0x30400000,0x32000000,
    0x38000000,0x3a000000,0x100000000ull,0x200000000ull};
#endif
enum { REGIONS=sizeof edges/sizeof edges[0]-1 };
static int mirror(unsigned i) { return i==1 || i==3 || i==5 || i==7 || i==9 || i==11; }
static int absent(unsigned i) { return i==GAP; }  /* never reserved (the bounds-checked layout's gap) */
static uint64_t section_offset(unsigned i) { return edges[i]&0x01ffffff; }
static int commit_range(uint64_t lo, uint64_t hi) {
    for (unsigned i=0;i<REGIONS-1;i++) {
        uint64_t a=lo>edges[i]?lo:edges[i], b=hi<edges[i+1]?hi:edges[i+1];
        if (a<b && absent(i)) return 0;
        if (!mirror(i) && a<b && !os_commit(host_of(a),b-a)) return 0;
    }
    return 1;
}
/* Whether a host address is in the guest's reservation (the fault reporter's test; plain
   arithmetic, safe in a signal handler). */
int host64_memory_owns(uintptr_t host) {
    uint64_t at;
    return host64_memory_guest(host,&at);
}
/* The guest address of a host address in the guest's reservation (the fault reporter's; plain
   arithmetic, safe in a signal handler): 1 with *at, 0 when the reservation does not hold it. */
int host64_memory_guest(uintptr_t host,uint64_t *at) {
    if (!guest_base) return 0;
#if HOST64_TWO_PARTS
    uintptr_t high=(uintptr_t)host64_high_base;
    if (host-(high+edges[GAP+1])<edges[REGIONS]-edges[GAP+1]) { *at=host-high; return 1; }
    if (host-guest_base<edges[GAP]) { *at=host-guest_base; return 1; }
    return 0;
#else
    if (host<guest_base || host-guest_base>=edges[REGIONS]) return 0;
    *at=host-guest_base;
    return 1;
#endif
}
/* Whether guest [at, at + size) lies in the guest's reservation, inside the memory's size. */
int host64_memory_reserved(uint64_t at,uint64_t size) {
    if (!owner || at+size>owner->size || at+size<at) return 0;
    for (unsigned i=0;i<REGIONS;i++)
        if (absent(i) && at<edges[i+1] && at+size>edges[i]) return 0;
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
#if HOST64_TWO_PARTS
/* The core's initial memory ends at 0x70E00000 (build_wasm.py's INITIAL_MEMORY): every
   reservation's high part reaches HOST64_HIGH_END_MIN at least, which memory_access.h's plain
   window 56 needs. A module that needs more (the self-test's) is placed again at its
   instantiation. */
#define EARLY_NEED HOST64_HIGH_END_MIN
static uintptr_t early_base, early_high;
static uint64_t early_end;
static int early_errno;
/* Reserves the two parts where the free ranges hold them, the high part ending at need to the 2 GiB
   maximum plus the window. 1 with *base (the low part's host address), *high (the high part's: guest
   HOST64_HIGH_BASE) and *end. On failure with say set, dhd.log gets what was asked and what is free. */
static int place(uint64_t need,uintptr_t *base,uintptr_t *high,uint64_t *end,int say) {
    GuestPieces p={edges[GAP],edges[GAP+1],need,0x80000000ull+WINDOW,0x10000};
    for (int attempt=0;attempt<8;attempt++) {  /* another thread may map between the list and the reservation */
        GuestRange *free_ranges=NULL;
        unsigned n=os_free_ranges(&free_ranges);
        uint64_t b=0, h=0, e=0;
        if (!guest_place(free_ranges,n,&p,&b,&h,&e)) {
            if (say) {
                GuestRange big=guest_largest_free(free_ranges,n);
                fprintf(stderr,"host64 memory: no room for the guest's memory: it needs %llu MiB free at one "
                        "address and %llu MiB more anywhere (findings 258, 267); the largest free range is "
                        "%llu MiB at 0x%llx (%u free ranges)\n",(unsigned long long)(p.low>>20),
                        (unsigned long long)((need-p.high+0xfffff)>>20),(unsigned long long)((big.hi-big.lo)>>20),
                        (unsigned long long)big.lo,n);
                for (unsigned i=0;i<n;i++)  /* the address space's shape, for the next fix */
                    if (free_ranges[i].hi-free_ranges[i].lo>=0x1000000)
                        fprintf(stderr,"host64 memory: free 0x%llx-0x%llx (%llu MiB)\n",
                                (unsigned long long)free_ranges[i].lo,(unsigned long long)free_ranges[i].hi,
                                (unsigned long long)((free_ranges[i].hi-free_ranges[i].lo)>>20));
            }
            free(free_ranges);
            errno=ENOMEM;
            return 0;
        }
        free(free_ranges);
        if (os_reserve_at((uintptr_t)b,p.low)) {
            if (os_reserve_at((uintptr_t)h,e-p.high)) {
                *base=(uintptr_t)b; *high=(uintptr_t)h; *end=e;
                return 1;
            }
            os_release((void *)(uintptr_t)b,p.low);
        }
    }
    if (say) fprintf(stderr,"host64 memory: the guest's memory was placed but taken before its reservation, 8 times\n");
    errno=ENOMEM;
    return 0;
}
/* At the process's start (the launcher and the game are one process on iOS): the addresses below
   the shared cache are still free then. Kept for the game; a run that never plays loses only
   address space, no memory. */
__attribute__((constructor)) static void reserve_early(void) {
    if (!place(EARLY_NEED,&early_base,&early_high,&early_end,0)) { early_base=0; early_errno=errno; }
}
static uintptr_t reserve(uint64_t initial) {
    uint64_t need=initial*65536+WINDOW, end=early_end;
    if (need<HOST64_HIGH_END_MIN) need=HOST64_HIGH_END_MIN;
    uintptr_t base=early_base, high=early_high;
    const char *when="at the start";
    early_base=0;
    if (base && end<need) {  /* too small for this module: placed again */
        os_release((void *)base,edges[GAP]);
        os_release((void *)high,end-edges[GAP+1]);
        base=0;
    }
    if (!base) {
        if (early_errno) fprintf(stderr,"host64 memory: the start's reservation failed (%d: %s); trying now\n",
                                 early_errno,strerror(early_errno));
        when="now";
        if (!place(need,&base,&high,&end,1)) {
            void host_error_box(const char *,const char *) __attribute__((weak));
            if (host_error_box)
                host_error_box("Downhill Revived","The game can't get the memory it needs on this device "
                               "(iOS gives the app too little address space). Please share the log "
                               "(launcher > Help > Share log file) with us so we can fix it.");
            errno=ENOMEM;
            return 0;
        }
    }
    if (!os_backing_open()) os_fail("guest RAM backing");
    edges[REGIONS-1]=end-WINDOW; edges[REGIONS]=end; host64_guest_end=end;
    host64_high_base=(uint8_t *)(high-edges[GAP+1]);
    fprintf(stderr,"host64 memory: guest memory at 0x%llx, reserved %s: [0, 0x%llx) there and [0x%llx, 0x%llx) "
            "at 0x%llx, room to grow to %llu MiB\n",(unsigned long long)base,when,(unsigned long long)edges[GAP],
            (unsigned long long)edges[GAP+1],(unsigned long long)end,(unsigned long long)high,
            (unsigned long long)((end-WINDOW)>>20));
    return base;
}
#else
static uintptr_t reserve(uint64_t initial) { (void)initial; return os_reserve(); }
#endif
void wasm_rt_allocate_memory(wasm_rt_memory_t *m,uint64_t initial,uint64_t max,
                             bool is64,uint32_t page_size) {
    if (owner || is64 || page_size!=65536 || initial<HOST64_MIN_MEMORY_SIZE/65536 ||
        initial>max || max>GUEST_MAX_PAGES || initial*65536<=HOST64_MODULE_BASE)
        os_fail("unsupported module memory layout");
    guest_base=reserve(initial);
    if (!guest_base) os_fail("reserve guest memory");
    for (unsigned i=0;i<REGIONS-1;i++)
        if (!absent(i) && !os_split(host_of(edges[i]),edges[i+1]-edges[i])) os_fail("split reservation");
    for (unsigned i=0;i<REGIONS-1;i++) {
        if (absent(i)) continue;
        if (mirror(i)) {
            if (!os_map_ram(host_of(edges[i]),edges[i+1]-edges[i],section_offset(i)))
                os_fail("RAM mirror");
        } else if (!os_private(host_of(edges[i]),edges[i+1]-edges[i]))
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
    if (end>edges[REGIONS-1]) return UINT32_MAX;  /* beyond the reservation (the bounds-checked layout's limit) */
    if (!commit_range(m->size,end)) {
        /* Growth starts beyond all mirror windows. Roll back partial commits. */
        os_decommit((void *)host_of(m->size),end-m->size);
        return UINT32_MAX;
    }
    m->pages+=pages; m->size=end; m->data_end=m->data+end; return old;
}
void wasm_rt_free_memory(wasm_rt_memory_t *m) {
    if (m!=owner) os_fail("free of non-owner");
    for (unsigned i=0;i<REGIONS;i++) {
        void *p=(void *)host_of(edges[i]);
        if (absent(i)) continue;
        if (mirror(i)) {
            if (!os_unmap_view(p,edges[i+1]-edges[i])) os_fail("unmap RAM");
        } else if (!os_release(p,edges[i+1]-edges[i])) os_fail("release reservation");
    }
    os_backing_close();
    guest_base=0; owner=NULL;
#if HOST64_TWO_PARTS
    host64_guest_end=UINT64_MAX; host64_high_base=NULL;
#endif
    os_fault_remove();
    m->data=m->data_end=NULL; m->size=m->pages=0;
}
