/* Per-field state trace (build/audit/h4-state-trace.md, section 7; finding 081 phase 1).

   DHD_STATE_TRACE=<prefix> writes <prefix>.tsv: a header line, a column line, then one row per
   game field (vsync). Each section is hashed with XXH64 over an explicit little-endian
   serialization, field by field (never a memcpy of a host struct: padding and pointers differ
   between builds), so the same machine state gives the same hash in every build.

   Host addresses are normalised by value. The game copies host values into its memory and
   registers (uninitialised words, saved registers that held host pointers; findings 041, 081):
   stack, heap and DLL addresses that move with Windows' ASLR. Every 32-bit word whose value
   points into host-owned memory is replaced by 0x57AC57AC before hashing and dumping. Host-owned
   means, from a VirtualQuery walk of the address space each field: private or mapped memory
   (stacks, heaps, VirtualAlloc) and other modules' images, but not the PS2 memory (RAM, its
   mirrors and low view, scratchpad, VU and register pages) and not dhd.exe's own image, which
   is at a fixed address. The map has 64 KB granularity (Windows' allocation granularity). The
   count of replaced words per field is the stackwords column (not part of combined). Risk: a
   genuine game value that happens to equal a host address is hidden too; such values are rare
   and mostly far from game data (floats near 1e-36, integers in the tens of millions).

   Env (all optional):
     DHD_STATE_TRACE=<prefix>  the TSV, and the prefix of DHD_STATE_DUMP files
     DHD_STATE_DUMP=<f,f,...>  raw section files for those fields: <prefix>-f<field>-<section>.bin
     DHD_STATE_MASK=<file>     EE word addresses to hash as zero: one per line, "0xADDR reason"
     DHD_STATE_CODEPTRS=<file> EE word addresses that hold code addresses (see above)
     DHD_STATE_NORM=host       normalise every host-owned address (the VirtualQuery map above)
     DHD_STATE_NORM=fibers     normalise only addresses in the fixed EE thread stacks (0x06000000,
                               16 MB), in ram, scratch, hw and cpu: the same rule in every build
     DHD_STATE_GPRS=1          <prefix>.gprs.tsv: per field, the raw GPR lanes and cpu's lane-0
                               classification, to prove a cpu difference is classification only
     DHD_STATE_STOP=<field>    end the process after that field's row (the run's length)
     DHD_STOP_FIELD=<field>    without DHD_STATE_TRACE: end after that field, nothing traced (timing)
     DHD_STATE_EXE=<sha256>    the executable's hash, copied into the header (the runner sets it)
     DHD_STATE_NOWAIT=1        no wait for the graphics workers at each row, so a deferred vsync
                               overlaps the fields as in play (finding 090 HO1): only the EE's own
                               sections (ram, scratch, cpu, sound, code) are hashed, the others are 0

   Sections: ram (32 MB of EE RAM), scratch (16 KB), hw (the EE register pages 0x10000000 and
   0x12000000), cpu (ps2_cpu: gpr[0..31] and the registers), vu0 (VU0 and VU1 micro and data
   memory, 0x11000000 to 0x11010000, and VU1's register file), gs_stream (every packet fed to
   gs_feed this field, in call order), gs_regs (GS registers and path state, not the renderer's
   bookkeeping), gs_vram (the software VRAM, 4 MB, not normalised), sound (the PCM rendered this
   field and the rendered sample count), and the ids: field, menu, phase, race clock, next_vsync
   (in combined). The combined column hashes all section values and the ids, in order.

   Code pointers (finding 081, phase 2): some EE words hold host code addresses (function pointers
   of the data image and callbacks the game stores). Their values move with any code change, so
   they are left out of ram (hashed as zero) and hashed in the code section by identity instead:
   the function that contains the value (a stable id of its name, from the link-time table
   port/tools/codeids.py writes into dhd.exe) and the offset inside it; a value in no function is
   hashed raw. The words are the data image's fixups whose value lies in dhd.exe's .text, plus
   the addresses listed in DHD_STATE_CODEPTRS (one per line, "0xADDR reason"; the runner passes
   port/tools/state_trace_codeptrs.txt). A listed address in the scratchpad (0x70000000..
   0x70003FFF) is a scratchpad word: zeroed in scratch, its identity hashed in code after the
   RAM words. The count of such words is printed at start.

   Wall-clock and host state are never hashed (timers, pacing, histograms, host pointers). */
#include "state_trace.h"

#include "hle.h"
#include "../gs/gs.h"
#include "ps2asm.h"
#include "memory.h"

#include "../host/host_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "../gs/xxhash.h"

uint64_t snd_test_position(void); /* sound.c: samples rendered so far */

#define RAM_SIZE (32u << 20)
#define SCRATCH_SIZE (16u << 10)
#define HW_SIZE_A (64u << 10)  /* 0x10000000: timers, INTC, DMA */
#define HW_SIZE_B (8u << 10)   /* 0x12000000: GS privileged registers */
#define VU_MEMORY (64u << 10)  /* 0x11000000: VU0 micro (4 KB), VU0 data (4 KB), VU1 micro (16 KB), VU1 data (16 KB) */
#define STACK_WORD 0x57AC57ACu  /* what a host address is replaced with */
#define MENU_ADDR 0x0829DED8u  /* the same addresses autopad.c and fps.c read */
#define PHASE_ADDR 0x0843F348u
#define CLOCK_ADDR 0x0843EEC8u

/* The link-time code table (codeids.py). */
extern const uint32_t dhd_code_count;
extern const uint32_t dhd_code_table[][3];  /* {start, size, id} by start */
extern const uint32_t dhd_data_count;
extern const uint32_t dhd_data_table[][3];  /* the data and bss symbols, same rows */

/* Growable byte buffer for explicit serialization. */
typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void buf_reset(Buf *b) { b->n = 0; }
static void buf_bytes(Buf *b, const void *data, size_t size)
{
    if (b->n + size > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + size) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) { fprintf(stderr, "state-trace: out of memory\n"); host_exit(3); }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, size);
    b->n += size;
}

/* Host-owned 64 KB granules of the 4 GB address space, rebuilt each field, and how many words
   were replaced in this field. */
static uint8_t host_granule[65536];
static uint64_t stack_words;
static int norm_fibers;  /* DHD_STATE_NORM=fibers: ram, scratch, hw and cpu only (no host pointer reaches VU or GS) */
static int norm_off;     /* set around the vu0 and gs_regs serializations under norm_fibers */

static int ps2_owned(uintptr_t a)
{
    return a < PS2_RAM_SIZE  /* the low view (PS2_LOW_VIEW up) and the null page below it */
        || (a >= PS2_RAM_BASE && a < PS2_RAM_BASE + PS2_RAM_SIZE)
        || (a >= (0x20000000u | PS2_RAM_BASE) && a < (0x20000000u | PS2_RAM_BASE) + PS2_RAM_SIZE)
        || (a >= (0x30000000u | PS2_RAM_BASE) && a < (0x30000000u | PS2_RAM_BASE) + PS2_RAM_SIZE)
        || (a >= 0x10000000u && a < 0x10010000u) || (a >= 0x11000000u && a < 0x11010000u)
        || (a >= 0x12000000u && a < 0x12002000u)
        || (a >= PS2_SCRATCHPAD && a < PS2_SCRATCHPAD + SCRATCH_SIZE);
}

/* Each host memory region's 64 KB granules that PS2 memory doesn't own are host addresses. */
static void mark_host_region(uint32_t ctx, uintptr_t base, uintptr_t end, int host)
{
    if (!host) return;
    for (uintptr_t g = base & ~(uintptr_t)0xFFFF; g < end && g >= (base & ~(uintptr_t)0xFFFF); g += 0x10000)
        if (!ps2_owned(g)) host_granule[g >> 16] = 1;
}

static void map_host_memory(void)
{
    memset(host_granule, 0, sizeof host_granule);
    const char *norm = getenv("DHD_STATE_NORM");
    if (norm && !strcmp(norm, "fibers")) {
        /* Only the fixed EE thread stacks of a state trace (host_fiber_create, 0x06000000, 16 MB),
           at the same addresses in every build: a C local's address that the game keeps (a stale
           pointer, a register spilled by recompiled code) depends on the compiler's frame layout. */
        for (uint32_t g = 0x0600; g < 0x0700; g++) host_granule[g] = 1;
        norm_fibers = 1;
        return;
    }
    if (!norm || strcmp(norm, "host")) return;  /* off unless asked for */
    host_callback_memory_regions(HOST_CALLBACK(mark_host_region), 0);
}

/* A 32-bit word, with a host address replaced (and counted). */
static uint32_t norm32(uint32_t v)
{
    if (norm_off || !host_granule[v >> 16]) return v;
    stack_words++;
    return STACK_WORD;
}
/* Replaces the normalised words of a little-endian byte range in place (size a multiple of 4). */
static void norm_bytes(uint8_t *p, size_t size)
{
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t v;
        memcpy(&v, p + i, 4);
        v = norm32(v);
        memcpy(p + i, &v, 4);
    }
}

static struct {
    int on;
    int nowait;                 /* DHD_STATE_NOWAIT */
    FILE *tsv;
    uint32_t *mask;             /* sorted EE word offsets (address & 0x1FFFFFC), zeroed in ram */
    size_t mask_count;
    unsigned long long dump[64];
    unsigned dump_count;
    int dumping;                /* any DHD_STATE_DUMP field: keep the field's PCM and packets */
    unsigned long long stop;    /* 0: run until killed */
    char prefix[400];
    XXH64_state_t pcm, packets; /* this field's streams; reset at each row */
    Buf pcm_log, packet_log;    /* this field's bytes, for DHD_STATE_DUMP only */
    uint8_t *ram_copy;          /* normalised copy of EE RAM (32 MB) */
    uint8_t scratch_copy[SCRATCH_SIZE];
    Buf cpu, vu, gs, code, ids; /* section serializations, reused each field */
} st;

static void read_mask(const char *path)
{
    FILE *f = fopen(path, "r");
    size_t cap = 0;
    if (!f) { fprintf(stderr, "state-trace: cannot read DHD_STATE_MASK %s\n", path); host_exit(3); }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *end;
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        unsigned long address = strtoul(s, &end, 0);
        if (end == s) continue;
        if (st.mask_count == cap) {
            cap = cap ? cap * 2 : 64;
            st.mask = realloc(st.mask, cap * sizeof *st.mask);
        }
        st.mask[st.mask_count++] = (uint32_t)address & 0x1FFFFFCu;
    }
    fclose(f);
    /* sort, then drop duplicates */
    for (size_t i = 1; i < st.mask_count; i++) {  /* insertion sort: a short list */
        uint32_t v = st.mask[i];
        size_t j = i;
        while (j > 0 && st.mask[j - 1] > v) { st.mask[j] = st.mask[j - 1]; j--; }
        st.mask[j] = v;
    }
    size_t out = 0;
    for (size_t i = 0; i < st.mask_count; i++)
        if (!out || st.mask[out - 1] != st.mask[i]) st.mask[out++] = st.mask[i];
    st.mask_count = out;
    fprintf(stderr, "state-trace: %zu EE words masked by %s\n", st.mask_count, path);
}

/* The code-pointer words: sorted EE word offsets (address & 0x1FFFFFC) in PS2 RAM. */
static uint32_t *code_words;
static size_t code_word_count;
static FILE *ptrs_log;      /* <prefix>.ptrs.tsv: a code word's identity and offset when they change */
static FILE *gprs_log;      /* <prefix>.gprs.tsv (DHD_STATE_GPRS): each field's raw GPR lanes, how cpu
                               classified lane 0, and the hash of cpu after the GPRs */
static uint32_t *ptrs_last;  /* 3 per code word: the identity triple last written */
/* Listed code-pointer words in the scratchpad (0x70000000..0x70003FFF): byte offsets into it. */
static uint32_t scratch_code_words[64];
static size_t scratch_code_count;

static void code_word_add(uint32_t address)
{
    static size_t cap;
    if (code_word_count == cap) {
        cap = cap ? cap * 2 : 1024;
        uint32_t *p = realloc(code_words, cap * sizeof *code_words);
        if (!p) { fprintf(stderr, "state-trace: out of memory\n"); host_exit(3); }
        code_words = p;
    }
    code_words[code_word_count++] = address & 0x1FFFFFCu;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void code_pointers_init(void)
{
    /* Small absolute data constants can equal function-table indices. Only
       function-valued relocations establish pointer provenance here. */
    extern const uint32_t dhd_code_fixup_count, dhd_code_fixups[];
    size_t fixups = dhd_code_fixup_count;
    for (size_t i = 0; i < fixups; i++) code_word_add(dhd_code_fixups[i]);
    size_t listed = 0;
    const char *path = getenv("DHD_STATE_CODEPTRS");
    if (path && *path) {
        FILE *f = fopen(path, "r");
        if (!f) { fprintf(stderr, "state-trace: cannot read DHD_STATE_CODEPTRS %s\n", path); host_exit(3); }
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *end;
            const char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
            unsigned long address = strtoul(s, &end, 0);
            if (end == s) continue;
            if (address >= 0x70000000ul && address < 0x70000000ul + SCRATCH_SIZE) {
                if (scratch_code_count == sizeof scratch_code_words / sizeof *scratch_code_words) {
                    fprintf(stderr, "state-trace: too many scratchpad code words\n"); host_exit(3);
                }
                scratch_code_words[scratch_code_count++] = (uint32_t)address & (SCRATCH_SIZE - 4);
            } else
                code_word_add((uint32_t)address);
            listed++;
        }
        fclose(f);
    }
    qsort(code_words, code_word_count, sizeof *code_words, cmp_u32);
    size_t out = 0;
    for (size_t i = 0; i < code_word_count; i++)
        if (!out || code_words[out - 1] != code_words[i]) code_words[out++] = code_words[i];
    code_word_count = out;
    fprintf(stderr, "state-trace: %zu code-address words (%zu from the fixups, %zu listed; %u table symbols)\n",
            code_word_count, fixups, listed, (unsigned)dhd_code_count);
}

/* A host address by identity: tag 1 for a code symbol (dhd_code_table), tag 2 for a data or bss
   symbol (dhd_data_table), each with the symbol's id and the offset inside it; tag 0 with the raw
   value when no symbol contains it. */
static int in_symbol_table(const uint32_t (*table)[3], uint32_t count, uint32_t v, uint32_t *id, uint32_t *offset)
{
    size_t lo = 0, hi = count;  /* the last entry whose start is <= v */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (table[mid][0] <= v) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return 0;
    uint32_t start = table[lo - 1][0], size = table[lo - 1][1];
    if (v - start >= size) return 0;
    *id = table[lo - 1][2];
    *offset = v - start;
    return 1;
}

static void code_identity(uint32_t v, uint32_t *tag, uint32_t *id, uint32_t *offset)
{
    *tag = 0; *id = v; *offset = 0;
    if (in_symbol_table(dhd_code_table, dhd_code_count, v, id, offset)) { *tag = 1; return; }
    if (in_symbol_table(dhd_data_table, dhd_data_count, v, id, offset)) { *tag = 2; return; }
}

static void parse_dump(const char *list)
{
    while (*list && st.dump_count < 64) {
        char *end;
        unsigned long long field = strtoull(list, &end, 10);
        if (end == list) break;
        st.dump[st.dump_count++] = field;
        list = *end == ',' ? end + 1 : end;
    }
    st.dumping = st.dump_count > 0;
}

static int dump_wanted(uint64_t field)
{
    for (unsigned i = 0; i < st.dump_count; i++)
        if (st.dump[i] == field) return 1;
    return 0;
}

/* The environment that the determinism rules depend on: copied into the header line. */
static const char *header_env[] = {
    "DHD_FRAME_CLOCK", "DHD_HEADLESS", "DHD_SKIP_MOVIES", "DHD_MEMCARD", "DHD_SND_OFF", "DHD_RENDER",
    "DHD_RACE_SEED", "DHD_NAV", "DHD_AUTOPAD", "DHD_AUTOPAD_FRAMES", "DHD_RACE_SYNC", "DHD_RACE_PAD",
    "DHD_RACE_PAD1", "DHD_RACE_PAD2", "DHD_RACE_PAD3", "DHD_PADS", "DHD_FPS", "DHD_STEP_CLOCK", "DHD_LATENCY",
    "DHD_INTERP", "DHD_KEYS", "DHD_GS_CAPTURE", "DHD_HASH_LOG", "DHD_PROFILE", "DHD_STATE_STOP",
    "DHD_STATE_MASK", "DHD_STATE_CODEPTRS", "DHD_STATE_NORM", "DHD_STATE_NOWAIT", "DHD_SYNC_STRICT",
    "DHD_KEYBOARD", "DHD_PAD1", "DHD_PAD2", "DHD_TEST_PAD", "DHD_TEST_PAD2", NULL};

const char *state_trace_watch_spec(void)
{
    const char *w = getenv("DHD_STATE_WATCH");
    return getenv("DHD_STATE_TRACE") && w && *w ? w : NULL;
}

void state_trace_init(void)
{
    const char *prefix = getenv("DHD_STATE_TRACE");
    if (!prefix || !*prefix) return;
    snprintf(st.prefix, sizeof st.prefix, "%s", prefix);
    char path[512];
    snprintf(path, sizeof path, "%s.tsv", prefix);
    st.tsv = fopen(path, "wb");
    if (!st.tsv) { fprintf(stderr, "state-trace: cannot write %s\n", path); host_exit(3); }
    setvbuf(st.tsv, NULL, _IOFBF, 1 << 20);
    if (getenv("DHD_STATE_MASK")) read_mask(getenv("DHD_STATE_MASK"));
    code_pointers_init();
    snprintf(path, sizeof path, "%s.ptrs.tsv", prefix);
    ptrs_log = fopen(path, "wb");
    if (!ptrs_log) { fprintf(stderr, "state-trace: cannot write %s\n", path); host_exit(3); }
    fprintf(ptrs_log, "field\taddr\ttag\tid\toffset\n");
    if (getenv("DHD_STATE_GPRS") && *getenv("DHD_STATE_GPRS")) {
        snprintf(path, sizeof path, "%s.gprs.tsv", prefix);
        gprs_log = fopen(path, "wb");
        if (!gprs_log) { fprintf(stderr, "state-trace: cannot write %s\n", path); host_exit(3); }
        fprintf(gprs_log, "field\trest\tr0..r31 as lane0:tag:id:offset:lane1:lane2:lane3\n");
    }
    ptrs_last = malloc((code_word_count ? code_word_count : 1) * 3 * sizeof *ptrs_last);
    if (!ptrs_last) { fprintf(stderr, "state-trace: out of memory\n"); host_exit(3); }
    for (size_t i = 0; i < 3 * code_word_count; i++) ptrs_last[i] = 0xFFFFFFFFu;
    if (getenv("DHD_STATE_DUMP")) parse_dump(getenv("DHD_STATE_DUMP"));
    if (getenv("DHD_STATE_STOP")) st.stop = strtoull(getenv("DHD_STATE_STOP"), NULL, 10);
    st.ram_copy = malloc(RAM_SIZE);
    if (!st.ram_copy) { fprintf(stderr, "state-trace: out of memory\n"); host_exit(3); }
    XXH64_reset(&st.pcm, 0);
    XXH64_reset(&st.packets, 0);
    st.on = 1;
    st.nowait = getenv("DHD_STATE_NOWAIT") && *getenv("DHD_STATE_NOWAIT");
    if (!st.nowait) host_service_gs_trace_begin();

    fprintf(st.tsv, "# state-trace 1 exe=%s prefix=%s env=", getenv("DHD_STATE_EXE") ? getenv("DHD_STATE_EXE") : "-",
            prefix);
    for (int i = 0; header_env[i]; i++) {
        const char *v = getenv(header_env[i]);
        if (v && *v) fprintf(st.tsv, "%s=%s ", header_env[i], v);
    }
    fprintf(st.tsv, "\n");
    fprintf(st.tsv, "field\tmenu\tphase\tclock\tram\tscratch\thw\tcpu\tvu0\tgs_stream\tgs_regs\tgs_vram\tsound\tcode\tcombined\tstackwords\n");
    fprintf(stderr, "state-trace: writing %s\n", path);
}

void state_trace_pcm(const int16_t *samples, size_t frames)
{
    if (!st.on) return;
    XXH64_update(&st.pcm, samples, frames * 4);
    if (st.dumping) buf_bytes(&st.pcm_log, samples, frames * 4);
}

void state_trace_gs_packet(struct Gs *g, unsigned path, const uint32_t *words, size_t count)
{
    (void)g;
    if (!st.on) return;
    uint8_t head[12];
    uint32_t p = path;
    uint64_t n = count;
    memcpy(head, &p, 4);
    memcpy(head + 4, &n, 8);
    XXH64_update(&st.packets, head, sizeof head);
    XXH64_update(&st.packets, words, count * 4);
    if (st.dumping) {
        buf_bytes(&st.packet_log, head, sizeof head);
        buf_bytes(&st.packet_log, words, count * 4);
    }
}

/* ---- section serializations: every 32-bit value goes through norm32 ---- */

static void put8(Buf *b, uint8_t v) { buf_bytes(b, &v, 1); }
static void put16(Buf *b, uint16_t v) { uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; buf_bytes(b, t, 2); }
static void put32raw(Buf *b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    buf_bytes(b, t, 4);
}
static void put32(Buf *b, uint32_t v) { put32raw(b, norm32(v)); }
static void put64(Buf *b, uint64_t v) { put32(b, (uint32_t)v); put32(b, (uint32_t)(v >> 32)); }
static void put64raw(Buf *b, uint64_t v) { put32raw(b, (uint32_t)v); put32raw(b, (uint32_t)(v >> 32)); }
static void putf(Buf *b, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);  /* the bit pattern, not the value: NaN payloads and -0 count */
    put32(b, bits);
}
static void put_q128(Buf *b, const ps2_q128 *q)
{
    for (int i = 0; i < 4; i++) put32(b, q->w[i]);  /* the four 32-bit lanes */
}

/* A MIPS register: its low lane is hashed by identity when it holds a host code address (a function
   the game got as an argument; it moves with any code change, as the code pointers in memory do),
   and raw otherwise. */
static void put_gpr(Buf *b, const ps2_q128 *q)
{
    uint32_t tag, id, offset;
    code_identity(q->w[0], &tag, &id, &offset);
    put32raw(b, tag);
    if (tag) { put32raw(b, id); put32raw(b, offset); }
    else put32(b, q->w[0]);
    for (int i = 1; i < 4; i++) put32(b, q->w[i]);
}

static void serialize_cpu(Buf *b, const ps2_cpu *c)
{
    buf_reset(b);
    /* gpr[0..31] are the MIPS registers. gpr[32..63] stand for the asm operands of one template
       (ps2asm.h), so they are scratch between fields, and they can hold host stack pointers.
       Not hashed. */
    for (int i = 0; i < 32; i++) put_gpr(b, &c->gpr[i]);
    put64(b, c->lo); put64(b, c->hi); put64(b, c->lo1); put64(b, c->hi1);
    for (int i = 0; i < 64; i++) putf(b, c->fpr[i]);
    putf(b, c->fpu_acc);
    for (int i = 0; i < 32; i++) put_q128(b, &c->vf[i]);
    for (int i = 0; i < 4; i++) putf(b, c->vacc[i]);
    for (int i = 0; i < 16; i++) put16(b, c->vi[i]);
    putf(b, c->q);
    putf(b, c->i);
    put32(b, c->r);
    for (int i = 0; i < 32; i++) put32(b, c->cop0[i]);
    for (int i = 0; i < 16; i++) put32(b, c->vu0_ctrl[i]);
    putf(b, c->p);
    /* vu0_code and vu0_mem are pointers: their contents are in the vu0 section */
}

static void serialize_gs(Buf *b, const Gs *g)
{
    buf_reset(b);
    for (int i = 0; i < 128; i++) put64(b, g->reg[i]);
    for (size_t i = 0; i < sizeof g->privileged / sizeof g->privileged[0]; i++) put64(b, g->privileged[i]);
    for (int i = 0; i < 1024; i++) put16(b, g->clut[i]);
    put16(b, g->cbp[0]); put16(b, g->cbp[1]);
    for (int p = 0; p < 3; p++) {
        const GsPath *s = &g->path[p];
        for (int i = 0; i < 4; i++) put32(b, s->words[i]);
        put32(b, s->have); put32(b, s->left); put32(b, s->nreg); put32(b, s->reg); put32(b, s->mode);
        put32(b, s->eop); put64(b, s->regs); put32(b, s->q);
    }
    put64(b, g->transfer_bits);
    put32(b, g->transfer_count); put32(b, g->transfer_have);
    for (int v = 0; v < 4; v++) {
        const GsVertex *x = v < 1 ? &g->vertex : &g->vertices[v - 1];
        put32(b, x->x); put32(b, x->y); put32(b, x->z); put32(b, x->rgba); put32(b, x->fog);
        put32(b, x->u); put32(b, x->v); putf(b, x->s); putf(b, x->t); putf(b, x->q);
    }
    put32(b, g->vertex_count); put32(b, g->strip_parity);
    for (int i = 0; i < 3; i++) put64(b, g->packets[i]);
    put64(b, g->kicks); put64(b, g->uploads); put64(b, g->pixels_written); put64(b, g->state_version);
}

/* Raw section bytes; returns the XXH64 and leaves the bytes in the buffer for dumps. */
static uint64_t hash_buf(const Buf *b) { return XXH64(b->p, b->n, 0); }

static void write_file(uint64_t field, const char *name, const void *data, size_t size)
{
    char path[512];
    snprintf(path, sizeof path, "%s-f%llu-%s", st.prefix, (unsigned long long)field, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "state-trace: cannot write %s\n", path); return; }
    fwrite(data, 1, size, f);
    fclose(f);
}

void state_trace_field(uint64_t field, uint64_t next_vsync)
{
    /* DHD_STOP_FIELD=N without DHD_STATE_TRACE: end at the same field as DHD_STATE_STOP=N, with
       nothing hashed or written (timing runs of the trace recipe). */
    static long long stop_field = -1;
    if (stop_field < 0) {
        const char *s = getenv("DHD_STOP_FIELD");
        stop_field = s && *s ? strtoll(s, NULL, 10) : 0;
    }
    if (!st.on && stop_field && field >= (uint64_t)stop_field) host_exit(0);
    if (!st.on) return;
    static Gs snapshot;
    if (!st.nowait) {
        host_service_graphics_wait();  /* every packet fed this field has reached the GS state */
        /* Queue retirement above publishes every copied packet and the GS state.
           Replay the original hash operation on the module's current wasm stack. */
        host_service_gs_snapshot(GADDR(&snapshot), (uint32_t)offsetof(Gs,sync));
        uint32_t packet_bytes;
        while ((packet_bytes=host_service_gs_trace_size())!=0) {
            uint32_t *packet=malloc(packet_bytes);
            if (!packet) abort();
            host_service_gs_trace_pop(GADDR(packet));
            state_trace_gs_packet(NULL,packet[0],packet+2,packet[1]);
            free(packet);
        }
    }
    int32_t menu = *(volatile int32_t *)(uintptr_t)MENU_ADDR;
    int32_t phase = *(volatile int32_t *)(uintptr_t)PHASE_ADDR;
    int32_t clock = *(volatile int32_t *)(uintptr_t)CLOCK_ADDR;

    /* the host-owned memory as it stands now */
    map_host_memory();
    stack_words = 0;

    int dump_now = st.dumping && dump_wanted(field);

    /* ram: a normalised copy (the hashed bytes; dumped as ee.bin), then the masked words and the
       code-pointer words zeroed (the code section hashes those). ee-raw.bin keeps them. */
    const uint8_t *ram = st.ram_copy;
    memcpy(st.ram_copy, (const void *)(uintptr_t)PS2_PTR(0), RAM_SIZE);
    norm_bytes(st.ram_copy, RAM_SIZE);
    uint8_t *ram_raw = NULL;
    if (dump_now) {
        ram_raw = malloc(RAM_SIZE);
        if (!ram_raw) { fprintf(stderr, "state-trace: out of memory\n"); host_exit(3); }
        memcpy(ram_raw, st.ram_copy, RAM_SIZE);
    }
    for (size_t i = 0; i < st.mask_count; i++) memset(st.ram_copy + st.mask[i], 0, 4);
    for (size_t i = 0; i < code_word_count; i++) memset(st.ram_copy + code_words[i], 0, 4);
    uint64_t h_ram = XXH64(ram, RAM_SIZE, 0);

    /* code: the code-pointer words by identity (their raw values are never hashed). The identity is
       the symbol and its tag, not the offset inside it: a return address inside a function may sit
       at another offset in another build of the same function. Offsets go to <prefix>.ptrs.tsv,
       when they change, so a difference stays visible. */
    Buf code_dump = {0, 0, 0};
    buf_reset(&st.code);
    for (size_t i = 0; i < code_word_count; i++) {
        uint32_t addr = code_words[i], v, tag, id, offset;
        memcpy(&v, (const void *)(uintptr_t)(PS2_RAM_BASE + addr), 4);
        code_identity(v, &tag, &id, &offset);
        put32raw(&st.code, addr); put32raw(&st.code, tag); put32raw(&st.code, id);
        uint32_t *last = &ptrs_last[3 * i];
        if (last[0] != tag || last[1] != id || last[2] != offset) {
            fprintf(ptrs_log, "%llu\t%08X\t%u\t%08X\t%X\n", (unsigned long long)field, addr, tag, id, offset);
            last[0] = tag; last[1] = id; last[2] = offset;
        }
        if (dump_now) {
            char row[96];
            snprintf(row, sizeof row, "%08X\t%08X\t%u\t%08X\t%X\n", addr, v, tag, id, offset);
            buf_bytes(&code_dump, row, strlen(row));
        }
    }
    for (size_t i = 0; i < scratch_code_count; i++) {
        uint32_t v, tag, id, offset;
        memcpy(&v, (const void *)(uintptr_t)(PS2_SCRATCHPAD + scratch_code_words[i]), 4);
        code_identity(v, &tag, &id, &offset);
        put32raw(&st.code, 0x70000000u | scratch_code_words[i]); put32raw(&st.code, tag); put32raw(&st.code, id);
    }
    uint64_t h_code = hash_buf(&st.code);

    /* scratch */
    memcpy(st.scratch_copy, (const void *)(uintptr_t)PS2_SCRATCHPAD, SCRATCH_SIZE);
    norm_bytes(st.scratch_copy, SCRATCH_SIZE);
    for (size_t i = 0; i < scratch_code_count; i++) memset(st.scratch_copy + scratch_code_words[i], 0, 4);
    uint64_t h_scratch = XXH64(st.scratch_copy, SCRATCH_SIZE, 0);

    /* hw: the two register pages, normalised words */
    buf_reset(&st.vu);
    buf_bytes(&st.vu, (const void *)(uintptr_t)0x10000000u, HW_SIZE_A);
    buf_bytes(&st.vu, (const void *)(uintptr_t)0x12000000u, HW_SIZE_B);
    norm_bytes(st.vu.p, st.vu.n);
    uint64_t h_hw = hash_buf(&st.vu);
    Buf hw_bytes = {0, 0, 0};
    if (dump_now) { buf_bytes(&hw_bytes, st.vu.p, st.vu.n); }

    /* cpu */
    serialize_cpu(&st.cpu, &ps2);
    uint64_t h_cpu = hash_buf(&st.cpu);

    /* vu0: VU memory (raw, 0x11000000 to 0x11010000, normalised) then VU1's register file */
    buf_reset(&st.vu);
    buf_bytes(&st.vu, (const void *)(uintptr_t)0x11000000u, VU_MEMORY);
    norm_off = norm_fibers;
    norm_bytes(st.vu.p, st.vu.n);
    {
        Buf regs = {0, 0, 0};
        _Static_assert(offsetof(ps2_cpu,vu0_code)==1892 && offsetof(ps2_cpu,cop0)==1900,"wasm32 VU snapshot layout");
        ps2_cpu vu1_snapshot;
        if (host_vu1_snapshot(GADDR(&vu1_snapshot))) serialize_cpu(&regs,&vu1_snapshot);
        buf_bytes(&st.vu, regs.p, regs.n);
        free(regs.p);
    }
    uint64_t h_vu0 = hash_buf(&st.vu);

    /* gs_stream and gs_regs and gs_vram */
    XXH64_hash_t h_stream = XXH64_digest(&st.packets);
    XXH64_reset(&st.packets, 0);
    serialize_gs(&st.gs, &snapshot);
    norm_off = 0;
    uint64_t h_gs_regs = hash_buf(&st.gs);
    uint64_t h_gs_vram = XXH64(snapshot.vram, sizeof snapshot.vram, 0);
    /* DHD_STATE_NOWAIT: the graphics workers may still own these (the CSR page may be provisional) */
    if (st.nowait) h_hw = h_vu0 = h_stream = h_gs_regs = h_gs_vram = 0;

    /* sound: the PCM of this field, and the rendered sample count (not normalised: no host values) */
    uint64_t pcm_digest = XXH64_digest(&st.pcm);
    XXH64_reset(&st.pcm, 0);
    Buf snd = {0, 0, 0};
    put32raw(&snd, (uint32_t)pcm_digest); put32raw(&snd, (uint32_t)(pcm_digest >> 32));
    put32raw(&snd, (uint32_t)snd_test_position()); put32raw(&snd, (uint32_t)(snd_test_position() >> 32));
    uint64_t h_sound = hash_buf(&snd);
    free(snd.p);

    /* combined: the section values, then the ids (raw: they are not host values) */
    Buf all = {0, 0, 0};
    put64raw(&all, h_ram); put64raw(&all, h_scratch); put64raw(&all, h_hw); put64raw(&all, h_cpu); put64raw(&all, h_vu0);
    put64raw(&all, h_stream); put64raw(&all, h_gs_regs); put64raw(&all, h_gs_vram); put64raw(&all, h_sound);
    put64raw(&all, h_code);
    put32raw(&all, (uint32_t)field); put32raw(&all, (uint32_t)(field >> 32));
    put32raw(&all, (uint32_t)menu); put32raw(&all, (uint32_t)phase); put32raw(&all, (uint32_t)clock);
    put32raw(&all, (uint32_t)next_vsync); put32raw(&all, (uint32_t)(next_vsync >> 32));
    uint64_t h_combined = hash_buf(&all);
    free(all.p);

    fprintf(st.tsv, "%llu\t%d\t%d\t%d\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%llu\n",
            (unsigned long long)field, menu, phase, clock, (unsigned long long)h_ram, (unsigned long long)h_scratch,
            (unsigned long long)h_hw, (unsigned long long)h_cpu, (unsigned long long)h_vu0,
            (unsigned long long)h_stream, (unsigned long long)h_gs_regs, (unsigned long long)h_gs_vram,
            (unsigned long long)h_sound, (unsigned long long)h_code, (unsigned long long)h_combined,
            (unsigned long long)stack_words);

    if (gprs_log) {
        /* put_gpr's sizes: tag, then id and offset or the value, then three lanes */
        size_t gpr_bytes = 0;
        uint32_t tag, id, offset;
        for (int r = 0; r < 32; r++) { code_identity(ps2.gpr[r].w[0], &tag, &id, &offset); gpr_bytes += tag ? 24 : 20; }
        fprintf(gprs_log, "%llu\t%016llx", (unsigned long long)field,
                (unsigned long long)XXH64(st.cpu.p + gpr_bytes, st.cpu.n - gpr_bytes, 0));
        for (int r = 0; r < 32; r++) {
            const uint32_t *w = ps2.gpr[r].w;
            code_identity(w[0], &tag, &id, &offset);
            fprintf(gprs_log, "\t%08X:%u:%08X:%X:%08X:%08X:%08X", w[0], tag, id, offset, w[1], w[2], w[3]);
        }
        fprintf(gprs_log, "\n");
    }

    if (dump_now) {
        write_file(field, "ee.bin", ram, RAM_SIZE);
        write_file(field, "ee-raw.bin", ram_raw, RAM_SIZE);  /* the code-pointer and masked words as they are */
        write_file(field, "code.tsv", code_dump.p, code_dump.n);  /* addr, value, tag, id, offset */
        free(code_dump.p);
        free(ram_raw);
        write_file(field, "scratch.bin", st.scratch_copy, SCRATCH_SIZE);
        write_file(field, "hw.bin", hw_bytes.p, hw_bytes.n);
        write_file(field, "cpu.bin", st.cpu.p, st.cpu.n);
        /* Raw lanes let audits distinguish actual register differences from
           symbol/index classification collisions in the serialized CPU. */
        {
            Buf raw = {0, 0, 0};
            for (int r = 0; r < 32; r++)
                for (int lane = 0; lane < 4; lane++) put32raw(&raw, ps2.gpr[r].w[lane]);
            write_file(field, "gpr-raw.bin", raw.p, raw.n);
            free(raw.p);
        }
        write_file(field, "vu0.bin", st.vu.p, st.vu.n);  /* still holds the vu0 bytes that were hashed */
        {
            Buf gs_bytes = {0, 0, 0};
            serialize_gs(&gs_bytes, &snapshot);
            write_file(field, "gsregs.bin", gs_bytes.p, gs_bytes.n);
            free(gs_bytes.p);
        }
        write_file(field, "gsvram.bin", snapshot.vram, sizeof snapshot.vram);
        write_file(field, "gsstream.bin", st.packet_log.p, st.packet_log.n);
        write_file(field, "sound.pcm", st.pcm_log.p, st.pcm_log.n);
    }
    free(hw_bytes.p);
    buf_reset(&st.packet_log);
    buf_reset(&st.pcm_log);

    if (st.stop && field >= st.stop) {
        fclose(st.tsv);
        st.tsv = NULL;
        if (ptrs_log) fclose(ptrs_log);
        if (gprs_log) fclose(gprs_log);
        st.on = 0;
        host_exit(0);
    }
}
