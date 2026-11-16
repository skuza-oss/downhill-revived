/* Native memory operations used by the generated boundary. The bridge arena
   follows the 64 EE stacks and precedes RAM; module-owned data lives above VU. */
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "core_host.h"
#include "memory_layout.h"  /* HOST64_TWO_PARTS */
#include "../include/guest.h"
static wasm_rt_memory_t *memory;
static uint32_t next;
void host64_bridge_bind(wasm_rt_memory_t *m) { memory=m; next=0x07000000; }
void *host64_bridge_pointer(uint32_t at,uint32_t bytes) {
    if (!memory || (!at && bytes) || (uint64_t)at+bytes>memory->size) wasm_rt_trap(WASM_RT_TRAP_OOB);
#if HOST64_TWO_PARTS
    /* iOS: the gap between the guest's two parts is not the guest's (guest_memory.c, finding 258) */
    if (bytes && (uint64_t)at+bytes>0x3a000000u && at<0x70000000u) wasm_rt_trap(WASM_RT_TRAP_OOB);
#endif
    return at ? GUEST(at) : NULL;
}
const char *host64_bridge_string(uint32_t at) {
    const char *s=host64_bridge_pointer(at,1);
    if (!memchr(s,0,(size_t)(memory->size-at))) wasm_rt_trap(WASM_RT_TRAP_OOB);
    return s;
}
uint32_t host64_bridge_array_bytes(int32_t count,uint32_t stride) {
    if (count<0 || (uint64_t)count*stride>UINT32_MAX) wasm_rt_trap(WASM_RT_TRAP_OOB);
    return (uint32_t)count*stride;
}
uint32_t host64_bridge_pcm_bytes(int32_t frames) {
    return host64_bridge_array_bytes(frames,4);
}
uint32_t host64_bridge_alloc(uint32_t bytes,uint32_t align) {
    uint64_t at=((uint64_t)next+align-1)&~((uint64_t)align-1);
    if (at+bytes>0x07800000) wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
    if (bytes && !host64_memory_commit(at,bytes)) wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
    next=(uint32_t)(at+bytes); return (uint32_t)at;
}
/* The allocator has already committed linear memory and installed these exact
   aliases before module instantiation. Never release its reservation here. */
int host_ram_create(uint32_t size) { return memory && size==0x02000000; }
int host_ram_view(uint32_t at,uint32_t offset,uint32_t size,int over) {
    (void)over;
    if (!memory) return 0;
    if (!offset && size==0x02000000)
        return at==0x08000000 || at==0x28000000 || at==0x38000000;
    return offset==0x00400000 && size==0x01c00000 &&
        (at==0x00400000 || at==0x20400000 || at==0x30400000);
}
/* The module commits the scratchpad and the hardware windows it uses. */
int host_mem_commit(uint32_t at,uint32_t size) {
    return memory && host64_memory_commit(at,size);
}
/* The allocator reserves the whole space but, on iOS, the gap between its two parts
   (guest_memory.c, finding 258): every other in-range span qualifies. */
int host_mem_reserved(uint32_t at,uint32_t size) {
    return memory && host64_memory_reserved(at,size);
}
/* host.c arms DHD_STATE_WATCH; the i686 definition (state_trace.c) is module code here.
   Same rule: only under DHD_STATE_TRACE, NULL when unset. */
const char *state_trace_watch_spec(void) {
    const char *w=getenv("DHD_STATE_WATCH");
    return getenv("DHD_STATE_TRACE") && w && *w ? w : NULL;
}
