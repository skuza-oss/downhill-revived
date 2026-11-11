/* The test link (puppet_link.h, finding 202): parsing, a perfect link, delay and jitter, loss near
   its rate, outages, the same sequence from the same seed, and the hitch clocks. */
#include "puppet_link.h"
#include <assert.h>
#include <stdio.h>

static int near(double a, double b, double eps) { return fabs(a - b) <= eps; }

int main(void)
{
    PuppetLink l;
    double at;

    assert(!link_parse(&l, NULL) && !link_parse(&l, ""));
    assert(link_parse(&l, "delay=80,jitter=20 loss=2,burst=8:200-500,reorder=1,rate=30,skew=-40,seed=7,hitch=12:600,rhitch=20:300"));
    assert(near(l.delay, 0.08, 1e-12) && near(l.jitter, 0.02, 1e-12) && near(l.loss, 0.02, 1e-12));
    assert(near(l.burst_every, 8, 1e-12) && near(l.burst_min, 0.2, 1e-12) && near(l.burst_max, 0.5, 1e-12));
    assert(near(l.reorder, 0.01, 1e-12) && l.rate == 30 && near(l.skew, -0.04, 1e-12) && l.state == 7);
    assert(near(l.hitch_at, 12, 1e-12) && near(l.hitch_len, 0.6, 1e-12));
    assert(near(l.rhitch_at, 20, 1e-12) && near(l.rhitch_len, 0.3, 1e-12));

    /* a link of only a delay: everything arrives, that much later */
    link_parse(&l, "delay=50");
    for (int i = 0; i < 100; i++) assert(link_pass(&l, i / 60.0, &at) && near(at, i / 60.0 + 0.05, 1e-12));

    /* jitter only adds, at most 6 times its mean; loss close to its rate over 60,000 messages */
    link_parse(&l, "delay=30,jitter=10,loss=5,seed=3");
    int passed = 0;
    double most = 0;
    for (int i = 0; i < 60000; i++)
        if (link_pass(&l, i / 60.0, &at)) {
            passed++;
            assert(at - i / 60.0 >= 0.03 - 1e-12);
            if (at - i / 60.0 > most) most = at - i / 60.0;
        }
    assert(passed > 60000 * 0.94 && passed < 60000 * 0.96);
    assert(most <= 0.03 + 0.06 + 1e-12 && most > 0.06);

    /* outages: about every 4 s, 200-500 ms of nothing */
    link_parse(&l, "burst=4:200-500,seed=11");
    int lost = 0, gaps = 0, run = 0, longest = 0;
    for (int i = 0; i < 60 * 600; i++) {
        if (!link_pass(&l, i / 60.0, &at)) {
            lost++, run++;
        } else {
            if (run) gaps++, longest = run > longest ? run : longest;
            run = 0;
        }
    }
    assert(gaps > 100 && gaps < 200);       /* 600 s / 4 s = 150 */
    assert(longest <= 31 && longest >= 20);  /* at most 500 ms of 60 a second */
    assert(lost > gaps * 12 && lost < gaps * 30);

    /* the same seed, the same fate */
    PuppetLink a, b;
    link_parse(&a, "delay=80,jitter=30,loss=3,reorder=5,burst=5:200-400,seed=42");
    link_parse(&b, "delay=80,jitter=30,loss=3,reorder=5,burst=5:200-400,seed=42");
    for (int i = 0; i < 5000; i++) {
        double x = -1, y = -1;
        assert(link_pass(&a, i / 60.0, &x) == link_pass(&b, i / 60.0, &y) && x == y);
    }

    /* a hitch: stopped 0.6 s at race second 10, then twice real time until caught up at 11.2 */
    assert(link_hitch_wall(-1, 0.6, 5) == 5 && link_hitch_race(-1, 0.6, 5) == 5);
    assert(near(link_hitch_wall(10, 0.6, 9.5), 9.5, 1e-12));
    assert(near(link_hitch_wall(10, 0.6, 10), 10.6, 1e-12));
    assert(near(link_hitch_wall(10, 0.6, 11), 11.1, 1e-12));
    assert(near(link_hitch_wall(10, 0.6, 12), 12, 1e-12));
    assert(near(link_hitch_race(10, 0.6, 10.3), 10, 1e-12));
    assert(near(link_hitch_race(10, 0.6, 11.1), 11, 1e-12));
    assert(near(link_hitch_race(10, 0.6, 13), 13, 1e-12));
    for (double t = 0; t < 20; t += 0.01) assert(near(link_hitch_race(10, 0.6, link_hitch_wall(10, 0.6, t)), t, 1e-9));

    printf("puppet-link: ok\n");
    return 0;
}
