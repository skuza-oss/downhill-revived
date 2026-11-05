/* SDK graphics boundaries that return GS data to the EE. */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "../gs/gs.h"
#include "../runtime/memory.h"

/* Deferred store-image reads (gs_defer_read_image): until the bytes are written, their RAM pages
   are inaccessible in every view of PS2 RAM, so the first access from the game, on any path,
   settles them before it proceeds. Kernel calls on PS2 RAM (file I/O) fail on such pages instead
   of faulting: graphics_settle runs before them. */
enum { MAX_GUARDED = 64 };
static uint32_t guarded[MAX_GUARDED];  /* PS2 RAM offsets of the guarded 4 KB pages */
static unsigned guarded_count;
static int defer = -1;
/* Queued reads (host_service_gs_store_image, finding 148): the read runs on the thread that owns
   the GS, in order with the drawing, and its bytes wait there until the first access settles
   them. sync_reads: reads of the older path (gs_defer_read_image on this thread) are guarded. */
static int queued_reads, sync_reads;

static void protect_views(uint32_t offset, int protection)
{
    static const uint32_t mirrors[3] = {0, 0x20000000, 0x30000000};
    for (unsigned m = 0; m < 3; m++) {
        host_mem_protect(mirrors[m] | (PS2_RAM_BASE + offset), 4096, protection);
        if (offset >= PS2_LOW_VIEW) host_mem_protect(mirrors[m] | offset, 4096, protection);
    }
}

static int guarded_page(uintptr_t address)
{
    uint32_t offset = (uint32_t)address & 0x0FFFFFFF;
    if (offset >= PS2_RAM_BASE) offset -= PS2_RAM_BASE;
    else if (offset < PS2_LOW_VIEW) return 0;
    if (offset >= PS2_RAM_SIZE) return 0;
    for (unsigned i = 0; i < guarded_count; i++)
        if (guarded[i] == (offset & ~4095u)) return 1;
    return 0;
}

void graphics_settle(void)
{
    /* With queued reads in flight the GS owner's deferred state is its own (it settles each read
       at once): only the older path's reads need the whole queue drained here. */
    const int sync = sync_reads || (!queued_reads && host_service_gs_reads_pending());
    if (!guarded_count && !sync && !queued_reads) return;
    if (sync) host_service_graphics_wait();
    for (unsigned i = 0; i < guarded_count; i++) protect_views(guarded[i], HOST_MEM_READWRITE);
    guarded_count = 0;
    if (queued_reads) host_service_gs_store_settle();  /* waits for its marker, writes the bytes */
    queued_reads = 0;
    if (sync) host_service_gs_settle_reads();
    sync_reads = 0;
}

/* An access fault on a guarded page (the host calls this on the game thread): settle, and retry. */
static int guard_fault(uint32_t ctx, uint32_t address)
{
    if (!guarded_page(address)) return 0;
    graphics_settle();
    return 1;
}

static int guard(uint32_t destination, size_t bytes)
{
    uint32_t first = (destination & 0x0FFFFFFF) - PS2_RAM_BASE;
    uint32_t last = first + (uint32_t)bytes - 1;
    if (destination < PS2_RAM_BASE || last >= PS2_RAM_SIZE || last < first) return 0;
    for (uint32_t page = first & ~4095u; page <= last; page += 4096) {
        unsigned i = 0;
        while (i < guarded_count && guarded[i] != page) i++;
        if (i < guarded_count) continue;
        if (guarded_count == MAX_GUARDED) return 0;
        guarded[guarded_count++] = page;
        protect_views(page, HOST_MEM_NOACCESS);
    }
    return 1;
}

static int defer_read(uint32_t destination, size_t bytes)
{
    if (!guard(destination, bytes)) return 0;
    sync_reads = 1;
    return host_service_gs_defer_read_image(destination, bytes) == bytes;
}

/* The bytes a store-image packet reads, from the packet itself: one VIF qword ending in DIRECT,
   then a PACKED A+D GIF tag whose registers set BITBLTBUF, TRXREG and TRXDIR (local to host).
   0 for any other shape: the caller takes the waiting path, which reads the GS registers. */
static size_t store_bytes(uint32_t packet)
{
    const uint32_t *w = GUEST(packet);
    for (unsigned i = 0; i < 3; i++) {
        unsigned cmd = w[i] >> 24 & 0x7F;
        if (cmd > 0x07 && cmd != 0x10 && cmd != 0x11 && cmd != 0x13) return 0;  /* only codes without data */
    }
    if ((w[3] >> 24 & 0x7F) != 0x50 || (w[3] & 0xFFFF) != 6) return 0;  /* DIRECT, 6 qwords */
    const uint32_t *tag = w + 4;
    unsigned nloop = tag[0] & 0x7FFF, flg = tag[1] >> 26 & 3, nreg = tag[1] >> 28;
    if (flg != 0 || nreg != 1 || (tag[2] & 15) != 0xE || !nloop || nloop > 5) return 0;
    uint64_t blt = 0, size = 0, dir = 0;
    unsigned have = 0;
    for (unsigned i = 0; i < nloop; i++) {
        const uint32_t *q = tag + 4 + 4 * i;
        uint64_t data = q[0] | (uint64_t)q[1] << 32;
        switch (q[2] & 0xFF) {
        case GS_BITBLTBUF: blt = data; have |= 1; break;
        case GS_TRXREG: size = data; have |= 2; break;
        case GS_TRXDIR: dir = data; have |= 4; break;
        default: break;
        }
    }
    if (have != 7 || (dir & 3) != 1) return 0;
    unsigned bits = host_service_gs_bits_per_pixel((unsigned)(blt >> 24) & 63);
    return (size_t)(size & 4095) * ((size >> 32) & 4095) * bits / 8;
}

/* Both addresses are guest addresses (the game calls this as func_00244228). */
int sceGsExecStoreImage(uint32_t packet, uint32_t destination) RETAIL(func_00244228);
int sceGsExecStoreImage(uint32_t packet, uint32_t destination)
{
    /* The SDK packet is one VIF qword and six GIF qwords. Execute it before
       reading, so pending raster work is visible to depth/occlusion queries. */
    kernel_poll();
    if (defer < 0) {
        const char *option = getenv("DHD_DEFER_READS");
        defer = !option || strcmp(option, "0");
        if (defer) defer = host_callback_fault_guard(HOST_CALLBACK(guard_fault), 0);
    }
    /* Queued (finding 148): the packet, the read and the unmask below go behind the drawing in the
       graphics queue, and the game goes on without waiting for VU1 and the GS to finish it; the
       first access to the bytes (the next frame's func_002286F8) waits only until the read has
       run. The pages are guarded first, as below. */
    static int queue = -1;
    if (queue < 0) {
        const char *option = getenv("DHD_QUEUE_READS");
        queue = !option || strcmp(option, "0");
    }
    size_t queued = defer && queue && host_fault_guard_here() ? store_bytes(packet) : 0;
    if (queued) {
        if (sync_reads) graphics_settle();
        /* The packet is copied out of PS2 RAM before the destination is guarded: where the OS page
           is bigger than 4 KB (16 KB on Apple silicon and some Android phones) the guard can cover
           the packet too, and reading it would fault and settle, unguarding the destination before
           its read is queued (the game would then read the old bytes). The copy is in the module's
           own memory, never guarded; the queue takes its words at once. */
        static uint32_t copy[28] __attribute__((aligned(16)));
        memcpy(copy, GUEST(packet), sizeof copy);
        if (guard(destination, queued) && host_service_gs_store_image(GADDR(copy), destination, (uint32_t)queued)) {
            queued_reads = 1;  /* the host consumes FINISH there, as below, and the unmask follows */
            return 0;
        }
        graphics_settle();  /* releases the pages just guarded */
    }
    /* vif_feed runs here, on this thread: the workers must be idle first */
    host_service_graphics_wait();
    host_service_vif_feed(1, packet, 28);
    host_service_graphics_wait(); /* A split GS worker must retire this readback setup too. */
    uint64_t size = host_service_gs_register(GS_TRXREG);
    unsigned bits = host_service_gs_bits_per_pixel((unsigned)(host_service_gs_register(GS_BITBLTBUF) >> 24) & 63);
    size_t bytes = (size_t)(size & 4095) * ((size >> 32) & 4095) * bits / 8;
    /* Guarded before it is deferred. A refusal leaves pages guarded: settling releases them. */
    if (!defer || !host_fault_guard_here() || !bytes ||
        !(defer_read(destination, bytes) || (graphics_settle(), defer_read(destination, bytes)))) {
        graphics_settle();
        if (host_service_gs_read_image(destination, bytes) != bytes) return -1;
    }
    /* Retail consumes FINISH and restores the bus and PATH3 direction. */
    host_service_gs_priv_write(GS_CSR, 2);
    *(volatile uint64_t *)GUEST(0x12001000) = host_service_gs_priv_read(GS_CSR);
    const uint32_t unmask = 0x06000000;
    host_service_vif_feed(1, GADDR(&unmask), 1);
    return 0;
}
