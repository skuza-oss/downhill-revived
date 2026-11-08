#define _CRT_SECURE_NO_WARNINGS
#include "hle.h"
#include "ps2asm.h"
#include "../gs/gs.h"
#include "gs_queue.h"
#include <windows.h>
#include <string.h>

ps2_cpu ps2;
static unsigned failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL VIF line %d: %s\n", __LINE__, #c); failures++; } } while (0)
void hle_log(const char *fmt, ...) { va_list args; va_start(args,fmt); vfprintf(stderr,fmt,args); va_end(args); }
void kernel_poll(void) {}
double host_seconds(void) { return 0; }
void host_thread_name(const char *name) { (void)name; }
double host_current_thread_cpu(void) { return 0; }
int host_callback_fault_guard(uint32_t fn, uint32_t ctx) { (void)fn; (void)ctx; return 0; }  /* no deferred reads */
int host_fault_guard_here(void) { return 0; }
int host_mem_protect(uint32_t at, uint32_t size, int protection) { (void)at; (void)size; (void)protection; return 1; }
unsigned host_fp_csr(void) { return 0; }
void host_fp_set_csr(unsigned csr) { (void)csr; }
uint64_t host_cycles(void) { return 0; }
double host_wall_seconds(void) { return 0; }
volatile uint32_t gs_presented_frames;
uint64_t kernel_vsync_count;
#ifndef GS_QUEUE_TEST
GraphicsProvenance graphics_provenance;
void graphics_gif_feed(unsigned path,const uint32_t *words,unsigned count) { gs_feed(&gs,path,words,count); }
int graphics_xgkick(uint32_t memory,unsigned qword) { return gs_xgkick(&gs,GUEST(memory),qword); }
#endif
#ifndef GRAPHICS_QUEUE_TEST
void graphics_wait(void) {}
void host_service_graphics_wait(void) { graphics_wait(); }  /* graphics_queue.c's, without the queue */
/* graphics_queue.c's without workers: nothing queued, the store-image read waits */
uint32_t host_service_gs_store_image(uint32_t packet, uint32_t destination, uint32_t bytes)
{ (void)packet; (void)destination; (void)bytes; return 0; }
void host_service_gs_store_settle(void) {}
#endif
extern int sceGsExecStoreImage(uint32_t, uint32_t) RETAIL(func_00244228);
static void code(uint32_t value) {
#ifdef GRAPHICS_QUEUE_TEST
    graphics_feed(1, GADDR(&value), 1);
    graphics_wait();
#else
    vif_feed(1, &value, 1);
#endif
}

/* UNPACK fast paths against the generic per-vector path (vif_unpack_reference): random formats,
   masks, USN, STMOD, CL/WL (skipping, equal and filling), counts, start addresses and DMA chunk
   splits; then a probe UNPACK writes the row registers (mode 2 changes them) to memory. The whole
   16 KB data memory must match. */
extern int vif_unpack_reference;
static uint32_t fuzz_state;
static uint32_t fuzz(void)
{
    fuzz_state ^= fuzz_state << 13; fuzz_state ^= fuzz_state >> 17; fuzz_state ^= fuzz_state << 5;
    return fuzz_state;
}

static unsigned unpack_fuzz(unsigned trials)
{
    static uint32_t stream[8192], initial[4096], reference[4096];
    uint32_t *data = (uint32_t *)0x1100C000;
    unsigned bad = 0, vectors = 0;
    for (unsigned t = 0; t < trials; t++) {
        fuzz_state = t * 2654435761u + 12345u;
        unsigned n = 0, cl = 1 + fuzz() % 6, wl = 1 + fuzz() % 6;
        stream[n++] = 0x01000000 | wl << 8 | cl;           /* STCYCL */
        stream[n++] = 0x05000000 | fuzz() % 4;             /* STMOD */
        stream[n++] = 0x20000000; stream[n++] = fuzz();    /* STMASK */
        stream[n++] = 0x30000000; for (int i = 0; i < 4; i++) stream[n++] = fuzz() % 7 ? fuzz() % 1000 : fuzz();
        stream[n++] = 0x31000000; for (int i = 0; i < 4; i++) stream[n++] = fuzz();
        for (unsigned k = 1 + fuzz() % 3; k; k--) {
            unsigned vn = fuzz() % 4, vl = vn == 3 ? fuzz() % 4 : fuzz() % 3, masked = fuzz() % 2;
            unsigned num = fuzz() % 23 ? 1 + fuzz() % 60 : 256, usn = fuzz() % 2;
            unsigned bits = vl == 3 ? 16 : 32 >> vl, count = vl == 3 ? 1 : vn + 1, written = num;
            if (wl > cl) written = num / wl * cl + (num % wl < cl ? num % wl : cl);
            unsigned words = (written * bits * count + 31) / 32;
            stream[n++] = (0x60 | masked << 4 | vn << 2 | vl) << 24 | (num & 255) << 16 | usn << 14 | fuzz() % 1024;
            for (unsigned i = 0; i < words; i++) stream[n++] = fuzz();
            vectors += num;
        }
        stream[n++] = 0x01000101;                          /* the row registers to qword 1023 */
        stream[n++] = 0x20000000; stream[n++] = 0x55555555;
        stream[n++] = 0x7C0103FF; for (int i = 0; i < 4; i++) stream[n++] = 0;
        uint32_t chunks = fuzz();
        for (unsigned i = 0; i < 4096; i++) initial[i] = fuzz();
        for (int arm = 0; arm < 2; arm++) {
            memcpy(data, initial, sizeof initial);
            vif_unpack_reference = arm == 0;
            fuzz_state = chunks | 1;
            for (unsigned at = 0; at < n;) {
                unsigned take = 1 + fuzz() % (fuzz() % 4 ? 64 : 4);
                if (take > n - at) take = n - at;
                vif_feed(1, stream + at, take);
                at += take;
            }
            if (!arm) memcpy(reference, data, sizeof reference);
        }
        vif_unpack_reference = 0;
        if (memcmp(reference, data, sizeof reference) && bad++ < 5) {
            unsigned q = 0;
            while (!memcmp(reference + q * 4, data + q * 4, 16)) q++;
            printf("FAIL VIF unpack trial %u: qword %u differs\n", t, q);
        }
    }
    printf("VIF UNPACK fast paths: %u trials, %u vectors, %u differing\n", trials, vectors, bad);
    return bad;
}

int main(void)
{
    void *vu = VirtualAlloc((void *)0x11000000, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void *hw = VirtualAlloc((void *)0x10000000, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (vu != (void *)0x11000000 || hw != (void *)0x10000000) return 1;
    gs_reset(&gs);
    void *gs_hw = VirtualAlloc((void *)0x12000000, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (gs_hw != (void *)0x12000000) return 1;
    uint64_t store[] = {UINT64_C(0x06008000) << 32, UINT64_C(0x50000006) << 32 | 0x13000000,
        UINT64_C(1) << 60 | 0x8005, 14,
        UINT64_C(48) << 24 | UINT64_C(8) << 16, GS_BITBLTBUF,
        13 | UINT64_C(27) << 16, GS_TRXPOS,
        4 | UINT64_C(4) << 32, GS_TRXREG, 0, GS_FINISH, 1, GS_TRXDIR};
    uint32_t depth[17];
    for (unsigned i = 0; i < 16; i++) gs_write_pixel(&gs, GS_PSMZ32, 0, 8, 13 + i % 4, 27 + i / 4, i * 0x1234567u);
    depth[16] = 0xDEADBEEF;
    CHECK(sceGsExecStoreImage(GADDR(store), GADDR(depth)) == 0);
    for (unsigned i = 0; i < 16; i++) CHECK(depth[i] == i * 0x1234567u);
    CHECK(depth[16] == 0xDEADBEEF && !(gs_priv_read(&gs, GS_CSR) & 2));
    gs_reset(&gs);
    uint32_t *data = (uint32_t *)0x1100C000;
    /* XTOP, XITOP, ISW x2, E+delay, followed by a second program for MSCNT. */
    uint32_t program[] = {
        0x800106BC,0x2FF, 0x800206BD,0x2FF,
        0x0B010000,0x2FF, 0x0B020001,0x2FF,
        0x8000033C,0x400002FF, 0x8000033C,0x2FF,
        0x800106BC,0x2FF, 0x800206BD,0x2FF,
        0x0B010002,0x2FF, 0x0B020003,0x400002FF, 0x8000033C,0x2FF
    };
    code(0x4A0B0000);
    for (unsigned i = 0; i < sizeof program / 4; i++) code(program[i]);
    code(0x03000100); code(0x02000020); code(0x04000055); code(0x14000000);
    CHECK(data[0] == 0x100 && data[4] == 0x55);
    code(0x6C018005); /* relative V4-32 targets TOPS=0x120 */
    code(1); code(2); code(3); code(4);
    CHECK(data[0x125 * 4] == 1 && data[0x125 * 4 + 3] == 4);
    code(0x04000066); code(0x17000000);
    CHECK(data[8] == 0x120 && data[12] == 0x66);
    code(0x01000000); code(0x6C018005); code(5); code(6); code(7); code(8);
    CHECK(data[0x105 * 4] == 5 && data[0x105 * 4 + 3] == 8);
    code(0x50000002);
    code(0x8001); code(1u << 28); code(14); code(0);
    code(0x11223344); code(0); code(GS_FOGCOL); code(0);
    CHECK(gs.reg[GS_FOGCOL] == 0x11223344 && gs.packets[1] == 1);
    failures += unpack_fuzz(200000);
    VirtualFree(vu, 0, MEM_RELEASE); VirtualFree(hw, 0, MEM_RELEASE);
    printf("VIF1 upload, double buffering, MSCNT and DIRECT tests: %u failures\n", failures);
    return failures != 0;
}
