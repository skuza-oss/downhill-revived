/* Native 989snd (finding 022): see sound.h. Voices decode PS2 ADPCM and mix in
   integer arithmetic at 48000 Hz stereo. Banks arrive by disc LSN (the EE only
   ever sends the LSN; the real driver reads the disc itself, and so does this
   one, through cdvd_read). Streams (.VAG/.VPK on the disc) load whole at start
   and decode as ticks advance them, so a slow host disc can never change what
   the game observes: every position below is a function of the vsync count. */
#include "hle.h"
#include "sound.h"
#include "state_trace.h"
#include "../host/host_api.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <emmintrin.h> /* the normal i686 port requires SSE2 */


#define SND_RATE 48000
#define SND_VOICES 48
#define SND_BANKS 64
#define SND_STREAMS 8
#define BLOCK_SAMPLES 28

#include "sound_gaussian.h"

int32_t snd_gaussian_sample(const int16_t samples[4], unsigned phase)
{
    const int16_t *coefficients = snd_gaussian_table[phase & 255];
    /* Reconstruct four signed-32 products, then shift each independently.
       A packed dot product followed by one shift would round differently. */
    __m128i input = _mm_loadl_epi64((const __m128i *)samples);
    __m128i coeff = _mm_loadl_epi64((const __m128i *)coefficients);
    __m128i low = _mm_mullo_epi16(input, coeff), high = _mm_mulhi_epi16(input, coeff);
    __m128i product = _mm_srai_epi32(_mm_unpacklo_epi16(low, high), 15);
    product = _mm_add_epi32(product, _mm_shuffle_epi32(product, _MM_SHUFFLE(1,0,3,2)));
    product = _mm_add_epi32(product, _mm_shuffle_epi32(product, _MM_SHUFFLE(2,3,0,1)));
    return _mm_cvtsi128_si32(product);
}

extern volatile int32_t D_004CD380[]; /* disc-read status (iop.c, finding 020) */

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DHD_SND_TRACE") != NULL;
    return on;
}

/* ---------------- PS2 ADPCM (VAG) ----------------
   One block is 16 bytes: a predictor/shift byte, a flag byte and 28 signed
   nibbles. The predictor filters are the SPU2 coefficients (PCSX2 SPU2 is the
   behaviour reference); the decoded value is (nibble << 12) >> shift, filtered
   by the two previous samples, all in integers so every host agrees. */
static const int32_t k_filter[5][2] = {
    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60},
};

uint8_t snd_adpcm_block(SndAdpcm *st, const uint8_t *block, int16_t out[28])
{
    int shift = block[0] & 0xF;
    int filter = (block[0] >> 4) & 0xF;
    int32_t f0, f1;
    int i;
    if (filter > 4) filter = 0;
    f0 = k_filter[filter][0], f1 = k_filter[filter][1];
    for (i = 0; i < 28; i++) {
        int byte = block[2 + i / 2];
        int nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
        int32_t s;
        if (nib > 7) nib -= 16;
        s = ((nib * 4096) >> shift) + ((st->prev1 * f0 + st->prev2 * f1 + 32) >> 6);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
        st->prev2 = st->prev1;
        st->prev1 = s;
    }
    return block[1];
}

/* ---------------- SPU2 levels and ADSR ----------------
   Signed Q15 multiplication; fixed volume registers are signed 15-bit, while
   bit 15 selects a counter-driven sweep. ADSR uses the chip's separate phase
   step/shift/direction/exponential fields. The direct-step test mode is not
   used by tone grains. All envelope counters advance at 48000 Hz. */
int32_t snd_q15(int32_t sample, int32_t volume)
{
    return (int32_t)(((int64_t)sample * volume) >> 15);
}

void snd_volume_set(SndVolume *v, uint16_t reg)
{
    v->reg = reg; v->counter = 0;
    if (!(reg & 0x8000)) v->level = (int16_t)(uint16_t)(reg * 2u);
}

int32_t snd_volume_tick(SndVolume *v)
{
    int shift, step, dec, exp, negative, change;
    uint32_t inc;
    if (!(v->reg & 0x8000)) return v->level;
    shift = (v->reg >> 2) & 31; step = 7 - (v->reg & 3);
    dec = (v->reg >> 13) & 1; exp = (v->reg >> 14) & 1;
    negative = (v->reg >> 12) & 1;
    if (dec) step = ~step;
    inc = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    change = step * (1 << (shift < 11 ? 11 - shift : 0));
    if (exp && !dec && v->level > 0x6000) inc >>= 2;
    if (exp && dec) change = snd_q15(change, v->level);
    if ((v->reg & 127) != 127 && !inc) inc = 1;
    if (!(exp && dec) && negative) change = -change;
    v->counter += inc;
    if (v->counter >= 0x8000) {
        int low = dec && (!negative || exp) ? 0 : -32768;
        int high = dec && negative && !exp ? 0 : 32767;
        v->counter = 0; v->level += change;
        if (v->level < low) v->level = low;
        if (v->level > high) v->level = high;
    }
    return v->level;
}

static void snd_adsr_prepare(SndAdsr *e, int phase)
{
    int shift, step, exp, dec = phase != 0;
    if (phase == 0) {
        shift = (e->adsr1 >> 10) & 31; step = 7 - ((e->adsr1 >> 8) & 3);
        exp = (e->adsr1 >> 15) & 1;
    } else if (phase == 1) {
        shift = (e->adsr1 >> 4) & 15; step = -8; exp = 1;
    } else if (phase == 2) {
        shift = (e->adsr2 >> 8) & 31; step = 7 - ((e->adsr2 >> 6) & 3);
        dec = (e->adsr2 >> 14) & 1; exp = (e->adsr2 >> 15) & 1;
        if (dec) step = ~step;
    } else {
        shift = e->adsr2 & 31; step = -8; exp = (e->adsr2 >> 5) & 1;
    }
    e->rate.increment = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    e->rate.change = step * (1 << (shift < 11 ? 11 - shift : 0));
    e->rate.flags = (unsigned)exp | ((unsigned)dec << 1); e->rate.phase = phase;
}

void snd_adsr_set(SndAdsr *e, uint32_t adsr1, uint32_t adsr2)
{
    memset(e, 0, sizeof *e);
    e->direct = (adsr1 >> 31) != 0;
    if (e->direct) {
        e->direct_rate.attack_step = (int32_t)(adsr1 & 0x7FFF);
        e->direct_rate.decay_step = -(int32_t)((adsr1 >> 15) & 0x7FFF);
        e->sustain_level = (int32_t)(adsr2 & 0x7FFF);
        e->direct_rate.release_step = -(int32_t)((adsr2 >> 15) & 0x7FFF);
    } else {
        e->adsr1 = (uint16_t)adsr1; e->adsr2 = (uint16_t)adsr2;
        e->sustain_level = (int32_t)((adsr1 & 15) + 1) * 2048;
        snd_adsr_prepare(e, 0);
    }
}

void snd_adsr_start(SndAdsr *e)
{
    e->phase = 0;
    e->level = 0;
    e->counter = 0;
    if (e->direct && e->direct_rate.attack_step <= 0) { e->level = 0x7FFF; e->phase = 1; }
}

void snd_adsr_release(SndAdsr *e)
{
    if (e->phase < 3) { e->phase = 3; e->counter = 0; }
}

int32_t snd_adsr_tick(SndAdsr *e)
{
    if (!e->direct && e->phase < 4) {
        uint32_t inc;
        if (e->rate.phase != e->phase) snd_adsr_prepare(e, e->phase);
        inc = e->rate.increment;
        if (e->rate.flags == 1 && e->level > 0x6000) inc >>= 2;
        if (!inc) inc = 1;
        e->counter += inc;
        if (e->counter >= 0x8000) {
            int change = e->rate.change;
            if (e->rate.flags == 3) change = snd_q15(change, e->level);
            e->counter = 0; e->level += change;
            if (e->level < 0) e->level = 0;
            if (e->level > 32767) e->level = 32767;
        }
        if (e->phase == 0 && e->level == 32767) e->phase = 1;
        else if (e->phase == 1 && e->level <= e->sustain_level) e->phase = 2;
        else if (e->phase >= 2 && !e->level) e->phase = 4;
        return e->level;
    }
    switch (e->phase) {
    case 0:
        e->level += e->direct_rate.attack_step;
        if (e->level >= 0x7FFF) { e->level = 0x7FFF; e->phase = 1; }
        break;
    case 1:
        e->level += e->direct_rate.decay_step;
        if (e->level <= e->sustain_level) { e->level = e->sustain_level; e->phase = 2; }
        break;
    case 2:
        break;
    case 3:
        e->level += e->direct_rate.release_step;
        if (e->level <= 0) { e->level = 0; e->phase = 4; }
        break;
    default:
        e->level = 0;
        break;
    }
    return e->level;
}

/* ---------------- bank (.BNK, 989snd SBlk) ----------------
   The file attributes describe metadata and sample chunks. Version-1 sound
   records address grain programs; tone grains address the sample chunk. The
   hashed name table addresses sound records. Never infer sound/sample pairing
   by scanning ADPCM end flags: retail loop trailers also carry end flags. */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static int region(uint32_t off, uint32_t bytes, uint32_t end)
{
    return off <= end && bytes <= end - off;
}

int snd_bank_tone(const SndBank *bank, uint32_t grain_off, SndTone *t)
{
    const uint8_t *p;
    uint32_t off, at, end;
    if (!region(grain_off, 40, bank->metadata_end)) return 0;
    p = bank->data + grain_off + 8;
    off = rd32(p + 16);
    if ((off & 15) || !region(off, 16, bank->wave_len)) return 0;
    memset(t, 0, sizeof *t);
    t->priority = (int8_t)p[0]; t->volume = (int8_t)p[1];
    t->center_note = (int8_t)p[2]; t->center_fine = (int8_t)p[3];
    t->pan = (int16_t)rd16(p + 4);
    t->pb_low = (int8_t)p[8]; t->pb_high = (int8_t)p[9];
    t->adsr1 = rd16(p + 10); t->adsr2 = rd16(p + 12); t->flags = rd16(p + 14);
    t->adpcm_off = bank->wave_off + off;
    end = bank->wave_off + bank->wave_len;
    at = t->adpcm_off;
    while (region(at, 16, end)) {
        uint8_t flags = bank->data[at + 1];
        at += 16;
        if (flags & 1) { t->adpcm_len = at - t->adpcm_off; return 1; }
    }
    return 0; /* truncated tone: do not cross into the next resource */
}

int snd_bank_parse(SndBank *bank, const uint8_t *data, uint32_t size)
{
    uint32_t meta_size, first_sound, first_grain, names, table;
    int ns, i, bucket;
    memset(bank, 0, sizeof *bank);
    if (size < 0x54 || rd32(data + 8) != 0x18 ||
        memcmp(data + 0x18, "SBlk", 4) || rd32(data + 0x1C) != 1) return 0;
    meta_size = rd32(data + 12);
    bank->wave_off = rd32(data + 16); bank->wave_len = rd32(data + 20);
    if (!region(0x18, meta_size, size) || !region(bank->wave_off, bank->wave_len, size) ||
        bank->wave_off < 0x18 + meta_size) return 0;
    bank->metadata_end = 0x18 + meta_size;
    ns = rd16(data + 0x18 + 0x16);
    if (!ns || ns > 512) return 0;
    first_sound = rd32(data + 0x18 + 0x1C);
    first_grain = rd32(data + 0x18 + 0x20);
    if (!region(first_sound, (uint32_t)ns * 12, meta_size) || first_grain > meta_size)
        return 0;
    bank->data = data; bank->size = size; bank->nsounds = ns;
    for (i = 0; i < ns; i++) {
        SndBankSound *sound = &bank->sounds[i];
        const uint8_t *p = data + 0x18 + first_sound + i * 12;
        uint32_t go = rd32(p + 8);
        int k;
        sound->index = i; sound->volume = (int8_t)p[0]; sound->group = p[1];
        sound->pan = (int16_t)rd16(p + 2); sound->ngrains = p[4]; sound->flags = rd16(p + 6);
        if (!sound->ngrains) continue; /* retail empty sounds carry a -40 offset */
        if (go > meta_size - first_grain ||
            !region(first_grain + go, (uint32_t)sound->ngrains * 40, meta_size)) return 0;
        sound->grain_off = 0x18 + first_grain + go;
        for (k = 0; k < sound->ngrains; k++) {
            uint32_t at = sound->grain_off + k * 40, type = rd32(data + at);
            if (type == 1 || type == 9) {
                SndTone tone;
                if (!snd_bank_tone(bank, at, &tone)) return 0;
                if (!sound->adpcm_len) {
                    sound->adpcm_off = tone.adpcm_off; sound->adpcm_len = tone.adpcm_len;
                }
            }
        }
    }
    /* Named records are hashed into 32 buckets, each terminated by a zero
       first word. Indices address the sound table, not a waveform ordinal. */
    names = rd32(data + 0x18 + 0x34);
    if (!region(names, 0x98, meta_size)) return 0;
    table = rd32(data + 0x18 + names + 8);
    if (table > meta_size - names) return 0;
    table += names;
    for (bucket = 0; bucket < 32; bucket++) {
        int index = (int16_t)rd16(data + 0x18 + names + 0x18 + bucket * 2);
        uint32_t at;
        int count = 0;
        if (index < 0) continue;
        if ((uint32_t)index > (meta_size - table) / 20) return 0;
        at = table + (uint32_t)index * 20;
        for (;;) {
            const uint8_t *p;
            if (!region(at, 20, meta_size) || ++count > ns + 1) return 0;
            p = data + 0x18 + at;
            if (!rd32(p)) break;
            index = (int16_t)rd16(p + 16);
            if (index < 0 || index >= ns) return 0;
            memcpy(bank->sounds[index].name, p, 16);
            at += 20;
        }
    }
    return 1;
}

const SndBankSound *snd_bank_find(const SndBank *bank, const char name[16])
{
    int i;
    for (i = 0; i < bank->nsounds; i++)
        if (memcmp(bank->sounds[i].name, name, 16) == 0)
            return &bank->sounds[i];
    return NULL;
}

/* ---------------- streams (.VAG / .VPK headers) ---------------- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

int snd_vag_parse(const uint8_t *data, uint32_t size, uint32_t *rate,
                  uint32_t *adpcm_off, uint32_t *adpcm_len)
{
    SndStreamInfo info;
    if (size < 48 || memcmp(data, "VAGp", 4) != 0 ||
        !snd_stream_parse(data, size, &info))
        return 0;
    *rate = info.rate;
    *adpcm_off = info.offset;
    *adpcm_len = info.channel_bytes;
    return 1;
}

int snd_stream_parse(const uint8_t *data, uint32_t size, SndStreamInfo *info)
{
    uint64_t end;
    memset(info, 0, sizeof *info);
    if (size >= 48 && !memcmp(data, "VAGp", 4)) {
        info->rate = be32(data + 16);
        info->offset = 48;
        info->channel_bytes = be32(data + 12);
        info->channels = 1;
        info->interleave = 16;
        end = (uint64_t)info->offset + info->channel_bytes;
    } else if (size >= 2048 && !memcmp(data, " KPV", 4)) {
        /* VGMStream's meta/vpk.c documents the per-channel length and
           interleave field (half the stored value, including multichannel). */
        uint32_t last, groups;
        info->channel_bytes = rd32(data + 4);
        info->offset = rd32(data + 8);
        info->interleave = rd32(data + 12) / 2;
        info->rate = rd32(data + 16);
        info->channels = rd32(data + 20);
        info->loop_byte = rd32(data + 0x7FC);
        if (!info->channel_bytes || info->offset < 2048 || !info->interleave ||
            (info->interleave & 15) || info->channels < 1 || info->channels > 2 ||
            (info->loop_byte & 15) || info->loop_byte >= info->channel_bytes)
            return 0;
        groups = (info->channel_bytes - 1) / info->interleave;
        last = info->channel_bytes - groups * info->interleave;
        end = (uint64_t)info->offset +
              (uint64_t)groups * info->interleave * info->channels +
              (info->channels - 1) * (uint64_t)info->interleave + last;
    } else {
        return 0;
    }
    return info->rate >= 8000 && info->rate <= 48000 &&
           info->channel_bytes >= 16 && !(info->channel_bytes & 15) && end <= size;
}

/* ---------------- voices and the mixer ---------------- */

typedef struct {
    int used;
    int paused;
    uint32_t handle;
    const uint8_t *adpcm;
    uint32_t adpcm_len;
    uint32_t block, loop_block;
    uint8_t flags;
    int sample;              /* 0..27 inside the decoded block */
    int16_t decoded[BLOCK_SAMPLES];
    int16_t preview[BLOCK_SAMPLES];
    int preview_ready;
    SndAdpcm dec;
    uint32_t frac, step;     /* source position, 16.16 samples per output sample */
    uint32_t prefetch_write;
    int finite_endpoint;
    int32_t vol_l, vol_r;    /* 0..0x7FFF */
    int volume;             /* application volume, 0..1024 */
    SndAdsr env;
    int releasing, core;
    uint32_t owner;
    SndTone tone;
    int tone_volume, tone_pan;
    int zero_env_ticks; /* 989snd requires four consecutive zero ENVX polls */
    SndVolume gain[2];
} Voice;

typedef struct {
    int used;
    uint32_t handle;
    uint8_t *image;          /* owned bank bytes (NULL for test banks) */
    SndBank bank;
    int16_t grain_choice[4096]; /* deterministic mutable RAND_PLAY/PLAY_CYCLE state */
} BankSlot;

typedef struct SndStream {
    int used, paused;
    uint32_t group;
    const uint8_t *image[2];
    uint8_t *owned[2];
    SndStreamInfo info;
    uint32_t block, frac, source_samples;
    uint32_t loop_block;
    int repeat, ended, decoded;
    SndAdpcm dec[2];
    SndAdsr env; /* identical keyed envelopes for the two stream voices */
    int16_t pcm[2][BLOCK_SAMPLES];
    int16_t preview[2][BLOCK_SAMPLES];
    int preview_ready;
    int sample;
    int32_t vol_l, vol_r;
    int volume, pan;
    int pmod, pbend, auto_pan_from, auto_pan_to, auto_pan_tick, auto_pan_time;
    int32_t matrix[2][2];
    int32_t mix_gain[2][2];
    uint32_t gain_epoch, pitch;
    int gain_valid, pitch_valid, pitch_pmod, pitch_pbend;
    uint32_t handle;
    uint32_t flags;
    int disc_managed; /* 989snd rewrites buffer flags; RPC bit 4 controls file repeat */
    uint64_t ready_at, play_at; /* game-sample deadlines, independent of host reads */
    struct SndStream *queue, *tail;
    int queued_count;
} Stream;

static BankSlot banks[SND_BANKS];
static Voice voices[SND_VOICES];
static Stream streams[SND_STREAMS];
/* Deterministic startup-service approximation. Retail paired VAG ready bits
   appear ~200 ms after a cold request and ~100 ms later for the next preload.
   Resident bytes never decide these deadlines. Seek/IOP parity limits are
   documented in finding 022; this is not a full simulated DVD drive. */
static uint32_t stream_read_samples = SND_RATE / 10;
static uint64_t stream_io_deadline;
static uint32_t next_handle = 1;
static uint32_t rendered_samples;
static uint64_t vsync_count;
static int32_t master_vol = 32766; /* fixed SPU register 0x3FFF, sign-extended then doubled */
static uint32_t stream_gain_epoch;
static int16_t reverb_depth[2][2];
static int voice_range[16][2] = {
    {0,47},{0,47},{0,47},{0,47},{0,47},{0,47},{0,47},{0,47},
    {0,47},{0,47},{0,47},{0,47},{0,47},{0,47},{0,47},{0,47}
};
static int playback_mode;
static int group_vol[17] = {1024,1024,1024,1024,1024,1024,1024,1024,
                            1024,1024,1024,1024,1024,1024,1024,1024,1024};
static int group_duck[15] = {65536,65536,65536,65536,65536,65536,65536,65536,
                             65536,65536,65536,65536,65536,65536,65536};
typedef struct { int enabled, source, depth, attack, release, current; uint32_t targets; } SndDucker;
static SndDucker duckers[4];

#include "sound_debug.h"
#include "sound_reverb.h"

static int application_volume(int old, int volume)
{
    /* 989snd's stream normalization (IOP 0x18188): INT_MAX preserves the
       value; negative values request an absolute 0..127 volume. */
    if (volume == INT_MAX) return old;
    if (volume < 0) {
        int64_t absolute = -(int64_t)volume;
        volume = absolute >= 127 ? 1024 : (int)(absolute * 1024 / 127);
    }
    return volume > 1024 ? 1024 : volume;
}

static void voice_volume(Voice *v, int volume)
{
    v->volume = application_volume(v->volume, volume);
    v->vol_l = v->vol_r = v->volume * 0x7FFF / 1024;
}

static Voice *alloc_voice(int group)
{
    int i;
    if (group < 0 || group > 15) group = 15;
    for (i = voice_range[group][0]; i <= voice_range[group][1]; i++)
        if (!voices[i].used)
            return &voices[i];
    return NULL;
}

static int start_voice(const uint8_t *adpcm, uint32_t len, int volume, int pitch,
                       int pan, int group)
{
    Voice *v = alloc_voice(group);
    uint32_t step;
    if (!v || len < 16)
        return 0;
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->zero_env_ticks = 4;
    v->core = (int)(v - voices) / 24;
    v->handle = next_handle++;
    if (next_handle == 0)
        next_handle = 1;
    v->adpcm = adpcm;
    v->adpcm_len = len & ~15u;
    v->finite_endpoint = (adpcm[v->adpcm_len - 15] & 3) == 1;
    /* pitch: SPU units, 0x1000 is the recorded rate; 0 asks for it */
    if (pitch <= 0)
        pitch = 0x1000;
    step = (uint32_t)(((uint64_t)(uint32_t)pitch << 16) / 0x1000);
    /* bank waveforms are recorded at 22050 Hz unless a play says otherwise */
    v->step = (step * 22050 + SND_RATE / 2) / SND_RATE;
    if (v->step == 0)
        v->step = 1;
    v->volume = 1024;
    voice_volume(v, volume);
    if (pan < -0x400)
        pan = -0x400;
    if (pan > 0x400)
        pan = 0x400;
    if (pan < 0)
        v->vol_r = v->vol_r * (0x400 + pan) / 0x400;
    else
        v->vol_l = v->vol_l * (0x400 - pan) / 0x400;
    snd_adsr_set(&v->env, 0x80000000u | 0x7FFF, (0x6000u << 15) | 0x4000);
    snd_adsr_start(&v->env);
    v->flags = snd_adpcm_block(&v->dec, v->adpcm, v->decoded);
    return (int)v->handle;
}

static void voice_stop(uint32_t handle)
{
    int i;
    for (i = 0; i < SND_VOICES; i++)
        if (voices[i].used && voices[i].handle == handle) {
            snd_adsr_release(&voices[i].env);
            voices[i].releasing = 1;
        }
}

#include "sound_grains.h"
static void stream_parameters_tick(void);
static void duckers_tick(void);
static void stream_clear(Stream *s);
static int stream_next(Stream *s);

static int32_t voice_interpolated(Voice *v)
{
    int16_t window[4];
    int i;
    if (v->sample <= BLOCK_SAMPLES - 4)
        return snd_gaussian_sample(v->decoded + v->sample, v->frac >> 8);
    if (v->sample > BLOCK_SAMPLES - 4 && !v->preview_ready) {
        uint32_t next = v->block + 1;
        SndAdpcm decoder = v->dec;
        if ((v->flags & 1) || (next + 1) * 16 > v->adpcm_len) {
            if ((v->flags & 3) == 3) next = v->loop_block;
            else next = UINT32_MAX;
        }
        if (next == UINT32_MAX) memset(v->preview, 0, sizeof v->preview);
        else snd_adpcm_block(&decoder, v->adpcm + next * 16, v->preview);
        v->preview_ready = 1;
    }
    for (i = 0; i < 4; i++) {
        int at = v->sample + i;
        window[i] = at < BLOCK_SAMPLES ? v->decoded[at] : v->preview[at - BLOCK_SAMPLES];
    }
    return snd_gaussian_sample(window, v->frac >> 8);
}

static uint8_t stream_decode_block(const Stream *s, uint32_t block, int ch,
                                    SndAdpcm *decoder, int16_t pcm[BLOCK_SAMPLES])
{
    uint32_t at = block * 16, off = s->info.offset;
    if (s->image[1]) off += at;
    else off += (at / s->info.interleave) * s->info.interleave * s->info.channels +
                ch * s->info.interleave + at % s->info.interleave;
    return snd_adpcm_block(decoder, s->image[s->image[1] ? ch : 0] + off, pcm);
}

static int32_t stream_interpolated(Stream *s, int ch)
{
    int16_t window[4];
    int i;
    if (s->sample <= BLOCK_SAMPLES - 4)
        return snd_gaussian_sample(s->pcm[ch] + s->sample, s->frac >> 4);
    if (s->sample > BLOCK_SAMPLES - 4 && !s->preview_ready) {
        const Stream *next = s;
        uint32_t block = s->block + 1;
        int channel;
        if (s->ended || block * 16 >= s->info.channel_bytes) {
            if (s->flags & 4) block = 0;
            else if (s->queue && grain_samples >= s->queue->play_at) { next = s->queue; block = 0; }
            else if (s->info.loop_byte || (!s->disc_managed && s->repeat))
                block = s->info.loop_byte ? s->info.loop_byte / 16 : s->loop_block;
            else next = NULL;
        }
        for (channel = 0; channel < (int)s->info.channels; channel++) {
            SndAdpcm decoder = next == s ? s->dec[channel] : (SndAdpcm){0};
            if (!next || channel >= (int)next->info.channels)
                memset(s->preview[channel], 0, sizeof s->preview[channel]);
            else stream_decode_block(next, block, channel, &decoder, s->preview[channel]);
        }
        s->preview_ready = 1;
    }
    for (i = 0; i < 4; i++) {
        int at = s->sample + i;
        window[i] = at < BLOCK_SAMPLES ? s->pcm[ch][at] : s->preview[ch][at - BLOCK_SAMPLES];
    }
    return snd_gaussian_sample(window, s->frac >> 4);
}

static void stream_mix_parameters(Stream *s)
{
    int ch;
    if (!s->gain_valid || s->gain_epoch != stream_gain_epoch) {
        for (ch = 0; ch < (int)s->info.channels; ch++) {
            s->mix_gain[ch][0] = group_gain(s->matrix[ch][0], s->group) & ~1;
            s->mix_gain[ch][1] = group_gain(s->matrix[ch][1], s->group) & ~1;
        }
        s->gain_epoch = stream_gain_epoch; s->gain_valid = 1;
    }
    if (!s->pitch_valid || s->pitch_pmod != s->pmod || s->pitch_pbend != s->pbend) {
        SndTone rate_tone = {0};
        uint32_t pitch;
        rate_tone.center_note = -60; rate_tone.pb_low = rate_tone.pb_high = 2;
        pitch = (uint32_t)((uint64_t)s->info.rate * 4096 / SND_RATE);
        pitch = (uint32_t)((uint64_t)pitch * tone_pitch(&rate_tone, s->pmod, s->pbend) / 4096);
        s->pitch = pitch > 0x3FFF ? 0x3FFF : pitch;
        s->pitch_pmod = s->pmod; s->pitch_pbend = s->pbend; s->pitch_valid = 1;
    }
}

/* DHD_SND_VOICE_TRACE: every 800 samples, each sounding voice as the SPU2 would hold it (the
   columns of the PCSX2 reference's voice dump, finding 133): its sound, its tone's offset in the
   bank's wave chunk, Q12 pitch, envelope, current left/right volume (Q15, sign kept) and routing.
   Read-only: it changes no PCM. */
static void snd_debug_voices(void)
{
    int i;
    if (!snd_voice_trace || grain_samples % 800) return;
    for (i = 0; i < SND_VOICES; i++) {
        const Voice *v = &voices[i];
        const SoundHandler *h = v->owner ? handler_find(v->owner) : NULL;
        if (!v->used || v->paused || v->env.phase == 4 || !v->env.level) continue;
        fprintf(snd_voice_trace, "%llu,%d,%d,%.16s,%d,%ld,%u,%d,%d,0x%04X,0x%04X,%d,%d,%d,%d,0x%X\n",
                (unsigned long long)grain_samples, i, v->core, h ? h->sound->name : "",
                h ? h->sound->group : -1,
                h ? (long)(v->adpcm - h->bank->bank.data) - (long)h->bank->bank.wave_off : -1L,
                v->owner ? v->step >> 4 : v->step * SND_RATE / 22050 >> 4, v->env.level, v->env.phase,
                v->env.adsr1, v->env.adsr2, v->vol_l, v->vol_r, !(v->tone.flags & 16),
                (v->tone.flags & (1 | 16)) != 0, v->tone.flags);
    }
    if (grain_samples % 48000 == 0) fflush(snd_voice_trace);
}

/* Advances every voice and stream by one output sample into mix[2]. */
static void mix_sample(int32_t mix[2])
{
    int i;
    int nvoices = 0, nstreams = 0, voice_gain = 0;
    int32_t components[3][2] = {{0,0},{0,0},{0,0}};
    int32_t wet[2][2] = {{0,0},{0,0}};
    snd_debug_voices();
    if (grain_samples && grain_samples % 200 == 0) {
        handlers_tick(); stream_parameters_tick();
        if (grain_samples % 800 == 0) duckers_tick();
    }
    grain_samples++;
    mix[0] = mix[1] = 0;
    for (i = 0; i < SND_VOICES; i++) {
        Voice *v = &voices[i];
        int32_t s, level;
        if (!v->used || v->paused)
            continue;
        if (v->env.phase == 4) continue; /* silent, but still allocated until driver polling */
        /* SPU2 reads four source samples per output frame until its FIFO is
           more than 12 ahead. Reading the last END word stops a nonlooping
           voice before the pending FIFO tail is played. Static bank data is
           immutable, so track the read head without a second PCM buffer.
           Disc streams use driver-rewritten buffer flags, not this endpoint. */
        if (v->finite_endpoint &&
            (int32_t)(v->prefetch_write - (v->block * BLOCK_SAMPLES + v->sample)) <= 12) {
            v->prefetch_write += 4;
            if (v->prefetch_write >= (v->adpcm_len / 16) * BLOCK_SAMPLES) {
                v->env.phase = 4; v->env.level = 0;
                if (!v->owner) v->used = 0;
                continue;
            }
        }
        level = snd_adsr_tick(&v->env);
        if (v->env.phase == 4) {
            if (!v->owner) v->used = 0;
            continue;
        }
        nvoices++;
        s = voice_interpolated(v);
        if (v->owner && ((v->gain[0].reg | v->gain[1].reg) & 0x8000)) {
            v->vol_l = snd_volume_tick(&v->gain[0]);
            v->vol_r = snd_volume_tick(&v->gain[1]);
        }
        if (snd_gain) {
            int left = v->vol_l < 0 ? -v->vol_l : v->vol_l;
            int right = v->vol_r < 0 ? -v->vol_r : v->vol_r;
            if (left > voice_gain) voice_gain = left;
            if (right > voice_gain) voice_gain = right;
        }
        /* Gaussian's absolute coefficient sum is at most 32643, so its
           four signed-16 inputs and the clamped ADSR fit a signed-32 product.
           Keep the general wide q15 helper for reverb's wider intermediates. */
        s = (s * level) >> 15;
        {
            int32_t left = (s * v->vol_l) >> 15, right = (s * v->vol_r) >> 15;
            if (!(v->tone.flags & 16)) { mix[0] += left; mix[1] += right; }
            if (v->tone.flags & (1 | 16)) { wet[v->core][0] += left; wet[v->core][1] += right; }
        }
        v->frac += v->step;
        while (v->frac >= 0x10000) {
            v->frac -= 0x10000;
            if (++v->sample >= BLOCK_SAMPLES) {
                v->sample = 0;
                v->preview_ready = 0;
                v->block++;
                if ((v->flags & 1) || (v->block + 1) * 16 > v->adpcm_len) {
                    if ((v->flags & 3) == 3) {
                        v->block = v->loop_block;
                    } else {
                        v->env.phase = 4; v->env.level = 0;
                        if (!v->owner) v->used = 0;
                        break;
                    }
                }
                v->flags = snd_adpcm_block(&v->dec, v->adpcm + v->block * 16, v->decoded);
                if (v->flags & 4) {
                    v->loop_block = v->block;
                }
            }
        }
        if (!v->owner && v->releasing && v->env.phase == 4)
            v->used = 0;
    }
    components[0][0] = mix[0]; components[0][1] = mix[1];
    for (i = 0; i < SND_STREAMS; i++) {
        Stream *s = &streams[i];
        int ch, level;
        if (!s->used || s->paused || grain_samples <= s->play_at)
            continue;
        nstreams++;
        level = snd_adsr_tick(&s->env);
        if (!s->decoded) {
            for (ch = 0; ch < (int)s->info.channels; ch++) {
                uint8_t flags;
                flags = stream_decode_block(s, s->block, ch, &s->dec[ch], s->pcm[ch]);
                if (ch == 0 && (flags & 4)) {
                    s->loop_block = s->block;
                }
                if (ch == 0) {
                    s->repeat = (flags & 3) == 3;
                    s->ended = (flags & 1) != 0;
                }
            }
            s->decoded = 1;
        }
        stream_mix_parameters(s);
        {
            /* Retail group gain is quadratic (IOP 0x1B144). Group 15
               bypasses group scaling; 16 is the final master control. */
            for (ch = 0; ch < (int)s->info.channels; ch++) {
                int32_t left = s->mix_gain[ch][0], right = s->mix_gain[ch][1];
                int32_t sample = (stream_interpolated(s, ch) * level) >> 15;
                mix[0] += snd_q15(sample, left);
                mix[1] += snd_q15(sample, right);
            }
        }
        /* libsd streams use a quantized SPU2 Q12 pitch register. Predictor
           history advances only when entering a new ADPCM block. */
        s->frac += s->pitch;
        while (s->frac >= 4096 && s->used) {
            s->frac -= 4096;
            s->source_samples++;
            if (++s->sample == BLOCK_SAMPLES) {
                s->sample = 0;
                s->block++;
                s->decoded = 0;
                s->preview_ready = 0;
                if (s->ended || s->block * 16 >= s->info.channel_bytes) {
                    /* 989snd maps RPC flag 4 to stream flag 0x400. Its
                       EOF loader rewinds the original LSN/data cursor before
                       considering the next cue, without re-keying the voice. */
                    if (s->flags & 4) {
                        s->block = 0;
                        s->source_samples = 0;
                    } else if (s->queue) {
                        stream_next(s);
                    } else if (s->info.loop_byte || (!s->disc_managed && s->repeat)) {
                        s->block = s->info.loop_byte ? s->info.loop_byte / 16 : s->loop_block;
                        s->source_samples = s->block * BLOCK_SAMPLES;
                    } else
                        s->used = 0;
                }
            }
        }
    }
    components[1][0] = mix[0] - components[0][0];
    components[1][1] = mix[1] - components[0][1];
    for (i = 0; i < 2; i++) {
        int32_t output[2];
        reverb_sample(&reverbs[i], wet[i], output);
        if (output[0] || output[1]) snd_wet_samples[i]++;
        mix[0] += snd_q15(output[0], reverb_depth[i][0]);
        mix[1] += snd_q15(output[1], reverb_depth[i][1]);
    }
    components[2][0] = mix[0] - components[0][0] - components[1][0];
    components[2][1] = mix[1] - components[0][1] - components[1][1];
    snd_debug_gain_frame(grain_samples, components, nvoices, nstreams, voice_gain);
}

#ifndef SND_NO_HOST
int movie_playing(void);                     /* port/hle/movie.c */
void movie_audio(int32_t *mix, int frames);  /* adds the movie's sound, moves its clock on */
#endif

static void render(int16_t *stereo, int samples)
{
    int i;
#ifndef SND_NO_HOST
    /* A movie's PCM (finding 055), added like a voice; its clock is these rendered samples. */
    int32_t movie[1024 * 2];
    int with_movie = samples <= 1024 && host_service_movie_playing();
    if (with_movie) {
        memset(movie, 0, (size_t)samples * sizeof movie[0] * 2);
        host_service_movie_audio(GADDR(movie), samples);
    }
#endif
    for (i = 0; i < samples; i++) {
        int32_t mix[2];
        mix_sample(mix);
#ifndef SND_NO_HOST
        if (with_movie) { mix[0] += movie[i * 2]; mix[1] += movie[i * 2 + 1]; }
#endif
        if (mix[0] > 32767) { mix[0] = 32767; snd_mix_clipped[0]++; }
        if (mix[0] < -32768) { mix[0] = -32768; snd_mix_clipped[0]++; }
        if (mix[1] > 32767) { mix[1] = 32767; snd_mix_clipped[1]++; }
        if (mix[1] < -32768) { mix[1] = -32768; snd_mix_clipped[1]++; }
        mix[0] = snd_q15(mix[0], master_vol);
        mix[1] = snd_q15(mix[1], master_vol);
        stereo[i * 2] = (int16_t)mix[0];
        stereo[i * 2 + 1] = (int16_t)mix[1];
    }
    rendered_samples += (uint32_t)samples;
}

/* Tests (SND_NO_HOST) have no output device: the host calls do nothing. */
#ifdef SND_NO_HOST
void host_sound_open(void) {}
void host_sound_write(const int16_t *stereo, int frames) { (void)stereo; (void)frames; }
void host_sound_reset(void) {}
void host_sound_tick(void) {}
void host_sound_log_stack(void) {}
void snd_host_bench(int end) { (void)end; }
/* The state trace (state_trace.c) is not part of the unit tests. */
void state_trace_pcm(const int16_t *samples, size_t frames) { (void)samples; (void)frames; }
#endif


/* ---------------- game time ---------------- */

void sound_tick(void)
{
    /* 48000 * 1001 / 60000 = 4004/5 samples per vsync, exactly. */
    uint32_t due = (uint32_t)((++vsync_count * 4004) / 5) - rendered_samples;
    int16_t buf[1024 * 2];
    host_sound_tick();
    host_sound_open();
    snd_debug_start();
    while (due > 0) {
        int chunk = due > 1024 ? 1024 : (int)due;
        render(buf, chunk);
        state_trace_pcm(buf, chunk);  /* DHD_STATE_TRACE: this field's rendered PCM, before the host */
        snd_debug_mix(buf, chunk);
        snd_report_dirty = 1;
        host_sound_write(buf, chunk);
        due -= (uint32_t)chunk;
    }
    snd_debug_report();
}

uint64_t snd_test_position(void) { return rendered_samples; }
int snd_test_active_voices(void)
{
    int i, count = 0;
    for (i = 0; i < SND_VOICES; i++)
        if (voices[i].used && !voices[i].paused && voices[i].env.phase < 4) count++;
    for (i = 0; i < SND_STREAMS; i++)
        if (streams[i].used && !streams[i].paused && grain_samples >= streams[i].play_at)
            count += streams[i].info.channels;
    return count;
}
int snd_test_reverb_depth(int core, int channel)
{
    return core >= 0 && core < 2 && channel >= 0 && channel < 2 ? reverb_depth[core][channel] : 0;
}

void snd_test_reverb_frame(int core, const int32_t input[2], int32_t output[2])
{
    if (core >= 0 && core < 2) reverb_sample(&reverbs[core], input, output);
    else output[0] = output[1] = 0;
}

void snd_test_reset(void)
{
    snd_debug_reset();
    memset(voices, 0, sizeof voices);
    memset(handlers, 0, sizeof handlers); memset(global_regs, 0, sizeof global_regs);
    grain_samples = 0; grain_random_seed(12345); stream_io_deadline = 0;
    {
        int i;
        for (i = 0; i < SND_STREAMS; i++) {
            stream_clear(&streams[i]);
        }
        for (i = 0; i < SND_BANKS; i++) free(banks[i].image);
    }
    memset(streams, 0, sizeof streams);
    memset(banks, 0, sizeof banks);
    next_handle = 1;
    rendered_samples = 0;
    vsync_count = 0;
    host_sound_reset();
    master_vol = 32766;
    memset(reverb_depth, 0, sizeof reverb_depth);
    memset(reverbs, 0, sizeof reverbs);
    memset(duckers, 0, sizeof duckers);
    playback_mode = 0;
    {
        int i;
        for (i = 0; i < 17; i++) group_vol[i] = 1024;
        for (i = 0; i < 15; i++) group_duck[i] = 65536;
        for (i = 0; i < 16; i++) { voice_range[i][0] = 0; voice_range[i][1] = 47; }
    }
}

void snd_test_render(int16_t *stereo, int samples) { render(stereo, samples); }
void snd_test_report(void) { snd_debug_start(); snd_debug_report(); }
void snd_test_stream_read_samples(uint32_t samples)
{
    /* Fixture policy; production never calls this. Zero isolates the codec
       from loading latency in the preexisting DSP and queue tests. */
    stream_read_samples = samples;
}

int snd_test_add_bank(const uint8_t *data, uint32_t size)
{
    int i;
    for (i = 0; i < SND_BANKS; i++)
        if (!banks[i].used) {
            banks[i].used = 1;
            banks[i].handle = next_handle++;
            banks[i].image = NULL;
            {
                int k;
                for (k = 0; k < 4096; k++) banks[i].grain_choice[k] = INT16_MIN;
            }
            if (!snd_bank_parse(&banks[i].bank, data, size)) {
                banks[i].used = 0;
                return 0;
            }
            return (int)banks[i].handle;
        }
    return 0;
}

int snd_test_play(int bank, const char name[16], int volume, int pitch)
{
    int i;
    for (i = 0; i < SND_BANKS; i++)
        if (banks[i].used && (int)banks[i].handle == bank) {
            const SndBankSound *s = snd_bank_find(&banks[i].bank, name);
            if (!s)
                return 0;
            return start_voice(banks[i].bank.data + s->adpcm_off, s->adpcm_len,
                               volume, pitch, 0, 15);
        }
    return 0;
}

int snd_test_raw_voice(const uint8_t *adpcm, uint32_t size, uint16_t pitch)
{
    int handle = start_voice(adpcm, size, 1024, 0, 0, 15), i;
    for (i = 0; handle && i < SND_VOICES; i++)
        if (voices[i].handle == (uint32_t)handle) voices[i].step = (uint32_t)pitch << 4;
    return handle;
}

/* The caller owns test images; disc images are retained in owned[] until
   completion/stop, then released before reusing the slot. */
static void stream_clear(Stream *s)
{
    Stream *node = s->queue;
    while (node) {
        Stream *next = node->queue;
        free(node->owned[0]); free(node->owned[1]); free(node); node = next;
    }
    free(s->owned[0]);
    free(s->owned[1]);
    memset(s, 0, sizeof *s);
}

static Stream *stream_by_handle(uint32_t handle)
{
    int i;
    for (i = 0; i < SND_STREAMS; i++)
        if (streams[i].used && streams[i].handle == handle)
            return &streams[i];
    return NULL;
}

static Stream *start_stream(const uint8_t *data, uint32_t size)
{
    SndStreamInfo info;
    int i;
    if (!snd_stream_parse(data, size, &info))
        return NULL;
    for (i = 0; i < SND_STREAMS; i++)
        if (!streams[i].used) {
            Stream *s = &streams[i];
            stream_clear(s);
            s->used = 1;
            s->handle = next_handle++;
            s->image[0] = data;
            s->info = info;
            s->vol_l = s->vol_r = 0x7FFF;
            s->matrix[0][0] = 0x7FFF;
            if (info.channels == 2) s->matrix[1][1] = 0x7FFF;
            else s->matrix[0][1] = 0x7FFF;
            s->volume = 1024;
            /* Retail stream voice setup, IOP 0x172C8/0x172D8 (and the
               stereo partner): ADSR1 0x080F, ADSR2 0x1FC0. The decay tick
               enters sustain near 0x3FFF even at the maximum sustain target.
               Bypassing this envelope made music about 6 dB too loud. */
            snd_adsr_set(&s->env, 0x080F, 0x1FC0); snd_adsr_start(&s->env);
            return s;
        }
    return NULL;
}

int snd_test_stream(const uint8_t *data, uint32_t size, int stereo)
{
    Stream *s;
    /* Stereo is described by a VPK header or the RPC's second VAG LSN. */
    if (stereo && size >= 4 && !memcmp(data, "VAGp", 4))
        return 0;
    s = start_stream(data, size);
    return s ? (int)s->handle : 0;
}

/* Header sizes are checked before allocation. Reads can start within a
   sector (packed resources); their logical offsets remain byte-relative. */
static uint8_t *read_stream(uint32_t lsn, uint32_t byte, uint32_t *size)
{
    uint8_t head[4096], *image;
    uint64_t length;
    SndStreamInfo info;
    uint32_t sectors;
    lsn += byte / 2048;
    byte %= 2048;
    if (!cdvd_read(lsn, 2, head))
        return NULL;
    if (!memcmp(head + byte, "VAGp", 4))
        length = 48ull + be32(head + byte + 12);
    else if (!memcmp(head + byte, " KPV", 4)) {
        uint32_t n = rd32(head + byte + 4), stride = rd32(head + byte + 12) / 2;
        uint32_t channels = rd32(head + byte + 20);
        if (!n || !stride || (stride & 15) || channels < 1 || channels > 2)
            return NULL;
        length = rd32(head + byte + 8) +
                 (uint64_t)((n - 1) / stride) * stride * channels +
                 (uint64_t)(channels - 1) * stride + (n - 1) % stride + 1;
    } else {
        return NULL; /* raw movie/packed streams need their separate contract */
    }
    if (length > 64u * 1024 * 1024 || length < 48)
        return NULL;
    *size = (uint32_t)length;
    sectors = (*size + byte + 2047) / 2048;
    image = malloc(sectors * 2048);
    if (!image || !cdvd_read(lsn, sectors, image)) {
        free(image);
        return NULL;
    }
    memmove(image, image + byte, *size);
    if (!snd_stream_parse(image, *size, &info)) {
        free(image);
        return NULL;
    }
    return image;
}

static void stream_volume(Stream *s, int volume)
{
    int amplitude, value;
    int32_t gain[2];
    s->gain_valid = 0;
    s->volume = application_volume(s->volume, volume);
    value = s->volume * 127 / 1024;
    amplitude = value * 258;
    if (s->info.channels == 2) {
        pan_volume(amplitude, s->pan + 270, s->matrix[0]);
        pan_volume(amplitude, s->pan + 90, s->matrix[1]);
        s->vol_l = s->matrix[0][0]; s->vol_r = s->matrix[1][1];
    }
    else {
        pan_volume(amplitude, s->pan, gain);
        s->vol_l = gain[0]; s->vol_r = gain[1];
        s->matrix[0][0] = gain[0]; s->matrix[0][1] = gain[1];
    }
}

static int stream_next(Stream *s)
{
    Stream *node = s->queue, *remaining, *tail;
    uint32_t handle = s->handle, group = s->group;
    int volume = s->volume, pan = s->pan, pmod = s->pmod, bend = s->pbend;
    int paused = s->paused, count = s->queued_count;
    SndAdsr envelope = s->env;
    uint32_t fraction = s->frac;
    if (!node) return 0;
    remaining = node->queue; tail = s->tail;
    free(s->owned[0]); free(s->owned[1]);
    *s = *node; free(node);
    s->handle = handle; s->group = group; s->paused = paused;
    s->env = envelope; /* queued data continues on the same keyed voices */
    s->frac = fraction;
    s->queue = remaining; s->tail = remaining ? tail : NULL; s->queued_count = count - 1;
    if (!(s->flags & 64)) { s->volume = volume; s->pan = pan; }
    s->pmod = pmod; s->pbend = bend; stream_volume(s, INT_MAX);
    return 1;
}

static int auto_pan_duration(int from, int *to)
{
    int delta = (*to - from) % 360, duration;
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    *to = from + delta;
    duration = 30 * (delta < 0 ? -delta : delta) / 180;
    return duration > 0 ? duration : 1;
}

static void stream_parameters_tick(void)
{
    int i;
    for (i = 0; i < SND_STREAMS; i++) {
        Stream *s = &streams[i];
        if (!s->used || s->paused || !s->auto_pan_time) continue;
        s->auto_pan_tick++;
        s->pan = s->auto_pan_from + (s->auto_pan_to - s->auto_pan_from) * s->auto_pan_tick / s->auto_pan_time;
        if (s->auto_pan_tick >= s->auto_pan_time) s->auto_pan_time = 0;
        stream_volume(s, INT_MAX);
    }
}

static void duckers_tick(void)
{
    uint32_t active = 0;
    int i, g, gains[15];
    for (i = 0; i < SND_VOICES; i++) if (voices[i].used && voices[i].owner) {
        SoundHandler *h = handler_find(voices[i].owner);
        if (h && h->sound->group < 15) active |= 1u << h->sound->group;
    }
    for (i = 0; i < SND_STREAMS; i++) if (streams[i].used && streams[i].group < 15)
        active |= 1u << streams[i].group;
    if (!active) return; /* Retail 0x1B470 freezes until a voice is allocated. */
    for (g = 0; g < 15; g++) gains[g] = 65536;
    for (i = 0; i < 4; i++) {
        SndDucker *d = &duckers[i];
        if (!d->enabled) continue;
        if (active & (1u << d->source)) d->current = bound(d->current - d->attack, d->depth, 65536);
        else d->current = bound(d->current + d->release, d->depth, 65536);
        for (g = 0; g < 15; g++) if ((d->targets & (1u << g)) && gains[g] > d->current) gains[g] = d->current;
    }
    for (g = 0; g < 15; g++) if (gains[g] != group_duck[g]) {
        stream_gain_epoch++;
        group_duck[g] = gains[g];
        for (i = 0; i < 128; i++) if (handlers[i].used && handlers[i].sound->group == g) handler_update(&handlers[i]);
    }
    for (g = 0; g < 15; g++) snd_debug_duck(group_duck[g]);
}

static uint32_t play_disc_stream(const uint8_t *p, int len)
{
    uint32_t size, size_r, lsn, lsn_r, packed, packed_r;
    uint8_t *image, *right = NULL;
    Stream *s, *previous;
    SndStreamInfo ri;
    if (len < 32) return 0;
    previous = stream_by_handle(rd32(p + 20));
    if (rd32(p + 20) && !previous && (rd32(p + 28) & 16)) return 0;
    if (previous && previous->queued_count >= 32) return 0;
    lsn = rd32(p); lsn_r = rd32(p + 4);
    packed = rd32(p + 8); packed_r = rd32(p + 12);
    image = read_stream(lsn, packed & 0xFFFF, &size);
    if (!image) return 0;
    if (lsn_r) {
        right = read_stream(lsn_r, packed_r & 0xFFFF, &size_r);
        if (!right || !snd_stream_parse(right, size_r, &ri) || ri.channels != 1) {
            free(image); free(right); return 0;
        }
    }
    if (previous) {
        s = calloc(1, sizeof *s);
        if (s && snd_stream_parse(image, size, &s->info)) { s->used = 1; s->image[0] = image; s->volume = 1024; }
        else { free(s); s = NULL; }
    } else s = start_stream(image, size);
    if (!s || (right && (s->info.channels != 1 || s->info.rate != ri.rate ||
                         s->info.channel_bytes != ri.channel_bytes))) {
        if (s) { if (previous) free(s); else s->used = 0; }
        free(image); free(right); return 0;
    }
    s->owned[0] = image; s->owned[1] = right;
    s->image[1] = right;
    if (right) s->info.channels = 2;
    s->paused = (rd32(p + 28) & 1) != 0;
    s->flags = rd32(p + 28);
    s->disc_managed = 1;
    if (stream_read_samples) {
        uint64_t start = grain_samples;
        if (start >= stream_io_deadline) start += stream_read_samples;
        else start = stream_io_deadline;
        s->ready_at = stream_io_deadline = start + stream_read_samples;
        s->play_at = s->ready_at + 200; /* one 240-Hz dispatch after transfer */
    }
    if (s->flags & ~(1u | 4u | 8u | 16u | 64u)) snd_commands[0x2C].unsupported++;
    s->group = rd32(p + 16);
    s->pan = (int16_t)(packed_r >> 16);
    if (s->pan == -1 || s->pan == -2) s->pan = 0;
    stream_volume(s, packed >> 16);
    if (previous) {
        if (previous->tail) previous->tail->queue = s;
        else previous->queue = s;
        previous->tail = s; previous->queued_count++;
        previous->preview_ready = 0; /* a late enqueue changes the filter lookahead */
        return 1; /* Retail 0x11E18 returns success, not a new sound handle. */
    }
    if (trace_on())
        hle_log("989snd: stream LSN %u/%u handle %u (%u Hz, %u channels, paused %d)\n",
                lsn, lsn_r, s->handle, s->info.rate, s->info.channels, s->paused);
    return s->handle;
}

/* ---------------- the RPC servers ---------------- */

static BankSlot *bank_by_handle(uint32_t handle)
{
    int i;
    for (i = 0; i < SND_BANKS; i++)
        if (banks[i].used && banks[i].handle == handle)
            return &banks[i];
    return NULL;
}

/* The driver reads banks from the disc itself: the EE sends only the LSN
   (func_00201718). The SBlk header's last two words sum to the file size. */
static uint32_t load_bank(uint32_t lsn)
{
    uint8_t head[2048];
    uint32_t size, sectors;
    uint8_t *image;
    BankSlot *slot = NULL;
    int i;
    if (!cdvd_read(lsn, 1, head))
        return 0;
    if (memcmp(head + 0x18, "SBlk", 4) != 0) {
        hle_log("989snd: bank at LSN %u is not an SBlk bank\n", lsn);
        return 0;
    }
    size = rd32(head + 16) + rd32(head + 20);
    if (size < 0x40 || size > 16u * 1024 * 1024)
        return 0;
    sectors = (size + 2047) / 2048;
    image = (uint8_t *)malloc(sectors * 2048);
    if (!image || !cdvd_read(lsn, sectors, image)) {
        free(image);
        return 0;
    }
    for (i = 0; i < SND_BANKS; i++)
        if (!banks[i].used) {
            slot = &banks[i];
            break;
        }
    if (!slot) {
        free(image);
        return 0;
    }
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    slot->handle = next_handle++;
    if (next_handle == 0)
        next_handle = 1;
    slot->image = image;
    for (i = 0; i < 4096; i++) slot->grain_choice[i] = INT16_MIN;
    if (!snd_bank_parse(&slot->bank, image, size)) {
        hle_log("989snd: bank at LSN %u did not parse\n", lsn);
        slot->used = 0;
        free(image);
        return 0;
    }
    if (hle_verbose || trace_on())
        hle_log("989snd: bank LSN %u loaded as handle %u (%d sounds)\n",
                lsn, slot->handle, slot->bank.nsounds);
    return slot->handle;
}

static void trace_cmd(const char *what, int cmd, const uint8_t *p, int len)
{
    int i;
    if (!trace_on())
        return;
    hle_log("989snd %s cmd 0x%X len %d:", what, cmd, len);
    for (i = 0; i + 4 <= len && i < 48; i += 4)
        hle_log(" %08X", rd32(p + i));
    hle_log("\n");
}

/* One queued command; returns the word the EE callback receives. */
static uint32_t exec_cmd(int cmd, const uint8_t *p, int len)
{
    if ((unsigned)cmd >= 65536) return 0;
    snd_commands[cmd].calls++;
    snd_report_dirty = 1;
    snd_debug_gain_command(cmd, p, len);
    switch (cmd) {
    case 0x00: /* Native sound-system initialization precedes all bank plays. */
        return 0;
    case 0x2A: /* Streaming buffers/IOP transfer threads are replaced by resident images. */
        return 0;
    case 0x64: /* SPDIF digital output mode; no effect on the stereo host PCM. */
        return 0;
    case 0x60: { /* ConfigureDucker(index, source, targets, depth, attack, release), 0x1B64C */
        SndDucker *d;
        int ticks, index;
        if (len < 8) return 0;
        index = (int)rd32(p);
        if (index < 0 || index >= 4) { snd_commands[cmd].unsupported++; return 0; }
        d = &duckers[index];
        if (rd32(p + 4) == UINT32_MAX) { memset(d, 0, sizeof *d); duckers_tick(); return 0; }
        if (len < 24 || rd32(p + 4) >= 15) { snd_commands[cmd].unsupported++; return 0; }
        d->enabled = 1; d->source = (int)rd32(p + 4); d->targets = rd32(p + 8);
        d->depth = bound((int)rd32(p + 12), 0, 65536); d->current = 65536;
        ticks = (int)((uint64_t)rd32(p + 16) * 30 >> 16);
        d->attack = bound((65536 - d->depth) / (ticks > 0 ? ticks : 1), 1, 65536);
        ticks = (int)((uint64_t)rd32(p + 20) * 30 >> 16);
        d->release = bound((65536 - d->depth) / (ticks > 0 ? ticks : 1), 1, 65536);
        return 0;
    }
    case 0x0D: { /* SetVoiceAllocatorMode(mode, digital), IOP 0x18D48 */
        int i, mode = len >= 4 ? (int)rd32(p) : 0;
        for (i = 0; i < 16; i++) {
            voice_range[i][0] = mode == 1 && i != 1 ? 24 : 0;
            voice_range[i][1] = mode == 1 && i == 1 ? 23 : 47;
        }
        return 0;
    }
    case 0x4E: { /* SetVoiceRange(group, first, last), IOP 0x18DE0 */
        int group, first, last;
        if (len < 12) return 0;
        group = (int)rd32(p); first = (int)rd32(p + 4); last = (int)rd32(p + 8);
        if (group < 0 || group > 15) { snd_commands[cmd].unsupported++; return 0; }
        first = bound(first, 0, 47); last = bound(last, 0, 47);
        voice_range[group][0] = first; voice_range[group][1] = last;
        trace_cmd("voice range", cmd, p, len);
        return 0;
    }
    case 0x0E: { /* SetReverbType(core mask, preset), IOP 0xEB3C */
        int core, mode;
        uint32_t mask;
        if (len < 8) return 0;
        mask = rd32(p); mode = (int)rd32(p + 4);
        for (core = 0; core < 2; core++) if (mask & (1u << core)) {
            if (!reverb_preset(core, mode, 0, 0)) snd_commands[cmd].unsupported++;
            else reverb_depth[core][0] = reverb_depth[core][1] = 0;
        }
        return 0;
    }
    case 0x0B: /* SetPlaybackMode, IOP 0x1A800; 1 requests mono gains */
        if (len >= 4) {
            int i;
            playback_mode = (int)rd32(p);
            for (i = 0; i < 128; i++) if (handlers[i].used) handler_update(&handlers[i]);
        }
        return 0;
    case 0x61: { /* StopAllSoundsInGroup(mask), IOP 0xFBBC */
        int i;
        if (len < 4) return 0;
        for (i = 0; i < 128; i++)
            if (handlers[i].used && handlers[i].sound->group < 32 &&
                (rd32(p) & (1u << handlers[i].sound->group))) handler_stop(&handlers[i]);
        for (i = 0; i < SND_STREAMS; i++)
            if (streams[i].used && streams[i].group < 32 &&
                (rd32(p) & (1u << streams[i].group))) stream_clear(&streams[i]);
        return 0;
    }
    case 0x06: { /* UnloadBank: handlers/voices must not retain freed sample data */
        BankSlot *bank = len >= 4 ? bank_by_handle(rd32(p)) : NULL;
        int i;
        if (!bank) return 0;
        for (i = 0; i < 128; i++) if (handlers[i].used && handlers[i].bank == bank) {
            handler_keyoff(&handlers[i], 1); handlers[i].used = 0;
        }
        for (i = 0; i < SND_VOICES; i++) if (voices[i].used &&
            (uintptr_t)voices[i].adpcm >= (uintptr_t)bank->bank.data &&
            (uintptr_t)voices[i].adpcm - (uintptr_t)bank->bank.data < bank->bank.size)
            voices[i].used = 0;
        free(bank->image); memset(bank, 0, sizeof *bank);
        return 0;
    }
    case 0x0F: { /* SetReverbDepth(core mask, left, right), IOP 0xEF64/0xEFE0 */
        uint32_t mask;
        int core;
        if (len < 12) return 0;
        mask = rd32(p);
        for (core = 0; core < 2; core++) if (mask & (1u << core)) {
            reverb_depth[core][0] = (int16_t)rd32(p + 4);
            reverb_depth[core][1] = (int16_t)rd32(p + 8);
        }
        return 0;
    }
    case 0x1F: case 0x20: {
        SoundHandler *h = len >= 6 ? handler_find(rd32(p)) : NULL;
        if (h) {
            if (cmd == 0x1F) h->pbend = (int16_t)rd16(p + 4);
            else h->pmod = (int16_t)rd16(p + 4);
            handler_update(h);
            handler_update_children(h, cmd == 0x1F ? 16 : 8);
        }
        return 0;
    }
    case 0x0A: { /* GetMasterVolume(group), IOP 0x1A7E4 */
        uint32_t group = len >= 4 ? rd32(p) : UINT32_MAX;
        if (group < 17) return (uint32_t)group_vol[group];
        snd_commands[cmd].unsupported++; return 0;
    }
    case 0x09: { /* SetMasterVolume(group, volume), IOP 0x1A6C4 */
        uint32_t group;
        int volume;
        if (len < 8) return 0;
        group = rd32(p); volume = (int)rd32(p + 4);
        if (volume < 0) volume = 0;
        if (volume > 1024) volume = 1024;
        if (group < 15) {
            stream_gain_epoch++;
            group_vol[group] = volume;
            {
                int i;
                for (i = 0; i < 128; i++)
                    if (handlers[i].used && handlers[i].sound->group == (int)group)
                        handler_update(&handlers[i]);
            }
        } else if (group == 16) { group_vol[group] = volume; master_vol = (volume * 16383 / 1024) * 2; }
        else if (group == 15) group_vol[group] = volume;
        else if (group != 15) snd_commands[cmd].unsupported++;
        return 0;
    }
    case 0x2C:
        return play_disc_stream(p, len);
    case 0x13: case 0x14: case 0x2D: case 0x2E: {
        Stream *s = len >= 4 ? stream_by_handle(rd32(p)) : NULL;
        int i;
        if (s) s->paused = cmd == 0x13 || cmd == 0x2D;
        if (len >= 4 && cmd < 0x2D) {
            SoundHandler *h = handler_find(rd32(p));
            if (h) handler_pause(h, cmd == 0x13);
        }
        if (len >= 4 && cmd < 0x2D)
            for (i = 0; i < SND_VOICES; i++)
                if (voices[i].used && voices[i].handle == rd32(p))
                    voices[i].paused = cmd == 0x13;
        return 0;
    }
    case 0x16: case 0x17: { /* pause/continue all sounds in selected groups */
        int i;
        if (len < 4) return 0;
        for (i = 0; i < SND_STREAMS; i++)
            if (streams[i].used && streams[i].group < 32 &&
                (rd32(p) & (1u << streams[i].group)))
                streams[i].paused = cmd == 0x16;
        for (i = 0; i < 128; i++)
            if (handlers[i].used && handlers[i].sound->group < 32 &&
                (rd32(p) & (1u << handlers[i].sound->group)))
                handler_pause(&handlers[i], cmd == 0x16);
        return 0;
    }
    case 0x19: { /* SoundIsStillPlaying: returns the handle or zero (IOP 0xDA94) */
        uint32_t handle = len >= 4 ? rd32(p) : 0;
        int i;
        if (stream_by_handle(handle)) return handle;
        if (handler_find(handle)) return handle;
        for (i = 0; i < SND_VOICES; i++)
            if (voices[i].used && voices[i].handle == handle) return handle;
        return 0;
    }
    case 0x4F: {
        Stream *s = len >= 4 ? stream_by_handle(rd32(p)) : NULL;
        return s ? (grain_samples >= s->ready_at) : 0xFFFFFFFFu;
    }
    case 0x30: case 0x31: case 0x32: {
        Stream *s = len >= 4 ? stream_by_handle(rd32(p)) : NULL;
        uint32_t total, pos;
        if (!s) return 0;
        total = (uint32_t)((uint64_t)(s->info.channel_bytes / 16) * 28 * 1024 / s->info.rate);
        pos = (uint32_t)((uint64_t)s->source_samples * 1024 / s->info.rate);
        return cmd == 0x30 ? pos : cmd == 0x32 ? total : total > pos ? total - pos : 0;
    }
    case 0x38: { /* disc read for the game's loader (func_00202760) */
        uint32_t lsn, sectors;
        if (len < 12) return 0;
        lsn = rd32(p); sectors = rd32(p + 4);
        void *dst = (void *)(uintptr_t)rd32(p + 8);
        int ok = cdvd_read(lsn, sectors, dst);
        D_004CD380[4] = ok ? 0 : 1;
        D_004CD380[0] = 0;
        return 0;
    }
    case 0x5C: { /* play a named sound (func_00201B68/func_00201BD0) */
        uint32_t bank_handle;
        if (len < 32) return 0;
        bank_handle = rd32(p);
        int volume = len >= 12 ? (int)rd32(p + 8) : 0x400;
        int pan = (int)rd32(p + 12);
        BankSlot *bank = bank_by_handle(bank_handle);
        const SndBankSound *sound;
        trace_cmd("play", cmd, p, len);
        if (!bank || len < 32)
            return 0;
        sound = snd_bank_find(&bank->bank, (const char *)p + 16);
        if (!sound) {
            if (trace_on())
                hle_log("989snd: sound %.16s not in bank %u\n", p + 16, bank_handle);
            if (trace_on()) host_sound_log_stack();
            return 0;
        }
        {
            SoundHandler *h = handler_start(bank, sound, volume, pan,
                                             len >= 36 ? (int16_t)rd16(p + 32) : 0,
                                             len >= 36 ? (int16_t)rd16(p + 34) : 0,
                                             NULL, -1, -1);
            return h ? h->handle : 0;
        }
    }
    case 0x15: case 0x2F: /* stop sound / stop VAG stream */
        if (len >= 4) {
            Stream *s = stream_by_handle(rd32(p));
            if (s) stream_clear(s);
            {
                SoundHandler *h = handler_find(rd32(p));
                if (h) handler_stop(h);
            }
            voice_stop(rd32(p));
        }
        return 0;
    case 0x08: { /* stop everything (func_002018E8) */
        int i;
        for (i = 0; i < SND_STREAMS; i++) stream_clear(&streams[i]);
        for (i = 0; i < 128; i++) if (handlers[i].used) handler_stop(&handlers[i]);
        for (i = 0; i < SND_VOICES; i++)
            if (voices[i].used)
                snd_adsr_release(&voices[i].env), voices[i].releasing = 1;
        return 0;
    }
    case 0x1B: case 0x21: { /* VolPan / SetSoundParams, IOP 0xDBB8/0xDC94 */
        Stream *s = len >= 12 ? stream_by_handle(rd32(p)) : NULL;
        uint32_t handle, flags;
        int i, volume, pan;
        if (len < 12) return 0;
        handle = rd32(p);
        flags = cmd == 0x1B ? 3 : rd32(p + 4);
        if ((flags & 6) == 6) flags &= ~2u; /* smooth pan wins over immediate pan */
        volume = (int)rd32(p + (cmd == 0x1B ? 4 : 8));
        pan = cmd == 0x1B ? (int)rd32(p + 8) : len >= 16 ? (int)rd32(p + 12) : -2;
        {
            SoundHandler *h = handler_find(handle);
            if (h) {
                if (flags & 1) h->volume = application_volume(h->volume, volume);
                if ((flags & 2) && pan != -2) h->pan = pan == -1 ? h->sound->pan : pan;
                if ((flags & 8) && len >= 18) h->pmod = (int16_t)rd16(p + 16);
                if ((flags & 16) && len >= 22) h->pbend = (int16_t)rd16(p + 20);
                if ((flags & 4) && pan != -2) {
                    h->auto_pan_from = h->pan; h->auto_pan_to = pan == -1 ? h->sound->pan : pan;
                    h->auto_pan_tick = 0; h->auto_pan_time = auto_pan_duration(h->pan, &h->auto_pan_to);
                } else if (flags & 2) h->auto_pan_time = 0;
                if (flags & ~31u) snd_commands[cmd].unsupported++;
                handler_update(h);
                handler_update_children(h, flags & 27);
                return cmd == 0x21 ? handle : 0;
            }
        }
        if (flags & ~31u)
            snd_commands[cmd].unsupported++;
        if (s) {
            if ((flags & 2) && pan != -2) s->pan = pan == -1 ? 0 : pan;
            if ((flags & 8) && len >= 18) s->pmod = (int16_t)rd16(p + 16);
            if ((flags & 16) && len >= 22) s->pbend = (int16_t)rd16(p + 20);
            if ((flags & 4) && pan != -2) {
                s->auto_pan_from = s->pan; s->auto_pan_to = pan == -1 ? 0 : pan; s->auto_pan_tick = 0;
                s->auto_pan_time = auto_pan_duration(s->pan, &s->auto_pan_to);
            } else if (flags & 2) s->auto_pan_time = 0;
            stream_volume(s, flags & 1 ? volume : INT_MAX);
        }
        for (i = 0; i < SND_VOICES; i++)
            if (voices[i].used && voices[i].handle == handle) {
                if (flags & 1) voice_volume(&voices[i], volume);
                return cmd == 0x21 ? handle : 0;
            }
        return cmd == 0x21 && s ? handle : 0;
    }
    default:
        if (!snd_commands[cmd].unimplemented++) {
            if (cmd != 0x38)
                trace_cmd("queued", cmd, p, len);
            if (hle_verbose)
                hle_log("989snd: queued command 0x%X (%d bytes)\n", cmd, len);
        }
        return 0;
    }
}

void sound_rpc(int fno, void *send, int ssize, void *recv, int rsize)
{
    if (recv && rsize > 0)
        memset(recv, 0, (size_t)rsize);
    if (fno == 0x4D && ssize >= 4) { /* batched queue (func_00202388) */
        const uint8_t *p = (const uint8_t *)send + 4, *end = (const uint8_t *)send + ssize;
        uint32_t count = rd32((const uint8_t *)send), i;
        uint32_t *reply = (uint32_t *)recv;
        for (i = 0; i < count && (size_t)(end - p) >= 4; i++) {
            int cmd = p[0] | p[1] << 8, len = p[2] | p[3] << 8;
            uint32_t result = 0;
            if ((size_t)(end - p - 4) < (size_t)len) break;
            result = exec_cmd(cmd, p + 4, len);
            if (reply && rsize >= (int)(i + 3) * 4)
                reply[i + 1] = result;
            {
                size_t stride = (4u + (unsigned)len + 3) & ~3u;
                if (stride > (size_t)(end - p)) break;
                p += stride;
            }
        }
        if (reply && rsize >= 4) {
            reply[0] = 0xFFFFFFFF;
            reply[rsize / 4 - 1] = 0xFFFFFFFF;
        }
        return;
    }
    if (fno != 0 && trace_on())
        hle_log("989snd: direct function %d (send %d, receive %d)\n", fno, ssize, rsize);
    if (recv && rsize >= 12)
        ((uint32_t *)recv)[1] = exec_cmd(fno, (const uint8_t *)send, ssize);
    /* Direct calls use the same dispatch table in the retail IOP. The EE
       reads reply word 1 and waits for the 0xFFFFFFFF framing words. */
    if (recv && rsize >= 8) {
        ((uint32_t *)recv)[0] = 0xFFFFFFFF;
        ((uint32_t *)recv)[rsize / 4 - 1] = 0xFFFFFFFF;
    }
}

void sound_rpc2(int fno, void *send, int ssize, void *recv, int rsize)
{
    if (recv && rsize > 0)
        memset(recv, 0, (size_t)rsize);
    if (fno == 3 && ssize >= 8) { /* load bank by LSN (func_00201718) */
        uint32_t handle = load_bank(rd32((const uint8_t *)send));
        if (recv && rsize >= 4)
            *(uint32_t *)recv = handle;
        return;
    }
    if (trace_on())
        hle_log("989snd (2): function %d (send %d, receive %d)\n", fno, ssize, rsize);
}



