/* Optional audio evidence, written only by the game/mixer thread. This is
   included by sound.c so the normal port and standalone tests use one path.
   Dumps contain retail audio: keep their destinations under ignored build/. */
#include <stdio.h>
#include <limits.h>

typedef struct { uint64_t calls, unimplemented, unsupported; } SndCommandCount;
static SndCommandCount snd_commands[65536];
static FILE *snd_wav;
static uint32_t snd_wav_bytes;
static uint64_t snd_wet_samples[2];
static int snd_debug_started, snd_report_dirty;
static const char *snd_report_path;
static uint64_t snd_grain_calls[46], snd_grain_unsupported[46];
static uint64_t snd_mix_clipped[2];
static FILE *snd_gain, *snd_gain_events;
static uint64_t snd_gain_energy[3][2];
static int snd_gain_peak[3], snd_gain_frames, snd_gain_voices, snd_gain_streams;
static int snd_gain_voice_max, snd_gain_lfo_min, snd_gain_lfo_max;
static int snd_gain_duck_min = 65536, snd_gain_duck_max;
static FILE *snd_voice_trace; /* DHD_SND_VOICE_TRACE: snd_debug_voices() */

/* A report is rewritten in place: the module's files have no rename (wasi_adapter.c), so a
   reader can see one half written. */
static int snd_report_close(FILE *file)
{
    int ok = !ferror(file);
    return !fclose(file) && ok;
}

static void snd_debug_lfo(int volume)
{
    if (!snd_gain) return;
    if (volume < snd_gain_lfo_min) snd_gain_lfo_min = volume;
    if (volume > snd_gain_lfo_max) snd_gain_lfo_max = volume;
}

static void snd_debug_duck(int gain)
{
    if (!snd_gain) return;
    if (gain < snd_gain_duck_min) snd_gain_duck_min = gain;
    if (gain > snd_gain_duck_max) snd_gain_duck_max = gain;
}

/* Component energies retain the correlations of each stereo channel. Their
   RMS values do not add: the final mix includes cross terms between sources. */
static void snd_debug_gain_frame(uint64_t sample, const int32_t components[3][2], int nvoices,
                                 int nstreams, int voice_gain)
{
    int i, ch;
    if (!snd_gain) return;
    for (i = 0; i < 3; i++) for (ch = 0; ch < 2; ch++) {
        int64_t value = components[i][ch];
        snd_gain_energy[i][ch] += (uint64_t)(value * value);
        if (value < 0) value = -value;
        if (value > snd_gain_peak[i]) snd_gain_peak[i] = (int)value;
    }
    if (nvoices > snd_gain_voices) snd_gain_voices = nvoices;
    if (nstreams > snd_gain_streams) snd_gain_streams = nstreams;
    if (voice_gain > snd_gain_voice_max) snd_gain_voice_max = voice_gain;
    if (++snd_gain_frames != 4800) return;
    fprintf(snd_gain, "%llu", (unsigned long long)(sample - 4800));
    for (i = 0; i < 3; i++)
        fprintf(snd_gain, ",%llu,%llu,%d", (unsigned long long)snd_gain_energy[i][0],
                (unsigned long long)snd_gain_energy[i][1], snd_gain_peak[i]);
    fprintf(snd_gain, ",%d,%d,%d,%d,%d,%d,%d,%d\n", snd_gain_voices, snd_gain_streams,
            snd_gain_voice_max, snd_gain_lfo_min, snd_gain_lfo_max,
            snd_gain_duck_min, snd_gain_duck_max, master_vol);
    fflush(snd_gain);
    memset(snd_gain_energy, 0, sizeof snd_gain_energy);
    memset(snd_gain_peak, 0, sizeof snd_gain_peak);
    snd_gain_frames = snd_gain_voices = snd_gain_streams = snd_gain_voice_max = 0;
    snd_gain_lfo_min = snd_gain_lfo_max = 0;
    snd_gain_duck_min = 65536; snd_gain_duck_max = 0;
}

static void snd_debug_gain_command(int cmd, const uint8_t *p, int len)
{
    int i;
    if (!snd_gain_events) return;
    if (cmd != 0x09 && cmd != 0x13 && cmd != 0x14 && cmd != 0x15 && cmd != 0x17 &&
        cmd != 0x21 && cmd != 0x2C && cmd != 0x2E && cmd != 0x2F && cmd != 0x5C) return;
    fprintf(snd_gain_events, "%llu,0x%04X,%d", (unsigned long long)rendered_samples, cmd, len);
    for (i = 0; i + 4 <= len; i += 4) fprintf(snd_gain_events, ",0x%08X", rd32(p + i));
    fprintf(snd_gain_events, "\n"); fflush(snd_gain_events);
}

static void snd_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int snd_wav_header(void)
{
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); snd_put32(h + 4, snd_wav_bytes + 36);
    memcpy(h + 8, "WAVEfmt ", 8); snd_put32(h + 16, 16);
    h[20] = 1; h[22] = 2; /* PCM, stereo */
    snd_put32(h + 24, SND_RATE); snd_put32(h + 28, SND_RATE * 4);
    h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4); snd_put32(h + 40, snd_wav_bytes);
    return !fseek(snd_wav, 0, SEEK_SET) && fwrite(h, 1, sizeof h, snd_wav) == sizeof h;
}

static void snd_debug_start(void)
{
    const char *path;
    if (snd_debug_started) return;
    snd_debug_started = 1;
    snd_report_path = getenv("DHD_SND_REPORT");
    path = getenv("DHD_SND_GAIN_TRACE");
    if (path && *path) {
        char events[1024];
        snd_gain = fopen(path, "w");
        if (snd_gain) fprintf(snd_gain, "sample,sfx_energy_left,sfx_energy_right,sfx_peak,stream_energy_left,stream_energy_right,stream_peak,wet_energy_left,wet_energy_right,wet_peak,voices,streams,max_voice_gain,min_lfo_volume,max_lfo_volume,min_duck_gain,max_duck_gain,master_gain\n");
        if (strlen(path) + 12 < sizeof events) {
            snprintf(events, sizeof events, "%s.events.csv", path);
            snd_gain_events = fopen(events, "w");
            if (snd_gain_events) fprintf(snd_gain_events, "sample,command,length,payload_words_hex\n");
        }
    }
    path = getenv("DHD_SND_VOICE_TRACE");
    if (path && *path) {
        snd_voice_trace = fopen(path, "w");
        if (snd_voice_trace)
            fprintf(snd_voice_trace, "sample,voice,core,sound,group,wave_offset,pitch,envx,phase,adsr1,adsr2,vol_l,vol_r,dry,wet,tone_flags\n");
    }
    path = getenv("DHD_SND_WAV");
    if (!path || !*path) return;
    snd_wav = fopen(path, "wb+");
    if (snd_wav && snd_wav_header() && !fflush(snd_wav))
        hle_log("989snd: final mix WAV %s (48000 Hz stereo PCM)\n", path);
    else {
        hle_log("989snd: cannot create final mix WAV %s\n", path);
        if (snd_wav) fclose(snd_wav);
        snd_wav = NULL;
    }
}

static void snd_debug_mix(const int16_t *pcm, int samples)
{
    uint8_t bytes[1024 * 4];
    int i;
    if (!snd_wav) return;
    /* Win32 fseek uses signed long. Stop before its limit, with a valid WAV. */
    if (samples > 1024 || snd_wav_bytes > INT_MAX - 44u - (uint32_t)samples * 4)
        goto fail;
    for (i = 0; i < samples * 2; i++) {
        bytes[i * 2] = (uint8_t)pcm[i];
        bytes[i * 2 + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
    }
    if (fseek(snd_wav, 44 + (long)snd_wav_bytes, SEEK_SET) ||
        fwrite(bytes, 4, (size_t)samples, snd_wav) != (size_t)samples)
        goto fail;
    snd_wav_bytes += (uint32_t)samples * 4;
    /* The run harness kills at its deadline. Publish a playable header every
       tick, without relying on shutdown/atexit in that process. */
    if (snd_wav_header() && !fflush(snd_wav)) return;
fail:
    hle_log("989snd: final mix WAV stopped (write failure or size limit)\n");
    fclose(snd_wav); snd_wav = NULL;
}

static void snd_debug_report(void)
{
    FILE *f;
    unsigned i;
    if (!snd_report_dirty || !snd_report_path || !*snd_report_path) return;
    f = fopen(snd_report_path, "w");
    if (!f) return;
    fprintf(f, "# game_samples=%llu; mix_clipped_left=%llu; mix_clipped_right=%llu; wet_samples_core0=%llu; wet_samples_core1=%llu; unimplemented=missing dispatcher; unsupported=missing sub-operation\n",
            (unsigned long long)rendered_samples, (unsigned long long)snd_mix_clipped[0],
            (unsigned long long)snd_mix_clipped[1], (unsigned long long)snd_wet_samples[0],
            (unsigned long long)snd_wet_samples[1]);
    fprintf(f, "command,calls,unimplemented,unsupported\n");
    for (i = 0; i < 65536; i++)
        if (snd_commands[i].calls)
            fprintf(f, "0x%04X,%llu,%llu,%llu\n", i,
                    (unsigned long long)snd_commands[i].calls,
                    (unsigned long long)snd_commands[i].unimplemented,
                    (unsigned long long)snd_commands[i].unsupported);
    if (snd_report_close(f)) snd_report_dirty = 0;
    {
        char path[1024];
        if (strlen(snd_report_path) + 16 >= sizeof path) return;
        snprintf(path, sizeof path, "%s.grains.csv", snd_report_path);
        f = fopen(path, "w");
        if (!f) return;
        fprintf(f, "grain,calls,unsupported\n");
        for (i = 0; i < 46; i++) if (snd_grain_calls[i])
            fprintf(f, "%u,%llu,%llu\n", i, (unsigned long long)snd_grain_calls[i],
                    (unsigned long long)snd_grain_unsupported[i]);
        snd_report_close(f);
    }
}

static void snd_debug_reset(void)
{
    if (snd_wav) fclose(snd_wav);
    if (snd_gain) fclose(snd_gain);
    if (snd_gain_events) fclose(snd_gain_events);
    if (snd_voice_trace) fclose(snd_voice_trace);
    snd_gain = snd_gain_events = snd_voice_trace = NULL;
    memset(snd_gain_energy, 0, sizeof snd_gain_energy);
    memset(snd_gain_peak, 0, sizeof snd_gain_peak);
    snd_gain_frames = snd_gain_voices = snd_gain_streams = snd_gain_voice_max = 0;
    snd_gain_lfo_min = snd_gain_lfo_max = 0;
    snd_gain_duck_min = 65536; snd_gain_duck_max = 0;
    snd_wav = NULL; snd_wav_bytes = 0;
    snd_debug_started = snd_report_dirty = 0; snd_report_path = NULL;
    memset(snd_commands, 0, sizeof snd_commands);
    memset(snd_grain_calls, 0, sizeof snd_grain_calls);
    memset(snd_grain_unsupported, 0, sizeof snd_grain_unsupported);
    memset(snd_mix_clipped, 0, sizeof snd_mix_clipped);
    memset(snd_wet_samples, 0, sizeof snd_wet_samples);
}
