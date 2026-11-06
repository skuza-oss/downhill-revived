/* Rate invariance checks against elapsed physical time, not implementation
   snapshots. Compiled with the production host clock and math helpers. */
#include "../include/fps.h"
#include "../host/host_api.h"
#include "pace_grid.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int D_0029E3D0=1,D_0029E3D4=60,D_0029E3D8,D_0029E3DC;
int D_0029E3E0,D_0029E3E8;
float D_0029E3FC=60,D_0029E400=1.0f/60;
static int online,waits,recoveries,recovery_kind,braking;
int pads_online(void) { return online; }
int func_00243CE8(int ignored) { (void)ignored; waits++; return 0; }
void hle_log(const char *fmt,...) { (void)fmt; }
static double fake_wall=1;
double host_wall_seconds(void) { return fake_wall; }
int host_display_hz(void) { return 0; }  /* DHD_FPS=monitor is not checked here */
void func_00155E78(void *p,int kind) { (void)p;recoveries++;recovery_kind=kind; }
int func_0012D300(void *p) { (void)p;return 0; }
float func_0012DF48(void *p) { (void)p;return (float)braking; }
uint64_t snd_test_position(void) { return 0; }
/* The paced frames' grid (pace_grid.h: pace() and the slow stretch), on a fake clock: frames take
   `work` seconds and wait for their due time. A change of period (load to GO, a replay's codes)
   waits at most one new period; the grid it had is kept, not restarted. */
static double grid_frames(PaceGrid *grid,double *now,double period,int frames,double work,double *longest)
{
    double due=0;
    for (int i=0;i<frames;i++) {
        due=pace_grid_next(grid,*now,period);
        if (due-*now>*longest) *longest=due-*now;
        if (due>*now) *now=due;
        *now+=work;
    }
    return due;
}
static void pace_grid_checks(void)
{
    PaceGrid grid={0};
    double now=10,longest=0;
    grid_frames(&grid,&now,1.0/144,1000,.002,&longest);
    longest=0;
    grid_frames(&grid,&now,1.0/60,1,.002,&longest);
    assert(longest<=1.0/60+1e-9);  /* was 1000 x (1/60 - 1/144) = 9.7 s */
    grid_frames(&grid,&now,1.0/60,100,.002,&longest);
    /* a replay: 200 frames at code 120, then 150 (1/7200 s each) */
    longest=0;
    grid_frames(&grid,&now,120/7200.0,200,.002,&longest);
    grid_frames(&grid,&now,150/7200.0,50,.002,&longest);
    assert(longest<=150/7200.0+1e-9);  /* was 0.83 s */
    /* a shorter period keeps the grid: the next frame is due one new period on, not a restart */
    double before=grid_frames(&grid,&now,1.0/60,10,.002,&longest);
    double after=grid_frames(&grid,&now,1.0/144,1,.002,&longest);
    assert(after && fabs(after-before-1.0/144)<1e-9);
    /* quick frames repay a slow one on the grid; more than two periods behind starts again */
    before=grid_frames(&grid,&now,1.0/60,1,.002,&longest);
    now+=1.5/60;
    after=grid_frames(&grid,&now,1.0/60,1,.002,&longest);
    assert(fabs(after-before-1.0/60)<1e-9);
    now+=3.0/60;
    assert(pace_grid_next(&grid,now,1.0/60)==0 && grid.base==now);
}
int main(int argc,char **argv)
{
    pace_grid_checks();
    int n=atoi(argv[1]);
    /* the core's guest memory: the registers at 0x10000000, PS2 RAM at 0x08000000 */
    host_putenv_s("DHD_FPS",argv[1]);  /* the host environment port_fps reads */
    assert(port_fps()==n);
    int wall=argc>2 && !strcmp(argv[2],"wall");
    if(n==60 && !wall) {
        assert(!port_delta_clock());
        assert(port_step()==1 && port_decay(.9f)==.9f && port_blend(.1f)==.1f);
        assert(port_animation_scale()==1 && port_animation_step()==1);
        port_fps_contact_recovery(NULL); /* original path cannot dereference a bike */
        puts("60: original arithmetic preserved"); return 0;
    }
    if (wall) port_clock_wall(1);
    assert(port_delta_clock() && port_animation_scale()==1024);
    port_clock_reset();
    port_loop_frame(1);*(int *)0x0829DED8=9;*(int *)0x0843F348=2;   /* a race's loop runs (fps.h) */
    unsigned char __attribute__((aligned(16))) rider[0x7BD0]={0},bike[0x4C0]={0};
    if (wall) {
        /* The delta-time clock (finding 139): each step is the wall time since the last frame,
           smoothed, between 60/n fields and two fields. */
        const double least=60.0/n;
        port_clock_reset();D_0029E3D8=0;
        double simulated=0,elapsed=0; int animation=0;
        float point[12]={1.0f/n,0,0,0, 0,0,0,0, 0,0,0,0};
        float accelerated[12]={0};double acceleration_time=0;
        for (int i=0;i<6000;i++) {
            double dt=1.0/n;
            /* Alternating jitter, sustained missed deadlines, one 50-ms hitch,
               then recovery to the selected rate. */
            if (i>=500 && i<2500) dt=fmax(1.0/n,.012)+(i%2 ? .0007 : -.0007);
            if (i==3000) dt=.05;
            if (i) elapsed+=dt,fake_wall+=dt;
            port_clock_wait(NULL);port_clock_step();
            assert(port_step()>=least-.000001 && port_step()<=2.000001);
            float before=point[0];
            port_ragdoll_step(point,1,sizeof point,1);
            assert(fabs((point[0]-before)-D_0029E400)<.00001);
            /* Recenter both endpoints to keep float position quantization
               from swamping a velocity check after a hundred seconds. */
            point[0]-=point[4];point[4]=0;
            if (i>=500 && i<540) {
                double dt=D_0029E400;
                if (i==500) accelerated[4]=(float)(15*dt*dt),accelerated[8]=30;
                float old=accelerated[0];
                port_ragdoll_step(accelerated,1,sizeof accelerated,1);
                double expected=30*acceleration_time*dt+15*dt*dt;
                assert(fabs((accelerated[0]-old)-expected)<.00001);
                accelerated[0]-=accelerated[4];accelerated[4]=0;
                acceleration_time+=dt;
            }
            simulated+=D_0029E400;
            animation+=port_animation_step();
            /* time is conserved: never more than about two frames apart */
            assert(fabs(simulated-(elapsed+1.0/n))<2.0/30+.00001);
        }
        assert(fabs(simulated-(elapsed+1.0/n))<least/60+.00001);
        assert(fabs(D_0029E3D8/60.0-simulated)<1.0/60+.00001);
        assert(fabs(animation/(60.0*1024)-simulated)<.0001);
        printf("%d: paced wall speed %.9f over %.1f s with jitter and a hitch\n",n,simulated/(elapsed+1.0/n),elapsed);
        /* Jitter reaches the steps at about a third of its size: 9 / 17 ms frames alternating
           (above the rate's own period, or the selected period +-4 ms where that is longer). */
        {
            double a=fmax(.009,1.0/n),b=a+.008,lo=9,hi=0;
            port_clock_rebase();
            for (int i=0;i<200;i++) {
                fake_wall+=i%2 ? a : b;
                port_clock_wait(NULL);port_clock_step();
                if (i>=100) lo=fmin(lo,port_step()),hi=fmax(hi,port_step());
            }
            double range=(hi-lo)/60;
            printf("%d: frames alternating %.1f / %.1f ms give steps %.2f-%.2f ms\n",n,a*1000,b*1000,lo/.06,hi/.06);
            if (b*60<=2) assert(range<=(b-a)*.34+.00001);
        }
        /* A hitch is spread over the next frames, not leapt and not dragged out: the time owed
           is given back within ten frames, never more than two fields a step. */
        {
            port_clock_rebase();
            double game=0,wall=0;
            for (int i=0;i<10;i++) { fake_wall+=1.0/n,wall+=1.0/n;port_clock_wait(NULL);port_clock_step();game+=port_step(); }
            fake_wall+=.05,wall+=.05;
            for (int i=0;i<10;i++) {
                if (i) fake_wall+=1.0/n,wall+=1.0/n;
                port_clock_wait(NULL);port_clock_step();game+=port_step();
                assert(port_step()<=2.000001);
            }
            /* the frames before the first count no time (a rebase) */
            assert(fabs(game/60-wall)<(n>=60 ? 1.0/n : .05-.0333+1.0/n)+.0005);
        }
        /* Below 30 a second: offline the steps stop at two fields and the game slows down (25 a
           second runs at 30/25 = 0.83), with no debt to pay back; online the race keeps the time
           and pays it back at up to two fields a step once the frames are quicker (finding 114). */
        for (int pass=0;pass<2;pass++) {
            online=pass;
            port_clock_rebase();port_clock_wait(NULL);port_clock_step();
            double game=0,wall=0;
            for (int i=0;i<250;i++) { fake_wall+=.04,wall+=.04;port_clock_wait(NULL);port_clock_step();game+=port_step()/60; }
            double slow=game/wall, after=0, owed_seconds=port_fps_behind();
            if (!pass) assert(fabs(slow-.0333/.04)<.01 && port_fps_behind()<=2.0/60+1e-9);
            else assert(fabs(port_fps_behind()-(wall-game))<1e-5 && port_fps_behind()>1.5);  /* port_step is a float */
            int steps=0;
            for (;steps<20*n && (steps<n || port_fps_behind()>1.0/60);steps++) {
                fake_wall+=1.0/n,wall+=1.0/n;port_clock_wait(NULL);port_clock_step();game+=port_step()/60;after+=port_step()/60;
                assert(port_step()<=2.000001);
            }
            printf("%d: %s, 10 s at 25 fps: game speed %.3f, owed %.3f s; then %d steps at %d: %.3f s of game time in %.3f s\n",
                   n,pass ? "online race" : "offline",slow,owed_seconds,steps,n,after,(double)steps/n);
            fflush(stdout);
            if (!pass) assert(after<(double)steps/n+4.0/60);              /* no fast-forward */
            else if (n>30) assert(fabs(game-wall)<2.0/60);                /* online: caught up */
        }
        online=0;
        /* The mixer's VBLANK time (finding 139): two VBLANKs per update, both at its end, add up
           to wall time, not twice it; a 50-ms hitch is repaid in full at two fields per VBLANK, a
           2-s stall by 0.1 s. */
        port_clock_rebase();
        double audio=0,audio_wall=0;
        for (int i=0;i<4000;i++) {
            double dt=i%3 ? 1.0/550 : 1.0/400;
            fake_wall+=dt,audio_wall+=dt;
            audio+=port_vblank_seconds();
            audio+=port_vblank_seconds();
        }
        assert(fabs(audio-audio_wall)<1.0/n+.003); /* the first VBLANK counts a nominal field */
        for (int k=0;k<2;k++) {
            double stall=k ? 2 : .05;
            fake_wall+=stall;audio=0;
            for (int i=0;i<40;i++) {
                double t=port_vblank_seconds();
                assert(t>=0 && t<=2.0/60+1e-12);
                audio+=t;
            }
            assert(fabs(audio-(k ? .1 : stall))<1e-9);
        }
        /* The rate's readers (1/func_00238F30: the integrator, the contact solver) take this
           step's own seconds: frames paced on the selected period (59.94 Hz at 60), each 10-200 us
           late as pace() leaves them. The integer rate, truncated, was 59 at 60 (1.6 % too long a
           step); rounded it is the rate. */
        {
            const double period=n==60 ? 1001.0/60000 : 1.0/n;
            double read=0,stepped=0;
            unsigned seed=12345;
            port_clock_rebase();
            for (int i=0;i<3000;i++) {
                seed=seed*1103515245u+12345u;
                fake_wall+=period+(10+(seed>>16)%191)*1e-6;
                port_clock_wait(NULL);port_clock_step();
                assert(fabs(port_step_seconds()/D_0029E400-1)<.001);
                assert(fabs(1/port_step_rate()/D_0029E400-1)<.001);
                assert(D_0029E3D4==(int)lround(1/D_0029E400));
                if (i>=10) read+=port_step_seconds(),stepped+=D_0029E400;
            }
            assert(fabs(read/stepped-1)<.001);
            if (n==60) assert(D_0029E3D4==60);
            printf("%d: paced rate readers: dt / step %.6f, integer rate %d\n",n,read/stepped,D_0029E3D4);
        }
        /* A lobby or start wait (kernel_pace_slip/shared) is no game time at all. */
        fake_wall+=10;port_clock_rebase();
        port_clock_wait(NULL);port_clock_step();
        assert(fabs(port_step()-least)<.000001);
        port_clock_wall(0);
        assert(port_vblank_seconds()==1.0/n);
        return 0;
    }
    *(int *)0x0829DED8=22;   /* a career continued from a save races on screen 22 (finding 159) */
    *(void **)(bike+0x158)=rider;bike[0x354]=bike[0x3D4]=1;
    *(int *)(rider+0x7A5C)=1;
    *(float *)(rider+0x7A78)=1;
    for (int i=0;i<n*3;i++) {
        *(float *)(bike+0x50)=2.0f*i/n;
        port_fps_contact_recovery(bike);
    }
    assert(!recoveries); /* slow actual progress must remain playable */
    port_clock_reset();
    for (int i=0;i<n*2;i++) {
        *(float *)(bike+0x50)=i&1 ? .05f : -.05f;
        *(float *)(bike+0xC0)=i&1 ? 10.0f : -10.0f;
        port_fps_contact_recovery(bike);
        assert(recoveries==(i==n*2-1)); /* velocity jitter cannot reset immobility */
    }
    assert(recovery_kind==2);
    port_clock_reset();recoveries=0;port_loop_frame(0);
    for (int i=0;i<n*3;i++) {
        *(float *)(bike+0x50)=i&1 ? .05f : -.05f;
        port_fps_contact_recovery(bike);
    }
    assert(!recoveries); /* the menus after a race: the phase is left at 2-4, the front end runs */
    port_loop_frame(1);
    port_clock_reset();recoveries=0;braking=1;
    for (int i=0;i<n*3;i++) port_fps_contact_recovery(bike);
    assert(!recoveries); /* holding the brake is intentional */
    braking=0;port_clock_reset();*(int *)(rider+0x7A5C)=3;
    for (int i=0;i<n*3;i++) port_fps_contact_recovery(bike);
    assert(!recoveries); /* AI retains its separate retail recovery timers */
    *(int *)(rider+0x7A5C)=1;
    port_clock_reset();
    int airborne=0;
    char body;
    for (int i=1;i<=n;i++) {
        airborne=port_air_tick(&body,airborne);
        assert(port_air_first(&body,airborne)==(i==1));
        /* An integer >1 timer first fires after two elapsed fields. */
        if ((double)i*60/n<1.99) assert(airborne==1);
        if ((double)i*60/n>2.01) assert(airborne>1);
    }
    airborne=port_air_tick(&body,0);
    /* the take-off step counts at most one field, as retail's first airborne frame (finding 139) */
    assert(airborne==1 && port_air_first(&body,airborne));
    if (port_step()>1) assert(port_air_tick(&body,airborne)==(int)(1+port_step()));
    const float life_rates[]={.013f,.1f,1.0f/120,.037f};
    for (int i=0;i<4;i++) {
        float age=0;int legacy_fields=1,updates=0;
        while (age<1) { age+=life_rates[i];legacy_fields++; }
        age=0;
        do {
            updates++;
            if (port_particle_dead(age,life_rates[i])) break;
            age+=life_rates[i]*port_step();
        } while (updates<n*10);
        assert(fabs((double)updates/n-(double)legacy_fields/60)<=1.0/n+.000001);
    }
    float point[12]={1,2,3,7, 0,0,0,99, 0,0,0,0};
    port_ragdoll_step(point,1,sizeof point,.99f);
    assert(point[4]==1 && point[5]==2 && point[6]==3 && point[7]==7 && point[3]==7);
    /* Force clears apply to intervals ending at a legacy boundary too.
       Integer arithmetic defines the interval's field without float rounding:
       ceil(end_field)-1 = floor((step*60-1)/hz). */
    port_clock_reset();D_0029E3D8=0;
    int previous_field=-1;
    float *test_force=(float *)(bike+0xE0);
    for (int step=1;step<=n;step++) {
        port_clock_wait(NULL);port_clock_step();
        int field=(step*60-1)/n;
        for (int k=0;k<8;k++) test_force[k]=2;
        port_fps_force_window(bike,0);
        assert(test_force[0]==(field==previous_field ? 0 : 2));
        if (field!=previous_field) port_fps_force_window(bike,1);
        previous_field=field;
    }
    port_clock_reset();D_0029E3D8=0;waits=0;
    float velocity=0,damped=1,blend=0;
    int ticks=0,animation=0;
    for(int i=0;i<n*10;i++) {
        port_clock_wait(NULL);port_clock_step();
        ticks+=port_whole_step();animation+=port_animation_step();
        /* Constant thrust: mass cancels, 30 units/s², not per-step impulse. */
        velocity+=30.0f*D_0029E400;
        assert(port_step_seconds()==D_0029E400 && port_step_rate()==D_0029E3FC);
        damped*=port_decay(.99f);
        blend+=(1-blend)*port_blend(.01f);
    }
    assert(waits==n*10 && ticks==600 && D_0029E3D8==600);
    assert(animation==600*1024 && port_animation_fields((float)animation)==600);
    assert(fabsf(velocity-300)<.02f);
    assert(fabsf(damped-powf(.99f,600))<.00001f);
    assert(fabsf(blend-(1-powf(.99f,600)))<.00001f);
    /* Fractional particle displacement composes to the original field step. */
    float q=.9f, v=1,p=0;
    for(int i=0;i<n;i++) { p+=v*port_particle_sum(q);v*=port_decay(q); }
    float expected=q*(1-powf(q,60))/(1-q);
    assert(fabsf(p-expected)<.0001f);
    float particle_displacement=p;
    q=.999999f;v=1;p=0;
    for(int i=0;i<n;i++) { p+=v*port_particle_sum(q);v*=port_decay(q); }
    assert(fabsf(p-60)<.01f); /* catches cancellation in the fractional sum */
    float c=cosf(.1f),s=sinf(.1f),x=1,y=0;
    port_particle_rotation(&c,&s);
    for(int i=0;i<n;i++) { float a=x*c-y*s;y=x*s+y*c;x=a; }
    assert(fabsf(x-cosf(6))<.0001f && fabsf(y-sinf(6))<.0001f);
    /* Two-field steps (finding 139): a countdown stops at 0 instead of jumping past it, and an
       up-counter reaches an exact mark once, on the update that passes it. */
    {
        int down=21,up=0,reached=0,negative=0;
        for (int i=0;i<n*3;i++) {
            port_clock_wait(NULL);port_clock_step();
            if (down>0) down=port_count_down(down);
            negative|=down<0;
            up+=port_whole_step();
            reached+=port_count_reached(up,0x79);
        }
        assert(down==0 && !negative && up==180 && reached==1);
    }
    /* The item list's 300-field timer (00176ED8 x1310, patches_fps.py): 0017C638 runs before the
       update's whole fields come off it, clears the flag at 1 and starts x1318 at 60, each once
       at any step; x1318 then keeps retail's relation to x1310 (one more after the update). It
       comes off with port_count_down, so it rests at 0 as retail's (whose per-field call is inside
       x1310 > 0), never below: a two-field step from 1 (from 301, odd) ends at 0, not -1. */
    for (int start=300; start<=301; start++) {
        int timer=start,copy=0,cleared=0,marks=0;
        while (timer>0) {
            port_clock_wait(NULL);port_clock_step();
            if (port_count_reached(timer,1)) cleared++;
            if (copy>0) copy=port_count_down(copy);
            if (port_count_reached(timer,60)) { marks++; copy=timer-port_whole_step()+1; }
            timer=port_count_down(timer);
            if (marks && timer>0) assert(copy==timer+1);
        }
        assert(timer==0 && cleared==1 && marks==1);
    }
    int scale=port_animation_scale();
    online=1;
    assert(port_fps()==n && port_delta_clock()); /* online keeps the selected rate (finding 114) */
    assert(port_animation_scale()==scale); /* joining online cannot reinterpret timestamps */
    printf("%d: 10 seconds, %d timer ticks, %.6f thrust velocity, particle displacement %.6f / %.6f; online %d\n",
           n,ticks,velocity,particle_displacement,expected,port_fps());
    return 0;
}
