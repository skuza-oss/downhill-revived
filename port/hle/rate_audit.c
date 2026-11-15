/* Per-update rate audit (finding 090, FPS1). Off unless DHD_RATE_AUDIT=<file> is set.

   Problem it answers: at 240 fps the game runs four real updates per 60 Hz field, and code that
   advances a phase/angle/scale by a constant once per update then runs four times too fast
   (finding 067/090 SP5b fixed the timers that read the decompiled C; these accumulators are
   harder to find by reading). The audit measures them instead.

   Each game update (the game's frame tick, func_00238C00, where the field clock advances) the
   whole 32 MB of EE RAM is compared with the previous update, and each word accumulates its
   absolute change ("travel"). One run at 60 (the retail step) and one at DHD_FPS=240 over a
   window of updates (the same update count times rate/60, so the raw ratio is about rate/60
   wherever the game's update rate scales with the selected rate):
     - a word advanced once per update travels equally far per update at both rates;
     - a word advanced by game time (port_step()/port_whole_step()/port_blend(), finding 090
       SP5b) travels 60/240 = a quarter as far per update at 240.
   port/tools/rate_audit.py runs both and ranks the words by travel per update.

   Env:
     DHD_RATE_AUDIT=<file>       the output (required; the audit is off without it)
     DHD_RATE_AUDIT_SKIP=<n>     updates to pass before the window (default 0)
     DHD_RATE_AUDIT_SAMPLES=<n>  updates to measure (default 300)
     DHD_RATE_AUDIT_MENU=<n>     wait for the menu id (0x0829DED8) to be n before the window
     DHD_RATE_AUDIT_PHASE=<lo-hi>  ... and the race phase (0x0843F348) to be in lo..hi
     DHD_RATE_AUDIT_MIN=<x>      smallest travel written (default 0.01)
     DHD_RATE_AUDIT_MAX=<n>      give up after n updates without the gates holding (default 200000)
     DHD_RATE_AUDIT_STEPS=<file> one line per sampled update: index, menu, phase, field
     DHD_RATE_AUDIT_CLOCK=<n>    ... and the race clock (0x0843EEC8, fields since GO) to be at least n:
                                 the race phase word keeps a stale value from the menus and the
                                 race load (rate_sweep.py's race scenes)
     DHD_RATE_AUDIT_RACE=1|2     ... and a race's loop running (port_in_race(): before the first
                                 race the phase and clock words hold stale values); 2: a replay
                                 playing (the clock's replay grid, finding 142)
     DHD_RATE_AUDIT_FIELDS=1     compare RAM only at updates that end a field, with RAM at the
                                 previous field's end (port/tools/rate_sweep.py, finding 255): both
                                 rates then see the same instants, so a curve sampled four times
                                 as finely does not gain travel, and a word flipped each update
                                 and back within a field shows as no travel at all
     DHD_RATE_WATCH=<file>       separately, every update: the field counter, the step just run
                                 (port_step(), fields) and the words of DHD_RATE_WATCH_WORDS
                                 (hex EE addresses, comma-separated) in hex (test_menu_rate.py);
                                 DHD_RATE_WATCH_STOP=<n> ends the process at game field n
   The gates only choose where the window starts; once it has started, sampling runs to its end
   and counts how often the gates stopped holding (the header's gate_fail), so a flapping menu id
   cannot silently cut a run short. Skip and samples are update counts at this run's rate.

   A word's change is read as a float when both values look like floats and as an integer
   otherwise: small integers and addresses are denormal or absurd floats and stay integers.
   Travel is in float units for float words and in integer units for integer words.

   The output is one row per word whose travel reaches the minimum:
       addr  travel  kind  value  changes  net
   addr is the EE word address (the form port/tools/state_trace_mask.txt uses); value is the word at
   the last sampled update, changes the number of sampled updates at which it changed (a per-update
   accumulator changes at almost every one; a one-time buffer write does not), travel the sum of
   |change| and net the signed sum of the changes. Travel alone ranks an oscillator (a field-parity
   word flipping 0/8) like an accumulator; net cancels it and keeps the words that actually move one
   way, which is what a phase, angle or scale does. The file holds game-derived state like a state
   trace: keep it under build/ or an F: scratch folder, never commit it. */
#include "hle.h"
#include "rate_audit.h"
#include "../include/fps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS (PS2_RAM_SIZE / 4u)
#define BLOCKS (WORDS / 2u)   /* the scan compares two 32-bit words per load */
#define MENU_ADDR 0x0829DED8u   /* the same words fps.c and the state trace read */
#define PHASE_ADDR 0x0843F348u
#define CLOCK_ADDR 0x0843EEC8u

extern int D_0029E3D8;  /* the game's 60 Hz field counter (patch of func_00238C00, fps.c) */

static struct {
    int on;                  /* -1: not read yet, 0: off, 1: on */
    int armed;               /* the baseline is taken; measuring from the next update */
    int done;
    FILE *out, *steps;
    uint64_t *prev;          /* EE RAM at the previous update, two words per entry */
    float *travel;           /* accumulated |change| per word */
    double *net;             /* accumulated signed change per word (cancels oscillators) */
    uint16_t *changes;       /* updates at which the word changed */
    uint8_t *kind;           /* 0: integer units, 1: float units (of the last change) */
    double min_travel;
    unsigned long long skip, want, skip_left, samples, ticks, max_ticks, rows;
    unsigned long long field0, field1, gate_fail;
    int menu0, menu1, phase0, phase1;
    int gate_menu, gate_phase_lo, gate_phase_hi, gate_clock, gate_race;
    int rate;
    int fields_only;         /* DHD_RATE_AUDIT_FIELDS: scan at field ends only */
    int32_t last_field;
} ra = { .on = -1, .gate_menu = -1, .gate_clock = -1, .gate_phase_lo = -1, .gate_phase_hi = -1 };

static unsigned long long env_ull(const char *name, unsigned long long fallback)
{
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    return strtoull(s, NULL, 0);
}

/* A float as the game stores it: zero, or a normal float of sane magnitude. Small integers
   (1, 1000, 0x08xxxxxx) are denormals or astronomically large floats and fail this. */
static int float_bits(uint32_t bits)
{
    unsigned exponent = (bits >> 23) & 0xFFu;
    if (bits == 0 || bits == 0x80000000u) return 1;
    if (exponent < 1 || exponent > 254) return 0;
    float f;
    memcpy(&f, &bits, sizeof f);
    return fabsf(f) <= 1.0e9f;
}

/* |new - old|, read as a float when both words look like floats, else as signed integers.
   *net receives the signed change. */
static double word_delta(uint32_t old, uint32_t now, uint8_t *kind, double *net)
{
    if (float_bits(old) && float_bits(now)) {
        float fo, fn;
        memcpy(&fo, &old, sizeof fo);
        memcpy(&fn, &now, sizeof fn);
        double d = (double)fn - (double)fo;
        if (isfinite(d)) { *kind = 1; *net = d; return fabs(d); }
    }
    *kind = 0;
    *net = (double)(int32_t)now - (double)(int32_t)old;
    return fabs(*net);
}

static void audit_scan(void)
{
    const uint64_t *now = (const uint64_t *)(uintptr_t)PS2_RAM_BASE;
    uint64_t *prev = ra.prev;
    for (size_t block = 0; block < BLOCKS; block++) {
        uint64_t value = now[block];
        if (value == prev[block]) continue;
        uint32_t old0 = (uint32_t)prev[block], old1 = (uint32_t)(prev[block] >> 32);
        uint32_t now0 = (uint32_t)value, now1 = (uint32_t)(value >> 32);
        prev[block] = value;
        if (old0 != now0) {
            size_t word = 2 * block;
            double net;
            ra.travel[word] += (float)word_delta(old0, now0, &ra.kind[word], &net);
            ra.net[word] += net;
            if (ra.changes[word] != 0xFFFFu) ra.changes[word]++;
        }
        if (old1 != now1) {
            size_t word = 2 * block + 1;
            double net;
            ra.travel[word] += (float)word_delta(old1, now1, &ra.kind[word], &net);
            ra.net[word] += net;
            if (ra.changes[word] != 0xFFFFu) ra.changes[word]++;
        }
    }
}

static void audit_finish(const char *why)
{
    const uint32_t *ram = (const uint32_t *)(uintptr_t)PS2_RAM_BASE;
    if (!ra.out) return;
    /* The header holds no game state; the rows below do, as a state trace's would. */
    fprintf(ra.out, "# rate-audit 1 fields_only=%d rate=%d skip=%llu samples=%llu updates=%llu field0=%llu "
                    "field1=%llu menu0=%d menu1=%d phase0=%d phase1=%d gate_fail=%llu end=%s\n",
            ra.fields_only, ra.rate, ra.skip, ra.want, ra.samples, ra.field0, ra.field1, ra.menu0, ra.menu1,
            ra.phase0, ra.phase1, ra.gate_fail, why);
    for (size_t word = 0; word < WORDS; word++) {
        if (!(ra.travel[word] >= ra.min_travel)) continue;
        uint32_t value = ram[word];
        char shown[32];
        if (ra.kind[word]) {
            float f;
            memcpy(&f, &value, sizeof f);
            snprintf(shown, sizeof shown, "%.9g", (double)f);
        } else {
            snprintf(shown, sizeof shown, "%d", (int32_t)value);
        }
        fprintf(ra.out, "%08X\t%.9g\t%c\t%s\t%u\t%.9g\n", (unsigned)(word * 4),
                (double)ra.travel[word], ra.kind[word] ? 'f' : 'i', shown,
                (unsigned)ra.changes[word], ra.net[word]);
        ra.rows++;
    }
    fclose(ra.out);
    ra.out = NULL;
    if (ra.steps) { fclose(ra.steps); ra.steps = NULL; }
    fprintf(stderr, "rate-audit: %d fps, %llu samples (%llu updates, fields %llu..%llu, menu %d..%d, "
            "phase %d..%d, gate_fail %llu), %llu rows (travel >= %g, end %s)\n", ra.rate,
            ra.samples, ra.ticks, ra.field0, ra.field1, ra.menu0, ra.menu1, ra.phase0, ra.phase1,
            ra.gate_fail, ra.rows, ra.min_travel, why);
    host_exit(0);
}

/* The window gate: the scene both runs must be in (none by default). */
static int gate_holds(void)
{
    if (ra.gate_menu >= 0 && *(volatile int32_t *)(uintptr_t)MENU_ADDR != ra.gate_menu) return 0;
    if (ra.gate_race && (!port_in_race() || (ra.gate_race == 2 && !port_clock_playing()))) return 0;
    if (ra.gate_clock >= 0 && *(volatile int32_t *)(uintptr_t)CLOCK_ADDR < ra.gate_clock) return 0;
    if (ra.gate_phase_lo >= 0) {
        int32_t phase = *(volatile int32_t *)(uintptr_t)PHASE_ADDR;
        if (phase < ra.gate_phase_lo || phase > ra.gate_phase_hi) return 0;
    }
    return 1;
}
static int32_t menu_now(void) { return *(volatile int32_t *)(uintptr_t)MENU_ADDR; }
static int32_t phase_now(void) { return *(volatile int32_t *)(uintptr_t)PHASE_ADDR; }

static void audit_init(void)
{
    const char *path = getenv("DHD_RATE_AUDIT");
    ra.on = 0;
    if (!path || !*path) return;
    ra.rate = port_fps();  /* reads DHD_FPS (pads_online() is false headless) */
    ra.skip = env_ull("DHD_RATE_AUDIT_SKIP", 0);
    ra.want = env_ull("DHD_RATE_AUDIT_SAMPLES", 300);
    ra.max_ticks = env_ull("DHD_RATE_AUDIT_MAX", 200000);
    ra.fields_only = env_ull("DHD_RATE_AUDIT_FIELDS", 0) != 0;
    ra.min_travel = getenv("DHD_RATE_AUDIT_MIN") ? atof(getenv("DHD_RATE_AUDIT_MIN")) : 0.01;
    if (getenv("DHD_RATE_AUDIT_MENU") && *getenv("DHD_RATE_AUDIT_MENU"))
        ra.gate_menu = atoi(getenv("DHD_RATE_AUDIT_MENU"));
    ra.gate_race = (int)env_ull("DHD_RATE_AUDIT_RACE", 0);
    if (getenv("DHD_RATE_AUDIT_CLOCK") && *getenv("DHD_RATE_AUDIT_CLOCK"))
        ra.gate_clock = atoi(getenv("DHD_RATE_AUDIT_CLOCK"));
    const char *phase = getenv("DHD_RATE_AUDIT_PHASE");
    if (phase && *phase) {
        ra.gate_phase_lo = atoi(phase);
        ra.gate_phase_hi = ra.gate_phase_lo;
        const char *dash = strchr(phase, '-');
        if (dash) ra.gate_phase_hi = atoi(dash + 1);
    }
    ra.prev = malloc(PS2_RAM_SIZE);
    ra.travel = calloc(WORDS, sizeof *ra.travel);
    ra.net = calloc(WORDS, sizeof *ra.net);
    ra.changes = calloc(WORDS, sizeof *ra.changes);
    ra.kind = calloc(WORDS, sizeof *ra.kind);
    ra.out = fopen(path, "wb");
    if (!ra.prev || !ra.travel || !ra.net || !ra.changes || !ra.kind || !ra.out) {
        fprintf(stderr, "rate-audit: cannot start (%s)\n", path);
        host_exit(3);
    }
    const char *steps = getenv("DHD_RATE_AUDIT_STEPS");
    if (steps && *steps) {
        ra.steps = fopen(steps, "wb");
        if (!ra.steps) { fprintf(stderr, "rate-audit: cannot write %s\n", steps); host_exit(3); }
        fprintf(ra.steps, "update\tmenu\tphase\tfield\n");
    }
    /* The header (rate, window, scene) is written by audit_finish. */
    ra.on = 1;
    fprintf(stderr, "rate-audit: %s at %d fps, gate menu %d phase %d..%d, skip %llu, samples %llu\n",
            path, ra.rate, ra.gate_menu, ra.gate_phase_lo, ra.gate_phase_hi, ra.skip, ra.want);
}

/* DHD_RATE_WATCH: a few words every update, with the field counter and the step. */
static void watch_update(void)
{
    static int on = -1, count;
    static unsigned stop;
    static uint32_t words[64];
    static FILE *out;
    if (on < 0) {
        const char *path = getenv("DHD_RATE_WATCH"), *list = getenv("DHD_RATE_WATCH_WORDS");
        on = 0;
        if (!path || !*path || !list) return;
        while (*list && count < 64) {
            char *end;
            unsigned long a = strtoul(list, &end, 16);
            if (end == list) break;
            words[count++] = (uint32_t)a;
            list = *end == ',' ? end + 1 : end;
        }
        stop = (unsigned)env_ull("DHD_RATE_WATCH_STOP", 0);
        out = fopen(path, "wb");
        if (!out) { fprintf(stderr, "rate-watch: cannot write %s\n", path); host_exit(3); }
        on = 1;
    }
    if (!on) return;
    fprintf(out, "%u %.9g", (unsigned)(uint32_t)D_0029E3D8, (double)port_step());
    for (int i = 0; i < count; i++)
        fprintf(out, " %08x", (unsigned)*(volatile uint32_t *)(uintptr_t)(PS2_RAM_BASE + (words[i] & 0x01FFFFFCu)));
    fputc('\n', out);
    if (stop && (unsigned)(uint32_t)D_0029E3D8 >= stop) { fclose(out); out = NULL; on = 0; host_exit(0); }
}

void rate_audit_update(void)
{
    watch_update();
    if (ra.on < 0) audit_init();
    if (!ra.on || ra.done) return;
    if (!ra.armed) {
        if (!gate_holds()) {
            if (++ra.ticks > ra.max_ticks) { ra.done = 1; audit_finish("no-gate"); }
            return;
        }
        if (ra.skip_left < ra.skip) { ra.skip_left++; return; }
        memcpy(ra.prev, (const void *)(uintptr_t)PS2_RAM_BASE, PS2_RAM_SIZE);
        ra.armed = 1;
        ra.field0 = (unsigned long long)(uint32_t)D_0029E3D8;
        ra.last_field = D_0029E3D8;
        ra.menu0 = menu_now();
        ra.phase0 = phase_now();
        return;  /* the first armed update is the baseline */
    }
    if (!gate_holds()) ra.gate_fail++;
    if (!ra.fields_only || D_0029E3D8 != ra.last_field) audit_scan();
    ra.last_field = D_0029E3D8;
    ra.samples++;
    ra.field1 = (unsigned long long)(uint32_t)D_0029E3D8;
    ra.menu1 = menu_now();
    ra.phase1 = phase_now();
    if (ra.steps)
        fprintf(ra.steps, "%llu\t%d\t%d\t%llu\n", ra.samples, ra.menu1, ra.phase1, ra.field1);
    if (ra.samples >= ra.want) { ra.done = 1; audit_finish("samples"); return; }
    if (++ra.ticks > ra.max_ticks) { ra.done = 1; audit_finish("max"); return; }
}
