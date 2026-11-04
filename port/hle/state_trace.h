/* Per-field state trace (build/audit/h4-state-trace.md section 7; plan finding 081, phase 1).
   Off unless DHD_STATE_TRACE=<prefix> is set; then one row per game field goes to <prefix>.tsv.
   Every call below returns at once when the trace is off. */
#ifndef DHD_STATE_TRACE_H
#define DHD_STATE_TRACE_H

#include <stddef.h>
#include <stdint.h>

struct Gs;

/* Reads DHD_STATE_TRACE, DHD_STATE_DUMP, DHD_STATE_MASK and DHD_STATE_STOP, opens the TSV and
   installs the GS packet hook. Call after gs_reset (which clears the GS struct). */
void state_trace_init(void);
/* DHD_STATE_WATCH=address (hex EE address), read only under DHD_STATE_TRACE: the write watchpoint
   host.c arms like DHD_WATCH (the writer, its field and value are printed). NULL when unset. */
const char *state_trace_watch_spec(void);

/* The end of vsync(): hashes every section of this field and writes its row. next_vsync is the
   kernel's next VBLANK deadline (virtual clock). Ends the process at DHD_STATE_STOP. */
void state_trace_field(uint64_t field, uint64_t next_vsync);

/* sound_tick: one rendered chunk of interleaved stereo 16-bit frames, before it goes to the host. */
void state_trace_pcm(const int16_t *samples, size_t frames);

/* gs_feed: every packet fed to the GS, in call order (installed as Gs.record_packet). */
void state_trace_gs_packet(struct Gs *g, unsigned path, const uint32_t *words, size_t count);

#endif
