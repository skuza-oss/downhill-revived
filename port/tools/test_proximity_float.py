"""Execute the actual proximity update when its distance output is a sentinel."""
from pathlib import Path
import re
import sys
import testprog
ROOT=Path(__file__).resolve().parents[2]


def main():
    sys.path.insert(0,str(ROOT/'port'))
    import patches
    code=(ROOT/'src/unsorted/0011EDA8.c').read_text()
    for kind,old,new in patches.PATCHES['unsorted/0011EDA8.c']:
        if kind=='re':  # the frame-rate gate on its ambient event roll (patches_fps.py)
            code,n=re.subn(old[0],new,code);assert n==old[1]
            continue
        assert kind=='text' and code.count(old)==1
        code=code.replace(old,new)
    out=ROOT/'build/port/runtime/proximity-float';out.mkdir(parents=True,exist_ok=True)
    source=out/'fixture.c'
    source.write_text('''#include "ps2float.h"
#include <assert.h>
#include <stdio.h>
static float ps2_fmul(float a,float b){return ps2_float_mul(a,b);}
static int port_delta_clock(void){return 0;}  /* the 60 path */
static int port_whole_step(void){return 1;}
'''+code+r'''
Game D_004B25A0; State D_002AF2C0; float D_0029C168[]={3.4028234663852886e38f};
static float distance;static int calls,found,actions;
#ifdef DHD_RIDER_QUERIES_H
int SharedCall0011C0D0(struct Shared0011C0D0_World *s,struct Vector4 *p,int flag,float range){return 1;}
struct Rider *SharedCall0011C168(struct Shared0011C168_Game *s,float *p,float *out,float range){
 *out=distance;return (struct Rider *)(uintptr_t)(calls++==0?found:0);
}
#else
int func_0011C0D0(State *s,Vec4 *p,float range,int flag){return 1;}
int func_0011C168(State *s,Vec4 *p,float range,float *out){*out=distance;return calls++==0?found:0;}
#endif
int func_0011B180(State *s,Vec4 *p,float *out,int flag,float range){*out=distance;return found;}
void func_00204358(void *p,int value){}
void func_0011E568(Obj *p,int value){actions++;}
#define STUB(name) void name(Obj *p){}
STUB(func_0011F1E0) STUB(func_0011F298) STUB(func_0011F3B8) STUB(func_0011F4C0)
STUB(func_0011F760) STUB(func_0011F880) STUB(func_0011F8C0)
short func_00222BD8(int a,int b){return 0;}
void func_00203DC8(Handle *p,int value){}
void func_00203E10(Handle *p,float value){}
void func_00202E38(Handle *p,int a,int b,float value){}
int main(void){
 Obj obj={0};Model model={0};obj.type=9;obj.model=&model;
 distance=3.4028234663852886e38f;calls=0;found=0;func_0011EDA8(&obj);
 assert(obj.x250==distance && obj.x38==0 && actions==0);
 distance=12;calls=0;found=1;func_0011EDA8(&obj);
 assert(obj.x250==144 && obj.x38==1 && actions==1);
 distance=3e20f;calls=0;found=1;func_0011EDA8(&obj);
 assert(obj.x250==3.4028234663852886e38f && actions==1);
 puts("Real proximity update: missing-rider sentinel, ordinary distance and square overflow pass");return 0;
}
''')
    testprog.run(testprog.guest('proximity-float', [source], out=out, includes=[ROOT / 'include']))


if __name__=='__main__':main()
