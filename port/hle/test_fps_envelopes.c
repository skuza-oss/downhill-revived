/* Actual transformed 00239778/002398C0 with synthetic keys. These feed camera
   displacement (00239858), rather than the sample-driven audio mixer. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct Key { float value,time; } Key;
int D_0027EC68[4];
float D_0027EC78[4],D_0027EC9C[1],D_0075A690[4],D_0075A6A0[4];
Key D_0027EC88[4]={{0,0},{1,.1f},{.4f,.3f},{0,1}};
static int hz;
int port_delta_clock(void) { return hz>60; }
float port_step(void) { return 60.0f/hz; }
int func_00238F60(void) { return 1; }
void func_00239778(int);
void func_002398C0(unsigned int);

int main(void)
{
    double reference_end=0,reference_decay=0;
    for (int trial=0;trial<3;trial++) {
        hz=trial==0 ? 60 : trial==1 ? 120 : 144;
        memset(D_0027EC68,0,sizeof D_0027EC68);
        memset(D_0075A690,0,sizeof D_0075A690);
        D_0027EC78[0]=1;
        int steps=0,changes[3]={0};
        while (D_0027EC68[0]<3 && steps<hz*2) {
            int before=D_0027EC68[0];steps++;
            func_002398C0(0);
            if (D_0027EC68[0]!=before) changes[before]=steps;
        }
        assert(D_0027EC68[0]==3);
        double end=(double)steps/hz;
        D_0027EC68[0]=2;D_0075A690[0]=1;D_0027EC9C[0]=.1f;
        steps=0;
        while (D_0075A690[0]>D_0027EC9C[0] && steps<hz*2) {
            steps++;func_00239778(0);
        }
        double decay=(double)steps/hz;
        if (!trial) { reference_end=end;reference_decay=decay; }
        /* The original integer-field curve itself rounds event boundaries to
           a legacy field. Keep that bound explicit instead of claiming PCM
           or substep-exact camera equivalence. */
        assert(fabs(end-reference_end)<=1.0/60+.000001);
        assert(fabs(decay-reference_decay)<=1.0/60+.000001);
        printf("%d: envelope %.9f s (keys %.9f %.9f %.9f), decay %.9f s\n",hz,end,
            (double)changes[0]/hz,(double)changes[1]/hz,(double)changes[2]/hz,decay);
    }
    return 0;
}
