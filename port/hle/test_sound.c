/* Tests for the native 989snd core (sound.c): ADPCM decoding, ADSR envelopes,
   bank/VAG parsing and mixer determinism. Expected sample values are computed
   from the SPU2 ADPCM definition (filter table, (nibble << 12) >> shift), not
   copied from the implementation's output. Built by test_runtime.py with
   SND_NO_HOST, so no audio device is touched. */
#include "hle.h"
#include "sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sound.c's outside world, stubbed: no disc, log to the test output. */
int hle_verbose;
volatile int32_t D_004CD380[8];
static uint8_t disc[8 * 2048];
static unsigned disc_read_burn;
void hle_log(const char *fmt, ...)
{
    (void)fmt;
}
/* A stand-in for the disc's libsd module (MOD\LIBSD.IRX), where the mixer reads its reverb
   presets: some bytes, then the table in libsd's shape (ten work-area sizes in units of 4 sample
   words, ten {flags, reg[32]} entries). Mode 0 is OFF; modes 1-9 are invented rooms, made up for
   these tests (not the retail values): a small work area, two comb taps and the all-pass pair. */
#define TEST_MODULE_LSN 64
static uint8_t test_module[2048];
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void build_test_module(void)
{
    static const uint16_t room[30] = {
        0x0020, 0x0018, 0x6000, 0x5000, 0xC000, 0x3000, 0xD000, 0x2800, 0x4000, 0xC000,
        0x0300, 0x02F0, 0x0280, 0x0270, 0x0200, 0x01F0, 0x0301, 0x02F1, 0x0240, 0x0230,
        0x01C0, 0x01B0, 0x0180, 0x0170, 0x0241, 0x0231, 0x0100, 0x00F0, 0x0060, 0x0050};
    uint8_t *t = test_module + 100;
    int mode, i;
    memset(test_module, 0xA5, sizeof test_module);
    for (mode = 0; mode < 10; mode++) {
        uint8_t *e = t + 40 + mode * 68;
        uint32_t size = mode ? 0x100 + mode * 0x20 : 2;
        t[mode * 4] = (uint8_t)size; t[mode * 4 + 1] = (uint8_t)(size >> 8); t[mode * 4 + 2] = t[mode * 4 + 3] = 0;
        memset(e, 0, 68);
        for (i = 0; i < 32; i++)
            put16(e + 4 + i * 2, mode ? (i < 30 ? (uint16_t)(room[i] + (i >= 10 ? mode : 0)) : 0x8000)
                                      : (i == 10 || i == 11 || i == 18 || i == 19));
    }
}
int cdvd_read(uint32_t lsn, uint32_t count, void *dst)
{
    volatile unsigned burn;
    if (lsn == TEST_MODULE_LSN && count == 1) {
        if (!test_module[0]) build_test_module();
        memcpy(dst, test_module, sizeof test_module);
        return 1;
    }
    if (lsn >= 8 || count > 8 - lsn) return 0;
    for (burn = 0; burn < disc_read_burn; burn++) {} /* vary host read cost only */
    memcpy(dst, disc + lsn * 2048, count * 2048);
    return 1;
}
int cdvd_find(const char *path, uint32_t *lsn, uint32_t *size)
{
    if (strcmp(path, "/MOD/LIBSD.IRX;1")) return 0;
    *lsn = TEST_MODULE_LSN; *size = sizeof test_module;
    return 1;
}

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void block(uint8_t out[16], int predictor_shift, int flags, const uint8_t *nibbles)
{
    int i;
    memset(out, 0, 16);
    out[0] = (uint8_t)predictor_shift;
    out[1] = (uint8_t)flags;
    for (i = 0; i < 28; i += 2)
        out[2 + i / 2] = (uint8_t)(nibbles[i] | nibbles[i + 1] << 4);
}

static void test_adpcm(void)
{
    uint8_t b[16];
    int16_t out[28];
    SndAdpcm st = {0, 0};
    /* silence stays silent */
    {
        uint8_t nib[28] = {0};
        block(b, 0x00, 0, nib);
        CHECK(snd_adpcm_block(&st, b, out) == 0);
        CHECK(out[0] == 0 && out[27] == 0 && st.prev1 == 0);
    }
    /* filter 0, shift 12: sample = signed nibble */
    {
        uint8_t nib[28] = {0};
        nib[0] = 1, nib[1] = 2, nib[2] = 8, nib[3] = 15;
        block(b, 0x0C, 0, nib);
        snd_adpcm_block(&st, b, out);
        CHECK(out[0] == 1 && out[1] == 2);
        CHECK(out[2] == -8 && out[3] == -1);
    }
    /* filter 0, shift 0: sample = nibble << 12 */
    {
        uint8_t nib[28] = {0};
        SndAdpcm fresh = {0, 0};
        nib[0] = 1, nib[1] = 8;
        block(b, 0x00, 0, nib);
        snd_adpcm_block(&fresh, b, out);
        CHECK(out[0] == 4096 && out[1] == -32768);
        CHECK(fresh.prev1 == 0); /* later nibbles are zero */
    }
    /* filter 1 predicts from the previous sample: (4096 * 60 + 32) >> 6 = 3840 */
    {
        uint8_t nib[28] = {0};
        SndAdpcm fresh = {4096, 0};
        block(b, 0x10, 0, nib);
        snd_adpcm_block(&fresh, b, out);
        CHECK(out[0] == 3840);
    }
    /* loop/end flags pass through */
    {
        uint8_t nib[28] = {0};
        block(b, 0x00, 3, nib);
        CHECK(snd_adpcm_block(&st, b, out) == 3);
    }
}

static void test_adsr(void)
{
    SndAdsr e;
    /* direct steps: attack +0x4000, decay -0x2000, sustain 0x4000,
       release -0x4000 */
    snd_adsr_set(&e, 0x80000000u | (0x2000u << 15) | 0x4000,
                 (0x4000u << 15) | 0x4000);
    snd_adsr_start(&e);
    CHECK(snd_adsr_tick(&e) == 0x4000);
    CHECK(snd_adsr_tick(&e) == 0x7FFF); /* attack clamps, now decaying */
    CHECK(e.phase == 1);
    CHECK(snd_adsr_tick(&e) == 0x7FFF - 0x2000);
    snd_adsr_release(&e);
    CHECK(e.phase == 3);
    CHECK(snd_adsr_tick(&e) == 0x7FFF - 0x2000 - 0x4000);
    snd_adsr_tick(&e);
    CHECK(e.phase == 4);
    CHECK(snd_adsr_tick(&e) == 0);
}

static void test_gaussian(void)
{
    int16_t flat[4] = {4096,4096,4096,4096};
    int16_t high[4] = {32767,32767,32767,32767};
    int16_t low[4] = {-32768,-32768,-32768,-32768};
    int16_t mixed[4] = {4096,-4096,8192,-8192};
    /* PCSX2 GetVoiceValues goldens: shift each signed product before
       summing. Shifting only the total loses the negative-tap rounding. */
    CHECK(snd_gaussian_sample(flat, 0) == 4077);
    CHECK(snd_gaussian_sample(high, 0) == 32637);
    CHECK(snd_gaussian_sample(low, 0) == -32640);
    CHECK(snd_gaussian_sample(mixed, 0) == -1054);
    CHECK(snd_gaussian_sample(mixed, 127) == 1919);
    CHECK(snd_gaussian_sample(mixed, 255) == 3928);
}

static void test_spu_levels(void)
{
    SndVolume v = {0};
    SndAdsr e;
    int i;
    CHECK(snd_q15(32767, 32767) == 32766);
    CHECK(snd_q15(-32768, 32767) == -32767);
    CHECK(snd_q15(8192, -16384) == -4096);
    snd_volume_set(&v, 0x3FFF); CHECK(snd_volume_tick(&v) == 32766);
    snd_volume_set(&v, 0x4000); CHECK(snd_volume_tick(&v) == -32768);
    snd_volume_set(&v, 0x7FFF); CHECK(snd_volume_tick(&v) == -2);
    snd_volume_set(&v, 0); snd_volume_set(&v, 0x8000);
    CHECK(snd_volume_tick(&v) == 14336);
    CHECK(snd_volume_tick(&v) == 28672);
    CHECK(snd_volume_tick(&v) == 32767);
    snd_volume_set(&v, 0xA000); CHECK(snd_volume_tick(&v) == 16383);
    CHECK(snd_volume_tick(&v) == 0);
    /* Slow attack shift 12: a +7 change every two hardware samples. */
    snd_adsr_set(&e, 12u << 10, 0); snd_adsr_start(&e);
    CHECK(snd_adsr_tick(&e) == 0); CHECK(snd_adsr_tick(&e) == 7);
    /* Exponential release shift zero empties a full envelope in one tick. */
    snd_adsr_set(&e, 0, 0x20); snd_adsr_start(&e);
    for (i = 0; i < 3; i++) snd_adsr_tick(&e);
    snd_adsr_release(&e); CHECK(snd_adsr_tick(&e) == 16383);
    CHECK(snd_adsr_tick(&e) == 8191);
    /* A linear release (bit 5 clear) is not an exponential mode bit. */
    snd_adsr_set(&e, 0, 0); snd_adsr_start(&e);
    for (i = 0; i < 3; i++) snd_adsr_tick(&e);
    snd_adsr_release(&e); CHECK(snd_adsr_tick(&e) == 16383);
    CHECK(snd_adsr_tick(&e) == 0 && e.phase == 4);
}

/* A minimal SBlk bank with two one-block sounds. */
static uint32_t put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
    return 4;
}

#define TEST_WAVE 0x300
#define TEST_BANK_SIZE (TEST_WAVE + 32)
static int build_bank(uint8_t *buf)
{
    uint8_t nib[28];
    int i;
    memset(buf, 0, 2048);
    put32(buf, 3); put32(buf + 4, 2); put32(buf + 8, 0x18);
    put32(buf + 12, TEST_WAVE - 0x18); put32(buf + 16, TEST_WAVE); put32(buf + 20, 32);
    memcpy(buf + 0x18, "SBlk", 4); put32(buf + 0x1C, 1);
    buf[0x18 + 0x16] = 2;
    put32(buf + 0x18 + 0x1C, 0x3C); put32(buf + 0x18 + 0x20, 0x54);
    put32(buf + 0x18 + 0x34, 0x100);
    for (i = 0; i < 2; i++) {
        int sound = 0x18 + 0x3C + i * 12, grain = 0x18 + 0x54 + i * 40;
        buf[sound] = 127; buf[sound + 1] = 15; buf[sound + 4] = 1;
        put32(buf + sound + 8, i * 40); put32(buf + grain, 1);
        buf[grain + 8] = 100; buf[grain + 9] = 127; buf[grain + 10] = (uint8_t)-60;
        put32(buf + grain + 24, i * 16);
    }
    put32(buf + 0x118 + 8, 0x98);
    memcpy(buf + 0x1B0, "TEST_ONE", 8); memcpy(buf + 0x1C4, "TEST_TWO", 8);
    buf[0x1C4 + 16] = 1;
    memset(nib, 0, sizeof nib);
    nib[0] = 1;
    block(buf + TEST_WAVE, 0x00, 1, nib);  /* filter 0: first sample 4096, then end */
    block(buf + TEST_WAVE + 16, 0x00, 1, nib);
    (void)i;
    return TEST_BANK_SIZE;
}

static void test_bank_and_mix(void)
{
    static uint8_t image[2048];
    static int16_t first[480 * 2], second[480 * 2];
    int size = build_bank(image), bank, voice;
    SndBank parsed;
    char name[16] = "TEST_ONE";
    CHECK(snd_bank_parse(&parsed, image, (uint32_t)size) == 1);
    CHECK(parsed.nsounds == 2);
    CHECK(snd_bank_find(&parsed, name) != NULL);
    snd_test_reset();
    bank = snd_test_add_bank(image, (uint32_t)size);
    CHECK(bank != 0);
    voice = snd_test_play(bank, name, 0x400, 0);
    CHECK(voice != 0);
    snd_test_render(first, 480);
    CHECK(snd_test_position() == 480);
    /* the first rendered sample is the ADPCM peak under a full attack */
    CHECK(first[0] != 0);
    /* determinism: the same voice rendered again is byte-identical */
    snd_test_reset();
    bank = snd_test_add_bank(image, (uint32_t)size);
    CHECK(snd_test_play(bank, name, 0x400, 0) != 0);
    snd_test_render(second, 200);
    snd_test_render(second + 200 * 2, 280); /* different chunking, same stream */
    CHECK(memcmp(first, second, sizeof first) == 0);
    CHECK(snd_test_position() == 480);
}

static void test_tone_addresses(void)
{
    uint8_t image[2048], nib[28];
    SndBank parsed;
    const SndBankSound *s;
    int16_t out[200 * 2];
    int i, h;
    build_bank(image);
    /* A valid waveform, its repeating filler footer, and another waveform.
       The first sound deliberately references the third block. Selecting
       ADPCM end blocks by sound ordinal would turn the footer into DC. */
    put32(image + 20, 48);
    image[TEST_WAVE + 1] = 1;
    memset(nib, 7, sizeof nib); block(image + TEST_WAVE + 16, 0, 7, nib);
    for (i = 0; i < 28; i++) nib[i] = i & 1 ? 15 : 1;
    block(image + TEST_WAVE + 32, 0, 1, nib);
    put32(image + 0x18 + 0x54 + 24, 32);
    CHECK(snd_bank_parse(&parsed, image, TEST_BANK_SIZE + 16));
    s = snd_bank_find(&parsed, "TEST_ONE\0\0\0\0\0\0\0\0");
    CHECK(s && s->adpcm_off == TEST_WAVE + 32 && s->adpcm_len == 16);
    snd_test_reset(); h = snd_test_add_bank(image, TEST_BANK_SIZE + 16);
    CHECK(snd_test_play(h, "TEST_ONE\0\0\0\0\0\0\0\0", 1024, 0));
    CHECK(snd_test_play(h, "TEST_ONE\0\0\0\0\0\0\0\0", 1024, 0));
    snd_test_render(out, 200);
    CHECK(out[0] == -3326 && out[198] == 0 && out[398] == 0);
    put32(image + 0x18 + 0x54 + 24, 0x7FFFFFF0);
    CHECK(!snd_bank_parse(&parsed, image, TEST_BANK_SIZE + 16));
    snd_test_reset();
    /* Names past index 63 must resolve to their actual sound record. */
    {
        uint8_t large[4096] = {0};
        put32(large, 3); put32(large + 4, 2); put32(large + 8, 24);
        put32(large + 12, 0x900); put32(large + 16, 0x918); put32(large + 20, 16);
        memcpy(large + 24, "SBlk", 4); put32(large + 28, 1); large[24 + 22] = 128;
        put32(large + 24 + 28, 0x3C); put32(large + 24 + 32, 0x700);
        put32(large + 24 + 52, 0x700); put32(large + 24 + 0x700 + 8, 0x98);
        memcpy(large + 24 + 0x798, "HIGH_INDEX", 10);
        large[24 + 0x798 + 16] = 100;
        large[0x919] = 1;
        CHECK(snd_bank_parse(&parsed, large, 0x928));
        s = snd_bank_find(&parsed, "HIGH_INDEX\0\0\0\0\0\0");
        CHECK(s && s->index == 100 && parsed.nsounds == 128);
    }
}

static void test_vag_stream(void)
{
    static uint8_t vag[48 + 32];
    static int16_t out[480 * 2];
    uint8_t nib[28] = {0};
    uint32_t rate, off, len;
    memcpy(vag, "VAGp", 4);
    vag[11] = 32;                     /* data length, big-endian at +12 */
    vag[15] = 32;
    vag[18] = 0xAC, vag[19] = 0x44;   /* 44100 Hz at +16 */
    nib[0] = 1;
    block(vag + 48, 0x00, 1, nib);
    block(vag + 64, 0x00, 1, nib);
    CHECK(snd_vag_parse(vag, sizeof vag, &rate, &off, &len) == 1);
    CHECK(rate == 44100 && off == 48 && len == 32);
    snd_test_reset();
    CHECK(snd_test_stream(vag, sizeof vag, 0) != 0);
    snd_test_render(out, 480);
    CHECK(out[0] != 0);
}

static uint32_t rpc(int cmd, const void *payload, int bytes)
{
    uint32_t reply[3];
    sound_rpc(cmd, (void *)payload, bytes, reply, sizeof reply);
    CHECK(reply[0] == 0xFFFFFFFF && reply[2] == 0xFFFFFFFF);
    return reply[1];
}

static void make_vag(uint8_t *p, int rate, int nibble)
{
    memset(p, 0, 48 + 32);
    memcpy(p, "VAGp", 4);
    p[15] = 32;
    p[16] = (uint8_t)(rate >> 24); p[17] = (uint8_t)(rate >> 16);
    p[18] = (uint8_t)(rate >> 8); p[19] = (uint8_t)rate;
    memset(p + 50, nibble | nibble << 4, 14);
    memset(p + 66, nibble | nibble << 4, 14);
    p[65] = 1;
}

static void test_stream_rate_and_rpc(void)
{
    uint32_t args[8] = {1, 3, 0x4000000 | 112, 96, 1, 0, 0, 1};
    uint32_t h, update[6];
    int16_t out[112 * 2], split[112 * 2];
    int i;
    /* Header can start within the disc sector. Separate L/R VAG files. */
    make_vag(disc + 2048 + 112, 24000, 1);
    make_vag(disc + 6144 + 96, 24000, 2);
    snd_test_reset();
    h = rpc(0x2C, args, sizeof args);
    CHECK(h != 0);
    CHECK(rpc(0x4F, &h, 4) == 1);
    CHECK(rpc(0x19, &h, 4) == h); /* status queries preserve playback */
    args[5] = 0xDEAD; args[7] |= 16; CHECK(rpc(0x2C, args, sizeof args) == 0);
    CHECK(rpc(0x19, &h, 4) == h); args[5] = 0; args[7] = 1;
    snd_test_render(out, 112);
    for (i = 0; i < 224; i++) CHECK(out[i] == 0); /* starts paused */
    rpc(0x14, &h, 4);
    snd_test_render(out, 100);
    for (i = 10; i < 100; i++) CHECK(out[i * 2] >= 2036 && out[i * 2] <= 2037 && out[i * 2 + 1] >= 4076 && out[i * 2 + 1] <= 4077);
    CHECK(rpc(0x30, &h, 4) == 2); /* 50 source samples * 1024 / 24000 */
    CHECK(rpc(0x19, &h, 4) == h);
    rpc(0x13, &h, 4);
    snd_test_render(split, 10);
    CHECK(rpc(0x30, &h, 4) == 2);
    rpc(0x2E, &h, 4);
    memset(update, 0, sizeof update); update[0] = h; update[1] = 1;
    rpc(0x21, update, sizeof update); /* volume zero */
    snd_test_render(split, 12);
    CHECK(rpc(0x4F, &h, 4) == 0xFFFFFFFF); /* exact end at 112 output samples */
    for (i = 0; i < 24; i++) CHECK(split[i] == 0);
    /* Replay with different chunking; handle allocation also resets. */
    snd_test_reset(); args[7] = 0;
    CHECK(rpc(0x2C, args, sizeof args) == h);
    snd_test_render(out, 112);
    snd_test_reset();
    CHECK(rpc(0x2C, args, sizeof args) == h);
    snd_test_render(split, 37); snd_test_render(split + 74, 75);
    CHECK(memcmp(out, split, sizeof out) == 0);
    snd_test_reset(); h = rpc(0x2C, args, sizeof args);
    rpc(0x15, &h, 4);
    CHECK(rpc(0x19, &h, 4) == 0);
    CHECK(rpc(0x4F, &h, 4) == 0xFFFFFFFF);
    /* Malformed queue must not execute beyond its transmitted payload. */
    {
        uint8_t queue[8] = {1, 0, 0, 0, 0x2C, 0, 32, 0};
        uint32_t reply[3];
        sound_rpc(0x4D, queue, sizeof queue, reply, sizeof reply);
        CHECK(reply[1] == 0);
        queue[6] = 0xFF; queue[7] = 0xFF;
        sound_rpc(0x4D, queue, sizeof queue, reply, sizeof reply);
        CHECK(reply[1] == 0);
    }
    snd_test_reset();
    sound_tick(); sound_tick(); sound_tick(); sound_tick(); sound_tick();
    CHECK(snd_test_position() == 4004);
}

static void test_vpk(void)
{
    uint8_t vpk[2048 + 128];
    int16_t out[84 * 2];
    SndStreamInfo info;
    int i;
    memset(vpk, 0, sizeof vpk);
    memcpy(vpk, " KPV", 4);
    put32(vpk + 4, 48); put32(vpk + 8, 2048);
    put32(vpk + 12, 64); /* 32-byte interleave per channel */
    put32(vpk + 16, 48000); put32(vpk + 20, 2);
    for (i = 0; i < 3; i++) {
        int off = 2048 + (i / 2) * 64 + (i % 2) * 16;
        memset(vpk + off + 2, 0x11, 14);
        memset(vpk + off + 32 + 2, 0x22, 14);
    }
    CHECK(snd_stream_parse(vpk, sizeof vpk, &info));
    CHECK(info.channels == 2 && info.interleave == 32);
    snd_test_reset();
    CHECK(snd_test_stream(vpk, sizeof vpk, 1));
    snd_test_render(out, 84);
    for (i = 10; i < 81; i++) CHECK(out[i * 2] >= 2036 && out[i * 2] <= 2037 && out[i * 2 + 1] >= 4076 && out[i * 2 + 1] <= 4077);
    CHECK(!snd_stream_parse(vpk, 2048 + 111, &info)); /* short right channel */
    put32(vpk + 12, 0); CHECK(!snd_stream_parse(vpk, sizeof vpk, &info));
    put32(vpk + 12, 64); put32(vpk + 16, 0);
    CHECK(!snd_stream_parse(vpk, sizeof vpk, &info));
    put32(vpk + 16, 48000); put32(vpk + 20, 3);
    CHECK(!snd_stream_parse(vpk, sizeof vpk, &info));
    snd_test_reset();
}

static void test_stream_end_before_filler(void)
{
    uint8_t vag[96], nib[28];
    int16_t pcm[400 * 2];
    uint32_t handle;
    int i;
    make_vag(vag, 48000, 1); vag[15] = 48;
    memset(nib, 7, sizeof nib);
    block(vag + 80, 0, 7, nib); /* physical trailer after the real END block */
    snd_test_reset(); handle = snd_test_stream(vag, sizeof vag, 0);
    CHECK(handle != 0);
    snd_test_render(pcm, 400);
    CHECK(pcm[110] == 297);
    for (i = 112; i < 800; i++) CHECK(pcm[i] == 0);
    CHECK(rpc(0x19, &handle, 4) == 0);
    /* A repeat/end loops the real region immediately, not the footer. */
    vag[49] = 4; vag[65] = 3;
    snd_test_reset(); CHECK(snd_test_stream(vag, sizeof vag, 0));
    snd_test_render(pcm, 400);
    for (i = 20; i < 800; i++) CHECK(pcm[i] == 2036);
    snd_test_reset();
}

static void test_loop_and_clipping(void)
{
    uint8_t bank[2048], vag[80];
    int16_t out[400 * 2];
    int h, i;
    char name[16] = "TEST_ONE";
    build_bank(bank);
    /* Forty-eight positive voices must saturate, without signed overflow. */
    memset(bank + TEST_WAVE + 2, 0x77, 14);
    snd_test_reset(); h = snd_test_add_bank(bank, TEST_BANK_SIZE);
    for (i = 0; i < 48; i++) CHECK(snd_test_play(h, name, 0x400, 0));
    snd_test_render(out, 400);
    CHECK(out[0] == 32765 && out[1] == 32765);
    CHECK(out[798] == 0 && out[799] == 0);
    {
        uint32_t master[2] = {16, 512};
        snd_test_reset(); h = snd_test_add_bank(bank, TEST_BANK_SIZE);
        rpc(0x09, master, 8);
        for (i = 0; i < 48; i++) CHECK(snd_test_play(h, name, 1024, 0));
        snd_test_render(out, 1);
        CHECK(out[0] == 16381 && out[1] == 16381); /* saturate before master */
        memset(bank + TEST_WAVE + 2, 0x88, 14);
        snd_test_reset(); h = snd_test_add_bank(bank, TEST_BANK_SIZE);
        for (i = 0; i < 48; i++) CHECK(snd_test_play(h, name, 1024, 0));
        snd_test_render(out, 1);
        CHECK(out[0] == -32766 && out[1] == -32766); /* signed, saturating */
        memset(bank + TEST_WAVE + 2, 0x77, 14);
    }
    /* Loop-start is bit 2; repeat is bit 1 on the END block. */
    bank[TEST_WAVE + 1] = 4; bank[TEST_WAVE + 17] = 3;
    memset(bank + TEST_WAVE + 18, 0x77, 14);
    snd_test_reset(); h = snd_test_add_bank(bank, TEST_BANK_SIZE);
    CHECK(snd_test_play(h, name, 0x400, 0));
    snd_test_render(out, 400);
    CHECK(out[0] == 28555 && out[798] >= 28554 && out[798] <= 28557);
    make_vag(vag, 48000, 1); vag[49] = 4; vag[65] = 3;
    snd_test_reset(); CHECK(snd_test_stream(vag, sizeof vag, 0));
    snd_test_render(out, 400);
    CHECK(out[0] == 443 && out[798] == 2036);
    vag[65] = 1; /* marked start alone must not make a one-shot loop */
    snd_test_reset(); CHECK(snd_test_stream(vag, sizeof vag, 0));
    snd_test_render(out, 400);
    CHECK(out[110] == 297 && out[112] == 0);
    snd_test_reset();
}

static void test_volume_commands(void)
{
    uint32_t args[8] = {1, 0, 0x4000000, 0, 1, 0, 0, 0};
    uint32_t update[6], h, group[2];
    int16_t out[400 * 2];
    uint8_t bank[2048], request[48] = {0};
    make_vag(disc + 2048, 48000, 1);
    disc[2048 + 49] = 4; disc[2048 + 65] = 3;
    snd_test_reset(); h = rpc(0x2C, args, sizeof args);
    CHECK(h != 0);
    snd_test_render(out, 10); /* finish the hardware attack before setter checks */
    update[0] = h; update[1] = 512; update[2] = (uint32_t)-2;
    rpc(0x1B, update, 12);
    snd_test_render(out, 1); CHECK(out[0] == 249 && out[1] == 249);
    update[1] = 0x7FFFFFFF; rpc(0x1B, update, 12);
    snd_test_render(out, 1); CHECK(out[0] == 249);
    update[1] = (uint32_t)-127; rpc(0x1B, update, 12);
    snd_test_render(out, 1); CHECK(out[0] == 1017);
    group[0] = 1; group[1] = 512; rpc(0x09, group, 8);
    CHECK(rpc(0x0A, group, 4) == 512);
    snd_test_render(out, 1); CHECK(out[0] == 253);
    group[0] = 16; rpc(0x09, group, 8);
    CHECK(rpc(0x0A, group, 4) == 512);
    snd_test_render(out, 1); CHECK(out[0] == 126);
    group[0] = 15; group[1] = 0; rpc(0x09, group, 8); /* bypass group */
    CHECK(rpc(0x0A, group, 4) == 0);
    snd_test_render(out, 1); CHECK(out[0] == 126);
    CHECK(rpc(0x19, &h, 4) == h); /* setters do not restart/stop the stream */
    snd_test_reset();
    build_bank(bank);
    h = snd_test_add_bank(bank, TEST_BANK_SIZE);
    put32(request, h); put32(request + 8, 1024);
    put32(request + 12, 45); /* angular pan must not become pitch 45/4096 */
    memcpy(request + 16, "TEST_ONE", 8); put32(request + 44, 0x3D);
    h = rpc(0x5C, request, sizeof request);
    CHECK(h != 0);
    snd_test_render(out, 400);
    CHECK(out[0] != 0 && rpc(0x19, &h, 4) == h);
    snd_test_render(out, 400); CHECK(rpc(0x19, &h, 4) == h);
    snd_test_render(out, 1); CHECK(rpc(0x19, &h, 4) == 0);
    /* 0x21 also updates a bank voice's volume without releasing it. */
    snd_test_reset();
    bank[TEST_WAVE + 1] = 4; bank[TEST_WAVE + 17] = 3;
    memset(bank + TEST_WAVE + 2, 0x11, 14); memset(bank + TEST_WAVE + 18, 0x11, 14);
    put32(request, snd_test_add_bank(bank, TEST_BANK_SIZE));
    put32(request + 12, 0);
    h = rpc(0x5C, request, sizeof request); CHECK(h != 0);
    memset(update, 0, sizeof update); update[0] = h; update[1] = 1;
    CHECK(rpc(0x21, update, sizeof update) == h);
    snd_test_render(out, 100);
    CHECK(out[0] == 0 && out[198] == 0 && rpc(0x19, &h, 4) == h);
    update[2] = 1024; rpc(0x21, update, sizeof update);
    snd_test_render(out, 1); CHECK(out[0] > 0 && out[0] == out[1]);
    snd_test_reset();
}

static void test_grain_timing(void)
{
    uint8_t image[2048], request[48] = {0};
    int16_t first[4000], second[4000];
    uint32_t h, bank, depth[3] = {1, 0x4000, 0x8000}, update[6] = {0};
    int i;
    build_bank(image);
    /* Tone followed by WAIT_FOR_ALL_VOICES. Its handle remains alive until
       the 240 Hz program observes the one-shot finish. */
    image[0x18 + 0x3C + 4] = 2;
    put32(image + 0x18 + 0x54 + 40, 38);
    snd_test_reset(); bank = snd_test_add_bank(image, TEST_BANK_SIZE);
    put32(request, bank); put32(request + 8, 1024);
    memcpy(request + 16, "TEST_ONE", 8);
    h = rpc(0x5C, request, 48); CHECK(h != 0);
    snd_test_render(first, 800); CHECK(rpc(0x19, &h, 4) == h);
    CHECK(snd_test_active_voices() == 0); /* quiet DSP, still a live driver allocation */
    snd_test_render(first + 1600, 1); CHECK(rpc(0x19, &h, 4) == 0);
    rpc(0x0F, depth, 12);
    CHECK(snd_test_reverb_depth(0, 0) == 16384 && snd_test_reverb_depth(0, 1) == -32768);
    CHECK(snd_test_reverb_depth(1, 0) == 0);
    depth[0] = 2; depth[1] = 0xFFFF; rpc(0x0F, depth, 12);
    CHECK(snd_test_reverb_depth(1, 0) == -1 && snd_test_reverb_depth(0, 0) == 16384);
    /* A delayed tone starts on an exact sample boundary across chunks. */
    build_bank(image); put32(image + 0x18 + 0x54 + 4, 2);
    snd_test_reset(); bank = snd_test_add_bank(image, TEST_BANK_SIZE); put32(request, bank);
    h = rpc(0x5C, request, 48); snd_test_render(first, 2000);
    for (i = 0; i < 800; i++) CHECK(first[i] == 0);
    CHECK(first[800] > 0 && rpc(0x19, &h, 4) == 0);
    snd_test_reset(); bank = snd_test_add_bank(image, TEST_BANK_SIZE); put32(request, bank);
    CHECK(rpc(0x5C, request, 48) == h);
    snd_test_render(second, 137); snd_test_render(second + 274, 1863);
    CHECK(!memcmp(first, second, sizeof first));
    /* Unloading while a program is delayed invalidates the handle and does
       not leave a program reading the released bank on its next tick. */
    h = rpc(0x5C, request, 48); CHECK(h != 0);
    rpc(0x06, &bank, 4); CHECK(rpc(0x19, &h, 4) == 0);
    snd_test_render(first, 2000);
    for (i = 0; i < 4000; i++) CHECK(first[i] == 0);
    (void)update;
    snd_test_reset();
}

/* An END grain (type 24) stops its sound as retail 0x5A60 -> 0xFE04 does: a looping tone before it
   is keyed off and released, instead of looping on after the program ends (finding 133). */
static void test_end_grain_keys_off(void)
{
    uint8_t image[2048], request[48] = {0}, nib[28] = {1};
    int16_t pcm[2 * 2000];
    uint32_t h, bank;
    int with_end, i, loud;
    for (with_end = 0; with_end < 2; with_end++) {
        build_bank(image);
        block(image + TEST_WAVE, 0x00, 7, nib);   /* one block: loop start, repeat, end */
        image[0x18 + 0x3C + 4] = 2;               /* sound 0: the tone, then grain 1 */
        put32(image + 0x18 + 0x54 + 40, with_end ? 24 : 0);
        put32(image + 0x18 + 0x54 + 40 + 4, 3);   /* three 240-Hz ticks after the tone */
        snd_test_reset(); bank = snd_test_add_bank(image, TEST_BANK_SIZE);
        put32(request, bank); put32(request + 8, 1024);
        memcpy(request + 16, "TEST_ONE", 8);
        h = rpc(0x5C, request, 48); CHECK(h != 0);
        snd_test_render(pcm, 400);
        CHECK(snd_test_active_voices() == 1);     /* looping before the END grain */
        for (i = 0; i < 20; i++) snd_test_render(pcm, 2000);
        for (i = loud = 0; i < 4000; i++) loud |= pcm[i] != 0;
        if (with_end) {
            CHECK(snd_test_active_voices() == 0 && !loud);   /* released and silent */
            CHECK(rpc(0x19, &h, 4) == 0);                     /* the sound is over */
        } else
            CHECK(snd_test_active_voices() == 1 && loud);    /* a program end alone keeps it */
    }
    snd_test_reset();
}

/* Instance limits and solo sounds, as retail 0x4A34/0xF34C/0x10038 apply them (finding 133). */
static uint32_t play_looping(uint8_t *image, int flags, int limit, int volume, int first)
{
    uint8_t request[48] = {0}, nib[28] = {1};
    static uint32_t bank;
    if (first) {
        build_bank(image);
        block(image + TEST_WAVE, 0x00, 7, nib);
        image[0x18 + 0x3C + 5] = (uint8_t)limit;
        put16(image + 0x18 + 0x3C + 6, (uint16_t)flags);
        snd_test_reset(); bank = snd_test_add_bank(image, TEST_BANK_SIZE);
    }
    put32(request, bank); put32(request + 8, (uint32_t)volume);
    memcpy(request + 16, "TEST_ONE", 8);
    return rpc(0x5C, request, 48);
}

static void test_instance_limits(void)
{
    uint8_t image[2048];
    int16_t pcm[2 * 2000];
    uint32_t a, b;
    int i;
    /* flag 8, limit 1, no steal rule: a second copy is refused while the first plays */
    a = play_looping(image, 8 | 1, 1, 1024, 1); CHECK(a != 0);
    b = play_looping(image, 8 | 1, 1, 1024, 0); CHECK(b == 0);
    snd_test_render(pcm, 400); CHECK(snd_test_active_voices() == 1 && rpc(0x19, &a, 4) == a);
    /* without flag 8 the limit byte does nothing */
    a = play_looping(image, 1, 1, 1024, 1); b = play_looping(image, 1, 1, 1024, 0);
    snd_test_render(pcm, 400); CHECK(a && b && snd_test_active_voices() == 2);
    /* flag 0x10: a louder new copy takes the quieter one's place; a quieter one is refused */
    a = play_looping(image, 0x18 | 1, 1, 300, 1); CHECK(a != 0);
    b = play_looping(image, 0x18 | 1, 1, 200, 0); CHECK(b == 0);
    b = play_looping(image, 0x18 | 1, 1, 900, 0); CHECK(b != 0);
    for (i = 0; i < 10; i++) snd_test_render(pcm, 2000);
    CHECK(snd_test_active_voices() == 1 && rpc(0x19, &a, 4) == 0 && rpc(0x19, &b, 4) == b);
    /* flag 0x20: the oldest copy goes */
    a = play_looping(image, 0x28 | 1, 1, 1024, 1); snd_test_render(pcm, 400);
    b = play_looping(image, 0x28 | 1, 1, 1024, 0); CHECK(a && b);
    for (i = 0; i < 10; i++) snd_test_render(pcm, 2000);
    CHECK(snd_test_active_voices() == 1 && rpc(0x19, &b, 4) == b);
    /* solo (flag 2): a new copy stops the playing one */
    a = play_looping(image, 2 | 1, 0, 1024, 1); b = play_looping(image, 2 | 1, 0, 1024, 0);
    for (i = 0; i < 10; i++) snd_test_render(pcm, 2000);
    CHECK(a && b && snd_test_active_voices() == 1 && rpc(0x19, &a, 4) == 0);
    snd_test_reset();
}

static void test_grain_random_replay(void)
{
    uint8_t image[2048], request[48] = {0};
    int16_t a[2000], b[2000];
    uint32_t bank, h;
    const int grain = 0x18 + 0x54;
    build_bank(image);
    /* Two alternative tones. The random control and both tone records live
       in one sound; the second waveform is distinct and ends normally. */
    memcpy(image + grain + 80, image + grain + 40, 40);
    memcpy(image + grain + 40, image + grain, 40);
    image[0x18 + 0x3C + 4] = 3;
    put32(image + grain, 25); memset(image + grain + 8, 0, 32);
    image[grain + 8] = 2; image[grain + 10] = 1;
    image[grain + 12] = image[grain + 13] = 0xFF;
    memset(image + TEST_WAVE + 18, 0x22, 14);
    snd_test_reset(); bank = snd_test_add_bank(image, TEST_BANK_SIZE);
    put32(request, bank); put32(request + 8, 1024); memcpy(request + 16, "TEST_ONE", 8);
    h = rpc(0x5C, request, 48); CHECK(h != 0);
    snd_test_render(a, 1000);
    CHECK(a[0] == 2521 && a[100] == 0);
    snd_test_reset(); bank = snd_test_add_bank(image, TEST_BANK_SIZE); put32(request, bank);
    CHECK(rpc(0x5C, request, 48) == h);
    snd_test_render(b, 31); snd_test_render(b + 62, 969);
    CHECK(!memcmp(a, b, sizeof a));
    snd_test_reset();
}

static void debug_env(const char *name, const char *value)
{
    host_putenv_s(name, value);
}

static void test_stream_parameters(void)
{
    uint32_t play[8] = {1,3,0x4000000,0,1,0,0,4};
    uint32_t update[6] = {0}, h;
    int16_t out[402];
    int i;
    snd_test_reset(); make_vag(disc + 2048, 24000, 1); make_vag(disc + 6144, 24000, 2);
    disc[2048 + 65] = disc[6144 + 65] = 3;
    h = rpc(0x2C, play, sizeof play); CHECK(h != 0);
    update[0] = h; update[1] = 2; update[3] = 180;
    rpc(0x21, update, sizeof update); snd_test_render(out, 1);
    CHECK(out[0] == 890 && out[1] == 443); /* angular 180 swaps the two sources */
    update[1] = 8; update[4] = 1536; /* one octave in modifier units */
    rpc(0x21, update, sizeof update); snd_test_render(out, 40);
    CHECK(rpc(0x30, &h, 4) == 1); /* 40 source samples; unchanged pitch would report 0 */
    snd_test_reset(); h = rpc(0x2C, play, sizeof play);
    update[0] = h; update[1] = 6; update[3] = 180; update[4] = 0;
    rpc(0x21, update, sizeof update); snd_test_render(out, 201);
    for (i = 10; i < 200; i++) CHECK(out[i * 2] >= 2036 && out[i * 2] <= 2037 && out[i * 2 + 1] >= 4076 && out[i * 2 + 1] <= 4077);
    CHECK(out[400] != out[398] || out[401] != out[399]); /* smooth wins over immediate */
    snd_test_reset();
}

static void test_queued_streams(void)
{
    uint32_t args[8] = {1, 0, 0x4000000, 270u << 16, 1, 0, 0, 0};
    uint32_t h;
    int16_t out[168 * 2];
    int i;
    snd_test_reset();
    for (i = 1; i <= 3; i++) {
        make_vag(disc + i * 2048, 48000, i);
        disc[i * 2048 + 65] = 3; /* disc one-shots ignore source repeat bits */
    }
    h = rpc(0x2C, args, sizeof args); CHECK(h != 0);
    args[5] = h; args[0] = 2; CHECK(rpc(0x2C, args, sizeof args) == 1);
    args[0] = 3; CHECK(rpc(0x2C, args, sizeof args) == 1);
    snd_test_render(out, 168);
    for (i = 10; i < 165; i++) {
        if (i < 53 || (i >= 56 && i < 109) || i >= 112)
            CHECK(out[i * 2] == (i < 56 ? 2036 : i < 112 ? 4076 : 6116));
        CHECK(out[i * 2] > 0); /* lookahead blends cues without silence or re-key */
        CHECK(out[i * 2 + 1] == 0);
    }
    CHECK(rpc(0x19, &h, 4) == 0);
    /* A paused chain advances neither its first cue nor queued images. Stop
       must free the entire chain and must not start a later cue. */
    snd_test_reset(); args[0] = 1; args[5] = 0; args[7] = 1;
    h = rpc(0x2C, args, sizeof args); args[0] = 2; args[5] = h;
    CHECK(rpc(0x2C, args, sizeof args) == 1);
    snd_test_render(out, 168);
    for (i = 0; i < 336; i++) CHECK(out[i] == 0);
    rpc(0x2F, &h, 4); CHECK(rpc(0x19, &h, 4) == 0);
    snd_test_render(out, 168);
    for (i = 0; i < 336; i++) CHECK(out[i] == 0);
    snd_test_reset();
}

static void test_stream_loop_flag(void)
{
    uint32_t args[8] = {1,0,1024u << 16,270u << 16,1,0,0,4};
    uint32_t h;
    uint8_t nib[28];
    int16_t source[28 * 20], out[480 * 2], chunked[480 * 2];
    SndAdpcm decoder = {0};
    SndAdsr envelope;
    int i, offset;
    snd_test_reset();
    make_vag(disc + 2048, 44100, 1);
    make_vag(disc + 4096, 48000, 3);
    for (i = 0; i < 28; i++) nib[i] = i & 1 ? 15 : 1;
    block(disc + 2048 + 48, 0, 0, nib);
    memset(nib, 0, sizeof nib);
    block(disc + 2048 + 64, 0x10, 1, nib); /* END, without repeat */
    /* Independent, continuous ADPCM decoding supplies the Gaussian window
       across every file seam. Looking ahead must not advance the live predictor. */
    for (i = 0; i < 20; i++)
        snd_adpcm_block(&decoder, disc + 2048 + 48 + (i & 1) * 16, source + i * 28);
    h = rpc(0x2C, args, sizeof args); CHECK(h != 0);
    args[0] = 2; args[5] = h; args[7] = 0;
    CHECK(rpc(0x2C, args, sizeof args) == 1);
    snd_test_render(out, 480);
    CHECK(rpc(0x19, &h, 4) == h); /* loop holds the head, with the next cue waiting */
    snd_adsr_set(&envelope, 0x080F, 0x1FC0); snd_adsr_start(&envelope);
    for (i = 0; i < 480; i++) {
        unsigned position = i * 3763u; /* 44100 * 4096 / 48000, register precision */
        int sample = snd_gaussian_sample(source + (position >> 12), (position >> 4) & 255);
        sample = snd_q15(snd_q15(snd_q15(sample, snd_adsr_tick(&envelope)), 32766), 32766);
        CHECK(out[i * 2] == sample && out[i * 2 + 1] == 0);
    }
    rpc(0x2F, &h, 4); CHECK(rpc(0x19, &h, 4) == 0);
    snd_test_render(chunked, 480);
    for (i = 0; i < 960; i++) CHECK(chunked[i] == 0);
    snd_test_reset(); args[0] = 1; args[5] = 0; args[7] = 4;
    h = rpc(0x2C, args, sizeof args);
    for (offset = 0; offset < 480; ) {
        int count = 17; if (count > 480 - offset) count = 480 - offset;
        snd_test_render(chunked + offset * 2, count); offset += count;
    }
    CHECK(!memcmp(out, chunked, sizeof out));
    args[7] = 0;
    /* Retail intro files carry END+REPEAT even for a one-shot RPC. 989snd
       rewrites streaming-buffer flags; only RPC bit 4 repeats the disc cue. */
    snd_test_reset(); disc[2048 + 65] = 3; h = rpc(0x2C, args, sizeof args);
    snd_test_render(out, 480); CHECK(rpc(0x19, &h, 4) == 0);
    for (i = 62; i < 480; i++) CHECK(out[i * 2] == 0 && out[i * 2 + 1] == 0);
    snd_test_reset();
}

static void test_reverb(void)
{
    int32_t input[2], wet[2];
    int16_t first[24000 * 2], second[24000 * 2];
    uint32_t preset[2] = {2, 3};
    int run, i, tail = 0;
    for (run = 0; run < 2; run++) {
        int16_t *capture = run ? second : first;
        snd_test_reset(); rpc(0x0E, preset, sizeof preset);
        for (i = 0; i < 24000; i++) {
            input[0] = i < 32 ? 12000 : 0; input[1] = 0;
            snd_test_reverb_frame(1, input, wet);
            capture[i * 2] = (int16_t)wet[0]; capture[i * 2 + 1] = (int16_t)wet[1];
            if (i > 1000 && (wet[0] || wet[1])) tail++;
        }
    }
    CHECK(tail > 1000); /* delayed reflections persist after the impulse */
    CHECK(!memcmp(first, second, sizeof first));
    /* Core masks are independent; OFF clears the tail and bypasses input. */
    input[0] = input[1] = 32767;
    snd_test_reverb_frame(0, input, wet); CHECK(!wet[0] && !wet[1]);
    preset[1] = 0; rpc(0x0E, preset, sizeof preset);
    snd_test_reverb_frame(1, input, wet); CHECK(!wet[0] && !wet[1]);
    snd_test_reset();
    {
        uint8_t bank[2048], request[48] = {0}, nib[28];
        uint32_t range[3] = {15,24,47}, depth[3] = {2,32767,32767};
        int tail_nonzero = 0, h;
        build_bank(bank); memset(nib, 1, sizeof nib);
        block(bank + TEST_WAVE, 0, 1, nib);
        put32(bank + 0x18 + 0x54 + 18, 0x1FC0000F);
        bank[0x18 + 0x54 + 22] = 16; /* wet-only; no direct path */
        preset[1] = 3; rpc(0x0E, preset, 8); rpc(0x4E, range, 12); rpc(0x0F, depth, 12);
        h = snd_test_add_bank(bank, TEST_BANK_SIZE);
        put32(request, h); put32(request + 8, 1024); memcpy(request + 16, "TEST_ONE", 8);
        CHECK(rpc(0x5C, request, sizeof request)); snd_test_render(first, 24000);
        for (i = 0; i < 28 * 2; i++) CHECK(first[i] == 0); /* FIR/network latency */
        for (i = 1000; i < 24000 * 2; i++) tail_nonzero += first[i] != 0;
        CHECK(tail_nonzero > 1000); /* voice ended, but its room tail is audible */
        snd_test_reset();
    }
}

static void test_modulation_and_ducking(void)
{
    uint8_t bank[2048], request[48] = {0}, nib[28];
    int16_t fixed[2000 * 2], modulated[2000 * 2], replay[2000 * 2];
    uint32_t h, args[7] = {0, 1, 1, 32768, 0, 0, 0};
    int run;
    build_bank(bank); memset(nib, 1, sizeof nib);
    block(bank + TEST_WAVE, 0, 3, nib); /* constant looping test tone */
    put32(bank + 0x18 + 0x3C + 8, 40); /* first sound's TONE in grain 1 */
    put32(bank + 0x18 + 0x54 + 40, 1);
    put32(bank + 0x18 + 0x54 + 40 + 24, 0);
    put32(bank + 0x18 + 0x54 + 40 + 18, 0x1FC0000F); /* rapid attack, held sustain */
    snd_test_reset(); h = snd_test_add_bank(bank, TEST_BANK_SIZE);
    put32(request, h); put32(request + 8, 1024); memcpy(request + 16, "TEST_ONE", 8);
    CHECK(rpc(0x5C, request, sizeof request)); snd_test_render(fixed, 2000);
    /* LFO grain before the tone: square volume modulation. */
    bank[0x18 + 0x3C + 4] = 2; put32(bank + 0x18 + 0x3C + 8, 0);
    memset(bank + 0x18 + 0x54, 0, 40); put32(bank + 0x18 + 0x54, 4);
    bank[0x18 + 0x54 + 9] = 1; bank[0x18 + 0x54 + 11] = 2;
    put32(bank + 0x18 + 0x54 + 12, (1024u << 16) | 1024);
    put32(bank + 0x18 + 0x54 + 20, 0x1000000);
    for (run = 0; run < 2; run++) {
        snd_test_reset(); h = snd_test_add_bank(bank, TEST_BANK_SIZE); put32(request, h);
        CHECK(rpc(0x5C, request, sizeof request));
        if (!run) snd_test_render(modulated, 2000);
        else { snd_test_render(replay, 601); snd_test_render(replay + 1202, 1399); }
    }
    CHECK(!memcmp(modulated, replay, sizeof replay));
    CHECK(memcmp(fixed, modulated, sizeof fixed) != 0);
    CHECK(modulated[1600] == 0); /* square reaches its silent half-cycle */
    /* A live source group ducks the target stream at the next driver tick. */
    snd_test_reset(); make_vag(disc + 2048, 48000, 1); disc[2048 + 65] = 3;
    {
        uint32_t play[8] = {1,0,0x4000000,270u<<16,0,0,0,4};
        CHECK(rpc(0x2C, play, sizeof play));
        play[4] = 1; CHECK(rpc(0x2C, play, sizeof play));
    }
    rpc(0x60, args, sizeof args); snd_test_render(replay, 801);
    CHECK(replay[1600] < replay[1598]); /* target's register gain is quadratic */
    args[1] = UINT32_MAX; rpc(0x60, args, 8); snd_test_render(replay, 1);
    CHECK(replay[0] > 3500); /* disabling restores target gain */
    /* Stopping a paused sound unpauses its release and releases the handle. */
    snd_test_reset(); h = snd_test_add_bank(bank, TEST_BANK_SIZE); put32(request, h);
    h = rpc(0x5C, request, sizeof request); rpc(0x13, &h, 4); rpc(0x15, &h, 4);
    snd_test_render(replay, 2000); CHECK(rpc(0x19, &h, 4) == 0);
    snd_test_reset();
}

static void test_stream_envelope_and_duck_release(void)
{
    /* SPU2 ADSR1=080F/ADSR2=1FC0, filter-zero 4096 PCM, full fixed
       volumes: attack reaches full level, then the decay tick enters the
       held sustain near half level. Values include both Q15 output gains. */
    static const int16_t attack[12] = {
        443,889,1335,1781,2227,2673,3119,3565,4011,4074,2036,2036
    };
    uint8_t vag[80];
    int16_t out[1600];
    uint32_t target, source, play[8] = {1,0,0x4000000,270u<<16,0,0,0,4};
    uint32_t duck[7] = {0,1,1,32768,32768,98304,0};
    int i, previous;
    snd_test_reset(); make_vag(vag, 48000, 1); vag[65] = 3;
    target = snd_test_stream(vag, sizeof vag, 0);
    snd_test_render(out, 12);
    for (i = 0; i < 12; i++) CHECK(out[i * 2] == attack[i]);
    rpc(0x13, &target, 4); snd_test_render(out, 800);
    for (i = 0; i < 1600; i++) CHECK(out[i] == 0);
    rpc(0x14, &target, 4); snd_test_render(out, 1);
    CHECK(out[0] == 2036); /* resume preserves the held envelope */

    snd_test_reset(); make_vag(disc + 2048, 48000, 1); disc[2048 + 65] = 3;
    target = rpc(0x2C, play, 32); play[4] = 1; source = rpc(0x2C, play, 32);
    CHECK(target && source); rpc(0x60, duck, sizeof duck);
    for (i = 0; i < 20; i++) snd_test_render(out, 800);
    rpc(0x15, &source, 4); /* target stays allocated while source releases */
    snd_test_render(out, 800); previous = out[1598];
    CHECK(previous > 500 && previous < 2036);
    for (i = 0; i < 50; i++) {
        snd_test_render(out, 800);
        CHECK(out[1598] >= previous && out[1598] <= 2038);
        previous = out[1598];
    }
    CHECK(previous >= 2036); /* release restores unity without an overshoot */
    snd_test_reset();
}

static void test_stream_readiness(void)
{
    static int16_t captures[2][16000 * 2], scratch[20000 * 2];
    uint32_t args[8] = {1,0,1024u << 16,270u << 16,1,0,0,4};
    uint32_t first, second;
    int run, i, position, target;
    snd_test_stream_read_samples(4800);
    for (run = 0; run < 2; run++) {
        snd_test_reset(); disc_read_burn = run ? 50000 : 0;
        make_vag(disc + 2048, 48000, 1); make_vag(disc + 4096, 48000, 2);
        first = rpc(0x2C, args, sizeof args);
        args[0] = 2; args[7] = 5; second = rpc(0x2C, args, sizeof args);
        args[0] = 1; args[7] = 4;
        CHECK(first && second); CHECK(rpc(0x4F, &first, 4) == 0);
        CHECK(rpc(0x19, &first, 4) == first); CHECK(rpc(0x30, &first, 4) == 0);
        position = 0;
        for (i = 0; i < 5; i++) {
            static const int times[] = {9599,9600,9800,14400,16000};
            target = times[i];
            while (position < target) {
                int count = target - position;
                if (run && count > 17) count = 17;
                snd_test_render(captures[run] + position * 2, count); position += count;
            }
            CHECK(rpc(0x4F, &first, 4) == (i != 0));
            CHECK(rpc(0x4F, &second, 4) == (i >= 3)); /* paused loading progresses */
            if (i == 2) CHECK(rpc(0x30, &first, 4) == 0);
            if (i == 3) rpc(0x14, &second, 4);
        }
        for (i = 0; i < 9800 * 2; i++) CHECK(captures[run][i] == 0);
        CHECK(captures[run][9805 * 2] > 0);
        CHECK(captures[run][15999 * 2] > captures[run][14000 * 2]);
    }
    CHECK(!memcmp(captures[0], captures[1], sizeof captures[0]));
    /* A short head finishes before its queued transfer: no future cue leaks
       through Gaussian lookahead, and no host completion can shorten the gap. */
    snd_test_reset(); args[7] = 0;
    first = rpc(0x2C, args, 32); args[0] = 2; args[5] = first;
    CHECK(rpc(0x2C, args, 32) == 1); snd_test_render(scratch, 20000);
    for (i = 9856; i < 14600; i++) CHECK(scratch[i * 2] == 0);
    CHECK(scratch[14605 * 2] > 0); CHECK(rpc(0x19, &first, 4) == 0);
    disc_read_burn = 0; snd_test_stream_read_samples(0); snd_test_reset();
}

static void test_stream_seek_and_cancel_readiness(void)
{
    static int16_t captures[2][15000 * 2];
    uint32_t args[8] = {8,0,1024u << 16,270u << 16,1,0,0,4};
    uint32_t canceled, next;
    int run, position, count, i;
    snd_test_stream_read_samples(4800);
    for (run = 0; run < 2; run++) {
        snd_test_reset(); disc_read_burn = run ? 50000 : 0;
        make_vag(disc + 2048, 48000, 1); make_vag(disc + 6 * 2048, 48000, 2);
        /* Failed acquisition reserves no game-time service slot. A canceled
           successful read retains its already scheduled FIFO service slot. */
        args[0] = 8; CHECK(rpc(0x2C, args, 32) == 0);
        args[0] = 1; canceled = rpc(0x2C, args, 32); CHECK(canceled);
        snd_test_render(captures[run], 123);
        CHECK(rpc(0x4F, &canceled, 4) == 0); rpc(0x2F, &canceled, 4);
        CHECK(rpc(0x4F, &canceled, 4) == 0xFFFFFFFFu);
        CHECK(rpc(0x19, &canceled, 4) == 0);
        args[0] = 6; next = rpc(0x2C, args, 32); CHECK(next);
        for (position = 123; position < 15000; position += count) {
            count = 15000 - position;
            if (position < 14399 && count > 14399 - position) count = 14399 - position;
            if (position == 14399) count = 1;
            if (run && count > 17) count = 17;
            if (position == 14399) CHECK(rpc(0x4F, &next, 4) == 0);
            if (position == 14400) CHECK(rpc(0x4F, &next, 4) == 1);
            snd_test_render(captures[run] + position * 2, count);
        }
        for (i = 0; i < 14600 * 2; i++) CHECK(captures[run][i] == 0);
        CHECK(captures[run][14605 * 2] > 0);
        /* Once the service queue is idle, a later seek has a fresh cold
           9600-sample deadline, independent of its LSN or host read cost. */
        rpc(0x2F, &next, 4); args[0] = 1;
        next = rpc(0x2C, args, 32); CHECK(next);
        for (i = 0; i < 9599; i++) snd_test_render(captures[run] + 14999 * 2, 1);
        CHECK(rpc(0x4F, &next, 4) == 0);
        snd_test_render(captures[run] + 14999 * 2, 1);
        CHECK(rpc(0x4F, &next, 4) == 1);
    }
    CHECK(!memcmp(captures[0], captures[1], sizeof captures[0]));
    disc_read_burn = 0; snd_test_stream_read_samples(0); snd_test_reset();
}

static void test_random_tone_parameters(void)
{
    uint8_t bank[2048], request[48] = {0}, nibble[28];
    int16_t pcm[800 * 2];
    uint32_t handle, update[2];
    int size = build_bank(bank), i;
    memset(nibble, 1, sizeof nibble); block(bank + TEST_WAVE, 0, 3, nibble);
    bank[0x18 + 0x54 + 9] = (uint8_t)-5;
    bank[0x18 + 0x54 + 14] = (uint8_t)-5; bank[0x18 + 0x54 + 15] = 255;
    put32(bank + 0x18 + 0x54 + 18, 0x1FC0000F);
    snd_test_reset(); put32(request, snd_test_add_bank(bank, size));
    put32(request + 8, 1024); memcpy(request + 16, "TEST_ONE", 8);
    handle = rpc(0x5C, request, 48); CHECK(handle);
    snd_test_render(pcm, 800); update[0] = handle; update[1] = 1024;
    {
        int16_t left = pcm[1598], right = pcm[1599];
        for (i = 0; i < 12; i++) {
            rpc(0x1B, update, 8); snd_test_render(pcm, 800);
            CHECK(pcm[1598] == left && pcm[1599] == right);
        }
    }
    snd_test_reset();
}

static void test_static_end_prefetch(void)
{
    static const uint16_t pitch[] = {4096,3763,2048,1448};
    /* Actual PCSX2 DecodeSamples/ConsumeSamples, one/four blocks (finding 022). */
    static const int stop[2][4] = {{12,14,24,34},{96,105,192,272}};
    uint8_t adpcm[64], nibbles[28];
    int16_t pcm[2];
    int b, p, n, blocks;
    memset(nibbles, 1, sizeof nibbles);
    for (b = 0; b < 2; b++) for (p = 0; p < 4; p++) {
        blocks = b ? 4 : 1; snd_test_reset();
        for (n = 0; n < blocks; n++) block(adpcm + n * 16, 0, n == blocks - 1, nibbles);
        CHECK(snd_test_raw_voice(adpcm, blocks * 16, pitch[p]));
        for (n = 0; n < stop[b][p]; n++) {
            snd_test_render(pcm, 1); CHECK(pcm[0] != 0); CHECK(snd_test_active_voices() == 1);
        }
        snd_test_render(pcm, 1); CHECK(pcm[0] == 0 && snd_test_active_voices() == 0);
    }
    /* Repeat remains keyed beyond that endpoint; pitch zero stalls the read
       head once the FIFO fills and must not invent an END timeout. */
    for (p = 0; p < 2; p++) {
        snd_test_reset(); block(adpcm, 0, p ? 1 : 3, nibbles);
        CHECK(snd_test_raw_voice(adpcm, 16, p ? 0 : 4096));
        for (n = 0; n < 100; n++) snd_test_render(pcm, 1);
        CHECK(pcm[0] != 0 && snd_test_active_voices() == 1);
    }
    snd_test_reset();
}

static void test_debug_capture(void)
{
    uint8_t vag[80], header[44];
    int16_t pcm[4004 * 2];
    FILE *f;
    char report[2048];
    size_t n;
    int i;
    debug_env("DHD_SND_WAV", "test_sound_capture.wav");
    debug_env("DHD_SND_REPORT", "test_sound_commands.csv");
    debug_env("DHD_SND_GAIN_TRACE", "test_sound_gain.csv");
    snd_test_reset();
    make_vag(vag, 48000, 1); vag[49] = 4; vag[65] = 3;
    CHECK(snd_test_stream(vag, sizeof vag, 0) != 0);
    rpc(0xAB, NULL, 0); rpc(0xAB, NULL, 0); rpc(0x1AB, NULL, 0);
    for (i = 0; i < 5; i++) sound_tick();
    {
        uint64_t before = snd_test_position();
        snd_test_report();
        CHECK(snd_test_position() == before);
    }
    /* Read while the capture is still open: a killed run needs no shutdown
       hook to repair its header or flush its final-mix samples. */
    f = fopen("test_sound_capture.wav", "rb"); CHECK(f != NULL);
    if (f) {
        CHECK(fread(header, 1, 44, f) == 44);
        CHECK(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8));
        CHECK(header[20] == 1 && header[22] == 2 && header[34] == 16);
        CHECK((header[24] | header[25] << 8) == 48000);
        CHECK((header[40] | header[41] << 8 | header[42] << 16) == 4004 * 4);
        CHECK(fread(pcm, 4, 4004, f) == 4004);
        CHECK(fgetc(f) == EOF);
        for (i = 20; i < 8008; i++) CHECK(pcm[i] == 2036);
        fclose(f);
    }
    f = fopen("test_sound_commands.csv", "rb"); CHECK(f != NULL);
    if (f) {
        n = fread(report, 1, sizeof report - 1, f); report[n] = 0;
        CHECK(strstr(report, "game_samples=4004") != NULL);
        CHECK(strstr(report, "0x00AB,2,2,0") != NULL);
        CHECK(strstr(report, "0x01AB,1,1,0") != NULL); /* no low-byte alias */
        fclose(f);
    }
    sound_tick(); /* sixth tick publishes the first complete 100-ms gain bin */
    f = fopen("test_sound_gain.csv", "rb"); CHECK(f != NULL);
    if (f) {
        n = fread(report, 1, sizeof report - 1, f); report[n] = 0;
        CHECK(strstr(report, "stream_energy_left") != NULL);
        CHECK(strstr(report, "\n0,") != NULL);
        fclose(f);
    }
    snd_test_reset(); /* close before deleting, including on Windows */
    remove("test_sound_capture.wav"); remove("test_sound_commands.csv");
    remove("test_sound_commands.csv.grains.csv");
    remove("test_sound_gain.csv"); remove("test_sound_gain.csv.events.csv");
    debug_env("DHD_SND_WAV", ""); debug_env("DHD_SND_REPORT", "");
    debug_env("DHD_SND_GAIN_TRACE", "");
}

int main(void)
{
    snd_test_stream_read_samples(0); /* DSP fixtures explicitly exclude IOP loading */
    test_adpcm();
    test_adsr();
    test_gaussian();
    test_spu_levels();
    test_bank_and_mix();
    test_tone_addresses();
    test_vag_stream();
    test_stream_rate_and_rpc();
    test_vpk();
    test_stream_end_before_filler();
    test_loop_and_clipping();
    test_volume_commands();
    test_grain_timing();
    test_end_grain_keys_off();
    test_instance_limits();
    test_grain_random_replay();
    test_reverb();
    test_queued_streams();
    test_stream_loop_flag();
    test_stream_parameters();
    test_modulation_and_ducking();
    test_stream_envelope_and_duck_release();
    test_stream_readiness();
    test_stream_seek_and_cancel_readiness();
    test_random_tone_parameters();
    test_static_end_prefetch();
    test_debug_capture();
    if (failures) {
        printf("test_sound: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_sound: ok\n");
    return 0;
}
