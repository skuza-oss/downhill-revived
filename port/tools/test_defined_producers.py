"""Exercise patched producer objects built by the normal O1/SSE2 port workflow.

Stub only the rider's downstream consumers and helper return modes; execute the
actual four patched production functions. Check all sixteen neutral vector
bytes and every byte of both 48-byte sound payloads under stack poisoning.
"""
from pathlib import Path

import testprog

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/port/defined-producers'
UNITS = ('001306C0', '00201B68', '00201BD0', '00136038')

HARNESS = r'''
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
#define UINT64_C(x) x##ULL
extern void *memcpy(void *,const void *,unsigned);
extern void *memset(void *,int,unsigned);
extern int memcmp(const void *,const void *,unsigned);
extern char *strstr(const char *,const char *);
extern int printf(const char *,...);
extern void abort(void);
#include "port.h"
#include "sound.h"
#include "scalar_api.h"
static unsigned failures, mode, helper_calls, trail_calls, sound_calls, force_mode, force_calls;
static float observed_w, observed_target;
static unsigned char payload[48];
static SoundCompletionCallback observed_cb;
static SoundCallbackData observed_extra;
#define CHECK(c) do { if (!(c)) { if(failures<12)printf("FAIL line%d: %s\n",__LINE__,#c);failures++; } }while(0)
static unsigned char owner[0x7B60] __attribute__((aligned(16)));
static unsigned char state[0xA0] __attribute__((aligned(16)));
static unsigned char other[0x2820] __attribute__((aligned(16)));
static unsigned char item[0x30] __attribute__((aligned(16)));
static float weights[4]={1,2,3,0};
extern void func_001306C0(void *);
unsigned char func_00138560(void *p) { return force_mode ? 0 : mode==0; }
void *func_00138558(void *p) { return item; }
float func_001385A8(void *p) { return 1; }
int func_001304F8(void *a,void *b,void *c,void *d,void *e,void *f,float *out) {
 helper_calls++;
 for(unsigned i=0;i<16;i++) CHECK(((unsigned char*)out)[i]==0);
 if(mode>=2) {out[0]=1;out[1]=2;out[2]=3;}
 if(mode==3) out[3]=4;
 return mode==3;
}
void func_001C2458(struct Scalar001C2458Obj **a,struct Scalar001C2458Obj **b,void *c,void *d,void *e,void *f,int g,int h,float w,float speed,unsigned flags) { trail_calls++;observed_w=w; }
void func_001C7FC8(void *p) { }
void func_001C8188(void *p) { }
float func_00236088(float current,float target,float rate,float minimum) { observed_target=target;CHECK(rate==.4f&&minimum==.2f);return target; }
/* The sole inline-asm call in this producer is its xyz dot product. */
void ps2_asm(const char *text,const char *constraints,int n,ps2_op *ops) {
 if(n==2 && strstr(text,"lq $2")) { memcpy(ops[0].p,*(void**)ops[1].p,16);return; }
 CHECK(n==7);CHECK(strstr(text,"mula.s")&&strstr(text,"madda.s")&&strstr(text,"madd.s"));
 if(n!=7)abort();
 float v=*(float*)ops[1].p**(float*)ops[2].p+*(float*)ops[3].p**(float*)ops[4].p+*(float*)ops[5].p**(float*)ops[6].p;
 *(float*)ops[0].p=v;
}
void func_00202048(int command,int length,void *data,SoundCompletionCallback cb,SoundCallbackData extra) {
 CHECK(command==0x5C&&length==48);memcpy(payload,data,48);observed_cb=cb;observed_extra=extra;sound_calls++;
}

/* Execute the actual gravity-force producer up to its first consumer. The
   full remaining function is linked, but longjmp bounds this focused test. */
#include <setjmp.h>
static jmp_buf force_jump;
static unsigned char body[0x4C0] __attribute__((aligned(16)));
static unsigned char world[0x7BC0] __attribute__((aligned(16)));
float D_0029C2E8=1,D_0029C2EC=1,D_0029C314=1;
float D_0029C370=2,D_0029C380=1,D_0029C384=.3f,D_0029C388=4,D_0029C38C=4;
float D_00768498[3]={1,1,1},D_0026DE20[1]={1};
extern void func_00136038(void *);
void func_00231C10(void *o) { }
void func_00231C20(void *o,float *force) {
 force_calls++;
 CHECK(force[0]==0&&force[1]==0&&force[2]==-64.4f);
 CHECK(((uint32_t*)force)[3]==0);
 CHECK(memcmp((unsigned char*)o+0x100,force,16)==0);
 longjmp(force_jump,1);
}
void func_00231D48(void *o,const void *p) {abort();}
int func_00238F60(void){return 1;}
/* the frame-rate hooks of the patched units (port/hle/fps.c) at 60 fields/s, the retail rate:
   no high-rate path, the retail decay, no force window */
int port_delta_clock(void){return 0;}
float port_decay(float factor){return factor;}
float port_air_age(void *body,int current){abort();}
void port_fps_force_window(void *body,int clear){}
float func_0012C9B8(void *o){return 1;}
float func_0012E028(void *o){return 0;}
#define NOOP(n) void n(void *p) { }
NOOP(func_00138658) NOOP(func_00138808) NOOP(func_00136AA8) NOOP(func_00136D68)
NOOP(func_00136D30) NOOP(func_00136F80) NOOP(func_00137950) NOOP(func_00137B70)
NOOP(func_00137E68) NOOP(func_00136790) NOOP(func_00137C78) NOOP(func_00137DE8)
NOOP(func_00136B40) NOOP(func_00136C20) NOOP(func_001EA9A0) NOOP(func_001EAAD0)
void func_00221C58(float *a,float *b){abort();}
float fabsf(float x){return x<0?-x:x;}

static void callback(int x,SoundCallbackData y) { }
__attribute__((noinline)) static void poison(unsigned byte) {volatile unsigned a[4096];for(unsigned i=0;i<4096;i++)a[i]=byte*0x01010101u;}
static void put32(unsigned char *p,uint32_t value){memcpy(p,&value,4);}
int main(void) {
 for(unsigned seed=0;seed<256;seed++) {
  memset(body,0xCD,sizeof body);memset(world,0,sizeof world);
  *(void**)(body+0x158)=world;*(float*)(body+0x150)=2;*(float*)(body+0x418)=1;
  force_mode=1;force_calls=0;
  if(!setjmp(force_jump)) {poison(seed);func_00136038(body);CHECK(0);}
  force_mode=0;CHECK(force_calls==1);
  for(mode=0;mode<4;mode++) {
   memset(owner,0,sizeof owner);memset(state,0,sizeof state);memset(other,0,sizeof other);memset(item,0,sizeof item);
   *(void**)(owner+0x792C)=state;*(void**)(owner+0x7A44)=other;*(void**)(owner+0x40F8)=weights;state[5]=1;
   helper_calls=trail_calls=0;observed_target=observed_w=-99;poison(seed);func_001306C0(owner);
   CHECK(helper_calls==(mode!=0));CHECK(trail_calls==1);CHECK(observed_w==(mode==3?4:0));
   CHECK(observed_target==(mode>=2?14:0));CHECK(*(float*)(owner+0x7AC4)==observed_target);
  }
  for(unsigned which=0;which<2;which++) for(unsigned named=0;named<2;named++) {
   SoundBankName bank=UINT64_C(0x8172635445362718);SoundName name;unsigned char expected[48]={0};
   unsigned char name_bytes[16];for(unsigned i=0;i<16;i++)name_bytes[i]=(unsigned char)(i*13+1);memcpy(&name,name_bytes,16);
   int id=named?0:0x12345678;
   if(named)memcpy(expected,&bank,8);else put32(expected,id);
   put32(expected+8,0x11121314);put32(expected+12,0x21222324);memcpy(expected+16,name_bytes,16);
   {uint16_t a=0x3132,b=0x4142;memcpy(expected+32,&a,2);memcpy(expected+34,&b,2);}
   put32(expected+40,named?0x3C:0x3D);
   memset(payload,0xCD,sizeof payload);observed_cb=0;observed_extra=0;poison(seed);
   if(which)func_00201BD0(id,bank,name,0x11121314,0x21222324,0x3132,0x4142,callback,UINT64_C(0x123456789ABCDEF0));
   else func_00201B68(id,bank,name,0x11121314,0x21222324,0x3132,0x4142);
   CHECK(memcmp(payload,expected,48)==0);
   CHECK(observed_cb==(which?callback:0));CHECK(observed_extra==(which?UINT64_C(0x123456789ABCDEF0):0));
  }
 }
 CHECK(sound_calls==1024);
 printf("Defined producer tests: %u failures; 256 stack patterns, four rider paths, full force quadword, both sound constructors/id modes\n",failures);
 return failures!=0;
}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    objects = [testprog.game_unit(f'unsorted/{unit}.c', OUT) for unit in UNITS]
    source=OUT/'test.c';source.write_text(HARNESS)
    testprog.run(testprog.guest('defined-producers', [source, *objects], out=OUT,
                                includes=[testprog.OUT/'include', ROOT/'include'], sjlj=True), timeout=30)

if __name__=='__main__':
    main()
