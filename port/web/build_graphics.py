"""Build the existing headless GS/VIF1/VU1 as a second wasm sharing core memory.

  python port/web/build_graphics.py [--no-vu-aot] [--vu-probe] [--threads]
                                    [--guarded-memory] [--legacy-memory] [--debug-memory]
                                    [--exact-float] [--vu-ieee-probe]

The reactor's GS device ops are the Rust renderer's, through port/web/gpu_rust_web.c: the
dhd_render imports (port/render/include/dhd_render.h, the Rust module's raw exports, wasm-to-wasm)
plus env.web_gpu_await, the reactor's own JSPI Suspending import that a split readback waits on;
with its native renderer (gpu_rust_web_native.inc over port/gs/native_core.inc) and the sharp font
(port/gs/font_sharp.c, built here with its SDL3 spinlock as a no-op). The import section is checked
after the link (check_imports).

Float ops: the reactor only makes pictures (VIF1/VU1, the GS front end); none of its float results
feed game state. By default they stay plain wasm IEEE ops (f32/f64, and wasm SIMD f32x4 for VU1's
SSE code through simde), built with -DPS2_VU_IEEE: VU1 then runs without the VU MXCSR, with its
DAZ and exponent-255 operand clamp done in vu0_interp.c's operand4; only its rounding differs
(nearest instead of toward zero). --exact-float instead rewrites every float op into postlink's
exact helpers (port/web/float.c, double.c) and runs VU1 under the VU mode, rounding as the x64
build does (slow: the helpers were 22-37% of the game worker's time, finding 086 W4).
--vu-ieee-probe (diagnostics, plain floats only): web_vu_ieee_probe counts VU1 operand lanes
flushed as denormals and clamped from exponent 255, and FMAC result lanes that underflowed to a
denormal or overflowed to inf (run_headless.mjs's report, vu_ieee_probe).

--threads (build.py's default; its --no-threads leaves it out): the reactor for its own worker (www/graphics.js), on the
core's memory made shared (port/web/threads.py), fed through the queue of port/web/web_queue.h:
  - the reactor gets web_queue.c's consumer (web_q_init, web_q_run, web_q_present) and the
    generated web_q_dispatch over its web_x_* wrappers, its own FP control word
    (WEB_Q_REACTOR_CSR, which --exact-float's float helpers read; IEEE mode at rest, so the
    wrappers do not switch it) and its own helper stack, and a shared memory import;
  - build/web/gqueue.wasm is the core's side: web_queue.c's producer plus the generated web_x_*
    wrappers (gqueue_calls.c), which the core's graphics imports link to in the game's worker.
  - VIF1/VU1 run in a third worker (www/vu.js; finding 090, SP4), as natively on the vu thread:
    build/web/vu.wasm is vif.c and vu0_interp.c (with the VU1 AOT blocks), host_graphics.c's and
    web_queue.c's VU sides and the VU1 calls' wrappers (web_exports_vu.c), linked in its own span
    of the memory (VU_SPAN) with its own FP control word and helper stack. graphics.wasm is then
    the GS worker's: the GS front end and the renderer, without VIF1/VU1, fed by the VU worker
    through web_queue.h's second ring.
"""
import json, os, re, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'port/tools'))
from gen_host64_imports import api_declarations, ctype
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches
EXACT_FLOAT='--exact-float' in sys.argv
THREADS='--threads' in sys.argv
# memory.c's guarded accesses use a scratch word at a fixed address that the core's use too
if THREADS and '--guarded-memory' in sys.argv:sys.exit('build_graphics: --threads and --guarded-memory do not go together')
# The retired JS renderer's device ops (never imported now) and the Rust provider's await.
JS_OPS={'web_gpu_write','web_gpu_copy','web_gpu_batch','web_gpu_record','web_gpu_submit','web_gpu_read'}
AWAIT='web_gpu_await'
def check_imports(wasm):
    """The linked reactor imports only declared dhd_render functions, env.web_gpu_await and none
    of the old JS renderer's device ops."""
    sys.path.insert(0,str(ROOT/'port/web'))
    import check_web_layout as layout
    imports=layout.parse('graphics',wasm).imports
    by={}
    for mod,field,kind in imports:by.setdefault(mod,set()).add(field)
    env=by.get('env',set()); render=by.get('dhd_render',set()); bad=[]
    other=set(by)-{'env','dhd_render','wasi_snapshot_preview1'}
    if other:bad.append('imports from unexpected module(s) %s'%sorted(other))
    # the frozen v1 header and its later additions (dhd_render_ext.h)
    declared=set()
    for h in ('dhd_render.h','dhd_render_ext.h'):
        declared|=set(re.findall(r'DHDR_FN\((\w+)\)',(ROOT/'port/render/include'/h).read_text()))
    if not render:bad.append('no dhd_render imports')
    if render-declared:bad.append('dhd_render imports not in dhd_render.h or dhd_render_ext.h: %s'%sorted(render-declared))
    if AWAIT not in env:bad.append('no env.%s import'%AWAIT)
    if THREADS and env-{'memory','host_wall_seconds',AWAIT}:bad.append('env imports %s (undefined in the GS worker\'s module?)'%sorted(env-{'memory','host_wall_seconds',AWAIT}))
    if env&JS_OPS:bad.append('JS device-op imports %s'%sorted(env&JS_OPS))
    if bad:sys.exit('build_graphics: %s: %s'%(wasm,'; '.join(bad)))
    print('build_graphics: renderer rust, imports: %s'%(', '.join('%s %d'%(m,len(f)) for m,f in sorted(by.items()))))
WASI=(DEVCACHES / 'wasi-sdk/wasi-sdk-34.0-x86_64-windows')
OUT=ROOT/'build/web'; OUT.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,TEMP=str(OUT),TMP=str(OUT))
# Only the VIF host-side selector changes. Never define _WIN64 on wasm32:
# guest.h would then expect a native 64-bit guest_base.
vif=(ROOT/'port/hle/vif.c').read_text().replace('_WIN64','DHD_WEB_HOST')
vif=vif.replace('#include "hle.h"','#include "'+(ROOT/'port/hle/hle.h').as_posix()+'"')
vif=vif.replace('#include "../', '#include "'+(ROOT/'port').as_posix()+'/')
(OUT/'vif.c').write_text(vif)
vu=(ROOT/'port/runtime/vu0_interp.c').read_text().replace(
    '#if defined(__wasm__)\n    if (ps2_vu1_regs_sync)',
    '#if 0 /* This is the web HOST VU1 file, not the EE forwarding copy. */\n    if (ps2_vu1_regs_sync)')
vu=vu.replace('#include "../','#include "'+(ROOT/'port').as_posix()+'/')
# Counters for the page's status line: VU1 steps through translated blocks and in all.
vu=vu.replace('        history[dispatched++ & 15] = pc * 8;','        if (unit) web_vu_steps++;\n        history[dispatched++ & 15] = pc * 8;')
vu=vu.replace('        if (translated) {','        if (translated) {\n            if (unit) web_vu_blocks++;')
vu=vu.replace('static inline __attribute__((always_inline)) int run(','double web_vu_steps, web_vu_blocks;\nstatic inline __attribute__((always_inline)) int run(',1)
if '--vu-probe' in sys.argv:  # diagnostics: translated blocks matching micro memory now
    vu=vu.replace('#include "vu1_aot.inc"\n','#include "vu1_aot.inc"\n'+(OUT/'vu_probe.c').read_text(),1)
(OUT/'vu0_interp.c').write_text(vu)
api=api_declarations(ROOT/'port/host/host_api.h')
exports=[n for n in api if
         n.startswith('host_service_gs_') or n in ('host_vu1_file','host_vu1_snapshot','host_service_vif_feed','host_service_graphics_feed')]
sources=[ROOT/'port/web/host_graphics.c',ROOT/'port/gs/gs.c',ROOT/'port/hle/vif.c']
defined=[n for n in exports if any(re.search(r'^[\w ]+'+n+r'\s*\([^;]*\{',f.read_text(),re.M) for f in sources)]
# The browser links these straight into the core's imports (no JS frame between the two
# modules), so a readback inside them can suspend through JSPI. Each wrapper does what the JS
# wrapper of run_headless.mjs does: host graphics runs in IEEE mode, the EE's mode is restored.
# The guest addresses these take: folded to canonical RAM here (the same map as postlink.py's
# accesses and the JS hosts' fold), so the reactor itself needs no adapted memory accesses.
GUEST_ARGS={'host_service_vif_feed':[1],'host_service_graphics_feed':[1],'host_service_gs_read_image':[0],
            'host_service_gs_defer_read_image':[0],'host_service_gs_trace_pop':[0],'host_service_gs_snapshot':[0],
            'host_vu1_snapshot':[0],'host_vu1_file':[0]}
FOLD='''static inline unsigned web_fold(unsigned a) {
    unsigned low = a & 0x1fffffffu;
    if (a >= 0x38000000u && a < 0x3a000000u) return a - 0x30000000u;
    if (a >= 0x30400000u && a < 0x32000000u) return a - 0x28000000u;
    if ((a & 0xe0000000u) == 0 || (a & 0xe0000000u) == 0x20000000u) {
        if (low >= 0x08000000u && low < 0x0a000000u) return low;
        if (low >= 0x00400000u && low < 0x02000000u) return low + 0x08000000u;
    }
    return a;
}'''
# --threads: the calls VIF1/VU1 own (defined in vif.c: host_vu1_*, the VIF feed) run in the VU
# worker's module (vu.wasm), the rest in the GS worker's (graphics.wasm); the one-worker reactor
# has them all.
FEEDS=('host_service_vif_feed','host_service_graphics_feed')
vif_text=(ROOT/'port/hle/vif.c').read_text()
VU_OWNED=[n for n in defined if re.search(r'^[\w ]+'+n+r'\s*\([^;]*\{',vif_text,re.M)]
if THREADS and set(VU_OWNED)!={'host_service_vif_feed','host_vu1_snapshot','host_vu1_file'}:
    sys.exit('build_graphics: vif.c defines %s: check which worker runs each'%VU_OWNED)
HEAD=['#include "'+(ROOT/'port/host/host_api.h').as_posix()+'"',FOLD,'void web_graphics_enter(void);','void web_gpu_reattach(void);','void web_vu_ready(void);']
lines,vu_lines=list(HEAD),list(HEAD)
for n in defined:
    if THREADS and n in FEEDS:continue   # web_queue.c runs the feeds itself
    ret,args=api[n];types=[ctype(a) for a in args];names=[f'a{i}' for i in range(len(args))]
    params=', '.join(f'{t} {v}' for t,v in zip(types,names)) or 'void'
    call=f'{n}({", ".join(f"web_fold({v})" if i in GUEST_ARGS.get(n,()) else v for i,v in enumerate(names))});'
    if n=='host_service_gs_reset':call+=' web_gpu_reattach();'  # gs_reset clears the hooks
    if n in ('host_service_vif_feed','host_service_graphics_feed','host_vu1_file'):call='web_vu_ready(); '+call  # VU1 microprograms come through VIF1
    body=f'{ret} r = {call} ' if ret!='void' else f'{call} '
    # One worker: the core's FP control word is the reactor's too, so each call from the core
    # switches it to the GS front end's IEEE mode and back. --threads: the reactor's own word is
    # in that mode at rest (web_q_init; whatever switches it switches it back).
    csr=('','') if THREADS else ('unsigned s = host_fp_csr(); host_fp_set_csr(0x1f80); ','host_fp_set_csr(s);')
    (vu_lines if THREADS and n in VU_OWNED else lines).append(f'__attribute__((export_name("web_x_{n}"))) {ret} web_x_{n}({params}) '
                 f'{{ {csr[0]}web_graphics_enter(); {body}'
                 f'{csr[1]}{" return r;" if ret!="void" else ""} }}')
QUEUE=ROOT/'port/web/web_queue.h'
if THREADS:
    # The threaded build's queued calls: everything above but the two feeds (web_queue.c copies
    # those). The core waits for each of them except the ones that neither return a value nor
    # write guest memory (natively they too run on the EE thread, after a graphics wait).
    import zlib
    ASYNC={'host_service_gs_vblank','host_service_gs_priv_write','host_service_gs_trace_begin'}
    calls=[n for n in defined if n not in FEEDS]
    for n in ASYNC:
        if n not in calls or api[n][0]!='void' or n in GUEST_ARGS:sys.exit('build_graphics: %s is not a queued void call'%n)
    abi=zlib.crc32((QUEUE.read_text()+repr([(n,api[n],n in ASYNC,n in VU_OWNED) for n in calls])).encode())
    dispatch=['uint64_t web_q_dispatch(uint32_t id, const uint64_t *a)','{','    switch (id) {']
    vu_dispatch=['uint64_t web_q_vu_dispatch(uint32_t id, const uint64_t *a)','{','    switch (id) {']
    producer=['/* Generated by port/web/build_graphics.py --threads: the core\'s side of the queued GS calls. */',
              '#include <stdint.h>','uint64_t web_q_call(uint32_t id, uint32_t wait, uint32_t flags, uint64_t a0, uint64_t a1, uint64_t a2);',
              f'__attribute__((export_name("web_q_abi"))) uint32_t web_q_abi(void) {{ return {abi}u; }}']
    for i,n in enumerate(calls):
        ret,args=api[n];types=[ctype(a) for a in args]
        if len(types)>3:sys.exit('build_graphics: %s has more than 3 arguments'%n)
        params=', '.join(f'{t} a{k}' for k,t in enumerate(types)) or 'void'
        values=[f'(uint64_t)a{k}' for k in range(len(types))]+['0']*(3-len(types))
        # WQ_VU_CALL (web_queue.h): the VU worker runs it
        call=f'web_q_call({i}, {0 if n in ASYNC else 1}, {0x200 if n in VU_OWNED else 0}, {", ".join(values)})'
        producer.append(f'__attribute__((export_name("web_x_{n}"))) {ret} web_x_{n}({params}) '
                        f'{{ {"return ("+ret+")" if ret!="void" else ""}{call}; }}')
        invoke=f'web_x_{n}({", ".join(f"({t})a[{k}]" for k,t in enumerate(types))})'
        (vu_dispatch if n in VU_OWNED else dispatch).append(f'    case {i}: {"return (uint64_t)"+invoke+";" if ret!="void" else invoke+"; return 0;"}')
    for d in (dispatch,vu_dispatch):
        d+=['    }','    __builtin_trap();','}',f'__attribute__((export_name("web_q_abi"))) uint32_t web_q_abi(void) {{ return {abi}u; }}']
    lines+=['#include <stdint.h>']+dispatch
    vu_lines+=['#include <stdint.h>']+vu_dispatch
    (OUT/'web_exports_vu.c').write_text(chr(10).join(vu_lines)+chr(10))
    (OUT/'gqueue_calls.c').write_text(chr(10).join(producer)+chr(10))
    # The core's side: no libc, no data, its stack in the web host's scratch region (only the
    # core's thread runs it), the core's memory imported shared.
    qcmd=[str(WASI/'bin/clang.exe'),'-O2','-matomics','-mbulk-memory','-nostdlib','-DWEB_QUEUE_PRODUCER',
          '-Wl,--no-entry','-Wl,--import-memory','-Wl,--shared-memory','-Wl,--no-stack-first',
          '-Wl,--global-base=%d'%0x07A10000,'-Wl,-z,stack-size=65536',
          '-Wl,--initial-memory=%d'%0x07B00000,'-Wl,--max-memory=2147483648',
          str(ROOT/'port/web/web_queue.c'),str(OUT/'gqueue_calls.c'),'-o',str(OUT/'gqueue.wasm')]
    r=subprocess.run(qcmd,env=env,capture_output=True,text=True)
    print(r.stdout+r.stderr)
    if r.returncode:sys.exit('build_graphics: gqueue.wasm: link failed')
    sys.path.insert(0,str(ROOT/'port/web'))
    import check_web_layout as layout
    q=layout.parse('gqueue',OUT/'gqueue.wasm')
    if [(m,f) for m,f,_ in q.imports]!=[('env','memory')]:sys.exit('build_graphics: gqueue.wasm imports %s, not only env.memory'%q.imports)
    print('build_graphics: gqueue.wasm, %d queued GS calls (%d not waited for), abi %08x'%(len(calls),len(ASYNC),abi))
else:
    (OUT/'gqueue.wasm').unlink(missing_ok=True)   # one-worker build: no stale queue module for worker.js
(OUT/'web_exports.c').write_text(chr(10).join(lines)+chr(10))
cmd=[str(WASI/'bin/clang.exe'),'-O2','-msimd128','-fno-strict-aliasing','-fwrapv',
     '-ffp-contract=off','-ftrivial-auto-var-init=zero','-DXXH_VECTOR=0','-DDHD_WEB_HOST=1',
     '-I'+str(ROOT/'port/runtime'),'-I'+str(ROOT/'port/hle'),'-I'+str(OUT/'shim'),
     '-I' + str(DEVCACHES / 'simde-src'),'-mexec-model=reactor','-Wl,--import-memory','-Wl,--export-memory',
     # --initial-memory=0x50000000 (plan 3.3): wasi-libc's __heap_end is the initial memory size, so
     # the reactor's malloc stays below the Rust renderer's window (0x60000000-0x69000000) and the
     # core's data; growth past it goes through memory.grow, above everything else.
     '-Wl,--initial-memory=1342177280','-Wl,--max-memory=2147483648',
     '-Wl,--global-base=1073741824','-Wl,-z,stack-size=1048576',
     '-Wl,--allow-undefined','-Wl,--export=web_frame','-Wl,--export=__stack_pointer']
if not EXACT_FLOAT:
    cmd+=['-DPS2_VU_IEEE=1']
    if '--vu-ieee-probe' in sys.argv:cmd+=['-DPS2_VU_IEEE_PROBE=1']
for n in exports: cmd+=['-Wl,--export-if-defined='+n]
shim=OUT/'shim'; shim.mkdir(exist_ok=True)
for name,target in [('xmmintrin.h','sse.h'),('emmintrin.h','sse2.h')]:
    (shim/name).write_text('#define SIMDE_ENABLE_NATIVE_ALIASES\n#include <simde/x86/'+target+'>\n')
cmd += [str(ROOT/p) for p in ('port/web/host_graphics.c','port/web/gpu_rust_web.c','port/gs/gs.c','port/gs/memory.c')]
# The native renderer's sharp font: font_sharp.c with its two SDL needs (a lock around its cache,
# SDL_strcasecmp) as no-ops for the one-threaded reactor, and its includes made absolute.
font=(ROOT/'port/gs/font_sharp.c').read_text()
sdl=('#include <strings.h>\n#define SDL_strcasecmp strcasecmp\ntypedef int SDL_SpinLock;\n'
     '#define SDL_LockSpinlock(l) ((void)(l))\n#define SDL_UnlockSpinlock(l) ((void)(l))')
for a,b in (('#include <SDL3/SDL_atomic.h>',sdl),('#include "font_sharp.h"','#include "'+(ROOT/'port/gs/font_sharp.h').as_posix()+'"'),
            ('#include "memory.h"','#include "'+(ROOT/'port/runtime/memory.h').as_posix()+'"'),
            ('#include "../','#include "'+(ROOT/'port').as_posix()+'/')):
    if a not in font:sys.exit('build_graphics: port/gs/font_sharp.c has no %r any more'%a)
    font=font.replace(a,b)
(OUT/'font_sharp.c').write_text(font)
cmd += ['-I'+str(ROOT/'port/render/include'),str(OUT/'font_sharp.c')]
cmd += [str(OUT/'web_exports.c')]
if THREADS:
    # The consumer and its atomics (wasm threads ops on the shared memory, which the import becomes
    # after postlink: threads.share), and the reactor's own FP control word (host_graphics.c).
    # VIF1/VU1 are in vu.wasm (below): this module is the GS worker's.
    cmd += ['-matomics','-mbulk-memory','-DDHD_WEB_THREADS=1','-DDHD_WEB_GS=1','-I'+str(ROOT/'port/web'),str(ROOT/'port/web/web_queue.c')]
# The native build's VU1 path (PS2_VU_AOT in build_host64.py): straight-line blocks translated
# from the player's microcode plus the hand-written geometry kernels, instead of the interpreter
# alone. Generated from assets/vutext.bin into build/web (game-derived: never committed).
VU=[]
if '--no-vu-aot' not in sys.argv:
    sys.path.insert(0,str(ROOT/'port/tools'))
    from vu1_aot import generate
    print('VU1 AOT: %d straight-line blocks' % generate(OUT/'vu1_aot.inc'))
    VU += ['-DPS2_VU_AOT','-I'+str(OUT)]
VU += [str(OUT/'vif.c'),str(OUT/'vu0_interp.c')]
if not THREADS:cmd += VU
cmd += ['-o',str(OUT/'graphics-raw.wasm')]
(OUT/'graphics-command.json').write_text(json.dumps(cmd,indent=2))
r=subprocess.run(cmd,env=env,capture_output=True,text=True)
(OUT/'graphics-build.log').write_text(r.stdout+r.stderr)
print(r.stdout+r.stderr)
if r.returncode:sys.exit(r.returncode)
check_imports(OUT/'graphics-raw.wasm')
# The threaded build's VU worker module (www/vu.js): VIF1 and VU1 (vif.c, vu0_interp.c with the AOT
# blocks), web_queue.c's VU side and host_graphics.c's (DHD_WEB_VU), the VU1 calls' wrappers
# (web_exports_vu.c). Its data, stack and heap lie in VU_SPAN, apart from every other module's:
# data from the bottom, the stack after it (no stack first: the GS module's stack is at 0), the
# heap up to the initial memory (wasi-libc's __heap_end), so its malloc never meets another's.
VU_SPAN=(0x58000000,0x5C000000)
def build_vu(gs_cmd):
    text=QUEUE.read_text()
    csr,stack=(int(re.search(r'#define %s (0x[0-9A-Fa-f]+)u'%n,text).group(1),16) for n in ('WEB_Q_VU_CSR','WEB_Q_VU_HELPER_STACK'))
    # vif.c's DHD_VU_SUSPECT watch reads the GS's draw counter, which is in the other worker
    v=(OUT/'vif.c').read_text()
    for a,b in (('uint64_t suspects = watch ? gs.suspect_draws : 0;','uint64_t suspects = 0; watch = 0; /* threaded: the GS is in its own worker */'),
                ('if (watch && gs.suspect_draws != suspects) save_suspect("DHD_VU_SUSPECT", "suspect");','(void)suspects;')):
        if a not in v:sys.exit('build_graphics: vif.c has no %r any more'%a)
        v=v.replace(a,b)
    (OUT/'vif_vu.c').write_text(v)
    # the GS module's compile flags and include paths (everything before its link options)
    vcmd=gs_cmd[:gs_cmd.index('-mexec-model=reactor')]+['-I'+str(ROOT/'port/web'),'-mexec-model=reactor','-Wl,--import-memory','-Wl,--export-memory',
          '-Wl,--no-stack-first','-Wl,--global-base=%d'%VU_SPAN[0],'-Wl,--initial-memory=%d'%VU_SPAN[1],
          '-Wl,--max-memory=2147483648','-Wl,-z,stack-size=1048576','-Wl,--allow-undefined','-Wl,--export=__stack_pointer',
          '-Wl,--export=__heap_base','-Wl,--export=__data_end',
          '-matomics','-mbulk-memory','-DDHD_WEB_THREADS=1','-DDHD_WEB_VU=1','-DWEB_QUEUE_VU=1']
    if not EXACT_FLOAT:vcmd+=['-DPS2_VU_IEEE=1']+(['-DPS2_VU_IEEE_PROBE=1'] if '--vu-ieee-probe' in sys.argv else [])
    vcmd+=[str(ROOT/'port/web/host_graphics.c'),str(OUT/'web_exports_vu.c'),str(ROOT/'port/web/web_queue.c')]
    vcmd+=[x if x!=str(OUT/'vif.c') else str(OUT/'vif_vu.c') for x in VU]+['-o',str(OUT/'vu-raw.wasm')]
    (OUT/'vu-command.json').write_text(json.dumps(vcmd,indent=2))
    r=subprocess.run(vcmd,env=env,capture_output=True,text=True)
    (OUT/'vu-build.log').write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr)
    if r.returncode:sys.exit('build_graphics: vu.wasm: link failed')
    sys.path.insert(0,str(ROOT/'port/web'))
    import check_web_layout as layout
    m=layout.parse('vu',OUT/'vu-raw.wasm')
    bad=sorted({(mod,f) for mod,f,_ in m.imports if mod!='wasi_snapshot_preview1'}-{('env','memory'),('env','host_wall_seconds')})
    if bad:sys.exit('build_graphics: vu.wasm imports %s (undefined in the VU worker\'s module?)'%bad)
    helper=OUT/'memory-vu.wasm'
    builtins=next(WASI.glob('lib/clang/*/lib/wasm32-unknown-wasi/libclang_rt.builtins.a'))
    subprocess.run([str(WASI/'bin/clang.exe'),'-O2','-msimd128','-mbulk-memory','-ffp-contract=off','-nostdlib','-Wl,--no-entry',
                    '-Wl,--export-all','-Wl,--import-memory','-DWEB_CSR_AT=%#xu'%csr,
                    *[str(ROOT/'port/web'/(n+'.c')) for n in ('memory','float','double')],str(builtins),'-o',str(helper)],check=True,env=env)
    from postlink import rewrite
    rewrite(OUT/'vu-raw.wasm',OUT/'vu.wasm',helper,float_ops=EXACT_FLOAT,memory=False,
            fast_memory='--legacy-memory' not in sys.argv,debug_memory='--debug-memory' in sys.argv,helper_stack=stack)
    from threads import share
    if share(OUT/'vu.wasm',OUT/'vu.wasm')!=1:sys.exit('build_graphics: vu.wasm has no memory import to share')
    print('build_graphics: vu.wasm, the VU worker\'s module (%d VU1 calls)'%len([n for n in VU_OWNED if n not in FEEDS]))
from postlink import rewrite
helper,stack=OUT/'memory.wasm',0x07920000
if THREADS:
    # The float/memory helpers as postlink.py builds them for the core, on the reactor's FP control
    # word, with their private stack apart from the core's (the two run at once now).
    text=QUEUE.read_text()
    csr,stack=(int(re.search(r'#define %s (0x[0-9A-Fa-f]+)u'%n,text).group(1),16) for n in ('WEB_Q_REACTOR_CSR','WEB_Q_REACTOR_HELPER_STACK'))
    helper=OUT/'memory-graphics.wasm'
    builtins=next(WASI.glob('lib/clang/*/lib/wasm32-unknown-wasi/libclang_rt.builtins.a'))
    subprocess.run([str(WASI/'bin/clang.exe'),'-O2','-msimd128','-mbulk-memory','-ffp-contract=off','-nostdlib','-Wl,--no-entry',
                    '-Wl,--export-all','-Wl,--import-memory','-DWEB_CSR_AT=%#xu'%csr,
                    *[str(ROOT/'port/web'/(n+'.c')) for n in ('memory','float','double')],str(builtins),'-o',str(helper)],check=True,env=env)
rewrite(OUT/'graphics-raw.wasm',OUT/'graphics.wasm',helper,float_ops=EXACT_FLOAT,memory='--guarded-memory' in sys.argv,
        fast_memory='--legacy-memory' not in sys.argv,debug_memory='--debug-memory' in sys.argv,helper_stack=stack)
if THREADS:
    from threads import share
    if share(OUT/'graphics.wasm',OUT/'graphics.wasm')!=1:sys.exit('build_graphics: graphics.wasm has no memory import to share')
    build_vu(cmd)
else:
    (OUT/'vu.wasm').unlink(missing_ok=True)   # one-worker build: no stale VU module
