/* Voice chat (finding 131): this PC's microphone to the race, and the others' voices heard.

   Transport: the race's own EVENT frames. Every server relays them to the race as they are and
   stamps the sender's slot in byte 1 (online/server/src/room.ts relay), so voice needs no server
   change, a sender can't pass as another, and a frame never leaves its race (the server keeps one
   race a room). Outside a race nothing is sent or heard (the server relays no binary frames
   there). A frame: [VOICE_EVENT_VERSION, VOICE_EVENT, sequence (16 bits, little-endian), Opus].

   Sending (the voice thread, never the game or render thread): the recording device (SDL3; the
   default one, or the first whose name holds DHD_VOICE_DEVICE) in 48 kHz mono, 20 ms frames
   (960 samples) encoded with libopus (VOIP, DHD_VOICE_BITRATE 8000-64000, built-in 24000) and
   sent with net_send_frame. It transmits while the push-to-talk key is held (DHD_VOICE_KEY, a
   letter, built-in V; not while a text field has the keyboard) or, with DHD_VOICE_MODE=open,
   while the microphone is louder than a gate. The device is opened when a race starts and closed
   when it ends; a device that fails or goes away is tried again every few seconds.

   Hearing (the output's device thread, voice_mix): each sender slot has a decoder and a bounded
   queue of packets (VOICE_QUEUE, the oldest dropped), ordered by sequence: playback starts once two
   are waiting (40 ms), a missing packet is concealed (Opus PLC) up to three times, late or repeated
   ones are dropped, and a flood beyond 60 packets a second is dropped. Each voice is mixed at the
   gain and pan the game gives it every frame (host_service_voice_place, online.c: the proximity
   and the groups), moved there smoothly, times DHD_VOICE_VOLUME; a muted slot is not heard.

   DHD_VOICE_TONE=Hz (tests): no microphone; a tone is sent as if the key were held, so two games
   prove the path end to end (online_race.py); each slot's packets taken and played are logged
   every 5 s ("voice: slot N: ...").

   Nothing here touches game state. Built only when libopus is (port/tools/opus.py: DHD_VOICE);
   elsewhere every call does nothing. */
#include "voice.h"
#include "host_api.h"
#include "../hle/hle.h"
#include "../hle/text_input.h"
#include "../net/net.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if DHD_VOICE
#include <opus.h>

enum { RATE = 48000, FRAME = 960, SLOTS = NET_MAX_SLOTS, VOICE_QUEUE = 16, PLC_MAX = 3,
       START_DEPTH = 2, FLOOD_PER_SECOND = 60, RETRY_MS = 4000 };

/* ---- the settings (dhd.ini through DHD_*; read again every second, so PC SETTINGS is live) ---- */

static int setting(const char *name, int fallback, int lo, int hi)
{
    const char *v = host_getenv(name);
    int n = v && *v ? atoi(v) : fallback;
    return n < lo ? lo : n > hi ? hi : n;
}

static struct {
    int enabled, open_mic, key, bitrate, mic_volume, volume, tone;
    char device[128];
} cfg;

static void read_settings(void)
{
    const char *v = host_getenv("DHD_VOICE");
    cfg.enabled = !(v && (!strcmp(v, "0") || !SDL_strcasecmp(v, "off")));
    v = host_getenv("DHD_VOICE_MODE");
    cfg.open_mic = v && !SDL_strcasecmp(v, "open");
    v = host_getenv("DHD_VOICE_KEY");
    cfg.key = v && v[0] && !v[1] && SDL_isalnum((unsigned char)v[0]) ? SDL_toupper((unsigned char)v[0]) : 'V';
    cfg.bitrate = setting("DHD_VOICE_BITRATE", 24000, 8000, 64000);
    cfg.mic_volume = setting("DHD_VOICE_MIC", 100, 0, 200);
    cfg.volume = setting("DHD_VOICE_VOLUME", 100, 0, 100);
    cfg.tone = setting("DHD_VOICE_TONE", 0, 0, 4000);
    v = host_getenv("DHD_VOICE_DEVICE");
    snprintf(cfg.device, sizeof cfg.device, "%s", v ? v : "");
}

/* ---- what the game reads (host_service_voice_state) ---- */
static SDL_AtomicInt transmitting, mic_state, level;   /* level: the microphone's peak, 0..1000 */
static SDL_AtomicInt heard_at[SLOTS];                   /* SDL_GetTicks of a slot's last packet */

/* ---- sending: the voice thread ---- */

static SDL_AudioStream *open_microphone(void)
{
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, RATE};
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (cfg.device[0]) {
        int count = 0;
        SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&count);
        for (int i = 0; ids && i < count; i++) {
            const char *name = SDL_GetAudioDeviceName(ids[i]);
            if (name && SDL_strcasestr(name, cfg.device)) { id = ids[i]; break; }
        }
        SDL_free(ids);
    }
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(id, &spec, NULL, NULL);
    if (!s) {
        hle_log("voice: no microphone (%s)\n", SDL_GetError());
        return NULL;
    }
    SDL_ResumeAudioStreamDevice(s);
    hle_log("voice: microphone %s\n", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s)) ?
            SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s)) : "(default)");
    return s;
}

static int talk_key_held(void)
{
    return keyboard_focused() && keyboard_is_down((unsigned)cfg.key) && !text_input_active();
}

static int voice_thread(void *unused)
{
    SDL_AudioStream *mic = NULL;
    OpusEncoder *encoder = NULL;
    int16_t pcm[FRAME];
    uint8_t frame[VOICE_HEADER + VOICE_PACKET_MAX];
    int have = 0, bitrate = 0, gate_hold = 0;
    uint16_t sequence = 0;
    Uint64 next_settings = 0, retry_at = 0;
    (void)unused;
    for (;;) {
        Uint64 now = SDL_GetTicks();
        if (now >= next_settings) { read_settings(); next_settings = now + 1000; }
        int want = cfg.enabled && net_racing();
        if (!want) {   /* no race (or voice off): the microphone is let go */
            if (mic) { SDL_DestroyAudioStream(mic); mic = NULL; hle_log("voice: microphone closed\n"); }
            SDL_SetAtomicInt(&transmitting, 0);
            have = 0;
            SDL_Delay(50);
            continue;
        }
        if (cfg.tone) {   /* tests: a tone in place of the microphone, sent at the microphone's pace */
            static double phase;
            for (int i = 0; i < FRAME; i++, phase += 2 * 3.14159265358979 * cfg.tone / RATE) pcm[i] = (int16_t)(8000 * sin(phase));
            if (!encoder) {
                int error = 0;
                encoder = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &error);
                if (!encoder) { SDL_Delay(1000); continue; }
            }
            int bytes = opus_encode(encoder, pcm, FRAME, frame + VOICE_HEADER, VOICE_PACKET_MAX);
            SDL_SetAtomicInt(&transmitting, 1);
            if (bytes > 0) {
                frame[0] = VOICE_EVENT_VERSION, frame[1] = VOICE_EVENT;
                frame[2] = (uint8_t)sequence, frame[3] = (uint8_t)(sequence >> 8);
                sequence++;
                net_send_frame(NET_FRAME_EVENT, frame, (size_t)(VOICE_HEADER + bytes));
            }
            SDL_Delay(20);
            continue;
        }
        if (!mic && now >= retry_at) {
            mic = open_microphone();
            SDL_SetAtomicInt(&mic_state, mic ? 1 : -1);
            if (!mic) retry_at = now + RETRY_MS;
            have = 0;
        }
        if (!encoder) {
            int error = 0;
            encoder = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &error);
            if (!encoder) { hle_log("voice: no Opus encoder (%d)\n", error); SDL_Delay(1000); continue; }
        }
        if (bitrate != cfg.bitrate) {
            bitrate = cfg.bitrate;
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
        }
        if (!mic) { SDL_Delay(20); continue; }
        int got = SDL_GetAudioStreamData(mic, pcm + have, (FRAME - have) * 2);
        if (got < 0) {   /* the device went away: let go, try again later */
            hle_log("voice: microphone lost (%s)\n", SDL_GetError());
            SDL_DestroyAudioStream(mic);
            mic = NULL;
            SDL_SetAtomicInt(&mic_state, -1);
            retry_at = now + RETRY_MS;
            continue;
        }
        have += got / 2;
        if (have < FRAME) { SDL_Delay(5); continue; }
        have = 0;
        int peak = 0;
        for (int i = 0; i < FRAME; i++) {
            int s = pcm[i] * cfg.mic_volume / 100;
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            pcm[i] = (int16_t)s;
            if (abs(s) > peak) peak = abs(s);
        }
        SDL_SetAtomicInt(&level, peak * 1000 / 32768);
        int talk;
        if (cfg.open_mic) {   /* a gate: above about -40 dBFS, held a quarter second after */
            if (peak > 330) gate_hold = 12;
            talk = gate_hold > 0;
            if (gate_hold > 0) gate_hold--;
        } else talk = talk_key_held();
        SDL_SetAtomicInt(&transmitting, talk);
        if (!talk) continue;
        int bytes = opus_encode(encoder, pcm, FRAME, frame + VOICE_HEADER, VOICE_PACKET_MAX);
        if (bytes <= 0) continue;
        frame[0] = VOICE_EVENT_VERSION, frame[1] = VOICE_EVENT;
        frame[2] = (uint8_t)sequence, frame[3] = (uint8_t)(sequence >> 8);
        sequence++;
        net_send_frame(NET_FRAME_EVENT, frame, (size_t)(VOICE_HEADER + bytes));
    }
    return 0;
}

/* ---- hearing: the queues (filled by the game thread) and the device thread's playback ---- */

typedef struct { uint16_t sequence; uint16_t bytes; uint8_t data[VOICE_PACKET_MAX]; } Packet;
static struct Speaker {
    SDL_SpinLock lock;
    Packet queue[VOICE_QUEUE];  /* in sequence order */
    int queued;
    int flood_count;
    Uint64 flood_second;
    int taken, played, log_at;  /* for the log (tests) */
    /* the device thread's own */
    OpusDecoder *decoder;
    int playing, next, misses;
    int16_t pcm[FRAME];
    int pcm_at, pcm_have;
    float left, right;          /* the gains now, moving toward the target */
} speakers[SLOTS];
static SDL_AtomicInt target_gain[SLOTS], target_pan[SLOTS], muted[SLOTS];  /* the game's, x1000 */

/* sequence a is before b (16-bit wraparound) */
static int before(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

void voice_receive(int slot, const uint8_t *in, int n)
{
    if (slot < 0 || slot >= SLOTS || n <= VOICE_HEADER || n > VOICE_HEADER + VOICE_PACKET_MAX) return;
    if (in[0] != VOICE_EVENT_VERSION || in[1] != VOICE_EVENT) return;
    struct Speaker *s = &speakers[slot];
    Uint64 now = SDL_GetTicks();
    uint16_t sequence = (uint16_t)(in[2] | in[3] << 8);
    SDL_LockSpinlock(&s->lock);
    if (now / 1000 != s->flood_second) s->flood_second = now / 1000, s->flood_count = 0;
    if (++s->flood_count > FLOOD_PER_SECOND) { SDL_UnlockSpinlock(&s->lock); return; }   /* a flood */
    int at_index = s->queued;
    while (at_index > 0 && before(sequence, s->queue[at_index - 1].sequence)) at_index--;
    if (at_index > 0 && s->queue[at_index - 1].sequence == sequence) { SDL_UnlockSpinlock(&s->lock); return; }
    if (s->queued == VOICE_QUEUE) {   /* full: the oldest goes */
        if (at_index == 0) { SDL_UnlockSpinlock(&s->lock); return; }
        memmove(s->queue, s->queue + 1, sizeof s->queue[0] * (VOICE_QUEUE - 1));
        s->queued--, at_index--;
    }
    memmove(s->queue + at_index + 1, s->queue + at_index, sizeof s->queue[0] * (s->queued - at_index));
    s->queue[at_index].sequence = sequence;
    s->queue[at_index].bytes = (uint16_t)(n - VOICE_HEADER);
    memcpy(s->queue[at_index].data, in + VOICE_HEADER, (size_t)(n - VOICE_HEADER));
    s->queued++;
    s->taken++;
    int log = (int)(now / 5000) != s->log_at;
    if (log) s->log_at = (int)(now / 5000);
    int taken = s->taken, played = s->played;
    SDL_UnlockSpinlock(&s->lock);
    SDL_SetAtomicInt(&heard_at[slot], (int)now);
    if (log) hle_log("voice: slot %d: %d packets taken, %d played\n", slot, taken, played);
}

void host_service_voice_frame(int slot, uint32_t at, int n) { voice_receive(slot, (const uint8_t *)GUEST(at), n); }

void voice_reload_settings(void) { read_settings(); }

void host_service_voice_place(int slot, int gain_permille, int pan_permille)
{
    if (slot < 0 || slot >= SLOTS) return;
    SDL_SetAtomicInt(&target_gain[slot], gain_permille < 0 ? 0 : gain_permille > 1000 ? 1000 : gain_permille);
    SDL_SetAtomicInt(&target_pan[slot], pan_permille < -1000 ? -1000 : pan_permille > 1000 ? 1000 : pan_permille);
}

void host_service_voice_mute(int slot, int on)
{
    if (slot >= 0 && slot < SLOTS) SDL_SetAtomicInt(&muted[slot], on != 0);
}

/* A slot's player left, or the race ended: its waiting packets go. */
void host_service_voice_drop(int slot)
{
    for (int i = 0; i < SLOTS; i++) {
        if (slot >= 0 && i != slot) continue;
        SDL_LockSpinlock(&speakers[i].lock);
        speakers[i].queued = 0;
        SDL_UnlockSpinlock(&speakers[i].lock);
        SDL_SetAtomicInt(&target_gain[i], 0);
        SDL_SetAtomicInt(&heard_at[i], 0);
    }
}

int host_service_voice_state(int field, int slot)
{
    switch (field) {
    case VOICE_STATE_ENABLED: return cfg.enabled;
    case VOICE_STATE_TRANSMITTING: return SDL_GetAtomicInt(&transmitting);
    case VOICE_STATE_MIC: return SDL_GetAtomicInt(&mic_state);
    case VOICE_STATE_LEVEL: return SDL_GetAtomicInt(&level);
    case VOICE_STATE_TALKING:   /* a packet from that slot in the last 300 ms */
        return slot >= 0 && slot < SLOTS && SDL_GetAtomicInt(&heard_at[slot]) &&
               (Uint32)SDL_GetTicks() - (Uint32)SDL_GetAtomicInt(&heard_at[slot]) < 300 && !SDL_GetAtomicInt(&muted[slot]);
    case VOICE_STATE_MUTED: return slot >= 0 && slot < SLOTS && SDL_GetAtomicInt(&muted[slot]);
    case VOICE_STATE_KEY: return cfg.key;
    }
    return 0;
}

/* The next 20 ms of one speaker into s->pcm (the device thread): its packet in sequence, a
   concealed one for a gap, or nothing (it stops until two are waiting again). */
static int next_frame(struct Speaker *s)
{
    Packet p;
    int have = 0;
    SDL_LockSpinlock(&s->lock);
    if (!s->playing && s->queued >= START_DEPTH) s->playing = 1, s->next = s->queue[0].sequence, s->misses = 0;
    while (s->playing && s->queued && before(s->queue[0].sequence, (uint16_t)s->next)) {   /* late: gone */
        memmove(s->queue, s->queue + 1, sizeof s->queue[0] * (s->queued - 1));
        s->queued--;
    }
    if (s->playing && s->queued && s->queue[0].sequence == (uint16_t)s->next) {
        p = s->queue[0];
        memmove(s->queue, s->queue + 1, sizeof s->queue[0] * (s->queued - 1));
        s->queued--;
        have = 1;
    }
    int waiting = s->queued;
    SDL_UnlockSpinlock(&s->lock);
    if (!s->playing) return 0;
    int n;
    if (have) n = opus_decode(s->decoder, p.data, p.bytes, s->pcm, FRAME, 0), s->misses = 0, s->played++;
    else if (waiting && s->misses < PLC_MAX) n = opus_decode(s->decoder, NULL, 0, s->pcm, FRAME, 0), s->misses++;
    else { s->playing = 0; return 0; }   /* nothing more: silence until the next start */
    s->next = (uint16_t)(s->next + 1);
    if (n <= 0) return 0;
    s->pcm_at = 0, s->pcm_have = n;
    return 1;
}

void voice_mix(int16_t *stereo, int frames)
{
    float volume = cfg.volume / 100.0f;
    for (int slot = 0; slot < SLOTS; slot++) {
        struct Speaker *s = &speakers[slot];
        if (!s->decoder) {
            int error = 0;
            s->decoder = opus_decoder_create(RATE, 1, &error);
            if (!s->decoder) continue;
        }
        float gain = SDL_GetAtomicInt(&muted[slot]) ? 0 : SDL_GetAtomicInt(&target_gain[slot]) / 1000.0f * volume;
        float pan = SDL_GetAtomicInt(&target_pan[slot]) / 1000.0f, angle = (pan + 1) * 0.78539816f;
        float want_l = gain * fminf(1, 1.41421356f * cosf(angle)), want_r = gain * fminf(1, 1.41421356f * sinf(angle));
        for (int i = 0; i < frames; i++) {
            if (s->pcm_at >= s->pcm_have && !next_frame(s)) break;
            /* the gains move a little each sample: no step is heard */
            s->left += (want_l - s->left) * 0.002f, s->right += (want_r - s->right) * 0.002f;
            if (want_l == 0 && s->left < 1e-4f) s->left = 0;     /* gone: exactly silent, not a whisper */
            if (want_r == 0 && s->right < 1e-4f) s->right = 0;
            float v = s->pcm[s->pcm_at++];
            int l = stereo[i * 2] + (int)(v * s->left), r = stereo[i * 2 + 1] + (int)(v * s->right);
            stereo[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            stereo[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
    }
}

void voice_open(void)
{
    static int opened;
    if (opened++) return;
    read_settings();
    SDL_Thread *t = SDL_CreateThread(voice_thread, "dhd voice", NULL);
    if (t) SDL_DetachThread(t);
    else hle_log("voice: can't start its thread (%s)\n", SDL_GetError());
}

#else   /* no libopus on this target: no voice */
void voice_open(void) {}
void voice_mix(int16_t *stereo, int frames) { (void)stereo, (void)frames; }
void host_service_voice_frame(int slot, uint32_t at, int n) { (void)slot, (void)at, (void)n; }
void voice_receive(int slot, const uint8_t *in, int n) { (void)slot, (void)in, (void)n; }
void voice_reload_settings(void) {}
void host_service_voice_place(int slot, int gain, int pan) { (void)slot, (void)gain, (void)pan; }
void host_service_voice_mute(int slot, int on) { (void)slot, (void)on; }
void host_service_voice_drop(int slot) { (void)slot; }
int host_service_voice_state(int field, int slot) { (void)field, (void)slot; return 0; }
#endif
