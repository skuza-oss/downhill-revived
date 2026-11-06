/* Native GS interface. Addresses use GS units: base pointers are 256-byte blocks,
   buffer widths are multiples of 64 pixels, vertex positions are 12.4 fixed point. */
#ifndef PORT_GS_H
#define PORT_GS_H
#include <stdint.h>
#include <stddef.h>

#ifndef PS2_NO_AUTO_INIT
/* The build zero-fills every local (-ftrivial-auto-var-init=zero: the game reads some before
   writing them, finding 081). A port local every read of which follows a write can skip that
   fill where it costs (finding 148). */
#if defined(__has_attribute)
#if __has_attribute(uninitialized)
#define PS2_NO_AUTO_INIT __attribute__((uninitialized))
#endif
#endif
#ifndef PS2_NO_AUTO_INIT
#define PS2_NO_AUTO_INIT
#endif
#endif

enum { GS_VRAM_SIZE = 4 * 1024 * 1024 };
enum {
    GS_PSMCT32 = 0, GS_PSMCT24 = 1, GS_PSMCT16 = 2, GS_PSMCT16S = 10,
    GS_PSMT8 = 19, GS_PSMT4 = 20, GS_PSMT8H = 27, GS_PSMT4HL = 36, GS_PSMT4HH = 44,
    GS_PSMZ32 = 48, GS_PSMZ24 = 49, GS_PSMZ16 = 50, GS_PSMZ16S = 58
};
enum {
    GS_PRIM = 0, GS_RGBAQ = 1, GS_ST = 2, GS_UV = 3, GS_XYZF2 = 4, GS_XYZ2 = 5,
    GS_TEX0_1 = 6, GS_TEX0_2 = 7, GS_CLAMP_1 = 8, GS_CLAMP_2 = 9, GS_FOG = 10,
    GS_XYZF3 = 12, GS_XYZ3 = 13, GS_TEX1_1 = 20, GS_TEX1_2 = 21,
    GS_TEX2_1 = 22, GS_TEX2_2 = 23, GS_XYOFFSET_1 = 24, GS_XYOFFSET_2 = 25,
    GS_PRMODECONT = 26, GS_PRMODE = 27, GS_TEXCLUT = 28, GS_TEXA = 59,
    GS_SCANMSK = 34, GS_SCISSOR_1 = 64, GS_SCISSOR_2 = 65, GS_ALPHA_1 = 66,
    GS_MIPTBP1_1 = 52, GS_MIPTBP1_2 = 53, GS_MIPTBP2_1 = 54, GS_MIPTBP2_2 = 55,
    GS_ALPHA_2 = 67, GS_DIMX = 68, GS_DTHE = 69, GS_COLCLAMP = 70,
    GS_TEST_1 = 71, GS_TEST_2 = 72, GS_PABE = 73, GS_FBA_1 = 74, GS_FBA_2 = 75,
    GS_FRAME_1 = 76, GS_FRAME_2 = 77, GS_ZBUF_1 = 78, GS_ZBUF_2 = 79,
    GS_FOGCOL = 61, GS_BITBLTBUF = 80, GS_TRXPOS = 81, GS_TRXREG = 82,
    GS_TRXDIR = 83, GS_HWREG = 84, GS_SIGNAL = 96, GS_FINISH = 97, GS_LABEL = 98
};
enum { GS_PMODE = 0, GS_SMODE2 = 0x20, GS_DISPFB1 = 0x70, GS_DISPLAY1 = 0x80,
       GS_DISPFB2 = 0x90, GS_DISPLAY2 = 0xA0, GS_BGCOLOR = 0xE0,
       GS_CSR = 0x1000, GS_IMR = 0x1010, GS_BUSDIR = 0x1040, GS_SIGLBLID = 0x1080 };

typedef struct {
    uint32_t x, y, z, rgba, fog, u, v;
    float s, t, q;
} GsVertex;

typedef struct {
    uint32_t words[4], have, left, nreg, reg, mode, eop;
    uint64_t regs;
    uint32_t q;
} GsPath;

typedef struct Gs Gs;
struct Gs {
    uint32_t vram[GS_VRAM_SIZE / 4];
    uint64_t reg[128], privileged[0x1100 / 16];
    uint16_t clut[1024], cbp[2];
    GsPath path[3];
    uint64_t transfer_bits;
    uint32_t transfer_count, transfer_have;
    GsVertex vertex, vertices[3];
    unsigned vertex_count, strip_parity;
    uint64_t packets[3], kicks, uploads, pixels_written, state_version;
    uint32_t host_dirty[16]; /* 8 KB pages changed by CPU transfers */
    /* The backend must finish pending draws before a CPU VRAM access. */
    void (*sync)(Gs *gs);
    void (*sync_range)(Gs *gs, unsigned byte_address, unsigned bytes);
    void (*draw)(Gs *gs, unsigned prim, const GsVertex *vertices, unsigned count);
    void (*record_transfer)(unsigned path, const uint32_t *qword);
    /* Called by gs_feed for every packet, before it is parsed (the state trace, hle/state_trace.c). */
    void (*record_packet)(Gs *g, unsigned path, const uint32_t *words, size_t count);
    /* Diagnostic provenance only; never serialized as PS2 state. */
    unsigned source_path, vu_entry, vu_top, vu_itop, vu_pc, vu_clip, vu_qword;
    unsigned vu_overlay; /* 1: particle/effect A; 2: mesh/screen B; 0: unknown */
    unsigned vu_program; /* most recent MSCAL byte address, retained across MSCNT */
    uint64_t adc_skips, suspect_draws;
    double trace_seconds;
    void *backend;
    /* Optional: runs a TRXDIR 2 transfer in the backend; returns 0 to leave it to the CPU. */
    int (*local_copy)(Gs *gs);
    /* Optional, for gs_defer_read_image: stage_pages captures the backend-written pages among
       want (setting them in staged) in order with the draws; fetch_pages copies them out later. */
    void (*stage_pages)(Gs *gs, const uint32_t want[16], uint32_t staged[16]);
    void (*fetch_pages)(Gs *gs, const uint32_t staged[16], uint32_t *image);
    uint64_t vram_epoch; /* counts draws and local copies; with pixels_written, any VRAM change */
    /* CPU pixel writes per 8 KB page, for host caches. Last, so earlier offsets stay fixed. */
    uint32_t page_writes[512];
    /* Finish recording earlier draws before a host upload changes their pages. */
    void (*flush_draw)(Gs *gs);
    /* Optional native high-byte overlay, before the CPU word/page version changes. */
    void (*write_high)(Gs *gs, unsigned bit_address, uint32_t value, uint32_t mask);
    /* Optional, with draw: a triangle strip's draws at once (gs_feed's PACKED strips, finding 146).
       Triangle i is list[i..i+2], and draw would have been called for it when drawn[i] is set; the
       same draws, in the same order, with the same GS state but for the vertex registers, which
       already hold the strip's last values. */
    void (*draw_strip)(Gs *gs, unsigned prim, const GsVertex *list, unsigned count, const uint8_t *drawn);
    /* load_clut's last CSM1 load (finding 148): its source bytes, where they came from and which
       entries they filled; valid while no other load has written the CLUT. clut_writes counts
       every change of the CLUT's entries (host palette caches). Not PS2 state. */
    struct { uint32_t valid, cbp, cpsm, count, start, bytes; uint32_t source[256]; } clut_memo;
    uint64_t clut_writes;
    uint64_t resets;  /* CSR.RESET writes so far (the presenter's movie hold, finding 160). Not PS2 state. */
};
extern Gs gs;

void gs_reset(Gs *g);
unsigned gs_bits_per_pixel(unsigned psm);
uint32_t gs_pixel_address(unsigned psm, unsigned bp, unsigned bw, unsigned x, unsigned y);
void gs_mark_pages(uint32_t pages[16], unsigned psm, unsigned bp, unsigned bw,
                   unsigned x0, unsigned y0, unsigned x1, unsigned y1);
uint32_t gs_read_pixel(const Gs *g, unsigned psm, unsigned bp, unsigned bw, unsigned x, unsigned y);
void gs_write_pixel(Gs *g, unsigned psm, unsigned bp, unsigned bw, unsigned x, unsigned y, uint32_t value);
/* count whole pixels of a host-to-local transfer from pixel first on (memory.c). */
void gs_write_pixels(Gs *g, unsigned psm, unsigned bp, unsigned bw, unsigned x0, unsigned y0, unsigned width,
                     unsigned first, unsigned count, const uint8_t *data);
uint32_t gs_color(uint32_t value, unsigned psm, uint64_t texa);
uint32_t gs_palette(const Gs *g, uint64_t tex0, unsigned index);
void gs_write(Gs *g, unsigned reg, uint64_t value);
void gs_priv_write(Gs *g, unsigned addr, uint64_t value);
uint64_t gs_priv_read(const Gs *g, unsigned addr);
void gs_vblank(Gs *g);
void gs_feed(Gs *g, unsigned path, const uint32_t *words, size_t count);
size_t gs_read_image(Gs *g, void *destination, size_t bytes);
/* A whole local-to-host transfer whose bytes are written later, by gs_settle_reads, exactly as
   gs_read_image would write them now. Returns 0 (nothing done) when it can't be deferred, or
   not before the pending reads are settled (a page they captured has changed since). */
size_t gs_defer_read_image(Gs *g, void *destination, size_t bytes);
int gs_reads_pending(const Gs *g);
void gs_settle_reads(Gs *g);
uint32_t gs_image_pixel(const uint32_t *vram, unsigned psm, unsigned bp, unsigned bw, unsigned x, unsigned y);
int gs_xgkick(Gs *g, const uint32_t *memory, unsigned qword);

#endif
