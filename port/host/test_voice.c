/* Voice chat (finding 131): port/host/voice.c's receive, jitter, decode and mix with the real
   libopus, and port/hle/voice_rules.h's proximity rules. Built and run by port/tools/test_voice.py.
   Each case prints PASS or FAIL and the program a final RESULT line. */
#include "voice.h"
#include "host_api.h"
#include "../hle/voice_rules.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* ---- what voice.c takes from the rest of the host (no game, no window, no network here) ---- */
void hle_log(const char *fmt, ...) { (void)fmt; }
uintptr_t guest_base;   /* host_service_voice_frame's GUEST(); the tests call voice_receive */
const char *host_getenv(const char *name) { return getenv(name); }
int keyboard_focused(void) { return 1; }
int keyboard_is_down(unsigned typed) { (void)typed; return 0; }
int text_input_active(void) { return 0; }
int net_racing(void) { return 0; }
int net_send_frame(int type, const void *payload, size_t n) { (void)type, (void)payload, (void)n; return 1; }

enum { RATE = 48000, FRAME = 960 };
static OpusEncoder *encoder;
static uint16_t sequence;

/* One 20 ms frame of a tone as a voice frame, with this sequence. */
static int voice_frame(uint8_t *out, double hz, uint16_t seq)
{
    static double phase;
    int16_t pcm[FRAME];
    for (int i = 0; i < FRAME; i++, phase += 2 * 3.14159265358979 * hz / RATE) pcm[i] = (int16_t)(12000 * sin(phase));
    int n = opus_encode(encoder, pcm, FRAME, out + VOICE_HEADER, VOICE_PACKET_MAX);
    out[0] = VOICE_EVENT_VERSION, out[1] = VOICE_EVENT, out[2] = (uint8_t)seq, out[3] = (uint8_t)(seq >> 8);
    return VOICE_HEADER + n;
}

static void send_frames(int slot, int count, double hz)
{
    uint8_t f[VOICE_HEADER + VOICE_PACKET_MAX];
    for (int i = 0; i < count; i++) voice_receive(slot, f, voice_frame(f, hz, sequence++));
}

/* The mix of `ms` milliseconds (in 10 ms device periods, as the output asks): its energy (RMS)
   and its zero crossings a second in the left channel. */
static double mix(int ms, double *crossings_per_second, double *left_rms, double *right_rms)
{
    static int16_t out[480 * 2];
    double sum = 0, l2 = 0, r2 = 0;
    int crossings = 0, last = 0, n = 0;
    for (int t = 0; t < ms / 10; t++) {
        memset(out, 0, sizeof out);
        voice_mix(out, 480);
        for (int i = 0; i < 480; i++, n++) {
            int l = out[i * 2], r = out[i * 2 + 1];
            sum += (double)l * l + (double)r * r, l2 += (double)l * l, r2 += (double)r * r;
            if ((l > 0) != (last > 0) && l != 0) crossings++;
            if (l) last = l;
        }
    }
    if (crossings_per_second) *crossings_per_second = crossings / (ms / 1000.0);
    if (left_rms) *left_rms = sqrt(l2 / n);
    if (right_rms) *right_rms = sqrt(r2 / n);
    return sqrt(sum / (2.0 * n));
}

int main(void)
{
    int error = 0;
    double hz, l, r;
    encoder = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &error);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    check(encoder != NULL, "the Opus encoder starts (VOIP, 48 kHz mono, 24 kbps)");
    SDL_setenv_unsafe("DHD_VOICE_VOLUME", "100", 1);
    voice_reload_settings();

    /* a voice heard: two waiting start playback, a 440 Hz tone comes out at about 440 Hz (15
       frames: the queue holds 16, 320 ms, as live voice never piles up more) */
    host_service_voice_place(3, 1000, 0);
    send_frames(3, 15, 440);
    double rms = mix(300, &hz, NULL, NULL);
    check(rms > 1000, "a slot's frames are decoded and heard");
    printf("     (%.0f zero crossings a second, RMS %.0f)\n", hz, rms);
    check(hz > 800 && hz < 960, "the tone comes out at its own pitch (about 880 crossings a second)");
    check(host_service_voice_state(VOICE_STATE_TALKING, 3), "the slot shows as talking");
    mix(400, NULL, NULL, NULL);   /* drained */
    check(mix(100, NULL, NULL, NULL) < 1, "nothing waiting: silence (no stale audio replayed)");

    /* gain 0 (out of range, another group) and mute: not heard */
    host_service_voice_place(3, 0, 0);
    send_frames(3, 10, 440);
    mix(100, NULL, NULL, NULL);   /* the gain glides down (no click), then exactly nothing */
    check(mix(150, NULL, NULL, NULL) < 1, "a slot placed at gain 0 is not heard (after its short fade)");
    host_service_voice_place(3, 1000, 0);
    host_service_voice_mute(3, 1);
    send_frames(3, 10, 440);
    check(mix(300, NULL, NULL, NULL) < 1, "a muted slot is not heard");
    check(!host_service_voice_state(VOICE_STATE_TALKING, 3), "a muted slot doesn't show as talking");
    host_service_voice_mute(3, 0);
    host_service_voice_drop(3);

    /* the pan: right of the view, louder on the right */
    host_service_voice_place(5, 1000, 800);
    send_frames(5, 20, 440);
    mix(100, NULL, NULL, NULL);   /* the gains settle */
    mix(200, NULL, &l, &r);
    check(r > 3 * l && r > 500, "panned right: the right channel carries it");
    host_service_voice_drop(-1);

    /* out of order and lost packets: still played, gaps concealed */
    host_service_voice_place(2, 1000, 0);
    {
        uint8_t f[12][VOICE_HEADER + VOICE_PACKET_MAX];
        int n[12];
        uint16_t base = sequence;
        for (int i = 0; i < 12; i++) n[i] = voice_frame(f[i], 440, (uint16_t)(base + i));
        sequence = (uint16_t)(base + 12);
        static const int order[] = {0, 2, 1, 3, 5, 4, 6, 8, 9, 10, 11};   /* 7 never comes */
        for (int i = 0; i < 11; i++) voice_receive(2, f[order[i]], n[order[i]]);
        double quiet = 1e9;
        for (int k = 0; k < 11; k++) {   /* each 20 ms of the 220 ms: none silent */
            double e = mix(20, NULL, NULL, NULL);
            if (e < quiet) quiet = e;
        }
        check(quiet > 300, "out-of-order frames reordered and a lost one concealed: no silent gap");
    }
    host_service_voice_drop(-1);

    /* two talkers at once, and the maximum: eight */
    for (int s = 0; s < 8; s++) host_service_voice_place(s, 1000, 0);
    for (int i = 0; i < 15; i++)
        for (int s = 0; s < 8; s++) {
            uint8_t f[VOICE_HEADER + VOICE_PACKET_MAX];
            voice_receive(s, f, voice_frame(f, 200 + 60 * s, (uint16_t)(sequence + i)));
        }
    sequence += 15;
    int talking = 0;
    mix(100, NULL, NULL, NULL);
    for (int s = 0; s < 8; s++) talking += host_service_voice_state(VOICE_STATE_TALKING, s);
    check(talking == 8 && mix(100, NULL, NULL, NULL) > 1000, "eight talkers at once are all heard, mixed");
    host_service_voice_drop(-1);

    /* malformed frames: a wrong version or type, too short, too long, a slot out of range */
    host_service_voice_place(4, 1000, 0);
    {
        uint8_t f[VOICE_HEADER + VOICE_PACKET_MAX + 64];
        int n = voice_frame(f, 440, sequence++);
        f[0] = 9;
        for (int i = 0; i < 5; i++) voice_receive(4, f, n);
        f[0] = VOICE_EVENT_VERSION, f[1] = 6;
        for (int i = 0; i < 5; i++) voice_receive(4, f, n);
        f[1] = VOICE_EVENT;
        voice_receive(4, f, VOICE_HEADER);                          /* no packet */
        voice_receive(4, f, VOICE_HEADER + VOICE_PACKET_MAX + 1);   /* too long */
        voice_receive(-1, f, n);
        voice_receive(10, f, n);
        check(mix(200, NULL, NULL, NULL) < 1 && !host_service_voice_state(VOICE_STATE_TALKING, 4),
              "malformed, oversized and out-of-range frames are dropped");
    }

    /* a flood: 200 frames at once; the queue stays bounded, the voice goes on */
    send_frames(4, 200, 440);
    double flood = mix(1000, NULL, NULL, NULL);
    check(flood > 1000 && mix(1000, NULL, NULL, NULL) < 1, "a flood is cut to a bounded queue and played out");
    host_service_voice_drop(-1);
    check(mix(100, NULL, NULL, NULL) < 1, "dropped (a player left, the race ended): nothing more heard");

    /* the proximity rules (voice_rules.h), at the built-in 50 m */
    float range = 50 * VOICE_UNITS_PER_METRE, d;
    d = 40 * VOICE_UNITS_PER_METRE;
    check(voice_in_range(d * d, range, 0), "40 m away: heard");
    d = 60 * VOICE_UNITS_PER_METRE;
    check(!voice_in_range(d * d, range, 0) && !voice_in_range(d * d, range, 1), "60 m away: not heard");
    d = 52 * VOICE_UNITS_PER_METRE;
    check(!voice_in_range(d * d, range, 0) && voice_in_range(d * d, range, 1),
          "52 m: not heard yet, but one heard stays heard (hysteresis to 55 m)");
    check(voice_gain(10 * VOICE_UNITS_PER_METRE, range) == 1, "near (a third of the range): full level");
    check(fabsf(voice_gain(range, range) - VOICE_EDGE) < 1e-6f && voice_gain(range * 1.05f, range) == VOICE_EDGE,
          "at the range and in the hysteresis band: the edge's level");
    check(voice_gain(30 * VOICE_UNITS_PER_METRE, range) < 1 && voice_gain(30 * VOICE_UNITS_PER_METRE, range) > VOICE_EDGE,
          "between: quieter with distance");
    check(voice_group(0, 0) == 1 && voice_group(1, 1) == 2 && !voice_group(0, 1) && !voice_group(1, 0),
          "racers hear racers by distance, the finished hear the finished, neither the other");

    opus_encoder_destroy(encoder);
    printf("RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
