/* Rate invariance checks against elapsed physical time, not implementation
   snapshots. Compiled with the production host clock and math helpers. */
#include "../include/fps.h"
#include "../host/host_api.h"
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
int main(int argc,char **argv)
{
    int n=atoi(argv[1]);
    /* the core's guest memory: the registers at 0x10000000, PS2 RAM at 0x08000000 */
    host_putenv_s("DHD_FPS",argv[1]);  /* the host environment port_fps reads */
    assert(port_fps()==n);
    if(n==60) {
        assert(port_step()==1 && port_decay(.9f)==.9f && port_blend(.1f)==.1f);
        assert(port_animation_scale()==1 && port_animation_step()==1);
        port_fps_contact_recovery(NULL); /* original path cannot dereference a bike */
        puts("60: original arithmetic preserved"); return 0;
    }
    port_clock_reset();
    *(int *)0x0829DED8=9;*(int *)0x0843F348=2;
    unsigned char __attribute__((aligned(16))) rider[0x7BD0]={0},bike[0x4C0]={0};
    if (argc>2 && !strcmp(argv[2],"wall")) {
        port_fps_wall_mode(1);port_clock_reset();D_0029E3D8=0;
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
            assert(port_step()>=60.0/n-.000001 && port_step()<=1.000001);
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
            assert(fabs(simulated-(elapsed+1.0/n))<.12);
        }
        assert(fabs(simulated-(elapsed+1.0/n))<.0001);
        assert(fabs(D_0029E3D8/60.0-simulated)<1.0/60+.00001);
        assert(fabs(animation/(60.0*1024)-simulated)<.0001);
        /* A freshly initialized throw must not inherit the old scene's dt. */
        fake_wall+=.025;port_clock_wait(NULL);port_clock_step();
        port_ragdoll_step(point,1,sizeof point,1);
        port_clock_reset();port_clock_wait(NULL);port_clock_step();
        point[0]=1.0f/n;point[4]=0;
        float start=point[0];port_ragdoll_step(point,1,sizeof point,1);
        assert(fabs((point[0]-start)-D_0029E400)<.000001);
        /* The mixer's VBLANK time (finding 113): two VBLANKs per update, both at its end, add up
           to wall time, not twice it; a 50-ms hitch is repaid in full at a field per VBLANK, a
           2-s stall by 0.1 s. */
        port_fps_wall_mode(1);
        double audio=0,wall=0;
        for (int i=0;i<4000;i++) {
            double dt=i%3 ? 1.0/550 : 1.0/400;
            fake_wall+=dt,wall+=dt;
            audio+=port_vblank_seconds();
            audio+=port_vblank_seconds();
        }
        assert(fabs(audio-wall)<1.0/n+.003); /* the first VBLANK counts a nominal field */
        for (int k=0;k<2;k++) {
            double stall=k ? 2 : .05;
            fake_wall+=stall;audio=0;
            for (int i=0;i<40;i++) {
                double t=port_vblank_seconds();
                assert(t>=0 && t<=1.0/60+1e-12);
                audio+=t;
            }
            assert(fabs(audio-(k ? .1 : stall))<1e-9);
        }
        port_fps_wall_mode(0);
        assert(port_vblank_seconds()==1.0/n);
        /* A two-second stall: offline at most half a second is repaid, in an online race all of
           it (finding 114). A lobby or start wait (kernel_pace_slip/shared) is no stall at all. */
        for (int pass=0;pass<2;pass++) {
            online=pass;
            assert(port_fps()==n);
            port_fps_wall_mode(1);port_clock_wait(NULL);port_clock_step();
            double before=port_field_seconds(D_0029E3D8);
            fake_wall+=2;
            /* a step repays at most 1 - 60/n fields: 84 a second at 144, one at 61 */
            int steps=(int)((2+130.0/(n-60))*n);
            for (int i=0;i<steps;i++) { fake_wall+=1.0/n;port_clock_wait(NULL);port_clock_step(); }
            double gained=port_field_seconds(D_0029E3D8)-before, want=(double)steps/n+(pass ? 2 : .5);
            printf("%d: %s race, 2 s stall then %.1f s: %.4f s of game time (want %.4f)\n",n,pass ? "online" : "offline",
                   (double)steps/n,gained,want);
            fflush(stdout);
            assert(fabs(gained-want)<1.0/60+.00001);
        }
        fake_wall+=10;port_fps_wall_mode(1); /* kernel's online wait (kernel_pace_slip) */
        online=0;port_clock_wait(NULL);port_clock_step();
        assert(fabs(port_step()-60.0/n)<.000001); /* no double-counted online time */
        printf("%d: paced wall speed %.9f, bounded smoothed steps, timers/animation\n",n,simulated/(elapsed+1.0/n));
        return 0;
    }
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
    assert(airborne==1 && port_air_first(&body,airborne));
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
    int scale=port_animation_scale();
    online=1;
    assert(port_fps()==n && port_fps_high()); /* online keeps the selected rate (finding 114) */
    assert(port_animation_scale()==scale); /* joining online cannot reinterpret timestamps */
    printf("%d: 10 seconds, %d timer ticks, %.6f thrust velocity, particle displacement %.6f / %.6f; online %d\n",
           n,ticks,velocity,particle_displacement,expected,port_fps());
    return 0;
}
