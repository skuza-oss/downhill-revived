/* The race's field grid and its replays (finding 141): fps.c's clock and replay.c's recorder,
   with a stand-in race. The race is scripted as the game runs it: each frame the race update
   (func_0019EE98's phase, then func_001A8DE8), then the clock's wait, func_001A9640 and the
   clock's step. Its "physics" moves the player's z by each update's step times the pad it sees,
   so a replay that does not repeat every step and every input exactly ends somewhere else.

   Checked at the rate given (argv[1], fixed steps or "wall" with jittered frames):
   - a race records one sample per field of race clock (60 a race second at every rate);
   - its replay, at another selected rate and with other frame times, gives every update the
     recorded step and input, plays one sample per field, passes every z check and ends at the
     recording's end;
   - a replay's pictures are held for the recorded steps;
   - the time trial's ghost (00138E08 as patched) samples every 8 fields of race clock at every
     rate. */
#include "fps.c"
#include "replay.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int D_0029E3D0=1,D_0029E3D4=60,D_0029E3D8,D_0029E3DC;
int D_0029E3E0,D_0029E3E8;
float D_0029E3FC=60,D_0029E400=1.0f/60;
char D_004B1F40[16], D_0028E888[16];
int pads_online(void) { return 0; }
int func_00243CE8(int ignored) { (void)ignored; return 0; }
void hle_log(const char *fmt,...) { (void)fmt; }
static double fake_wall=1;
double host_wall_seconds(void) { return fake_wall; }
int host_display_hz(void) { return 0; }
void func_00155E78(void *p,int kind) { (void)p;(void)kind; }
float func_0012DF48(void *p) { (void)p;return 0; }
uint64_t snd_test_position(void) { return 0; }

/* The stand-in pad and race. */
static int live_buttons, seen_buttons, overridden, stopped;
short func_00220268(int pad) { (void)pad; return (short)live_buttons; }
void func_00220280(int pad, unsigned char *a, unsigned char *b, unsigned char *c, unsigned char *d)
{
    (void)pad; *a=(unsigned char)live_buttons; *b=*c=*d=0x80;
}
void func_002202B8(int pad, short buttons) { (void)pad; seen_buttons=buttons; overridden=1; }
void func_002202D0(int pad, int a, int b, int c, int d) { (void)pad;(void)a;(void)b;(void)c;(void)d; }
void func_001A9568(void *rec) { (void)rec; }
void func_001A9598(void *rec, int camera) { (void)rec;(void)camera; }
static int director;
void func_001A9230(void *rec) { (void)rec; director++; }
void func_001A9EA8(void *rec) { (void)rec; }
void func_001AA0F0(void) {}
void func_0015C920(void) {}
void func_001B45F8(void *menu, int which, int flag) { (void)menu;(void)which;(void)flag; stopped++; }
void func_0020F588(void *u, int i, int r, int g, int b, int a) { (void)u;(void)i;(void)r;(void)g;(void)b;(void)a; }
void func_0020FB80(int i, int x, int y, int w, int h) { (void)i;(void)x;(void)y;(void)w;(void)h; }
void func_0020F9F0(int i, int f, int x, int y, int t) { (void)i;(void)f;(void)x;(void)y;(void)t; }

#define PLAYER_Z (*(float *)GUEST(0x09400018))
/* func_001A8EF0, retail: the sample at count, a two-field frame's step in the previous one, the
   player's z every 60 samples. */
void func_001A8EF0(void *r)
{
    Recorder *rec=r;
    Sample *s=SAMPLE(rec,rec->index);
    s->buttons=func_00220268(0);
    func_00220280(0,&s->stick[0],&s->stick[1],&s->stick[2],&s->stick[3]);
    s->step=1;
    if (D_0029E3D0>=2 && rec->index>0) SAMPLE(rec,rec->index-1)->step=(uint8_t)D_0029E3D0;
    if (rec->index-rec->checked>60) {
        CHECK_Z(rec,rec->index/60)=PLAYER_Z;
        rec->checked=rec->index;
    }
}

enum { UPDATES=400000 };
static float log_step[UPDATES];
static int log_buttons[UPDATES], log_whole[UPDATES];
static uint32_t frame_seed;

/* One frame's wall time: the selected rate's period, or (wall) uneven frames of 1 to 1.6 periods
   (pacing never makes them quicker than the rate). */
static double frame_time(int wall, double period)
{
    if (!wall) return period;
    frame_seed=frame_seed*1664525u+1013904223u;
    double r=(double)(frame_seed>>8)/(1<<24);
    double t=period*(1+0.6*r);
    return t<1.0/30 ? t : 1.0/30;  /* never over two fields: the race clock then keeps wall time */
}

/* One race from the load: 30 frames loading (phase 5), the countdown to 261 fields, then
   `seconds` of race clock. Returns the updates from GO; fills the log (recording) or checks
   against it (replay). */
static int race(Recorder *rec, int wall, int seconds, int replay, double *race_seconds)
{
    int phase=5, timer=0, frames=0, updates=0, go_field=0;
    double period=1.0/port_fps(), go_wall=0;
    float z=0;
    port_clock_reset();
    D_0029E3D8=0;
    PLAYER_Z=0;
    stopped=0;
    for (;;) {
        /* the race update: func_0019EE98 (the phase and the countdown), then func_001A8DE8 */
        overridden=0;
        port_replay_race_phase(phase);
        if (phase==5 && ++frames>=30) phase=1, timer=0;
        else if (phase==1) {
            timer+=D_0029E3D0;
            if (timer>=261) {
                /* GO: func_001A8A10 starts the recording or the replay again */
                phase=2;
                rec->index=0;
                rec->checked=-1000;
                go_field=D_0029E3D8-D_0029E3D0;
                go_wall=fake_wall;
            }
        }
        *(int *)GUEST(0x0843F348)=phase;
        if (phase==2) {
            live_buttons=(int)(fake_wall*13)&0xFF;
            seen_buttons=live_buttons;
            if (!port_replay_race(rec)) break;  /* the replay stopped */
            int seen=overridden ? seen_buttons : live_buttons;
            if (replay) {
                assert(updates<UPDATES);
                if (log_step[updates]!=port_step() || log_buttons[updates]!=seen || log_whole[updates]!=D_0029E3D0) {
                    printf("update %d differs: step %.6f/%.6f buttons %d/%d whole %d/%d\n",updates,
                           log_step[updates],port_step(),log_buttons[updates],seen,log_whole[updates],D_0029E3D0);
                    assert(0);
                }
            } else {
                assert(updates<UPDATES);
                log_step[updates]=port_step(), log_buttons[updates]=seen, log_whole[updates]=D_0029E3D0;
            }
            /* the stand-in physics: z moves by the step times the input */
            z+=port_step()*(1+(seen&7))*0.37f;
            PLAYER_Z=z;
            updates++;
            if (!replay && D_0029E3D8-go_field>=seconds*60) break;
        } else {
            assert(port_replay_race(rec)==1);
        }
        /* the picture, the clock's wait, func_001A9640, the clock's step */
        if (replay && phase==2 && rec->index>0) {
            /* a replay holds its pictures for the recorded steps (the step just forced) */
            assert(fabs(port_frame_period()-port_step()/60.0)<1e-7);
        }
        if (phase==1) assert(port_frame_period()==1.0/60 && port_step()==1);
        fake_wall+=frame_time(wall,port_frame_period()<period ? period : port_frame_period());
        port_clock_wait(NULL);
        port_replay_frame(rec);
        port_clock_step();
    }
    *race_seconds=fake_wall-go_wall;
    return updates;
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    int n=atoi(argv[1]), wall=argc>2 && !strcmp(argv[2],"wall");
    host_putenv_s("DHD_FPS",argv[1]);
    if (n==60 && !wall) host_putenv_s("DHD_STEP_CLOCK","1");  /* 60 on the delta-time paths */
    assert(port_fps()==n);
    port_clock_wall(wall);
    assert(port_delta_clock());
    /* the recorder and its buffers in guest memory, as func_001A8958 sets them up */
    Recorder *rec=GUEST(0x09000000);
    memset(rec,0,sizeof *rec);
    rec->samples=0x09100000, rec->header=0x09200000, rec->mode=3, rec->cameras=1;
    *(uint32_t *)GUEST(0x082AF2C0+0x50130)=0x09300000;
    *(uint32_t *)GUEST(0x09300000+0x792C)=0x09400000;
    *(int *)GUEST(0x084B26AC)=4;  /* a race, not the attract demo */

    /* Record 60 s of race clock: one sample per field. */
    double seconds;
    director=0;
    int updates=race(rec,wall,60,0,&seconds);
    int recorded=rec->index;
    assert(recorded==3600 || recorded==3601);  /* the last update can end two fields */
    assert(director==recorded);  /* the replay camera director ran once a field */
    printf("%d%s: 60 s of race clock in %.3f s wall, %d updates, %d samples (%.2f a race second)\n",
           n,wall ? " paced" : "",seconds,updates,recorded,recorded/60.0);
    if (wall) assert(fabs(seconds-60)<0.1);  /* the race clock keeps wall time */
    /* every code is a step the clock can make, and the codes change only when a field ends */
    for (int i=0;i<recorded-1;i++) assert(SAMPLE(rec,i)->step>=30 && SAMPLE(rec,i)->step<=240);
    int finish=RECORDED(rec);

    /* Replay it at another selected rate (frames at that rate's period, or uneven ones): the
       same steps and inputs, update for update. */
    int other=n==144 ? 45 : 144;
    requested=other;
    rec->playing=1, rec->mode=0;
    frame_seed=0x5EED;
    int played=race(rec,1,0,1,&seconds);
    assert(stopped==1);                       /* stopped once, at the end of the recording */
    assert(rec->index>=finish && rec->index<=finish+1);
    assert(PLAYER_Z!=0);
    int fields=0;
    for (int i=0;i<played;i++) fields+=log_whole[i];
    assert(rec->index==fields);               /* one sample per field */
    printf("%d%s: replayed at %d (frames of other lengths): %d updates, %d samples, every step, input and z check the same\n",
           n,wall ? " paced" : "",other,played,rec->index);
    requested=n;
    rec->playing=0, rec->mode=3;

    /* The time trial's ghost (00138E08): x316ec += whole fields; at 8 a sample, keeping the rest. */
    {
        int count=0, samples=0, retail=0, retail_samples=0;
        port_clock_reset();
        D_0029E3D8=0;
        port_clock_grid(GRID_RECORD);
        for (int i=0;i<10*n;i++) {
            fake_wall+=frame_time(wall,1.0/n);
            port_clock_grid(GRID_RECORD);
            port_clock_wait(NULL);port_clock_step();
            count+=port_whole_step();
            if (count>=8) count-=8, samples++;
            retail+=port_whole_step();
            if (retail>=8) retail=0, retail_samples++;
        }
        int fields=D_0029E3D8;
        assert(samples==fields/8);
        printf("%d%s: ghost: %d fields, %d samples (retail's count from 0: %d)\n",
               n,wall ? " paced" : "",fields,samples,retail_samples);
    }
    return 0;
}
