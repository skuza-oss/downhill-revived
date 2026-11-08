/* The EE's DMA controller (finding 020). The game starts a transfer by writing a channel's
   registers and setting CHCR.STR. MMIO store helpers start transfers immediately;
   dma_poll at kernel entries also finds any pending channels. Each transfer ends as the DMAC would:
   STR cleared, then the channel's interrupt. Scratchpad transfers copy, VIF transfers go to the
   VIFs (vif.c, normal or source chain mode); GIF transfers go to the GS (PATH3). */
#include "hle.h"

#include <string.h>

#include "ps2asm.h"
#include "../gs/gs.h"

enum {
    DMA_VIF0, DMA_VIF1, DMA_GIF, DMA_FROM_IPU, DMA_TO_IPU, DMA_SIF0, DMA_SIF1, DMA_SIF2,
    DMA_FROM_SPR, DMA_TO_SPR, DMA_CHANNELS
};

static const uint32_t channel_base[DMA_CHANNELS] = {
    0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
    0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400,
};
static const char *const channel_name[DMA_CHANNELS] = {
    "VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR",
};

typedef struct {  /* a channel's registers, 16 bytes apart */
    uint32_t chcr, pad0[3], madr, pad1[3], qwc, pad2[3], tadr, pad3[3];
    uint32_t asr0, pad4[3], asr1, pad5[3], pad6[8], sadr;
} Channel;

enum { CHCR_MODE = 0xC, CHCR_MODE_CHAIN = 0x4, CHCR_STR = 0x100 };

/* Host pointer of a DMA address: RAM (whose host addresses equal the PS2 values the game
   wrote, memory.c) or, with bit 31, the scratchpad. */
static void *dma_ptr(uint32_t addr)
{
    if (addr & 0x80000000)
        return (void *)(uintptr_t)(PS2_SCRATCHPAD + (addr & 0x3FF0));
    return (void *)(uintptr_t)(addr & 0x0FFFFFF0);
}

static void scratchpad_copy(int channel, Channel *c)
{
    if ((c->chcr & CHCR_MODE) == CHCR_MODE_CHAIN) {
        hle_log("dma %s: chain mode not written\n", channel_name[channel]);
        return;
    }
    uint8_t *ram = dma_ptr(c->madr), *spr = (uint8_t *)(uintptr_t)PS2_SCRATCHPAD;
    for (uint32_t q = 0; q < (c->qwc & 0xFFFF); q++) {  /* the scratchpad address wraps */
        uint8_t *s = spr + ((c->sadr + q * 16) & 0x3FF0);
        if (channel == DMA_TO_SPR)
            memcpy(s, ram + q * 16, 16);
        else
            memcpy(ram + q * 16, s, 16);
    }
    c->madr += (c->qwc & 0xFFFF) * 16;
    c->sadr = (c->sadr + (c->qwc & 0xFFFF) * 16) & 0x3FF0;
    c->qwc = 0;
}

enum { TAG_REFE, TAG_CNT, TAG_NEXT, TAG_REF, TAG_REFS, TAG_CALL, TAG_RET, TAG_END };
enum { CHCR_ASP = 0x30, CHCR_TTE = 0x40, CHCR_TIE = 0x80 };

/* A transfer from memory to a VIF: qwc qwords from MADR (normal mode), or the source chain
   from TADR. A chain's tags say where each block is and where the next tag is; with TTE the
   VIF also gets each tag's upper half (two VIF codes). */
#if defined(__wasm__)
void module_vif0_feed(int unit, const uint32_t *words, uint32_t count);
#endif
static void feed(int unit, const uint32_t *words, uint32_t count)
{
#if defined(__wasm__)
    if (unit==0) { module_vif0_feed(unit,words,count); return; }
#endif
    host_service_graphics_feed(unit, GADDR(words), count);
}

static void transfer_out(Channel *c, int unit)
{
    int guard;
    if ((c->chcr & CHCR_MODE) != CHCR_MODE_CHAIN) {
        feed(unit, dma_ptr(c->madr), (c->qwc & 0xFFFF) * 4);
        c->madr += (c->qwc & 0xFFFF) * 16;
        c->qwc = 0;
        return;
    }
    for (guard = 0; guard < 0x100000; guard++) {
        const uint32_t *tag = dma_ptr(c->tadr);
        uint32_t qwc = tag[0] & 0xFFFF, id = tag[0] >> 28 & 7, addr = tag[1], sp = (c->chcr & CHCR_ASP) >> 4;
        int end = 0;
        c->chcr = (c->chcr & 0xFFFF) | (tag[0] & 0xFFFF0000);  /* the tag's upper bits show in CHCR */
        if (unit != 2 && (c->chcr & CHCR_TTE))
            feed(unit,tag+2,2);
        c->madr = id == TAG_REFE || id == TAG_REF || id == TAG_REFS ? addr : c->tadr + 16;
        feed(unit, dma_ptr(c->madr), qwc * 4);
        c->madr += qwc * 16;
        switch (id) {
        case TAG_REFE: case TAG_END: end = 1; break;
        case TAG_CNT: c->tadr = c->madr; break;
        case TAG_NEXT: c->tadr = addr; break;
        case TAG_REF: case TAG_REFS: c->tadr += 16; break;
        case TAG_CALL:
            if (sp < 2) {
                (&c->asr0)[sp * 4] = c->madr;
                c->chcr = (c->chcr & ~CHCR_ASP) | (sp + 1) << 4;
            }
            c->tadr = addr;
            break;
        case TAG_RET:
            if (sp) {
                c->tadr = (&c->asr0)[(sp - 1) * 4];
                c->chcr = (c->chcr & ~CHCR_ASP) | (sp - 1) << 4;
            } else
                end = 1;
            break;
        }
        if (end || (tag[0] >> 31 && c->chcr & CHCR_TIE))
            break;
    }
    c->qwc = 0;
}

void dma_poll(void)
{
    static uint16_t dropped;
    static int active;
    if (active) return;
    active = 1;
    for (int ch = 0; ch < DMA_CHANNELS; ch++) {
        Channel *c = (Channel *)(uintptr_t)channel_base[ch];
        if (!(c->chcr & CHCR_STR))
            continue;
        if (ch == DMA_FROM_SPR || ch == DMA_TO_SPR)
            scratchpad_copy(ch, c);
        else if (ch == DMA_VIF0 || ch == DMA_VIF1 || ch == DMA_GIF)
            transfer_out(c, ch);
        else if (!(dropped & 1 << ch)) {
            dropped |= (uint16_t)(1 << ch);
            hle_log("dma %s: transfers dropped (no renderer yet)\n", channel_name[ch]);
        }
        c->chcr &= ~CHCR_STR;
        kernel_dmac_interrupt(ch);
    }
    active = 0;
}

/* sceGsSyncPath: waits for the drawing paths (VIF1, GIF, VU1) to go idle; here they finish at
   once. */
int sceGsSyncPath(int mode, int timeout) RETAIL(func_002433A8);
int sceGsSyncPath(int mode, int timeout)
{
    (void)mode, (void)timeout;
    kernel_poll();  /* tight render loops still deliver vsync and window messages */
    if (!graphics_ordered) host_service_graphics_wait();   /* hle.h: the DMA data is copied already */
    return 0;
}

/* One pass of WaitForRender's spin (func_001B4618, port/patches.py), which waits for the flags
   D_0029E1C8, D_0029E1CC and D_0029E1E4 to clear: a frame's render chain is queued and not yet
   sent. On the PS2 the VIF1 interrupt's handler (func_0021EAA0) sends it (func_0021E9A8 starts
   the chain and clears the flags) when the transfer before it ends. Here transfers end at once
   and that handler never runs: the game enables channel 1 only when a frame finds VIF1 still
   busy (func_00217D88's bc0t, always idle here, port/tools/recomp.py). So a frame that queues
   its chain and sends nothing leaves the spin waiting forever: the game freezes, its exit too.
   That fits issue 22's log; which frame did it there is not known (the load bar's render,
   func_00218720, can: it sets D_0029E1CC but sends only with D_0029E1C8 set). Each pass enters
   the kernel (vsync, window messages) and, with VIF1 idle, does what the interrupt would. */
extern void func_0021E9A8(void);

void hle_render_wait(void)
{
    static unsigned sent;
    kernel_poll();
    const volatile int32_t *c8 = PS2_PTR(0x0029E1C8), *cc = PS2_PTR(0x0029E1CC), *e4 = PS2_PTR(0x0029E1E4);
    if (((Channel *)(uintptr_t)channel_base[DMA_VIF1])->chcr & CHCR_STR || !(*c8 | *cc | *e4))
        return;
    unsigned ten = 10;
    while (ten < sent + 1 && ten < 1000000000u) ten *= 10;
    if (++sent <= 5 || sent == ten)  /* the first few, then each power of ten: whether normal play needs it */
        hle_log("render wait: VIF1 idle with a chain queued (flags %X %X %X), sent it (%u so far)\n",
                (unsigned)*c8, (unsigned)*cc, (unsigned)*e4, sent);
    func_0021E9A8();
}

/* Installed by kernel_init. The MMIO store has already reached the mapped page. */
void dma_mmio_write(uintptr_t address, uint64_t value, unsigned size)
{
    if (address == PS2_INTC_STAT) {
        kernel_intc_write((uint32_t)value);
        return;
    }
    if (address >= 0x10003C00 && address < 0x10004000) host_service_graphics_wait();
    if (address >= 0x10005000 && address + size <= 0x10005010) {
        host_service_graphics_feed(1, (uint32_t)address, size / 4);
        return;
    }
    for (unsigned ch = 0; ch < DMA_CHANNELS; ch++)
        if (address == channel_base[ch] && (value & CHCR_STR)) {
            dma_poll();
            return;
        }
    if (address >= 0x12000000 && address < 0x12001100 && !(address & 15)) {
        host_service_graphics_wait();
        host_service_gs_priv_write((unsigned)address, value);
        if (address == 0x12001000) {
            *(volatile uint64_t *)address = host_service_gs_priv_read(GS_CSR);
            /* A GS reset (sceGsResetGraph) cleared the display registers (gs_priv_write): so does
               this page, which the presenter reads at each vsync (finding 160). */
            if (value & 0x200)
                for (unsigned reg = GS_PMODE; reg <= GS_BGCOLOR; reg += 16)
                    *(volatile uint64_t *)(uintptr_t)(0x12000000u + reg) = host_service_gs_priv_read(reg);
        }
    }
}
