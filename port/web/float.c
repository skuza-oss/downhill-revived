/* Scalar wasm operations under the active EE MXCSR policy. No host rounding mode. */
#include <stdint.h>
typedef float v4f __attribute__((vector_size(16)));
static uint32_t bits(float x){uint32_t u;__builtin_memcpy(&u,&x,4);return u;}
static float value(uint32_t u){float x;__builtin_memcpy(&x,&u,4);return x;}
static uint64_t dbits(double x){uint64_t u;__builtin_memcpy(&u,&x,8);return u;}
/* The FP control word: the core's at 0x07800000; the threaded build's graphics reactor has its own
   (build_graphics.py --threads builds these helpers with WEB_CSR_AT=web_queue.h's WEB_Q_REACTOR_CSR). */
#ifndef WEB_CSR_AT
#define WEB_CSR_AT 0x07800000u
#endif
static int ee(void){return (*(volatile uint32_t *)WEB_CSR_AT&0x6000)==0x6000;}
static float daz(float a){uint32_t u=bits(a);return value((u&0x7f800000)?u:u&0x80000000);}
static float rz(double d){
    uint64_t u=dbits(d);unsigned e=(u>>52)&2047;uint32_t sign=(u>>32)&0x80000000;
    if(e==2047)return (float)d;
    if(e<897)return value(sign);
    if(e>1150)return value(sign|0x7f7fffff);
    return value(sign|((e-896)<<23)|((u>>29)&0x7fffff));
}
static uint32_t nan2(float a,float b){uint32_t x=bits(a),y=bits(b);
    if((x&0x7fffffff)>0x7f800000)return x|0x400000;
    if((y&0x7fffffff)>0x7f800000)return y|0x400000;
    return 0xffc00000;
}
static float sum(float a,float b){
    uint32_t ua=bits(a),ub=bits(b);unsigned ea=(ua>>23)&255,eb=(ub>>23)&255;
    if(ea && ea<255 && eb && eb<255){
        /* With an exponent gap <=24 the exact sum fits in binary64.
           With a larger gap the small magnitude is strictly below one
           predecessor ulp: RZ either keeps the large value or steps down. */
        int gap=(int)ea-(int)eb;
        if(gap>=25 || gap<=-25){
            uint32_t large=gap>0?ua:ub;
            return value(large-((ua^ub)>>31));
        }
        return rz((double)a+(double)b);
    }
    a=daz(a);b=daz(b);double x=a,y=b,s=x+y;
    if(__builtin_isnan(s))return value(nan2(a,b));
    double z=s-x,err=(x-(s-z))+(y-z);
    float r=rz(s);
    if((double)r==s && ((s>0&&err<0)||(s<0&&err>0)))r=value(bits(r)-1);
    return r;
}
float web_add(float a,float b){return ee()?sum(a,b):a+b;}
float web_sub(float a,float b){if(!ee())return a-b;
    if((bits(a)&0x7fffffff)>0x7f800000||(bits(b)&0x7fffffff)>0x7f800000)return value(nan2(a,b));
    return sum(a,value(bits(b)^0x80000000));}
float web_mul(float a,float b){
    if(!ee())return a*b;a=daz(a);b=daz(b);double d=(double)a*b;
    return __builtin_isnan(d)?value(nan2(a,b)):rz(d);
}
float web_div(float a,float b){
    if(!ee())return a/b;a=daz(a);b=daz(b);double d=(double)a/b;
    if(__builtin_isnan(d))return value(nan2(a,b));
    if(__builtin_isinf(d))return (float)d;
    /* A float quotient cannot lie within a double ulp of a float midpoint.
       RZ conversion is therefore exact except when d is a float itself;
       in that case use the exact 48-bit product to detect double rounding. */
    float r=rz(d);
    if((double)r==d && d!=0 && __builtin_fabs((double)r*(double)b)>__builtin_fabs((double)a))r=value(bits(r)-1);
    return r;
}
float web_sqrt(float a){
    if(!ee())return __builtin_sqrtf(a);a=daz(a);
    if((bits(a)&0x7fffffff)>0x7f800000)return value(bits(a)|0x400000);
    if(a<0)return value(0xffc00000);
    float r=__builtin_sqrtf(a);
    if((double)r*r>(double)a)r=value(bits(r)-1);
    return r;
}
float web_demote(double a){return ee()?rz(a):(float)a;}
float web_i32(int32_t a){return ee()?rz((double)a):(float)a;}
float web_u32(uint32_t a){return ee()?rz((double)a):(float)a;}
/* i64->f32: truncate the integer significand before converting, avoiding
   a double-rounding dependency at 2^63/2^64. */
static float integer(uint64_t a,uint32_t sign){
    if(!a)return 0;unsigned top=63-__builtin_clzll(a);
    if(top<=23)return sign?-(float)a:(float)a;
    return value(sign|((top+127)<<23)|((a>>(top-23))&0x7fffff));
}
float web_i64(int64_t a){
    if(!ee())return (float)a;
    uint64_t mag=a<0?0-(uint64_t)a:(uint64_t)a;
    if(mag<=0xffffff)return (float)a;
    return integer(mag,a<0?0x80000000:0);
}
float web_u64(uint64_t a){if(!ee()||a<=0xffffff)return (float)a;return integer(a,0);}
#define VECTOR(name) __attribute__((unused)) static v4f lanes_##name(v4f a,v4f b){v4f r;for(int i=0;i<4;i++)r[i]=web_##name(a[i],b[i]);return r;}
VECTOR(add) VECTOR(sub) VECTOR(mul) VECTOR(div)
/* Four lanes at once, bit-identical to the scalar helpers (sum()/web_mul()) lane by lane:
   - Zero and denormal operands are flushed (DAZ) as the scalar path does; a flushed zero makes
     the binary64 sum or product exact (the other operand, or a signed zero).
   - Two normal floats' product, and their sum when the exponents are less than 25 apart, are
     exact in binary64; dropping the double's low 29 significand bits gives the round-toward-zero
     float, and demoting it is exact when the result is a normal float at least 2^-125 (below
     that the scalar path flushes where demotion would round) or an exact zero.
   - Normal operands 25 or more exponents apart: the larger one, one ulp down when the signs
     differ (sum()'s own rule).
   Lanes outside these cases (inf, NaN, results out of range) run the scalar helper alone. */
#include <wasm_simd128.h>
static v128_t exps(v128_t a){return wasm_u32x4_shr(wasm_v128_and(a,wasm_i32x4_splat(0x7f800000)),23);}
static v128_t flush(v128_t a,v128_t zero){return wasm_v128_bitselect(wasm_v128_and(a,wasm_i32x4_splat((int)0x80000000)),a,zero);}
static v128_t truncate(v128_t d){return wasm_f32x4_demote_f64x2_zero(wasm_v128_and(d,wasm_i64x2_splat((int64_t)0xffffffffe0000000ull)));}
static v128_t low(v128_t a){return wasm_f64x2_promote_low_f32x4(a);}
static v128_t high(v128_t a){return wasm_f64x2_promote_low_f32x4(wasm_i32x4_shuffle(a,a,2,3,2,3));}
static v128_t join(v128_t lo,v128_t hi){return wasm_i32x4_shuffle(truncate(lo),truncate(hi),0,1,4,5);}
/* result lanes that are a normal float >= 2^-125 or an exact zero */
static v128_t in_range(v128_t r){
    return wasm_v128_or(wasm_u32x4_lt(wasm_i32x4_sub(exps(r),wasm_i32x4_splat(2)),wasm_i32x4_splat(253)),
                        wasm_i32x4_eq(wasm_v128_and(r,wasm_i32x4_splat(0x7fffffff)),wasm_i32x4_splat(0)));
}
/* op: 0 add, 1 sub, 2 mul (no function pointers: the helpers are linked without a table) */
static v4f finish(v128_t r,v128_t ok,v4f a,v4f b,int op){
    if(wasm_i32x4_all_true(ok))return (v4f)r;
    v4f out=(v4f)r;unsigned bad=~wasm_i32x4_bitmask(ok)&15;
    for(int i=0;i<4;i++)if(bad>>i&1)out[i]=op==0?web_add(a[i],b[i]):op==1?web_sub(a[i],b[i]):web_mul(a[i],b[i]);
    return out;
}
__attribute__((noinline)) static v4f vsum(v4f fa,v4f fb,v4f oa,v4f ob,int op){
    v128_t a=(v128_t)fa,b=(v128_t)fb,ea=exps(a),eb=exps(b),z=wasm_i32x4_splat(0);
    v128_t az=wasm_i32x4_eq(ea,z),bz=wasm_i32x4_eq(eb,z);
    v128_t finite=wasm_v128_and(wasm_i32x4_lt(ea,wasm_i32x4_splat(255)),wasm_i32x4_lt(eb,wasm_i32x4_splat(255)));
    v128_t x=flush(a,az),y=flush(b,bz);
    v128_t r=join(wasm_f64x2_add(low(x),low(y)),wasm_f64x2_add(high(x),high(y)));
    v128_t anyzero=wasm_v128_or(az,bz);
    v128_t far=wasm_v128_andnot(wasm_i32x4_ge(wasm_i32x4_abs(wasm_i32x4_sub(ea,eb)),wasm_i32x4_splat(25)),anyzero);
    v128_t large=wasm_v128_bitselect(a,b,wasm_i32x4_gt(ea,eb));
    r=wasm_v128_bitselect(wasm_i32x4_sub(large,wasm_u32x4_shr(wasm_v128_xor(a,b),31)),r,far);
    v128_t ok=wasm_v128_and(finite,wasm_v128_or(wasm_v128_or(anyzero,far),in_range(r)));
    return finish(r,ok,oa,ob,op);
}
/* The usual case first, with no shifts: every operand normal, (for sums) exponents less than
   25 apart, every result a normal float >= 2^-125. Anything else takes the general path above. */
#define EXP(x) wasm_v128_and((v128_t)(x),wasm_i32x4_splat(0x7f800000))
static int all_normal(v128_t ea,v128_t eb){
    v128_t one=wasm_i32x4_splat(0x00800000);
    return wasm_i32x4_all_true(wasm_u32x4_lt(wasm_u32x4_max(wasm_i32x4_sub(ea,one),wasm_i32x4_sub(eb,one)),wasm_i32x4_splat(0x7f000000)));
}
static int results_normal(v128_t r){
    return wasm_i32x4_all_true(wasm_u32x4_lt(wasm_i32x4_sub(EXP(r),wasm_i32x4_splat(0x01000000)),wasm_i32x4_splat(0x7e800000)));
}
static int near(v128_t ea,v128_t eb){
    return wasm_i32x4_all_true(wasm_u32x4_lt(wasm_i32x4_add(wasm_i32x4_sub(ea,eb),wasm_i32x4_splat(24<<23)),wasm_i32x4_splat(49<<23)));
}
/* Lanes that are +-0 (not denormal): x +- 0 and 0 * y are exact in binary64, as is 0 + 0 with
   the scalar path's sign; a zero result is then exactly the scalar one (an exact zero, or for
   products a binary64 value below 2^-150 that the scalar path flushes to the same signed zero). */
static v128_t zero_lanes(v128_t x){return wasm_i32x4_eq(wasm_v128_and(x,wasm_i32x4_splat(0x7fffffff)),wasm_i32x4_splat(0));}
static v128_t normal_lanes(v128_t e){return wasm_u32x4_lt(wasm_i32x4_sub(e,wasm_i32x4_splat(0x00800000)),wasm_i32x4_splat(0x7f000000));}
static v128_t result_lanes(v128_t r){
    return wasm_v128_or(wasm_u32x4_lt(wasm_i32x4_sub(EXP(r),wasm_i32x4_splat(0x01000000)),wasm_i32x4_splat(0x7e800000)),zero_lanes(r));
}
static inline v4f quick_sum(v4f a,v4f b,int op){
    v128_t x=(v128_t)a,y=(v128_t)b,ea=EXP(x),eb=EXP(y);
    if(all_normal(ea,eb)&&near(ea,eb)){
        v128_t r=join(wasm_f64x2_add(low(x),low(y)),wasm_f64x2_add(high(x),high(y)));
        if(results_normal(r))return (v4f)r;
    }
    {   /* second chance: some lanes are +-0 */
        v128_t za=zero_lanes(x),zb=zero_lanes(y);
        v128_t gap=wasm_u32x4_lt(wasm_i32x4_add(wasm_i32x4_sub(ea,eb),wasm_i32x4_splat(24<<23)),wasm_i32x4_splat(49<<23));
        v128_t ok=wasm_v128_and(wasm_v128_or(normal_lanes(ea),za),wasm_v128_or(normal_lanes(eb),zb));
        ok=wasm_v128_and(ok,wasm_v128_or(gap,wasm_v128_or(za,zb)));
        if(wasm_i32x4_all_true(ok)){
            v128_t r=join(wasm_f64x2_add(low(x),low(y)),wasm_f64x2_add(high(x),high(y)));
            if(wasm_i32x4_all_true(result_lanes(r)))return (v4f)r;
        }
    }
    return op?vsum(a,b,a,(v4f)wasm_v128_xor((v128_t)b,wasm_i32x4_splat((int)0x80000000)),1):vsum(a,b,a,b,0);
}
v4f web_vadd(v4f a,v4f b){if(!ee())return a+b;return quick_sum(a,b,0);}
v4f web_vsub(v4f a,v4f b){if(!ee())return a-b;return quick_sum(a,(v4f)wasm_v128_xor((v128_t)b,wasm_i32x4_splat((int)0x80000000)),1);}
__attribute__((noinline)) static v4f vmul(v4f fa,v4f fb){
    v128_t a=(v128_t)fa,b=(v128_t)fb,ea=exps(a),eb=exps(b),z=wasm_i32x4_splat(0);
    v128_t az=wasm_i32x4_eq(ea,z),bz=wasm_i32x4_eq(eb,z);
    v128_t finite=wasm_v128_and(wasm_i32x4_lt(ea,wasm_i32x4_splat(255)),wasm_i32x4_lt(eb,wasm_i32x4_splat(255)));
    v128_t x=flush(a,az),y=flush(b,bz);
    v128_t r=join(wasm_f64x2_mul(low(x),low(y)),wasm_f64x2_mul(high(x),high(y)));
    v128_t ok=wasm_v128_and(finite,wasm_v128_or(wasm_v128_or(az,bz),in_range(r)));
    return finish(r,ok,fa,fb,2);}
v4f web_vmul(v4f a,v4f b){if(!ee())return a*b;
    v128_t x=(v128_t)a,y=(v128_t)b;
    if(all_normal(EXP(x),EXP(y))){
        v128_t r=join(wasm_f64x2_mul(low(x),low(y)),wasm_f64x2_mul(high(x),high(y)));
        if(results_normal(r))return (v4f)r;
    }
    v128_t ok=wasm_v128_and(wasm_v128_or(normal_lanes(EXP(x)),zero_lanes(x)),wasm_v128_or(normal_lanes(EXP(y)),zero_lanes(y)));
    if(wasm_i32x4_all_true(ok)){
        v128_t r=join(wasm_f64x2_mul(low(x),low(y)),wasm_f64x2_mul(high(x),high(y)));
        if(wasm_i32x4_all_true(result_lanes(r)))return (v4f)r;
    }
    return vmul(a,b);}
/* Normal operands: the binary64 quotient rounded to nearest, truncated, is web_div's result
   unless its low 29 bits are all zero (then web_div checks the exact product) or the float
   result is not normal >= 2^-125. */
static int inexact(v128_t d){return wasm_i64x2_all_true(wasm_v128_and(d,wasm_i64x2_splat(0x1fffffff)));}
v4f web_vdiv(v4f a,v4f b){if(!ee())return a/b;
    v128_t x=(v128_t)a,y=(v128_t)b;
    if(all_normal(EXP(x),EXP(y))){
        v128_t lo=wasm_f64x2_div(low(x),low(y)),hi=wasm_f64x2_div(high(x),high(y));
        if(inexact(lo)&&inexact(hi)){v128_t r=join(lo,hi);if(results_normal(r))return (v4f)r;}
    }
    return lanes_div(a,b);}

v4f web_vsqrt(v4f a){v4f r;for(int i=0;i<4;i++)r[i]=web_sqrt(a[i]);return r;}
typedef int32_t v4i __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));
/* Every int32 is exact in binary64 and its truncation a normal float (or zero): web_i32 lane by lane. */
v4f web_vi32(v4i a){if(!ee())return __builtin_convertvector(a,v4f);
    v128_t x=(v128_t)a;
    return (v4f)join(wasm_f64x2_convert_low_i32x4(x),wasm_f64x2_convert_low_i32x4(wasm_i32x4_shuffle(x,x,2,3,2,3)));}
v4f web_vu32(v4u a){v4f r;for(int i=0;i<4;i++)r[i]=web_u32(a[i]);return r;}
