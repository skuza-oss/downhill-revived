"""Execute the real rider substep expression at a zero angular speed."""
from pathlib import Path
import sys
import re
import testprog
ROOT=Path(__file__).resolve().parents[2]


def main():
    sys.path.insert(0,str(ROOT/'port'));import patches
    text=(ROOT/'src/unsorted/00131998.cpp').read_text()
    for kind,old,new in patches.PATCHES['unsorted/00131998.cpp']:
        if kind=='re':
            text,count=re.subn(old[0],new,text);assert count==old[1]
        else:
            assert kind=='text' and text.count(old)==1;text=text.replace(old,new)
    expression=re.search(r'step = (Min\(remaining,.*?\));',text)[1]
    out=ROOT/'build/port/runtime/rider-step';out.mkdir(parents=True,exist_ok=True)
    source=out/'fixture.c';source.write_text('#include "ps2float.h"\n#include <xmmintrin.h>\n#include <assert.h>\n#include <stdio.h>\n'
        'static int rate=30;static int func_00238F30(void){return rate;}\n'
        'static int port_delta_clock(void){return 0;}\n'  # the original path
        'static float port_step_rate(void){return 0;}\n'
        'static float port_step_seconds(void){return 0;}\n'
        'static float ps2_fdiv(float a,float b){return ps2_float_div(a,b);}\n'
        'static float Min(float a,float b){return a<b?a:b;}\n'
        'static float substep(float remaining,float speed){return '+expression+';}\n'+r'''
int main(void){
 assert(substep(1,0)==1);assert(substep(0.125f,0)==0.125f);
 float expected=30.0f*1.5707963705062866f/100.0f;
 assert(substep(1,100)==expected);assert(substep(1,-100)==-expected);
 rate=0;assert(substep(1,0)==1);
 puts("Rider substep: stopped, ordinary, signed and zero-rate cases pass");return 0;
}
''')
    testprog.run(testprog.guest('rider-step', [source], out=out))


if __name__=='__main__':main()
