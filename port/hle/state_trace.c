/* Per-field state trace (finding 081, "The proof: state traces", phase 1).

   DHD_STATE_TRACE=<prefix> writes <prefix>.tsv: a header line, a column line, then one row per
   game field (vsync). Each section is hashed with XXH64 over an explicit little-endian
   serialization, field by field (never a memcpy of a host struct: padding and pointers differ
   between builds), so the same machine state gives the same hash in every build.

   The core's own addresses (finding 205). The core is one wasm32 module: its data, bss, shadow
   stack and heap sit in its linear memory from 0x70100000 up, and a code address is an index into
   its function table (from 0x12100). Where the linker puts them changes with any change to the
   core, so a trace that hashed them raw would differ between builds of the same game. Guest RAM
   holds such values only where a list says so (the code-pointer words below); the HLE gives the
   game no pointer into the core's data (what the game must see sits at a fixed address, memory.h's
   PS2_HLE_WINDOW). Registers are not listed: at a field boundary a register can hold a code
   address, a pointer into the core, or plain data that only looks like one (text, floats). So a
   register lane whose value falls in the core's ranges (CORE_* below: fixed bounds, the same in
   every build) is hashed in cpu as a placeholder, and written to the regptrs column instead: its
   raw value and its identity in this build (tag 1 a function, 2 a data symbol, 3 the shadow
   stack, 4 the heap, each with an id and the offset in it; tag 0 none). state_diff.py counts two
   lanes equal when their raw values are equal or their identities are (a pointer to the same
   object moved with the layout); anything else is a difference. A value that is not a pointer is
   therefore never compared as one: it is equal when it is the same value.

   Env (all optional):
     DHD_STATE_TRACE=<prefix>  the TSV, and the prefix of DHD_STATE_DUMP files
     DHD_STATE_DUMP=<f,f,...>  raw section files for those fields: <prefix>-f<field>-<section>.bin
     DHD_STATE_MASK=<file>     EE word addresses to hash as zero: one per line, "0xADDR reason"
     DHD_STATE_CODEPTRS=<file> EE word addresses that hold code addresses (see above)
     DHD_STATE_NORM=fibers     replace every word whose value lies in the fixed EE thread stacks
                               (0x06000000, 16 MB) by 0x57AC57AC in ram, scratch, hw and cpu, for
                               a comparison between two compilers (their frame layouts differ);
                               the stackwords column counts them. No other value is accepted.
     DHD_STATE_GPRS=1          <prefix>.gprs.tsv: per field, the raw GPR lanes and lane 0's
                               identity, and the hash of cpu after the GPRs
     DHD_STATE_FLIP=<list>     a test of the trace (state_trace_layout_test.py): changes real state
                               just before the named fields are hashed, so the diff against a run
                               without it must report them. Entries "<field>:ram:<EE address>:<xor>"
                               (a RAM word) and "<field>:reg:<GPR>:<xor>" (lane 0 of an EE register),
                               comma-separated
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
   (in combined). The combined column hashes all section values and the ids, in order; regptrs
   (the register lanes in the core's ranges, above) is compared by state_diff.py on its own.

   Code pointers (finding 081, phase 2): some EE words hold host code addresses (function pointers
   of the data image and callbacks the game stores). Their values move with any code change, so
   they are left out of ram (hashed as zero) and hashed in the code section by identity instead:
   the function that contains the value (a stable id of its name, from the link-time table
   port/tools/codeids.py writes into the core) and the offset inside it; a value in no function is
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
#define STACK_WORD 0x57AC57ACu  /* what DHD_STATE_NORM=fibers replaces a fiber stack address with */
#define MENU_ADDR 0x0829DED8u  /* the same addresses autopad.c and fps.c read */
#define PHASE_ADDR 0x0843F348u
#define CLOCK_ADDR 0x0843EEC8u

/* The core's ranges (finding 205): fixed bounds, the same in every build, so whether a value is
   looked up does not depend on the layout. The function table starts at build_wasm.py's
   TABLE_BASE; the module's own memory at HOST64_MODULE_BASE and ends below build_wasm.py's
   MAX_MEMORY. state_trace_init checks every table row lies inside them. */
#define CORE_TABLE_LO 0x00012100u
#define CORE_TABLE_HI 0x00022100u   /* room for 65536 functions */
#define CORE_MEMORY_LO 0x70100000u
#define CORE_MEMORY_HI 0x80000000u
/* wasm-ld's layout symbols: data and bss, then the shadow stack [__stack_low, __stack_high), then
   the heap from __heap_base. */
extern char __stack_low[], __stack_high[], __heap_base[];

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

/* DHD_STATE_NORM=fibers: words that point into the fixed EE thread stacks, replaced, and how many
   in this field (the stackwords column). */
static uint64_t stack_words;
static int norm_fibers;  /* ram, scratch, hw and cpu only (no stack address reaches VU or GS) */
static int norm_off;     /* set around the vu0 and gs_regs serializations */

static uint32_t norm32(uint32_t v)
{
    if (!norm_fibers || norm_off || (v >> 24) != 0x06u) return v;
    stack_words++;
    return STACK_WORD;
}
/* Replaces the normalised words of a little-endian byte range in place (size a multiple of 4). */
static void norm_bytes(uint8_t *p, size_t size)
{
    if (!norm_fibers || norm_off) return;
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

/* A core address by identity: tag 1 for a function (dhd_code_table), tag 2 for a data or bss
   symbol (dhd_data_table), each with the symbol's id and the offset inside it; tag 3 for the shadow
   stack (id 0, the offset below its top: a data change moves the stack, not the frames on it), tag
   4 for the heap (id 0, the offset from its base); tag 0 with the raw value otherwise. */
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
    uint32_t stack_low = (uint32_t)(uintptr_t)__stack_low, stack_high = (uint32_t)(uintptr_t)__stack_high;
    if (v >= stack_low && v < stack_high) { *tag = 3; *id = 0; *offset = stack_high - v; return; }
    if (v >= (uint32_t)(uintptr_t)__heap_base && v < CORE_MEMORY_HI) {
        *tag = 4; *id = 0; *offset = v - (uint32_t)(uintptr_t)__heap_base;
    }
}

/* A value the trace looks up (a register lane): in the function table's or the module memory's
   fixed range. */
static int in_core_range(uint32_t v)
{
    return (v >= CORE_TABLE_LO && v < CORE_TABLE_HI) || (v >= CORE_MEMORY_LO && v < CORE_MEMORY_HI);
}

/* Every identity table row inside the fixed ranges, else the trace would hash a real core address
   raw. */
static void check_core_ranges(void)
{
    for (uint32_t i = 0; i < dhd_code_count; i++)
        if (dhd_code_table[i][0] < CORE_TABLE_LO || dhd_code_table[i][0] + dhd_code_table[i][1] > CORE_TABLE_HI) {
            fprintf(stderr, "state-trace: function table row %08X outside [%08X, %08X)\n", dhd_code_table[i][0],
                    CORE_TABLE_LO, CORE_TABLE_HI);
            host_exit(3);
        }
    for (uint32_t i = 0; i < dhd_data_count; i++)
        if (dhd_data_table[i][0] < CORE_MEMORY_LO) {
            fprintf(stderr, "state-trace: data row %08X below %08X\n", dhd_data_table[i][0], CORE_MEMORY_LO);
            host_exit(3);
        }
    if ((uint32_t)(uintptr_t)__stack_low < CORE_MEMORY_LO || (uint32_t)(uintptr_t)__heap_base < CORE_MEMORY_LO) {
        fprintf(stderr, "state-trace: the shadow stack or the heap below %08X\n", CORE_MEMORY_LO);
        host_exit(3);
    }
}

/* DHD_STATE_FLIP: the changes to make, and at which field. */
static struct { unsigned long long field; int reg; uint32_t address, xor; } flips[16];
static unsigned flip_count;

static void parse_flips(const char *list)
{
    while (*list && flip_count < sizeof flips / sizeof *flips) {
        char kind[8] = {0};
        unsigned long long field;
        unsigned long target, xor;
        int used = 0;
        if (sscanf(list, "%llu:%7[a-z]:%li:%li%n", &field, kind, &target, &xor, &used) != 4
            || (strcmp(kind, "ram") && strcmp(kind, "reg"))) {
            fprintf(stderr, "state-trace: DHD_STATE_FLIP: bad entry at \"%s\"\n", list);
            host_exit(3);
        }
        int reg = !strcmp(kind, "reg");
        if (reg ? target >= 32 : target >= RAM_SIZE) {
            fprintf(stderr, "state-trace: DHD_STATE_FLIP: %s %lu out of range\n", kind, target);
            host_exit(3);
        }
        flips[flip_count].field = field;
        flips[flip_count].reg = reg;
        flips[flip_count].address = (uint32_t)target;
        flips[flip_count].xor = (uint32_t)xor;
        flip_count++;
        list += used;
        if (*list == ',') list++;
    }
}

static void apply_flips(uint64_t field)
{
    for (unsigned i = 0; i < flip_count; i++) {
        if (flips[i].field != field) continue;
        if (flips[i].reg)
            ps2.gpr[flips[i].address].w[0] ^= flips[i].xor;
        else
            *(volatile uint32_t *)PS2_PTR(flips[i].address & ~3u) ^= flips[i].xor;
        fprintf(stderr, "state-trace: DHD_STATE_FLIP at field %llu: %s %u ^= 0x%X\n", (unsigned long long)field,
                flips[i].reg ? "register" : "EE word", (unsigned)flips[i].address, (unsigned)flips[i].xor);
    }
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
    "DHD_STATE_MASK", "DHD_STATE_CODEPTRS", "DHD_STATE_NORM", "DHD_STATE_NOWAIT", "DHD_STATE_FLIP", "DHD_SYNC_STRICT",
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
    const char *norm = getenv("DHD_STATE_NORM");
    if (norm && *norm) {
        if (strcmp(norm, "fibers")) {
            fprintf(stderr, "state-trace: DHD_STATE_NORM=%s: the only value is fibers\n", norm);
            host_exit(3);
        }
        norm_fibers = 1;
    }
    check_core_ranges();
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
        fprintf(gprs_log, "field\trest\tr0..r31 as lane0:tag:id:offset:lane1:lane2:lane3 (raw lanes, lane 0's identity)\n");
    }
    ptrs_last = malloc((code_word_count ? code_word_count : 1) * 3 * sizeof *ptrs_last);
    if (!ptrs_last) { fprintf(stderr, "state-trace: out of memory\n"); host_exit(3); }
    for (size_t i = 0; i < 3 * code_word_count; i++) ptrs_last[i] = 0xFFFFFFFFu;
    if (getenv("DHD_STATE_DUMP")) parse_dump(getenv("DHD_STATE_DUMP"));
    if (getenv("DHD_STATE_FLIP")) parse_flips(getenv("DHD_STATE_FLIP"));
    if (getenv("DHD_STATE_STOP")) st.stop = strtoull(getenv("DHD_STATE_STOP"), NULL, 10);
    st.ram_copy = malloc(RAM_SIZE);
    if (!st.ram_copy) { fprintf(stderr, "state-trace: out of memory\n"); host_exit(3); }
    XXH64_reset(&st.pcm, 0);
    XXH64_reset(&st.packets, 0);
    st.on = 1;
    st.nowait = getenv("DHD_STATE_NOWAIT") && *getenv("DHD_STATE_NOWAIT");
    if (!st.nowait) host_service_gs_trace_begin();

    /* core=: where this build's linker put the core (the shadow stack's top, the heap's base, the
       function table's rows), so a comparison can show the two layouts differed; not compared */
    fprintf(st.tsv, "# state-trace 2 exe=%s core=%08X:%08X:%u prefix=%s env=",
            getenv("DHD_STATE_EXE") ? getenv("DHD_STATE_EXE") : "-", (unsigned)(uintptr_t)__stack_high,
            (unsigned)(uintptr_t)__heap_base, (unsigned)dhd_code_count, prefix);
    for (int i = 0; header_env[i]; i++) {
        const char *v = getenv(header_env[i]);
        if (v && *v) fprintf(st.tsv, "%s=%s ", header_env[i], v);
    }
    fprintf(st.tsv, "\n");
    fprintf(st.tsv, "field\tmenu\tphase\tclock\tram\tscratch\thw\tcpu\tvu0\tgs_stream\tgs_regs\tgs_vram\tsound\tcode\tregptrs\tcombined\tstackwords\n");
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

/* The regptrs column of this field: "<unit><register>.<lane>=<raw>:<tag>:<id>:<offset>" entries,
   comma-separated ("-" when none); unit c is the EE (cpu), v VU1's register snapshot (vu0). */
static Buf regptrs;

/* A MIPS register, lane by lane: a lane in the core's ranges is hashed as the placeholder
   CORE_LANE and goes to regptrs with its raw value and identity; any other lane is hashed as it is.
   Every lane takes 4 bytes, so the serialization's size does not depend on the values. */
#define CORE_LANE 0xC0BEC0BEu
static void put_gpr(Buf *b, const ps2_q128 *q, char unit, int reg)
{
    for (int i = 0; i < 4; i++) {
        uint32_t v = q->w[i];
        if (!in_core_range(v)) { put32(b, v); continue; }
        put32raw(b, CORE_LANE);
        uint32_t tag, id, offset;
        code_identity(v, &tag, &id, &offset);
        char entry[64];
        int n = snprintf(entry, sizeof entry, "%s%c%d.%d=%08X:%u:%08X:%X", regptrs.n ? "," : "", unit, reg, i,
                         (unsigned)v, (unsigned)tag, (unsigned)(tag ? id : 0u), (unsigned)offset);
        buf_bytes(&regptrs, entry, (size_t)n);
    }
}

static void serialize_cpu(Buf *b, const ps2_cpu *c, char unit)
{
    buf_reset(b);
    /* gpr[0..31] are the MIPS registers. gpr[32..63] stand for the asm operands of one template
       (ps2asm.h), so they are scratch between fields, and they can hold host stack pointers.
       Not hashed. */
    for (int i = 0; i < 32; i++) put_gpr(b, &c->gpr[i], unit, i);
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
    apply_flips(field);
    int32_t menu = *(volatile int32_t *)(uintptr_t)MENU_ADDR;
    int32_t phase = *(volatile int32_t *)(uintptr_t)PHASE_ADDR;
    int32_t clock = *(volatile int32_t *)(uintptr_t)CLOCK_ADDR;

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
    buf_reset(&regptrs);
    serialize_cpu(&st.cpu, &ps2, 'c');
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
        if (host_vu1_snapshot(GADDR(&vu1_snapshot))) serialize_cpu(&regs,&vu1_snapshot,'v');
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

    fprintf(st.tsv, "%llu\t%d\t%d\t%d\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%.*s\t%016llx\t%llu\n",
            (unsigned long long)field, menu, phase, clock, (unsigned long long)h_ram, (unsigned long long)h_scratch,
            (unsigned long long)h_hw, (unsigned long long)h_cpu, (unsigned long long)h_vu0,
            (unsigned long long)h_stream, (unsigned long long)h_gs_regs, (unsigned long long)h_gs_vram,
            (unsigned long long)h_sound, (unsigned long long)h_code, regptrs.n ? (int)regptrs.n : 1,
            regptrs.n ? (const char *)regptrs.p : "-", (unsigned long long)h_combined, (unsigned long long)stack_words);

    if (gprs_log) {
        size_t gpr_bytes = 32 * 16;  /* put_gpr: four 4-byte lanes per register */
        uint32_t tag, id, offset;
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
