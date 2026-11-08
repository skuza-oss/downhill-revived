/* Real transformed popup manager + button reader, with a deterministic device.
   Independent press intervals are in real-update indices, not legacy fields. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../include/fps.h"
#include "hle.h"
uint64_t kernel_vsync_count;
unsigned int D_0029E10C;
static int rate,field,whole,physical,raw,reads,action_reads;
unsigned char D_0029E2A8[8]={1};
int D_00660D80[8];
unsigned char D_003F52D0[0x134];
static int variable;  /* paced steps that follow a jittery wall clock (finding 139) */
int port_delta_clock(void) { return rate!=60 || variable; }
int port_whole_step(void) { return whole; }
int port_count_down(int v) { return v > 0 && v < whole ? 0 : v - whole; }  /* fps.c's */
double host_wall_seconds(void) { return 0; }
int func_00238F48(void) { return field; }
void func_0021F8E0(void) { raw=physical; if (!raw) D_00660D80[0]=0; reads++; }
int func_002205A0(int i) { return i; }
int func_00220AC0(int i) { return !i; }
int func_002208E8(int axis,int i) { return !i && axis==2 ? 200 : 128; }
int func_00220230(int i) { return !i?raw:0; }
int func_002209A8(int mask,int i) { return 0; }
int func_00220768(int mask,int i) { return 255; }
int func_00220640(int,int,int);
int func_00220618(int mask,int i) { return func_00220640(mask,i,0); }
int func_002201B0(int i) { return !i && raw; }
void func_0021F880(int i) { if (!i) raw=0; }
void func_00190C88(void *o,int k,int enabled,int bit) {
    unsigned *entry=(unsigned *)((char *)o+k*12+4);
    if (enabled) *entry|=1u<<bit; else *entry&=~(1u<<bit);
}
void func_001909A8(void *o);
void func_0017EB00(void *,int,int);
void func_0017EB88(void *o,int i,int v) { action_reads++; }
void func_0017EDE8(void *o,int i,int v) { abort(); }
int main(int argc,char **argv) {
    rate=atoi(argv[1]);
    variable=argc>2;
    union { uint64_t align; unsigned char b[0xC8]; } manager={0};
    unsigned *m=(unsigned *)manager.b;
    unsigned masks[14]={16,64,128,32,16384,4096,8192,32768,8,1,2048,512,1024,256};
    for (int i=0;i<14;i++) m[i*3]=masks[i];
    int edges=0,held=0,oldfield=0;
    double elapsed_fields=0;
    for (int i=1;i<=rate*2;i++) {
        D_0029E10C=i;kernel_vsync_count=i;
        if (variable) {
            double h=60.0/rate+((i/17)%2 ? .15 : 0);
            elapsed_fields+=h>2 ? 2 : h;field=(int)elapsed_fields;
        } else field=i*60/rate;
        whole=field-oldfield;oldfield=field;
        physical=((i>=rate/4 && i<rate/2) || (i>=rate && i<rate+rate/4))?64:0;
        func_001909A8(manager.b);
        unsigned edge=m[1*3+1]&16;
        if (edge) edges++;
        if (m[1*3+2]&16) held++;
        assert(manager.b[0xBA]==200); /* left stick stays continuous */
        int r=reads;
        kernel_vsync_count++; /* an interrupt inside the same game update */
        func_001909A8(manager.b);
        assert(reads==r && (m[1*3+1]&16)==edge); /* shared consumers in one update */
    }
    printf("%d%s: %d edges for 2 presses, %d held updates, %d polls\n",rate,argc>2 ? " variable steps" : "",edges,held,reads);
    assert(edges==2);
    assert(held==rate/2);
    if (port_delta_clock()) assert(reads==rate*2);
    assert(port_input_stamp(123)==(port_delta_clock() ? (int)D_0029E10C : 123));
    union { uint64_t align; unsigned char b[0x440]; } action={0};
    int *lock=(int *)(action.b+0x388);
    lock[5]=20;oldfield=0;
    int updates=(20*rate+59)/60;
    for (int i=1;i<=updates;i++) {
        field=i*60/rate;whole=field-oldfield;oldfield=field;
        func_0017EB00(action.b,5,0);
        assert(lock[5]==20-field && !action_reads);
    }
    assert(field==20 && lock[5]==0);
    func_0017EB00(action.b,5,0);assert(action_reads==1);
    printf("%d: action lock remains 20 legacy fields (%d updates)\n",rate,updates);
    if (variable) {
        lock[5]=20;action_reads=0;oldfield=field=0;elapsed_fields=0;
        int i=0;
        while (field<20) {
            double h=60.0/rate+((++i/7)%2 ? .15 : 0);
            elapsed_fields+=h>2 ? 2 : h;field=(int)elapsed_fields;
            whole=field-oldfield;oldfield=field;
            func_0017EB00(action.b,5,0);
            /* a two-field step from 1 stops at 0, never below it (finding 139) */
            assert(lock[5]==(field<20 ? 20-field : 0) && !action_reads);
        }
        func_0017EB00(action.b,5,0);assert(action_reads==1);
        printf("%d: variable-step action lock expires after 20 fields (%d updates, %d fields)\n",rate,i,field);
    }
    return 0;
}

void pads_begin_field(void) { abort(); }
int pads_multitap(void) { abort(); }
void pads_read(int i,HostPad *p) { abort(); }
int port_controls_capturing(void) { return 0; }  /* controls_settings.c: no binding waits */
int pads_online(void) { return 0; }
int pads_connected(void) { abort(); }
int pads_live(void) { return 0; }
void host_pad_device(int i,HostPad *p) { abort(); }
void host_pad_vibrate(int i,int a,int b) { abort(); }
void rpc_unknown(const char *s,int f,void *a,int n,void *b,int m) { abort(); }
