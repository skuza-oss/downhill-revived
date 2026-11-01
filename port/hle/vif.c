/* The VIFs: the DMA's front ends of VU0 and VU1 (finding 020). dma.c feeds each one the words a
   transfer carries; a VIF reads them as VIF codes and their data. VIF0 is complete for what the
   game sends it: MPG loads microcode, UNPACK fills data memory (with the cycle, mask and
   row/column registers), MSCAL/MSCNT start a microprogram (vu0_interp.c). VIF1 runs its
   own register file and 16 KB memories, with DIRECT on PATH2 and XGKICK on PATH1. */
#include "hle.h"

#include <string.h>
#include <stdlib.h>

#include "ps2asm.h"
#if !defined(DHD_VIF0_ONLY) && defined(__aarch64__)
/* The Android build (finding 091): SIMDe maps the WL=1 unpack's SSE2 integer calls to NEON. */
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/sse2.h>
#elif !defined(DHD_VIF0_ONLY)
#include <emmintrin.h>
#endif
#include "../runtime/memory.h"
#include "../gs/gs.h"
#if !defined(DHD_VIF0_ONLY)
#include "vu1_profile.h"
#include "gs_queue.h"
#endif

typedef struct {
    int unit;
    uint32_t code;           /* the command being received */
    uint32_t need, have;     /* its data words, and how many arrived */
    uint32_t data[4];        /* data of the short commands (STMASK, STROW, STCOL) */
    uint32_t addr;           /* MPG/UNPACK: where the next word or vector goes */
    uint32_t written;        /* UNPACK: vectors written; position in the CL/WL cycle */
    unsigned cl, wl, mode;
    uint32_t mask, row[4], col[4], itop, tpc;
    uint32_t base, offset, top, tops, dbf;
    uint32_t buf[8];         /* UNPACK: stream words not yet read, from bit `bit` of buf[0] */
    int buf_words, bit;
} Vif;

static Vif vif[2] = {{.unit = 0, .cl = 1, .wl = 1}, {.unit = 1, .cl = 1, .wl = 1}};

enum { NOP = 0x00, STCYCL = 0x01, OFFSET = 0x02, BASE = 0x03, ITOP = 0x04, STMOD = 0x05, MSKPATH3 = 0x06,
       MARK = 0x07, FLUSHE = 0x10, FLUSH = 0x11, FLUSHA = 0x13, MSCAL = 0x14, MSCALF = 0x15, MSCNT = 0x17,
       STMASK = 0x20, STROW = 0x30, STCOL = 0x31, MPG = 0x4A, DIRECT = 0x50, DIRECTHL = 0x51 };

/* VU1 memories, as guest addresses (graphics_xgkick takes the data memory's guest address). */
#define VU1_MICRO 0x11008000u
#define VU1_DATA 0x1100C000u
static uint8_t *micro(int unit) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    if (unit!=1) abort();
    return GUEST(VU1_MICRO);
#else
    return unit ? (uint8_t *)GUEST(VU1_MICRO) : ps2.vu0_code;
#endif
}
static uint32_t *data_mem(int unit) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    if (unit!=1) abort();
    return GUEST(VU1_DATA);
#else
    return unit ? (uint32_t *)GUEST(VU1_DATA) : ps2.vu0_mem->w;
#endif
}
static uint32_t data_qwords(int unit) { return unit ? 0x400 : 0x100; }

#if !defined(DHD_VIF0_ONLY)
static ps2_cpu vu1;
/* VU0 micro code reads these registers at its data address 0x4000. */
__attribute__((constructor)) static void publish_vu1(void) { ps2_vu1_regs = &vu1; }

#endif

static int trace_enabled(int unit)
{
    static int enabled[2] = {-1, -1};
    if (enabled[unit] < 0) enabled[unit] = getenv("DHD_VIF_TRACE") != NULL;
    return enabled[unit];
}

#if !defined(DHD_VIF0_ONLY)
double vif_gs_seconds; /* Opt-in profiling total; owned by the graphics worker. */

static int kick(void *opaque, unsigned qword)
{
    (void)opaque;
    if (!vu_profile_active) return graphics_xgkick(VU1_DATA, qword);
    double start = host_wall_seconds();
    int result = graphics_xgkick(VU1_DATA, qword);
    double elapsed = host_wall_seconds() - start;
    vu_profile_kick += elapsed; vif_gs_seconds += elapsed;
    return result;
}

static void kick_trace(unsigned pc, unsigned clip, unsigned qword)
{
    graphics_provenance.pc = pc; graphics_provenance.clip = clip; graphics_provenance.qword = qword;
}

/* Optional local snapshots for interpreter regression work; contains game data,
   so the output directory must stay outside version control. */
static void direct_gif(const uint32_t *words, unsigned count)
{
    if (!vu_profile_enabled()) { graphics_gif_feed(2, words, count); return; }
    double start = host_wall_seconds();
    graphics_gif_feed(2, words, count);
    double at = vu_profile_by_field ? (double)kernel_vsync_count : start;
    if (at >= vu_profile_after && at < vu_profile_until)
        vif_gs_seconds += host_wall_seconds() - start;
}

static void capture_micro(unsigned addr, unsigned top, unsigned itop)
{
    static unsigned seen[128], count;
    static const char *dir;
    static const char *entry;
    static int capture_ee;
    static int initialized;
    static double after;
    if (!initialized) {
        initialized = 1; dir = getenv("DHD_VU_CAPTURE");
        entry = getenv("DHD_VU_CAPTURE_ENTRY");
        capture_ee = getenv("DHD_VU_CAPTURE_EE") != NULL;
        const char *at = getenv("DHD_VU_CAPTURE_AT"); after = at ? atof(at) : 0;
    }
    if (!dir || count == 128 || host_seconds() < after) return;
    if (entry && addr != (unsigned)strtoul(entry, NULL, 16)) return;
    unsigned hash = 2166136261u;
    for (unsigned i = 0; i < 0x4000; i++) hash = (hash ^ vu1.vu0_code[i]) * 16777619u;
    unsigned key = hash ^ addr;
    for (unsigned i = 0; i < count; i++) if (seen[i] == key) return;
    seen[count++] = key;
    char path[1024];
    snprintf(path, sizeof path, "%s/vu1-%08X-%04X.bin", dir, hash, addr);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned header[4] = {addr, top, itop, sizeof vu1};
    fwrite(header, sizeof header, 1, f); fwrite(&vu1, sizeof vu1, 1, f);
    fwrite(vu1.vu0_code, 0x4000, 1, f); fwrite(vu1.vu0_mem, 0x4000, 1, f);
    fclose(f);
    /* Optional upstream context for finding the EE packet/object that supplied
       this input. Like the VU capture, this contains retail data, not source. */
    if (capture_ee) {
        snprintf(path, sizeof path, "%s/vu1-%08X-%04X.ee.bin", dir, hash, addr);
        f = fopen(path, "wb");
        if (f) { fwrite(GUEST(PS2_RAM_BASE), 0x2000000, 1, f); fclose(f); }
    }
}

/* DHD_VU_SEQ=dir: every VU1 run from DHD_VU_SEQ_AT seconds (game clock) for DHD_VU_SEQ_KICKS
   runs (default 8000), in order: dir/seq.bin holds per run the header (entry, TOP, ITOP, state
   size, code hash, field, MSCAL program, 1 for MSCNT), the registers and the 16 KB data memory
   before it; dir/code-HASH.bin each microprogram image once. For the kick dependency analysis (vu1_seq_dep.c). Game data:
   keep the directory under build/. */
static void capture_sequence(unsigned addr, unsigned top, unsigned itop)
{
    static const char *dir;
    static FILE *seq;
    static int initialized;
    static unsigned left, last_hash;
    static double after;
    if (!initialized) {
        initialized = 1; dir = getenv("DHD_VU_SEQ");
        const char *at = getenv("DHD_VU_SEQ_AT"), *n = getenv("DHD_VU_SEQ_KICKS");
        after = at ? atof(at) : 0; left = n ? (unsigned)atoi(n) : 8000;
    }
    if (!dir || !left || host_seconds() < after) return;
    char path[1024];
    if (!seq) {
        snprintf(path, sizeof path, "%s/seq.bin", dir);
        if (!(seq = fopen(path, "wb"))) { perror(path); abort(); }
    }
    unsigned hash = 2166136261u;
    for (unsigned i = 0; i < 0x4000; i++) hash = (hash ^ vu1.vu0_code[i]) * 16777619u;
    if (hash != last_hash) {
        last_hash = hash;
        snprintf(path, sizeof path, "%s/code-%08X.bin", dir, hash);
        FILE *f = fopen(path, "rb");
        if (f) fclose(f);
        else if ((f = fopen(path, "wb"))) { fwrite(vu1.vu0_code, 0x4000, 1, f); fclose(f); }
    }
    extern uint64_t kernel_vsync_count;
    unsigned header[8] = {addr, top, itop, sizeof vu1, hash, (unsigned)kernel_vsync_count,
                          graphics_provenance.program, (vif[1].code >> 24 & 0x7f) == MSCNT};
    fwrite(header, sizeof header, 1, seq); fwrite(&vu1, sizeof vu1, 1, seq); fwrite(vu1.vu0_mem, 0x4000, 1, seq);
    if (!--left) { fclose(seq); seq = NULL; hle_log("vif1: VU1 sequence captured in %s\n", dir); }
}

/* DHD_VU_SUSPECT: keep the inputs of each VU1 run and save those whose output the GS trace
   flagged (DHD_GS_TRACE_FOGGED), so the exact failing invocation can be replayed. */
static struct { unsigned header[4]; ps2_cpu cpu; uint8_t code[0x4000], mem[0x4000]; } suspect;

static void save_suspect(const char *variable, const char *label)
{
    static unsigned saved;
    char path[1024];
    const char *dir = getenv(variable);
    if (!dir || saved == 16) return;
    snprintf(path, sizeof path, "%s/%s-%02u-%04X.bin", dir, label, saved++, suspect.header[0]);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(suspect.header, sizeof suspect.header, 1, f); fwrite(&suspect.cpu, sizeof suspect.cpu, 1, f);
    fwrite(suspect.code, 0x4000, 1, f); fwrite(suspect.mem, 0x4000, 1, f);
    fclose(f);
}

/* Sample real dispatch inputs after the profiling window. Selection is by
   packet length, not by rendered appearance or whether clipping is cheap. */
static void capture_representative(unsigned addr, unsigned top, unsigned itop)
{
    static int initialized;
    static const char *dir;
    static unsigned count[5][2];
    static double next[5][2];
    if (!initialized) { initialized = 1; dir = getenv("DHD_VU_CAPTURE_REP"); }
    if (!dir || (addr != 0x1040 && addr != 0x1cb0)) return;
    unsigned dispatch = vu1.vi[13], group;
    switch (dispatch) {
    case 0x11: group=0; break; case 0x2c9: group=1; break; case 0x2fe: group=2; break;
    case 0x20c: group=3; break; case 0x260: group=4; break; default: return;
    }
    unsigned continuation = addr == 0x1cb0;
    if (count[group][continuation] >= 4) return;
    double now = host_wall_seconds();
    if (now < 155 || now < next[group][continuation] || now > 205) return;
    unsigned vertices = vu1.vu0_mem[vu1.vi[2] & 1023].w[0] & 0x7fff;
    if (vertices < 32 || vertices > 48) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/rep-%03X-%04X-%u-v%u.bin", dir, dispatch, addr,
             count[group][continuation], vertices);
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); abort(); }
    unsigned header[4] = {addr, top, itop, sizeof vu1};
    if (fwrite(header, sizeof header, 1, file) != 1 || fwrite(&vu1, sizeof vu1, 1, file) != 1 ||
        fwrite(vu1.vu0_code, 0x4000, 1, file) != 1 || fwrite(vu1.vu0_mem, 0x4000, 1, file) != 1 || fclose(file)) abort();
    count[group][continuation]++; next[group][continuation] = now + 2;
}

#endif

static void run_micro(Vif *v, uint32_t addr)
{
#if !defined(_WIN64) && !defined(__x86_64__) && !defined(__aarch64__)
    if (v->unit == 0) {
        if (ps2_vu0_run(&ps2, addr))
            hle_log("vif0: microprogram at 0x%X failed\n", addr);
        v->tpc = ps2.vu0_ctrl[10] / 8;
        return;
    }
#endif
#if !defined(DHD_VIF0_ONLY)
    vu1.vu0_code = micro(1);
    vu1.vu0_mem = (ps2_q128 *)data_mem(1);
    v->top = v->tops & 1023;
    v->dbf ^= 1;
    v->tops = (v->base + (v->dbf ? v->offset : 0)) & 1023;
    *(uint32_t *)GUEST(0x10003C00) = v->dbf << 7;
    *(uint32_t *)GUEST(0x10003CE0) = v->top;
    *(uint32_t *)GUEST(0x10003CC0) = v->tops;
    *(uint32_t *)GUEST(0x10003CD0) = v->itop;
    static int traced = -1;
    if (traced < 0) traced = getenv("DHD_GS_TRACE") != NULL;
    static int isolate = -1;
    if (isolate < 0) isolate = getenv("DHD_DRAW_SKIP_VU") != NULL || getenv("DHD_DRAW_SKIP_OVERLAY") != NULL || getenv("DHD_DRAW_SKIP_PROGRAM") != NULL;
    if (traced || isolate) ps2_vu1_set_kick_trace(kick_trace);
    graphics_provenance.entry = addr; graphics_provenance.top = v->top; graphics_provenance.itop = v->itop;
    if (traced || isolate) graphics_provenance.seconds = host_seconds();
    /* Compare the local retail package, never embed its instructions here.
       Both overlays upload at zero; entry numbers alone can overlap. */
    if (traced || isolate)
        graphics_provenance.overlay = !memcmp(micro(1), GUEST(PS2_RAM_BASE + 0x265C10), 32) ? 1 :
                        !memcmp(micro(1), GUEST(PS2_RAM_BASE + 0x269C60), 32) ? 2 : 0;
    capture_micro(addr, v->top, v->itop);
    capture_representative(addr, v->top, v->itop);
    capture_sequence(addr, v->top, v->itop);
    /* Capture the input, before VU stores destroy it, only when requested.
       The oracle accepts this same header/register/code/data format. */
    static int watch = -1, capture_failure;
    if (watch < 0) {
        watch = getenv("DHD_VU_SUSPECT") != NULL;
        capture_failure = getenv("DHD_VU_CAPTURE_FAILURE") != NULL;
    }
    uint64_t suspects = watch ? gs.suspect_draws : 0;
    if (watch || capture_failure) {
        suspect.header[0] = addr; suspect.header[1] = v->top; suspect.header[2] = v->itop;
        suspect.header[3] = sizeof vu1; suspect.cpu = vu1;
        memcpy(suspect.code, vu1.vu0_code, 0x4000); memcpy(suspect.mem, vu1.vu0_mem, 0x4000);
    }
    double profile_start = vu_profile_begin();
    unsigned profile_overlay = 0;
    if (profile_start) profile_overlay = !memcmp(micro(1), GUEST(PS2_RAM_BASE + 0x265C10), 32) ? 1 :
                                       !memcmp(micro(1), GUEST(PS2_RAM_BASE + 0x269C60), 32) ? 2 : 0;
    if (ps2_vu1_run(&vu1, addr, v->top, v->itop, kick, NULL)) {
        hle_log("vif1: microprogram at 0x%X failed\n", addr);
        if (capture_failure) save_suspect("DHD_VU_CAPTURE_FAILURE", "failed");
    }
    if (profile_start) vu_profile_end(profile_start, profile_overlay, graphics_provenance.program, addr, (v->code >> 24 & 0x7f) == MSCNT);
    if (watch && gs.suspect_draws != suspects) save_suspect("DHD_VU_SUSPECT", "suspect");
    v->tpc = vu1.vu0_ctrl[10] / 8;
#endif
}

/* Bits per element and elements per vector of an UNPACK command (vn, vl in bits 0-3). */
static void unpack_format(uint32_t cmd, int *bits, int *count)
{
    int vl = cmd & 3, vn = cmd >> 2 & 3;
    *count = vn + 1;
    *bits = vl == 3 ? 16 : 32 >> vl;  /* V4-5: one 16-bit word per vector (5:5:5:1) */
    if (vl == 3)
        *count = 1;
}

/* Data words an UNPACK of num vectors reads (the CL/WL cycle decides how many it takes). */
static uint32_t unpack_words(Vif *v, uint32_t cmd, uint32_t num)
{
    int bits, count;
    uint32_t vectors = num;
    unpack_format(cmd, &bits, &count);
    if (v->wl > v->cl && v->cl) {  /* filling: of each WL written, only CL come from the stream */
        uint32_t full = num / v->wl, rest = num % v->wl;
        vectors = full * v->cl + (rest < v->cl ? rest : v->cl);
    }
    return (vectors * bits * count + 31) / 32;
}

static int available_bits(const Vif *v) { return v->buf_words * 32 - v->bit; }

static uint32_t take_bits(Vif *v, int bits)
{
    uint64_t two = v->buf[0] | (uint64_t)(v->buf_words > 1 ? v->buf[1] : 0) << 32;
    uint32_t value = (uint32_t)(two >> v->bit) & (bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1);
    v->bit += bits;
    while (v->bit >= 32 && v->buf_words) {
        memmove(v->buf, v->buf + 1, (size_t)--v->buf_words * 4);
        v->bit -= 32;
    }
    return value;
}

/* The fields of one vector from its count elements: sign extended unless USN, then spread. */
static void unpack_fields(uint32_t in[4], int bits, int count, int usn)
{
    for (int i = 0; i < count; i++)
        if (bits < 32 && !usn)
            in[i] = (uint32_t)((int32_t)(in[i] << (32 - bits)) >> (32 - bits));
    if (count == 1)  /* S-: the one value goes to every field */
        in[1] = in[2] = in[3] = in[0];
    else if (count == 2) {  /* V2-: z and w undefined; the VIF repeats x and y */
        in[2] = in[0];
        in[3] = in[1];
    } else if (count == 3)
        in[3] = 0;
}

/* Writes one vector of an UNPACK: from the stream (from_stream; `in` holds its fields when
   `decoded`, else they are read from the buffer) or, in a filling cycle, the row/column
   registers only. */
static void unpack_vector(Vif *v, uint32_t cmd, int from_stream, const uint32_t *decoded)
{
    int bits, count, i;
    uint32_t in[4] = {0, 0, 0, 0}, out[4], *dst;
    uint32_t cycle = v->written % (v->wl ? v->wl : 1);
    int usn = v->code >> 14 & 1, masked = cmd >> 4 & 1;
    unpack_format(cmd, &bits, &count);
    if (decoded)
        memcpy(in, decoded, sizeof in);
    else if (from_stream) {
        if ((cmd & 3) == 3) {  /* V4-5 */
            uint32_t c = take_bits(v, 16);
            in[0] = (c & 0x1F) << 3;
            in[1] = (c >> 5 & 0x1F) << 3;
            in[2] = (c >> 10 & 0x1F) << 3;
            in[3] = (c >> 15 & 1) << 7;
        } else {
            for (i = 0; i < count; i++)
                in[i] = take_bits(v, bits);
            unpack_fields(in, bits, count, usn);
        }
    }
    dst = data_mem(v->unit) + ((v->addr % data_qwords(v->unit)) * 4);
    for (i = 0; i < 4; i++) {
        int m = masked ? (int)(v->mask >> ((cycle < 3 ? cycle : 3) * 8 + i * 2) & 3) : 0;
        uint32_t value = in[i];
        if (!from_stream && m == 0)
            m = 2;  /* a filled vector without data takes the column register */
        switch (m) {
        case 0:
            if (v->mode == 1)
                value += v->row[i];
            else if (v->mode == 2)
                value = v->row[i] = v->row[i] + value;
            out[i] = value;
            break;
        case 1: out[i] = v->row[i]; break;
        case 2: out[i] = v->col[cycle < 3 ? cycle : 3]; break;
        default: out[i] = dst[i]; break;  /* write protected */
        }
    }
    memcpy(dst, out, sizeof out);
}

int vif_unpack_reference;  /* tests: 1 runs every UNPACK through the generic per-vector path */

/* Any format straight from the stream, when no filling cycle writes vectors without data
   (WL <= CL) and nothing is buffered: what unpack_vector writes, vector by vector, with the mask
   decoded once. Returns the words used; a vector cut by the end of the transfer leaves its words
   buffered for the next one, as the generic path would. A field starts at a multiple of its own
   width (8, 16 or 32 bits; the stream starts at bit 0), so it never straddles two words.
   unpack_stream instantiates it with the format and mode as constants for the forms a race sends
   (DHD_VIF_STATS), the position, cycle and row registers kept in locals; any other form runs the
   same code with them as variables. */
static inline __attribute__((always_inline)) uint32_t stream_form(Vif *v, uint32_t cmd, const uint32_t *words, uint32_t n,
                                                                  uint32_t num, int mode)
{
    int bits, count;
    unpack_format(cmd, &bits, &count);
    const int v45 = (cmd & 3) == 3, usn = v->code >> 14 & 1, masked = cmd >> 4 & 1;
    const uint32_t span = v45 ? 16 : (uint32_t)(bits * count), field = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    const uint32_t wl = v->wl ? v->wl : 1, skip = v->wl < v->cl ? v->cl - v->wl : 0;
    const uint32_t last = data_qwords(v->unit) - 1, end = n * 32;
    uint32_t *mem = data_mem(v->unit), at = 0, written = v->written, addr = v->addr;
    uint32_t row[4], col[4];
    memcpy(row, v->row, sizeof row);
    memcpy(col, v->col, sizeof col);
    uint8_t m[4][4];  /* [cycle, 3 for every later one][field]: 0 data, 1 row, 2 column, 3 protected */
    for (unsigned c = 0; c < 4; c++)
        for (unsigned i = 0; i < 4; i++) m[c][i] = masked ? (uint8_t)(v->mask >> (c * 8 + i * 2) & 3) : 0;
    uint32_t cycle = written % wl;
    while (written < num && end - at >= span) {
        uint32_t in[4] = {0, 0, 0, 0};
        if (v45) {
            uint32_t c = words[at / 32] >> (at % 32) & 0xFFFF;
            in[0] = (c & 0x1F) << 3; in[1] = (c >> 5 & 0x1F) << 3; in[2] = (c >> 10 & 0x1F) << 3; in[3] = (c >> 15 & 1) << 7;
        } else {
            for (int k = 0; k < count; k++) {
                uint32_t b = at + (uint32_t)(k * bits);
                in[k] = bits == 32 ? words[b / 32] : words[b / 32] >> (b % 32) & field;
            }
            unpack_fields(in, bits, count, usn);
        }
        at += span;
        uint32_t *dst = mem + (addr & last) * 4;
        const uint8_t *mc = m[cycle < 3 ? cycle : 3];
        for (int i = 0; i < 4; i++) {
            uint32_t value = in[i];
            switch (masked ? mc[i] : 0) {
            case 0:
                if (mode == 1) value += row[i];
                else if (mode == 2) value = row[i] = row[i] + value;
                dst[i] = value;
                break;
            case 1: dst[i] = row[i]; break;
            case 2: dst[i] = col[cycle < 3 ? cycle : 3]; break;
            default: break;  /* write protected */
            }
        }
        written++;
        addr++;
        if (++cycle == wl) cycle = 0;
        if (skip && !cycle) addr += skip;
    }
    v->written = written;
    v->addr = addr;
    if (mode == 2) memcpy(v->row, row, sizeof row);
    if (written < num && at < end) {  /* the rest of a vector comes with the next transfer */
        v->bit = (int)(at % 32);
        for (uint32_t w = at / 32; w < n; w++) v->buf[v->buf_words++] = words[w];
    }
    return n;
}

#if !defined(DHD_VIF0_ONLY)
/* The fields of vector k of a WL 1 UNPACK (stream_wl1) as one SIMD value (SSE2: on wasm through
   simde), sign extended unless USN and spread as unpack_fields does. */
static inline __attribute__((always_inline)) __m128i wl1_fields(uint32_t cmd, int usn, const uint32_t *words, uint32_t k)
{
    const uint8_t *b = (const uint8_t *)words;
    switch (cmd & 0x0F) {
    case 0x02: {  /* S-8: the byte in every field */
        const uint32_t x = usn ? b[k] : (uint32_t)(int32_t)(int8_t)b[k];
        return _mm_set1_epi32((int)x);
    }
    case 0x05: {  /* V2-16: x y x y */
        uint32_t w;
        memcpy(&w, b + 4 * k, 4);
        const __m128i t = _mm_set1_epi32((int)w), low = _mm_slli_epi32(t, 16);
        return usn ? _mm_unpacklo_epi32(_mm_srli_epi32(low, 16), _mm_srli_epi32(t, 16))
                   : _mm_unpacklo_epi32(_mm_srai_epi32(low, 16), _mm_srai_epi32(t, 16));
    }
    case 0x0C:  /* V4-32 */
        return _mm_loadu_si128((const __m128i *)(b + 16 * k));
    case 0x0E: {  /* V4-8: each byte to the top of its lane, then shifted down */
        uint32_t w;
        memcpy(&w, b + 4 * k, 4);
        __m128i t = _mm_cvtsi32_si128((int)w);
        t = _mm_unpacklo_epi8(t, t);
        t = _mm_unpacklo_epi16(t, t);
        return usn ? _mm_srli_epi32(t, 24) : _mm_srai_epi32(t, 24);
    }
    default: {  /* V4-5 (5:5:5:1): each field masked in place, moved by a 16-bit multiply, then down 8 */
        uint16_t c;
        memcpy(&c, b + 2 * k, 2);
        const __m128i t = _mm_and_si128(_mm_set1_epi32(c), _mm_setr_epi32(0x1F, 0x3E0, 0x7C00, 0x8000));
        return _mm_srli_epi32(_mm_mullo_epi16(t, _mm_setr_epi32(2048, 64, 2, 1)), 8);
    }
    }
}

/* stream_form for WL 1 (any CL) and mode 0, the shape of VIF1's common UNPACKs (finding 090 VU1R:
   V2-16 and S-8 masked, S-8, V4-5, V4-8 and V4-32, all CL 3 WL 1, are 85% of a race's unpack
   time). Every vector is in cycle 0, so a masked UNPACK takes mask row 0 for all of them: one
   per-lane blend of the decoded fields, the row/column registers (column 0) and the old memory
   (write-protected lanes), set up once a command. Each vector advances the address by CL. Writes
   what stream_form writes, and buffers a vector cut by the end of the transfer the same way. */
static inline __attribute__((always_inline)) uint32_t stream_wl1(Vif *v, uint32_t cmd, int usn, const uint32_t *words,
                                                                 uint32_t n, uint32_t num)
{
    int bits, count;
    unpack_format(cmd, &bits, &count);
    const uint32_t span = (cmd & 3) == 3 ? 16 : (uint32_t)(bits * count), masked = cmd >> 4 & 1;
    const uint32_t stride = v->cl, last = data_qwords(v->unit) - 1;
    uint32_t *mem = data_mem(v->unit), addr = v->addr, vectors = num - v->written;
    if (vectors > n * 32 / span) vectors = n * 32 / span;
    __m128i take = _mm_set1_epi32(-1), fixed = _mm_setzero_si128(), keep = _mm_setzero_si128();
    int protect = 0;
    if (masked) {
        uint32_t t[4], f[4], p[4];
        for (int i = 0; i < 4; i++) {
            const uint32_t m = v->mask >> (i * 2) & 3;
            t[i] = m == 0 ? ~0u : 0;
            f[i] = m == 1 ? v->row[i] : m == 2 ? v->col[0] : 0;
            p[i] = m == 3 ? ~0u : 0;
            protect |= m == 3;
        }
        take = _mm_loadu_si128((const __m128i *)t);
        fixed = _mm_loadu_si128((const __m128i *)f);
        keep = _mm_loadu_si128((const __m128i *)p);
    }
    for (uint32_t k = 0; k < vectors; k++) {
        __m128i *dst = (__m128i *)(mem + (addr & last) * 4), out = wl1_fields(cmd, usn, words, k);
        if (masked) {
            out = _mm_or_si128(_mm_and_si128(out, take), fixed);
            if (protect) out = _mm_or_si128(out, _mm_and_si128(_mm_loadu_si128(dst), keep));
        }
        _mm_storeu_si128(dst, out);
        addr += stride;
    }
    const uint32_t at = vectors * span;
    v->written += vectors;
    v->addr = addr;
    if (v->written < num && at < n * 32) {  /* the rest of a vector comes with the next transfer */
        v->bit = (int)(at % 32);
        for (uint32_t w = at / 32; w < n; w++) v->buf[v->buf_words++] = words[w];
    }
    return n;
}
#endif

static uint32_t unpack_stream(Vif *v, uint32_t cmd, const uint32_t *words, uint32_t n, uint32_t num)
{
#if !defined(DHD_VIF0_ONLY)
    if (v->mode == 0 && v->wl == 1) {
        const int usn = v->code >> 14 & 1;
#define WL1(c) case c: return stream_wl1(v, c, 0, words, n, num)
#define WL1_USN(c) case c: return usn ? stream_wl1(v, c, 1, words, n, num) : stream_wl1(v, c, 0, words, n, num)
        switch (cmd & 0x1F) {
        WL1_USN(0x02); WL1_USN(0x12); WL1_USN(0x05); WL1_USN(0x15); WL1_USN(0x0E); WL1_USN(0x1E);
        WL1(0x0C); WL1(0x1C); WL1(0x0F); WL1(0x1F);
        }
#undef WL1
#undef WL1_USN
    }
#endif
#define FORM(c) case c: return stream_form(v, c, words, n, num, 0)
    if (v->mode == 0)
        switch (cmd & 0x1F) {
        FORM(0x00); FORM(0x02); FORM(0x05); FORM(0x08); FORM(0x0A); FORM(0x0C); FORM(0x0D); FORM(0x0E); FORM(0x0F);
        FORM(0x12); FORM(0x15); FORM(0x1C); FORM(0x1E); FORM(0x1F);
        }
#undef FORM
    return stream_form(v, cmd, words, n, num, (int)v->mode);
}

static void unpack_run(Vif *v, const uint32_t *words, uint32_t n)
{
    uint32_t cmd = v->code >> 24 & 0x1F, num = v->code >> 16 & 0xFF, pos = 0;
    if (!num)
        num = 256;
    while (v->written < num) {
        uint32_t cycle = v->written % (v->wl ? v->wl : 1);
        int from_stream = v->wl <= v->cl || cycle < v->cl;
        int bits, count;
        unpack_format(cmd, &bits, &count);
        uint32_t span = (uint32_t)(bits * count);
        if (!vif_unpack_reference && v->wl <= v->cl && !v->buf_words && !v->bit) {
            pos += unpack_stream(v, cmd, words + pos, n - pos, num);
            break;  /* the transfer is used up, or the UNPACK is complete */
        }
        if (from_stream && !v->buf_words && !v->bit && span % 32 == 0 && n - pos >= span / 32) {
            /* Whole words, nothing buffered (V4-32, V4-16, V4-8, ...): read them in place. */
            uint32_t in[4] = {0, 0, 0, 0};
            for (int k = 0; k < count; k++) {
                uint32_t at = (uint32_t)(k * bits);
                in[k] = bits == 32 ? words[pos + k] : words[pos + at / 32] >> (at % 32) & ((1u << bits) - 1);
            }
            unpack_fields(in, bits, count, v->code >> 14 & 1);
            pos += span / 32;
            unpack_vector(v, cmd, 1, in);
            v->written++;
            v->addr++;
            if (v->wl < v->cl && (v->written % v->wl) == 0)
                v->addr += v->cl - v->wl;
            continue;
        }
        if (from_stream) {
            while (available_bits(v) < bits * count && pos < n)
                v->buf[v->buf_words++] = words[pos++];
            if (available_bits(v) < bits * count)
                break;  /* the rest comes with the next transfer */
        }
        unpack_vector(v, cmd, from_stream, NULL);
        v->written++;
        v->addr++;
        if (v->wl < v->cl && (v->written % v->wl) == 0)
            v->addr += v->cl - v->wl;  /* skipping: the rest of the cycle's slots stay */
    }
}

/* One command's data word arrived (or all of it, for UNPACK). */
static void command_done(Vif *v)
{
    uint32_t cmd = v->code >> 24 & 0x7F, imm = v->code & 0xFFFF;
    switch (cmd) {
    case STMASK: v->mask = v->data[0]; break;
    case STROW: memcpy(v->row, v->data, 16); break;
    case STCOL: memcpy(v->col, v->data, 16); break;
    default: (void)imm; break;
    }
}

/* DHD_VIF_STATS=field: at that virtual field, the VIF1 UNPACK forms seen so far (format, mask,
   mode, CL/WL), by vectors written; a diagnostic for choosing fast paths. */
static void unpack_stats(const Vif *v, uint32_t cmd, uint32_t num)
{
    static int initialized; static uint64_t at;
    static struct { uint32_t key; uint64_t commands, vectors; } rows[256];
    extern uint64_t kernel_vsync_count;
    if (!initialized) { const char *s = getenv("DHD_VIF_STATS"); initialized = 1; at = s ? strtoull(s, NULL, 0) : 0; }
    if (!at || v->unit != 1) return;
    uint32_t key = (cmd & 0x1F) << 24 | (uint32_t)v->mode << 22 | (v->code >> 14 & 1) << 21 | (v->cl & 0xFF) << 8 | (v->wl & 0xFF);
    unsigned i = 0;
    while (i < 256 && rows[i].commands && rows[i].key != key) i++;
    if (i < 256) { rows[i].key = key; rows[i].commands++; rows[i].vectors += num; }
    if (kernel_vsync_count >= at) {
        for (i = 0; i < 256 && rows[i].commands; i++)
            hle_log("vif stats: format %02X masked %u mode %u usn %u cl %u wl %u: %llu commands, %llu vectors\n",
                    rows[i].key >> 24 & 0xF, rows[i].key >> 28 & 1, rows[i].key >> 22 & 3, rows[i].key >> 21 & 1,
                    rows[i].key >> 8 & 0xFF, rows[i].key & 0xFF, (unsigned long long)rows[i].commands,
                    (unsigned long long)rows[i].vectors);
        at = 0;
    }
}

/* A new VIF code: returns how many data words follow it. */
static uint32_t command(Vif *v, uint32_t code)
{
    uint32_t cmd = code >> 24 & 0x7F, num = code >> 16 & 0xFF, imm = code & 0xFFFF;
    static uint32_t unknown;
    static unsigned traced;
    if (v->unit && trace_enabled(v->unit) && traced++ < 600)
        hle_log("vif1 command %08X\n", code);
    v->code = code;
    v->have = 0;
    if ((cmd & 0x60) == 0x60) {  /* UNPACK */
        v->addr = imm & 0x3FF;
        if (v->unit == 1 && imm >> 15 & 1)
            v->addr += v->tops;  /* FLG addresses the next double-buffer region. */
        v->written = 0;
        v->buf_words = 0;
        v->bit = 0;
        unpack_stats(v, cmd, num ? num : 256);
        return unpack_words(v, cmd, num ? num : 256);
    }
    switch (cmd) {
    case NOP: case MARK: case FLUSHE: case FLUSH: case FLUSHA: case MSKPATH3:
        return 0;
    case BASE: v->base = imm & 1023; return 0;
    case OFFSET: v->offset = imm & 1023; v->dbf = 0; v->tops = v->base; return 0;
    case STCYCL:
        v->cl = (imm & 0xFF) ? imm & 0xFF : 256;
        v->wl = (imm >> 8) ? imm >> 8 : 256;
        return 0;
    case STMOD: v->mode = (uint8_t)(imm & 3); return 0;
    case ITOP: v->itop = imm & 0x3FF; return 0;
    case MSCAL: case MSCALF:
        v->tpc = imm;
#if !defined(DHD_VIF0_ONLY)
        if (v->unit) graphics_provenance.program = imm * 8u;
#endif
        run_micro(v, imm * 8u);
        return 0;
    case MSCNT:
        run_micro(v, v->tpc * 8u);
        return 0;
    case STMASK: return 1;
    case STROW: case STCOL: return 4;
    case MPG:
        v->addr = imm * 8u;
        return (num ? num : 256) * 2u;
    case DIRECT: case DIRECTHL:
        return (imm ? imm : 0x10000) * 4u;
    }
    if (!(unknown & 1u << (cmd & 31))) {
        unknown |= 1u << (cmd & 31);
        hle_log("vif%d: unknown VIF code %08X\n", v->unit, code);
    }
    return 0;
}

/* The words of one DMA transfer to VIF unit (0 or 1), in order. */
void vif_feed(int unit, const uint32_t *words, uint32_t n)
{
#if defined(DHD_VIF0_ONLY)
    if (unit!=0) abort();
#elif defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    if (unit!=1) abort();
#endif
    Vif *v = &vif[unit];
    uint32_t i = 0;
    while (i < n) {
        uint32_t cmd, take;
        if (v->need == 0) {
            v->need = command(v, words[i++]);
            continue;
        }
        cmd = v->code >> 24 & 0x7F;
        take = n - i < v->need ? n - i : v->need;
        if (cmd == MPG) {
            uint8_t *m = micro(unit);
            uint32_t size = unit ? 0x4000 : 0x1000, k;
            static unsigned traced_mpg;
            if (unit && trace_enabled(unit) && traced_mpg++ < 100)
                hle_log("vif1 MPG %X words %u first %08X from %p\n", v->addr, take, words[i], words + i);
            for (k = 0; k < take; k++, v->addr += 4)
                if (memcmp(m + (v->addr % size), &words[i + k], 4)) {
                    memcpy(m + (v->addr % size), &words[i + k], 4);
#if !defined(DHD_VIF0_ONLY)
                    if (unit) ps2_vu1_code_version++;
#endif
                    if (!unit) ps2_vu0_code_version++;  /* the native VU0 routines match again (vu0_interp.c) */
                }
        } else if ((cmd & 0x60) == 0x60)
            unpack_run(v, words + i, take);
        else if (cmd < DIRECT)
            for (uint32_t k = 0; k < take; k++)
                v->data[v->have + k] = words[i + k];
#if !defined(DHD_VIF0_ONLY)
        else if (unit == 1 && (cmd == DIRECT || cmd == DIRECTHL))
            direct_gif(words + i, take);
#endif
        v->have += take;
        v->need -= take;
        i += take;
        if (!v->need)
            command_done(v);
    }
}

#if !defined(DHD_VIF0_ONLY)
/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_vif_feed(int a0, uint32_t a1, uint32_t a2) { vif_feed(a0, (const uint32_t *)GUEST(a1), a2); }

int host_vu1_snapshot(uint32_t address) {
    /* wasm32 ps2_cpu has two 4-byte pointers between these fixed-width spans.
       Those pointer words are zero: trace serialization excludes them. */
    _Static_assert(offsetof(ps2_cpu,vu0_code)==(sizeof(void *)==8 ? 1896 : 1892),"VU register prefix layout");
    if (!ps2_vu1_regs) return 0;
    unsigned char *out=GUEST(address);
    memset(out,0,2096);
    memcpy(out,&vu1,1892);
    memcpy(out+1900,vu1.cop0,196);
    return 1;
}
void host_vu1_file(uint32_t address) { ps2_vu0_vu1_file(GUEST(address)); }
#endif
