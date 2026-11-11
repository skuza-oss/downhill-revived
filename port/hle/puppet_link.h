/* A bad network on demand, for tests (finding 202): what happens to each snapshot between the PC
   that rides a rider and the PC that shows it, deterministic by a seed. The puppet bench
   (DHD_PUPPET_PLAY with DHD_PUPPET_LINK, port/tools/puppet_bench.py) sends recorded riders
   through it; online, DHD_NET_LINK holds the STATE bundles a game receives the same way.

   A link is a list of NAME=VALUE (comma or space separated), all optional:
     delay=MS        one-way delay (default 0)
     jitter=MS       extra delay, exponential with this mean (capped at 6x), so a few arrive late
     loss=PCT        each message lost with this chance
     burst=S:A-B     about every S seconds (exponential), nothing gets through for A-B ms
     reorder=PCT     this share of messages are held one to three 60 Hz intervals more
     rate=N          snapshots a second the sender sends (60 or 30; default: the online rule,
                     a player's own rider 60, an AI rider 30; the bench only)
     skew=MS         the shared clock is off by this much: a snapshot is placed MS later than it
                     was taken (positive: it looks newer than it is)
     hitch=T:MS      the sender's game stops MS ms at its race second T, then catches up at two
                     fields a step (finding 139): its race clock trails the wall clock meanwhile
     rhitch=T:MS     the same on the receiving game
     seed=N          the random sequence (default 1)
   Pure functions; unit test test_puppet_link.c. */
#ifndef DHD_PUPPET_LINK_H
#define DHD_PUPPET_LINK_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct PuppetLink {
    double delay, jitter, loss, reorder;      /* s, s, 0-1, 0-1 */
    double burst_every, burst_min, burst_max; /* s */
    double skew;                              /* s */
    double hitch_at, hitch_len;               /* sender, s */
    double rhitch_at, rhitch_len;             /* receiver, s */
    int rate;                                 /* 0: the online rule */
    uint64_t state;                           /* the random sequence */
    double burst_from, burst_to;              /* the current or next outage, sender wall s */
    int on;
} PuppetLink;

static inline double link_random(PuppetLink *l)  /* splitmix64, [0, 1) */
{
    uint64_t z = (l->state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (double)(z >> 11) / 9007199254740992.0;
}

/* The next outage after time t (sender wall seconds). */
static inline void link_next_burst(PuppetLink *l, double t)
{
    if (!(l->burst_every > 0)) { l->burst_from = l->burst_to = INFINITY; return; }
    double gap = -log(1 - link_random(l)) * l->burst_every;
    l->burst_from = t + gap;
    l->burst_to = l->burst_from + l->burst_min + (l->burst_max - l->burst_min) * link_random(l);
}

/* Parses a link description (see above). 0: none given (an empty or missing text). */
static inline int link_parse(PuppetLink *l, const char *text)
{
    memset(l, 0, sizeof *l);
    l->hitch_at = l->rhitch_at = -1;
    l->state = 1;
    if (!text || !*text) return 0;
    const char *s = text;
    while (*s) {
        while (*s == ',' || *s == ' ' || *s == ';') s++;
        if (!*s) break;
        char name[16];
        int n = 0;
        while (*s && *s != '=' && *s != ',' && *s != ' ' && n < 15) name[n++] = *s++;
        name[n] = 0;
        if (*s != '=') { while (*s && *s != ',' && *s != ' ') s++; continue; }
        s++;
        char *end;
        double a = strtod(s, &end), b = 0, c = 0;
        s = end;
        if (*s == ':') { b = strtod(s + 1, &end); s = end; }
        if (*s == '-') { c = strtod(s + 1, &end); s = end; }
        if (!strcmp(name, "delay")) l->delay = a / 1000;
        else if (!strcmp(name, "jitter")) l->jitter = a / 1000;
        else if (!strcmp(name, "loss")) l->loss = a / 100;
        else if (!strcmp(name, "reorder")) l->reorder = a / 100;
        else if (!strcmp(name, "rate")) l->rate = (int)a;
        else if (!strcmp(name, "skew")) l->skew = a / 1000;
        else if (!strcmp(name, "seed")) l->state = (uint64_t)a;
        else if (!strcmp(name, "burst")) l->burst_every = a, l->burst_min = b / 1000, l->burst_max = (c > b ? c : b) / 1000;
        else if (!strcmp(name, "hitch")) l->hitch_at = a, l->hitch_len = b / 1000;
        else if (!strcmp(name, "rhitch")) l->rhitch_at = a, l->rhitch_len = b / 1000;
        while (*s && *s != ',' && *s != ' ' && *s != ';') s++;
    }
    l->on = 1;
    l->burst_from = l->burst_to = -INFINITY;
    return 1;
}

/* A game stopped len seconds at race second at, then catching up at two fields a step (twice
   real time): the wall moment its race clock shows race second t, and the race second its clock
   shows at wall moment w. Without a hitch both are the identity. */
static inline double link_hitch_wall(double at, double len, double t)
{
    if (at < 0 || t < at) return t;
    double w = at + len + (t - at) / 2;
    return w > t ? w : t;
}
static inline double link_hitch_race(double at, double len, double w)
{
    if (at < 0 || w < at) return w;
    if (w < at + len) return at;
    double t = at + 2 * (w - at - len);
    return t < w ? t : w;
}

/* A message the sender sends at its wall moment `sent` (seconds): 0 when it is lost, else 1 and
   the wall moment it arrives. Messages must be sent in time order. */
static inline int link_pass(PuppetLink *l, double sent, double *arrives)
{
    if (l->burst_to == -INFINITY) link_next_burst(l, sent);
    while (sent >= l->burst_to) link_next_burst(l, l->burst_to);
    double lost = link_random(l), extra = link_random(l), order = link_random(l), late = link_random(l);
    if (sent >= l->burst_from && sent < l->burst_to) return 0;
    if (lost < l->loss) return 0;
    double d = l->delay;
    if (l->jitter > 0) d += fmin(-log(1 - extra) * l->jitter, 6 * l->jitter);
    if (order < l->reorder) d += (1 + (int)(late * 3)) / 60.0;
    *arrives = sent + d;
    return 1;
}
#endif
