"""Prepare a linked dhd_core.wasm for direct Node/JSPI execution.

    python port/web/build.py [CORE] [--renderer-profile release|fixloop] [--no-opt] [--no-threads] ...

CORE defaults to build/wasm/dhd_core.wasm (port/tools/build_wasm.py's output).
The graphics reactor links gpu_rust_web.c and the Rust renderer module is built into
build/web/renderer (port/web/build_renderer.py). --renderer rust is accepted for old callers (the
JS renderer, www/gpu.js, is gone: cleanup CL5, finding 088).
Threads (the default; --no-threads for one worker; findings 087 BS2, 090 SP4): the game core, VIF1/VU1
and the GS reactor in three Web Workers on one shared memory (www/worker.js, www/vu.js and
www/graphics.js, the queues of port/web/web_queue.h): the core's browser copy, the two graphics
modules (build/web/vu.wasm, graphics.wasm) and the Rust renderer get a shared memory
(port/web/threads.py; the input core, the x64 build's, is not touched), build/web/gqueue.wasm is the
core's side of the queue, and the Rust glue renews its memory views on a grown shared memory. worker.js picks the mode from the
core module's memory. check_web_layout.py's rule M2 then wants every memory shared (gqueue.wasm
present), and none in a one-worker build.
dhd.ini's rules (www/settings.js) are the crate dhd_settings (port/render/settings), built on its own
by `rustgs.py build --target wasm32-unknown-unknown --crate dhd_settings` into build/web/dhd_settings.wasm
(its own memory, none of the game's: check_web_layout.py's rule P1; finding 102), then wasm-opt -Oz.
Input stays untouched; all derived modules, metadata and scratch stay in build/web.
No proprietary input or generated module belongs in a commit.
"""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
from gen_imports import generate
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/web'
BINARYEN=(DEVCACHES / 'binaryen/binaryen-version_133/bin/wasm-opt.exe')
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input',type=Path,nargs='?',default=ROOT/'build/wasm/dhd_core.wasm',
                   help='already-linked core, with indirect-call postlink applied (default build/wasm/dhd_core.wasm)')
    p.add_argument('--renderer',choices=('rust',),default='rust',help='accepted for old callers: the Rust renderer is the only one')
    p.add_argument('--renderer-profile',choices=('release','fixloop'),default='release',help='cargo profile of the Rust renderer')
    p.add_argument('--no-opt',action='store_true',help='keep instruction wrappers for cost measurements')
    p.add_argument('--debug-memory',action='store_true',help='assert every bypassed access is not a mirror')
    p.add_argument('--legacy-memory',action='store_true',help='profile the original per-access lowering')
    p.add_argument('--exact-float',action='store_true',help="the graphics reactor's float ops through the exact helpers (build_graphics.py)")
    p.add_argument('--no-threads',dest='threads',action='store_false',help='core and graphics in one worker (the threaded build, two workers on a shared memory, is the default)')
    p.add_argument('--no-inline-memory',action='store_true',help='profile the core with every access through its wrapper (no inline canonical test)')
    a=p.parse_args();source=a.input.resolve()
    if not source.is_file():p.error('no core module at %s (build it with port/tools/build_wasm.py)'%source)
    OUT.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TEMP=str(OUT),TMP=str(OUT),TMPDIR=str(OUT))
    def run(*cmd):subprocess.run(list(map(str,cmd)),check=True,env=env,cwd=ROOT)
    sys.path.insert(0,str(ROOT/'port/tools'))
    import objcache
    def wasm_opt(*cmd):  # input first, output last; its output from the object cache for the same input (objcache.tool_output)
        objcache.tool_output(list(map(str,cmd)),cmd[1],cmd[-1],lambda c:subprocess.run(c,check=True,env=env,cwd=ROOT))
    # first: a Rust failure stops the build before the long core pass
    from build_renderer import build as build_renderer,SERVED,RENDERER
    renderer=build_renderer(a.renderer_profile,not a.no_opt,OUT)
    settings=OUT/'dhd_settings.wasm'
    run(sys.executable,ROOT/'port/tools/rustgs.py','build','--target','wasm32-unknown-unknown','--crate','dhd_settings','--profile',a.renderer_profile)
    if not a.no_opt:
        tmp=OUT/'dhd_settings-optimized.wasm'
        wasm_opt(BINARYEN,settings,'-Oz','--all-features','--disable-compact-imports','-o',tmp)
        settings.write_bytes(tmp.read_bytes());tmp.unlink()
    run(sys.executable,ROOT/'port/web/postlink.py',source,OUT/'core-web-raw.wasm',*(['--debug-memory'] if a.debug_memory else []),*(['--legacy-memory'] if a.legacy_memory else []),*(['--no-inline-memory'] if a.no_inline_memory else []))
    run(sys.executable,ROOT/'port/web/build_graphics.py',*(['--debug-memory'] if a.debug_memory else []),*(['--legacy-memory'] if a.legacy_memory else []),*(['--exact-float'] if a.exact_float else []),*(['--threads'] if a.threads else []))
    if not a.threads:(OUT/'gqueue.wasm').unlink(missing_ok=True)  # a stale one would make check_web_layout treat the build as threaded
    if a.threads:
        import threads
        if threads.share(OUT/'core-web-raw.wasm',OUT/'core-web-raw.wasm')!=1:sys.exit('build: the core defines no memory to share')
        bg,js=RENDERER/SERVED[1],RENDERER/SERVED[0]
        if threads.share(bg,bg)!=1:sys.exit('build: the Rust renderer imports no memory to share')
        print('build: threads: shared memory; Rust glue: %d cached views renewed on a new buffer'%threads.glue(js))
        # renderer-manifest.json describes the served files: as they are now
        rm=OUT/'renderer-manifest.json';info=json.loads(rm.read_text())
        info['sha256']={str(RENDERER/n):hashlib.sha256((RENDERER/n).read_bytes()).hexdigest() for n in SERVED};info['threads']=True
        rm.write_text(json.dumps(info,indent=2)+'\n')
    for src,dst in ((OUT/'core-web-raw.wasm',OUT/'dhd_core.wasm'),(OUT/'graphics.wasm',OUT/'graphics.wasm'))+(((OUT/'vu.wasm',OUT/'vu.wasm'),) if a.threads else ()):
        if not a.no_opt:
            # Compact imports are a newer proposal unsupported by Node 24.
            tmp=OUT/(dst.stem+'-optimized.wasm')
            wasm_opt(BINARYEN,src,'-O3','-g','--all-features','--disable-compact-imports','-o',tmp)
            dst.write_bytes(tmp.read_bytes())
        elif src!=dst:dst.write_bytes(src.read_bytes())
    generate(OUT/'dhd_core.wasm',OUT/'imports.json')
    files=[source,OUT/'dhd_core.wasm',OUT/'graphics.wasm']+([OUT/'gqueue.wasm',OUT/'vu.wasm'] if a.threads else [])+[RENDERER/n for n in SERVED]+[settings]
    manifest={'input':str(source),'optimized':not a.no_opt,'renderer':'rust','threads':a.threads,
              'sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},'renderer_sizes':renderer['sizes']}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
