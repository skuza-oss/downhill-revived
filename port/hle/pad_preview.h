/* PC SETTINGS' vibration preview (finding 124), shared by port/hle/host.c (host_pad_vibrate, the
   pads' motors) and port/hle/pc_settings.c (the Vibration row's buzz).

   The game asks for the motor values every field, zeros included while nothing vibrates
   (func_0021F8E0 -> func_00220130 -> scePadSetActDirect -> host_service_host_pad_vibrate), so a
   buzz would be cancelled by the game's own values the field after it starts. A preview therefore
   wins for PAD_PREVIEW_SECONDS, after which the game's values apply again; switching Vibration off
   always stops at once. The preview travels in the port argument host_pad_vibrate takes, as
   PAD_PREVIEW_BIT | player: a bit no player index uses (players are 0-3). Output only: nothing
   comes back into the game. */
#ifndef DHD_PAD_PREVIEW_H
#define DHD_PAD_PREVIEW_H

enum { PAD_PREVIEW_BIT = 0x10 };   /* in host_pad_vibrate's port: the page's buzz for that player */
enum { PAD_PREVIEW_FIELDS = 18 };  /* pc_settings.c runs the buzz this many page fields: about 0.3 s */
#define PAD_PREVIEW_SECONDS 0.3    /* how long a preview wins over the game's own motor values */

/* One pad's preview state (host_pad_vibrate keeps one per player). */
typedef struct {
    int last;      /* the value published last, (light ? 256 : 0) | heavy; -1: none yet */
    double until;  /* wall time the preview wins until; 0: no preview */
} PadPreview;

/* host_pad_vibrate's decision for one player. light/heavy: what the caller asked for; on: the
   vibration setting is on (DHD_VIBRATION); preview: this call is the page's (PAD_PREVIEW_BIT);
   now: the wall clock. Returns 1 with *out_light / *out_heavy to publish (they differ from what
   the pad already has), 0 to leave the pad as it is. */
static inline int pad_preview_step(PadPreview *p, int light, int heavy, int on, int preview, double now,
                                   int *out_light, int *out_heavy)
{
    if (preview) p->until = on ? now + PAD_PREVIEW_SECONDS : 0;
    else if (on && p->until != 0 && now < p->until) return 0;  /* the game's own values wait */
    if (!on) light = heavy = 0;                                /* off: silence, at once */
    int key = (light ? 256 : 0) | (heavy & 255);
    if (key == p->last) return 0;                              /* nothing changed: nothing to send */
    p->last = key;
    *out_light = light, *out_heavy = heavy;
    return 1;
}

#endif
