/* Native 989snd: the game's IOP sound driver (server 0x123456) and its loading RPC
   (server 0x123457), replaced on the host (finding 022). The EE library in src/
   (func_00201180 and the func_00201xxx wrappers) queues small commands and sends
   them as RPC function 0x4D; banks are loaded by disc LSN through the second
   server. Here those calls start and stop voices that decode PS2 ADPCM (VAG),
   mix them in integer arithmetic and play through WASAPI.

   Determinism: voices, envelopes and stream positions advance only in
   sound_tick(), once per vsync, by 48000 * 1001 / 60000 samples. The WASAPI
   thread only drains what ticks rendered, so nothing the game can read (voice
   handles, bank handles, stream state) depends on the host audio clock.

   Evidence: DHD_SND_WAV=path writes the final 48000 Hz stereo 16-bit mix,
   before the output ring can drop samples. DHD_SND_REPORT=path writes command
   counts (calls, unimplemented dispatches, unsupported sub-operations). Both
   are refreshed at game ticks, including with DHD_SND_OFF=1. DHD_SND_VOICE_TRACE=path
   writes every sounding voice's sound, wave offset, pitch, envelope and volumes every
   800 samples (finding 133, for comparing with PCSX2's SPU2 voices). Use ignored
   build/ paths: a game WAV contains proprietary audio. */
#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* RPC servers (hle.h declares rpc_sound/rpc_sound2 with these signatures). */
void sound_rpc(int fno, void *send, int ssize, void *recv, int rsize);
void sound_rpc2(int fno, void *send, int ssize, void *recv, int rsize);

/* One frame of game time: render the samples that fell due. Called by vsync(). */
void sound_tick(void);

/* Host output counters over a DHD_BENCH window: start (0), print (1). */
void snd_host_bench(int end);

/* ---- testable core (test_sound.c; no host audio, no disc) ---- */

/* One PS2 ADPCM block decoded: 28 samples. State is the two previous samples. */
typedef struct { int32_t prev1, prev2; } SndAdpcm;

/* Decodes one 16-byte block into out[28]; returns its flag byte. When a block
   carries the loop-end flag and a loop start was seen, *loop_block (a block
   index) says where playback continues. All arithmetic is integer. */
uint8_t snd_adpcm_block(SndAdpcm *st, const uint8_t *block, int16_t out[28]);

/* Four consecutive source samples, fractional phase 0..255. Products are
   shifted separately, as in the SPU2; the sum remains signed 32-bit. */
int32_t snd_gaussian_sample(const int16_t samples[4], unsigned phase);

/* SPU2-style ADSR envelope, advanced one output sample at a time. Rates come
   from a libsd ADSR register pair; snd_adsr_set decodes them. The level is
   0..0x7FFF. Phases: 0 attack, 1 decay, 2 sustain, 3 release, 4 done. */
typedef struct {
    int phase;
    int32_t level;
    int32_t sustain_level;
    uint16_t adsr1, adsr2;
    uint32_t counter;
    int direct;
    union {
        struct { int32_t attack_step, decay_step, release_step; } direct_rate;
        struct { uint32_t increment; int32_t change; unsigned flags; int phase; } rate;
    };
} SndAdsr;
void snd_adsr_set(SndAdsr *e, uint32_t adsr1, uint32_t adsr2);
void snd_adsr_start(SndAdsr *e);
void snd_adsr_release(SndAdsr *e);
int32_t snd_adsr_tick(SndAdsr *e);

typedef struct { uint16_t reg; int32_t level; uint32_t counter; } SndVolume;
/* Fixed volume is signed 15-bit; bit 15 instead selects a sweep. */
void snd_volume_set(SndVolume *v, uint16_t reg);
int32_t snd_volume_tick(SndVolume *v);
int32_t snd_q15(int32_t sample, int32_t volume);
int snd_test_reverb_depth(int core, int channel);
void snd_test_reverb_frame(int core, const int32_t input[2], int32_t output[2]);

/* A parsed .BNK bank (989snd SBlk) or .VAG/.VPK stream header, from memory. */
typedef struct {
    char name[17];
    uint32_t adpcm_off, adpcm_len; /* bytes, inside the bank image */
    int index;
    uint32_t grain_off;
    int volume, group, pan, ngrains;
    int instance_limit;  /* record byte +5: live copies allowed when flags has 8 (finding 133) */
    uint16_t flags;      /* 1 loop, 2 solo, 8 instance limit, 0x10 steal the quietest, 0x20 the oldest */
} SndBankSound;

typedef struct {
    int priority, volume, center_note, center_fine, pan, pb_low, pb_high;
    uint16_t adsr1, adsr2, flags;
    uint32_t adpcm_off, adpcm_len;
} SndTone;

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t wave_off;
    uint32_t wave_len, metadata_end;
    int nsounds;
    SndBankSound sounds[512];
} SndBank;

/* Validated version-1 SBlk tables. Wave addresses come from tone grains,
   never waveform order (loop-ending filler blocks are not separate sounds). */
int snd_bank_parse(SndBank *bank, const uint8_t *data, uint32_t size);
const SndBankSound *snd_bank_find(const SndBank *bank, const char name[16]);
int snd_bank_tone(const SndBank *bank, uint32_t grain_off, SndTone *tone);

/* .VAG stream header: sample rate and ADPCM data offset/length, or 0. */
int snd_vag_parse(const uint8_t *data, uint32_t size, uint32_t *rate,
                  uint32_t *adpcm_off, uint32_t *adpcm_len);

typedef struct {
    uint32_t rate, offset, channel_bytes, interleave, channels, loop_byte;
} SndStreamInfo;
/* Validated mono VAG or one/two-channel VPK header. */
int snd_stream_parse(const uint8_t *data, uint32_t size, SndStreamInfo *info);

/* Test hooks: feed a parsed bank, start a voice by sound name, render. */
int snd_test_add_bank(const uint8_t *data, uint32_t size);
int snd_test_play(int bank, const char name[16], int volume, int pitch);
int snd_test_stream(const uint8_t *data, uint32_t size, int stereo);
/* Raw static ADPCM at an exact Q12 SPU pitch, for FIFO endpoint oracles. */
int snd_test_raw_voice(const uint8_t *adpcm, uint32_t size, uint16_t pitch);
void snd_test_render(int16_t *stereo, int samples);
/* Publish optional diagnostics without advancing the sample clock. */
void snd_test_report(void);
/* Fixture-only service-time override; default production read is 4800 game
   samples. Zero isolates DSP tests from modeled IOP loading. */
void snd_test_stream_read_samples(uint32_t samples);
uint64_t snd_test_position(void);
/* Active SPU-equivalent voices, including each channel of an unpaused stream. */
int snd_test_active_voices(void);
void snd_test_reset(void);

#endif
