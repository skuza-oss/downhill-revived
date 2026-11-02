/* 989snd version-1 sound programs. All timing (240 Hz) and PRNG state are
   advanced by rendered game samples. No callbacks from the output thread.
   Layout/semantics were cross-checked with retail wrappers and OpenGOAL's
   ISC 989snd implementation; this is a separate bounded C implementation. */
#include "sound_tables.h"
#include "sound_lfo_table.h"

typedef struct {
    uint32_t phase, step, original_step;
    int target, extra, shape, depth, original_depth, flags, duty, hold, half, tick;
} GrainLfo;

typedef struct {
    int used, paused, done, pc, delay, depth;
    int stopped; /* stopped (0xFE04): retail frees it at once, so it no longer counts as an instance */
    int volume, pan, pmod, pbend, original_volume;
    int lfo_volume, lfo_pan, lfo_pmod, lfo_bend;
    int auto_pan_from, auto_pan_to, auto_pan_tick, auto_pan_time;
    GrainLfo lfo[4];
    int play_count, skip_count;
    int8_t regs[4];
    uint32_t handle, parent;
    uint64_t created_at;
    BankSlot *bank;
    const SndBankSound *sound;
} SoundHandler;

static SoundHandler handlers[128];
static int8_t global_regs[32];
static uint64_t grain_samples;
#include "sound_rng.h"

static int bound(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static SoundHandler *handler_find(uint32_t handle)
{
    int i;
    for (i = 0; i < 128; i++)
        if (handlers[i].used && handlers[i].handle == handle) return &handlers[i];
    return NULL;
}

static int8_t *handler_reg(SoundHandler *h, int index)
{
    if (index >= 0 && index < 4) return &h->regs[index];
    if (index < 0 && index >= -32) return &global_regs[-index - 1];
    return NULL;
}

static int grain_value(SoundHandler *h, int value, int pan)
{
    int index;
    if (value >= 0) return value;
    if (value == -5) return (int)grain_random(pan ? 360 : 128);
    index = -value - 1;
    if (index < 4) value = h->regs[index];
    else if (index >= 5 && index < 37) value = global_regs[index - 5];
    else value = 0;
    return pan ? 360 * bound(value, 0, 127) / 127 : bound(value, 0, 127);
}

static int tone_pitch(const SndTone *tone, int pmod, int bend)
{
    int center = tone->center_note, total, semi, fine, octave, note, shift;
    uint32_t pitch;
    /* Negative center notes identify PS2 (48000 Hz); positive notes are PS1
       (44100 Hz). Fine units and pitch modulation are 1/128 semitone. */
    total = 60 * 128 + pmod + tone->center_fine;
    total += (int)((int64_t)(bend >= 0 ? tone->pb_high : tone->pb_low) * bend * 128 /
                   (bend >= 0 ? 32767 : 32768));
    total -= (center < 0 ? -center : center) * 128;
    semi = total / 128; fine = total % 128;
    if (fine < 0) { fine += 128; semi--; }
    octave = semi / 12; note = semi % 12;
    if (note < 0) { note += 12; octave--; }
    pitch = ((uint32_t)snd_note_table[note] * snd_note_table[fine + 12]) >> 16;
    shift = octave - 2;
    if (shift >= 0) pitch = shift > 1 ? 0x3FFF : pitch << shift;
    else if (shift < -31) pitch = 0;
    else pitch = (pitch + (1u << (-shift - 1))) >> -shift;
    if (center >= 0) pitch = pitch * 44100 / 48000;
    return (int)(pitch > 0x3FFF ? 0x3FFF : pitch);
}

static void pan_volume(int amplitude, int pan, int32_t out[2])
{
    int angle = pan % 360, at;
    if (playback_mode == 1) { out[0] = out[1] = amplitude; return; }
    if (angle < 0) angle += 360;
    at = (angle + 90) % 360;
    if (at < 180) {
        out[0] = amplitude * snd_pan_table[180 - at] / 16383;
        out[1] = amplitude * snd_pan_table[at] / 16383;
    } else {
        at -= 180;
        out[1] = amplitude * snd_pan_table[180 - at] / 16383;
        out[0] = amplitude * snd_pan_table[at] / 16383;
    }
}

static int32_t group_gain(int32_t value, int group)
{
    int sign = value < 0 ? -1 : 1;
    if (group < 0 || group >= 15) return value;
    if (value >= 32767) value = 32766;
    value = (int)((int64_t)value * ((int64_t)group_vol[group] * group_duck[group] / 65536) / 1024);
    return value * value / 32766 * sign;
}

static void handler_voice_update(SoundHandler *h, Voice *v)
{
    int32_t gain[2];
    int sound_volume = bound(h->volume * h->original_volume / 1024 + h->lfo_volume, 0, 127);
    int tone_volume = v->tone_volume;
    int amplitude = 32766 * sound_volume / 127 * tone_volume / 127;
    int pan = h->pan + h->lfo_pan + v->tone_pan;
    pan_volume(amplitude, pan, gain);
    snd_volume_set(&v->gain[0], (uint16_t)(group_gain(gain[0], h->sound->group) >> 1));
    snd_volume_set(&v->gain[1], (uint16_t)(group_gain(gain[1], h->sound->group) >> 1));
    v->vol_l = v->gain[0].level; v->vol_r = v->gain[1].level;
    v->step = (uint32_t)tone_pitch(&v->tone, h->pmod + h->lfo_pmod,
                                 bound(h->pbend + h->lfo_bend, -32768, 32767)) << 4;
}

static void handler_update(SoundHandler *h)
{
    int i;
    for (i = 0; i < SND_VOICES; i++)
        if (voices[i].used && voices[i].owner == h->handle) handler_voice_update(h, &voices[i]);
}

static void handler_update_children(SoundHandler *h, unsigned flags)
{
    int i;
    for (i = 0; i < 128; i++) if (handlers[i].used && handlers[i].parent == h->handle) {
        SoundHandler *child = &handlers[i];
        if (flags & 1) child->volume = application_volume(child->volume, h->volume * h->original_volume / 127);
        if (flags & 2) child->pan = h->pan;
        if (flags & 8) child->pmod = h->pmod;
        if (flags & 16) child->pbend = h->pbend;
        handler_update(child); handler_update_children(child, flags);
    }
}

static void handler_modulate(SoundHandler *h, int which)
{
    int i, changed = 0;
    for (i = which < 0 ? 0 : which; i < (which < 0 ? 4 : which + 1); i++) {
        GrainLfo *l = &h->lfo[i];
        int phase, value = 0, range, result, *target;
        if (!l->target || !(++l->tick & 1)) continue;
        phase = (int)(l->phase >> 16);
        l->phase = (l->phase + l->step * 2) & 0x7FFFFFF;
        switch (l->shape) {
        case 1: value = snd_lfo_sine[phase]; break;
        case 2: value = phase < l->duty ? 32767 : -32767; break;
        case 3: value = phase < 512 ? 32767 * phase / 512 : phase >= 1536 ?
                           32767 * (phase - 1536) / 512 - 32767 : 32767 - 65534 * (phase - 512) / 1024; break;
        case 4: value = phase < 1024 ? 32767 * phase / 1023 : 32767 * (phase - 1024) / 1024 - 32767; break;
        case 5:
            /* Retail hold2=1 denotes the negative (<1024) half, initialized
               by Init. Do not consume a second pair of random words at t=0. */
            if ((phase < 1024) != l->half) {
                l->half = phase < 1024;
                l->hold = !l->half ? 2 * ((int)grain_random(32768) - 16383) :
                                   -(int)grain_random(32768) * (int)grain_random(2);
            }
            value = l->hold; break;
        }
        if (l->flags & 1) value = -value;
        if (l->target >= 5) {
            GrainLfo *other = &h->lfo[l->extra];
            if (l->target == 5) {
                uint32_t range = ((other->original_step >> 8) * (uint32_t)l->depth) >> 10;
                other->step = other->original_step + (uint32_t)(((int64_t)range * value >> 15) * 256);
            } else {
                int range = other->original_depth * l->depth >> 10;
                other->depth = (uint16_t)(other->original_depth + ((int64_t)range * value >> 15));
            }
            continue;
        }
        target = l->target == 1 ? &h->lfo_volume : l->target == 2 ? &h->lfo_pan :
                 l->target == 3 ? &h->lfo_pmod : &h->lfo_bend;
        range = (l->target == 1 ? h->original_volume : l->target == 2 ? 180 : l->target == 3 ? 6096 : 32767);
        range = range * l->depth >> 10;
        result = l->target == 1 ? (range * (value - 32767) >> 16) : (int)((int64_t)range * value >> 15);
        if (l->target == 1) snd_debug_lfo(result);
        if (*target != result) { *target = result; changed = 1; }
    }
    if (which < 0 && h->auto_pan_time) {
        h->auto_pan_tick++;
        h->pan = (h->auto_pan_from + (h->auto_pan_to - h->auto_pan_from) * h->auto_pan_tick / h->auto_pan_time) % 360;
        if (h->pan < 0) h->pan += 360;
        if (h->auto_pan_tick >= h->auto_pan_time) h->auto_pan_time = 0;
        handler_update_children(h, 2);
        changed = 1;
    }
    if (changed) handler_update(h);
}

static int handler_voices(SoundHandler *h)
{
    int i;
    for (i = 0; i < SND_VOICES; i++)
        if (voices[i].used && voices[i].owner == h->handle) return 1;
    return 0;
}

static void handler_keyoff(SoundHandler *h, int kill)
{
    int i;
    for (i = 0; i < SND_VOICES; i++) if (voices[i].used && voices[i].owner == h->handle) {
        if (kill) voices[i].used = 0;
        else snd_adsr_release(&voices[i].env);
    }
}

static void handler_stop(SoundHandler *h)
{
    int i;
    h->done = 1; h->stopped = 1; h->paused = 0; handler_keyoff(h, 0);
    for (i = 0; i < SND_VOICES; i++) if (voices[i].owner == h->handle) voices[i].paused = 0;
    for (i = 0; i < 128; i++)
        if (handlers[i].used && handlers[i].parent == h->handle) handler_stop(&handlers[i]);
}

/* An END grain (type 24). Its retail handler (IOP 0x5A60) returns -1, so the program dispatcher
   (0x6A94) answers "done" and the 240-Hz update loop (0x1021C) stops the handler with 0xFE04(h, 0, 0):
   its own voices are keyed off into their release (0x19B18), and it leaves its parent/child chain
   without stopping the children, which go on as sounds of their own. Without the key-off a looping
   tone before END (E_TREE_WHOOSH's wind, the flywheels) never stops (finding 133). */
static void handler_end(SoundHandler *h)
{
    int i;
    h->done = 1; h->stopped = 1;
    handler_keyoff(h, 0);
    for (i = 0; i < 128; i++)
        if (handlers[i].used && handlers[i].parent == h->handle) handlers[i].parent = 0;
}

static void handler_pause(SoundHandler *h, int pause)
{
    int i;
    h->paused = pause;
    for (i = 0; i < SND_VOICES; i++)
        if (voices[i].used && voices[i].owner == h->handle) voices[i].paused = pause;
    for (i = 0; i < 128; i++)
        if (handlers[i].used && handlers[i].parent == h->handle) handler_pause(&handlers[i], pause);
}

static SoundHandler *handler_start(BankSlot *, const SndBankSound *, int, int, int, int,
                                    SoundHandler *, int, int);

static const uint8_t *handler_grain(SoundHandler *h, int pc)
{
    return h->bank->bank.data + h->sound->grain_off + pc * 40;
}

static void handler_run(SoundHandler *h)
{
    int budget = 256;
    while (!h->done && h->delay <= 0 && budget--) {
        const uint8_t *g = handler_grain(h, h->pc), *p = g + 8;
        int type = (int)rd32(g), pc = h->pc, next = pc + 1, extra_delay = 0;
        int a = (int16_t)rd16(p), b = (int16_t)rd16(p + 2), c = (int16_t)rd16(p + 4), i;
        int8_t *reg;
        snd_grain_calls[type < 45 && type >= 0 ? type : 45]++;
        switch (type) {
        case 0: case 20: case 21: case 35: break;
        case 4: {
            GrainLfo *l;
            if (p[0] >= 4 || p[1] > 6 || p[3] > 5 || (p[1] >= 5 && p[2] >= 4)) { snd_grain_unsupported[4]++; break; }
            l = &h->lfo[p[0]]; memset(l, 0, sizeof *l);
            l->target = p[1]; l->extra = p[2]; l->shape = p[3]; l->duty = rd16(p + 4);
            l->depth = l->original_depth = rd16(p + 6); l->flags = rd16(p + 8);
            l->phase = ((l->flags & 2) ? grain_random(2048) : rd16(p + 10) & 2047) << 16;
            l->step = l->original_step = rd32(p + 12);
            if (l->shape == 5) { l->hold = -(int)grain_random(32768) * (int)grain_random(2); l->half = 1; }
            handler_modulate(h, p[0]);
            break;
        }
        case 1: case 9: {
            SndTone tone;
            int voice, group = bound(h->sound->group, 0, 15);
            if (!snd_bank_tone(&h->bank->bank, h->sound->grain_off + pc * 40, &tone)) break;
            if (tone.flags & 8) { snd_grain_unsupported[type]++; break; }
            if (!alloc_voice(h->sound->group)) {
                Voice *victim = NULL;
                for (i = 0; i < SND_VOICES; i++)
                    if (voices[i].owner && i >= voice_range[group][0] &&
                        i <= voice_range[group][1] &&
                        (!victim || voices[i].tone.priority < victim->tone.priority))
                        victim = &voices[i];
                if (victim && victim->tone.priority <= tone.priority) victim->used = 0;
            }
            voice = start_voice(h->bank->bank.data + tone.adpcm_off, tone.adpcm_len, 1024, 0, 0, h->sound->group);
            if (voice) {
                Voice *v = NULL;
                for (i = 0; i < SND_VOICES; i++) if (voices[i].handle == (uint32_t)voice) v = &voices[i];
                v->owner = h->handle; v->tone = tone;
                /* Register/random tone parameters are captured at allocation,
                   as at IOP 5500/5588, then held across setters and LFO ticks. */
                v->tone_volume = bound(grain_value(h, tone.volume, 0), 0, 127);
                v->tone_pan = grain_value(h, tone.pan, 1);
                snd_adsr_set(&v->env, tone.adsr1, tone.adsr2); snd_adsr_start(&v->env);
                handler_voice_update(h, v);
            } else snd_grain_unsupported[type]++;
            break;
        }
        case 5: { /* same-bank child sound; inherits registers and application parameters */
            int id = (int)rd32(p + 12), vol = grain_value(h, (int)rd32(p), 0);
            const SndBankSound *child = NULL;
            if (id >= 0 && id < h->bank->bank.nsounds) child = &h->bank->bank.sounds[id];
            else if (id < 0) child = snd_bank_find(&h->bank->bank, (const char *)p + 16);
            if (child) handler_start(h->bank, child, h->volume * h->original_volume / 127,
                                     h->pan, h->pmod, h->pbend, h, vol,
                                     grain_value(h, (int)rd32(p + 4), 1));
            else snd_grain_unsupported[type]++;
            break;
        }
        case 6:
            for (i = 0; i < 128; i++)
                if (handlers[i].used && handlers[i].parent == h->handle &&
                    handlers[i].sound->index == (int)rd32(p + 12)) handler_stop(&handlers[i]);
            break;
        case 22:
            for (i = pc - 1; i >= 0; i--) if (rd32(handler_grain(h, i)) == 21) { next = i; break; }
            if (i < 0) snd_grain_unsupported[type]++;
            break;
        case 23:
            for (i = pc + 1; i < h->sound->ngrains; i++) if (rd32(handler_grain(h, i)) == 22) { next = i + 1; break; }
            break;
        case 24: handler_end(h); break;
        case 25: case 39: {
            uint32_t choice_slot = (h->sound->grain_off + pc * 40 - 0x18 - rd32(h->bank->bank.data + 0x38)) / 40;
            int previous = choice_slot < 4096 ? h->bank->grain_choice[choice_slot] : INT16_MIN, pick;
            if (a <= 0 || b <= 0 || a * b > h->sound->ngrains - next) { snd_grain_unsupported[type]++; h->done = 1; break; }
            if (previous == INT16_MIN) previous = c;
            pick = type == 25 ? (int)grain_random(a) : previous;
            if (type == 25 && pick == previous) pick = (pick + 1) % a;
            pick = bound(pick, 0, a - 1);
            if (choice_slot < 4096) h->bank->grain_choice[choice_slot] = (int16_t)(type == 25 ? pick : (pick + 1) % a);
            next += pick * b; h->play_count = b + 1; h->skip_count = (a - 1 - pick) * b;
            break;
        }
        case 26: extra_delay = (int)grain_random(rd32(p) + 1u); break;
        case 27: {
            /* IOP 5C0C: reduce the full R250 word modulo 32767, spread
               that fraction over signed pitch-bend range, then apply percent. */
            int bend = (int)(65535u * grain_random(32767) / 32767) - 32768;
            h->pbend = (int16_t)(a * bend / 100);
            handler_update(h); break;
        }
        case 28: h->pbend = bound(a * 32767 / 127, -32768, 32767); handler_update(h); break;
        case 29: h->pbend = bound(h->pbend + a * 32767 / 127, -32768, 32767); handler_update(h); break;
        case 30: case 31: case 32: case 33:
            reg = handler_reg(h, a);
            if (!reg) { snd_grain_unsupported[type]++; break; }
            if (type == 30) *reg = (int8_t)b;
            if (type == 31 && c >= b) *reg = (int8_t)(b + grain_random(c - b + 1));
            if (type == 32) *reg = (int8_t)bound(*reg + 1, -128, 127);
            if (type == 33) *reg = (int8_t)bound(*reg - 1, -128, 127);
            break;
        case 34:
            reg = handler_reg(h, a);
            if (reg && ((b == 0 && *reg >= c) || (b == 1 && *reg != c) || (b >= 2 && *reg <= c))) next++;
            break;
        case 36: case 37: {
            int mark = type == 37 && b >= a ? a + (int)grain_random(b - a + 1) : a;
            for (i = 0; i < h->sound->ngrains; i++) {
                const uint8_t *target = handler_grain(h, i);
                if (rd32(target) == 35 && (int16_t)rd16(target + 8) == mark) { next = i; break; }
            }
            if (i == h->sound->ngrains) snd_grain_unsupported[type]++;
            break;
        }
        case 38: if (handler_voices(h)) { next = pc; extra_delay = 1; } break;
        case 40: reg = handler_reg(h, b); if (reg) *reg = (int8_t)bound(*reg + a, -128, 127); break;
        case 41: handler_keyoff(h, 0); break;
        case 42: handler_keyoff(h, 1); break;
        case 43: next = h->sound->ngrains - 1; break;
        case 44: {
            int8_t *source = handler_reg(h, a), *destination = handler_reg(h, b);
            if (source && destination) *destination = *source;
            break;
        }
        default: snd_grain_unsupported[type < 45 && type >= 0 ? type : 45]++; break;
        }
        if (h->play_count && --h->play_count == 0) next += h->skip_count;
        h->pc = next;
        if (next >= h->sound->ngrains || next < 0) { h->done = 1; break; }
        h->delay = (int)rd32(handler_grain(h, next) + 4) + extra_delay;
    }
    /* Malformed zero-time loops must not hang an RPC or game frame. */
    if (!h->done && h->delay <= 0) { h->delay = 1; snd_grain_unsupported[45]++; }
}

/* Whether a new copy of `sound` at application volume `volume` may start, as retail's start
   (IOP 0x4A34) decides before it allocates the handler. A solo sound (flag 2, 0x10038) first stops
   every copy already playing. With flag 8 (0xF488 -> 0xF34C) at most instance_limit copies live:
   at the limit the weakest is stopped for the new one if flag 0x10 is set and it is quieter, or
   if flag 0x20 is set (the oldest), else the new play is refused. A victim is stopped with its
   children (0xFE04(h, 1, 0)). The port has no branch grain, so a handler's sound is its first. */
static int handler_admit(const SndBankSound *sound, int volume)
{
    SoundHandler *weakest = NULL;
    int i, count = 0;
    if (sound->flags & 2)
        for (i = 0; i < 128; i++)
            if (handlers[i].used && !handlers[i].stopped && handlers[i].sound == sound) handler_stop(&handlers[i]);
    if (!(sound->flags & 8)) return 1;
    if (sound->instance_limit <= 0) return 0;
    for (i = 0; i < 128; i++) {
        SoundHandler *h = &handlers[i];
        if (!h->used || h->stopped || h->sound != sound) continue;
        count++;
        if (!weakest || ((sound->flags & 0x10) && h->volume < weakest->volume) ||
            ((sound->flags & 0x20) && h->created_at < weakest->created_at)) weakest = h;
    }
    if (count < sound->instance_limit) return 1;
    if (weakest && (((sound->flags & 0x10) && weakest->volume < volume) || (sound->flags & 0x20))) {
        handler_stop(weakest);
        return 1;
    }
    return 0;
}

static SoundHandler *handler_start(BankSlot *bank, const SndBankSound *sound,
                                    int volume, int pan, int pmod, int bend,
                                    SoundHandler *parent, int original_volume, int original_pan)
{
    SoundHandler *h = NULL;
    int i;
    if (!sound->ngrains || (parent && parent->depth >= 8)) return NULL;
    if (!handler_admit(sound, application_volume(1024, volume))) return NULL;
    for (i = 0; i < 128; i++) if (!handlers[i].used) { h = &handlers[i]; break; }
    if (!h) return NULL;
    memset(h, 0, sizeof *h); h->used = 1;
    h->bank = bank; h->sound = sound; h->handle = next_handle++;
    h->created_at = grain_samples;
    h->original_volume = original_volume < 0 ? sound->volume : original_volume;
    h->volume = application_volume(1024, volume);
    h->pan = pan == -1 || pan == -2 ? (original_pan < 0 ? sound->pan : original_pan) : pan;
    h->pmod = pmod; h->pbend = bend;
    if (parent) { h->parent = parent->handle; h->depth = parent->depth + 1; memcpy(h->regs, parent->regs, 4); }
    h->delay = (int)rd32(handler_grain(h, 0) + 4);
    handler_run(h);
    return h;
}

static void handlers_tick(void)
{
    int i, k;
    /* Retail 10FB0/11014 samples ENVX on the 240-Hz driver tick. Four
       consecutive zeros release allocation and the handler's voice bit.
       A DSP endpoint and WAIT_FOR_ALL visibility are distinct events. */
    for (i = 0; i < SND_VOICES; i++) {
        Voice *v = &voices[i];
        if (!v->used || !v->owner || v->paused) continue;
        if (v->env.level) v->zero_env_ticks = 4;
        else if (!--v->zero_env_ticks) v->used = 0;
    }
    for (i = 0; i < 128; i++) {
        SoundHandler *h = &handlers[i];
        if (!h->used || h->paused || h->created_at == grain_samples) continue;
        handler_modulate(h, -1);
        if (!h->done && --h->delay <= 0) handler_run(h);
        if (!h->done || handler_voices(h)) continue;
        for (k = 0; k < 128; k++) if (handlers[k].used && handlers[k].parent == h->handle) break;
        if (k == 128) h->used = 0;
    }
}
