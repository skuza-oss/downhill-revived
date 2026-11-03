// Direct wasm/JSPI proof runner. No browser, wasm2c, display or audio device.
// A threaded build (port/web/build.py --threads: the core defines a shared memory) runs as the
// browser runs it: VIF1/VU1 (vu.wasm, --vu) and the GS reactor in two worker threads
// (headless_graphics.mjs) fed through the queues of port/web/web_queue.h, the core's graphics
// imports linked to gqueue.wasm (--queue; both by default next to --graphics).
import fs from 'node:fs';
import { Worker } from 'node:worker_threads';
import path from 'node:path';
import crypto from 'node:crypto';
import { WASI } from 'node:wasi';
import { fileURLToPath } from 'node:url';
import inspector from 'node:inspector';
import { thunks } from './www/thunks.js';
import { definesSharedMemory } from './www/graphics_common.js';
const await_inspector=()=>inspector;
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const args=process.argv.slice(2), option=(name,fallback)=>{
  const i=args.indexOf(name);return i<0?fallback:args[i+1];
};
const modulePath=path.resolve(option('--module',path.join(root,'build/web/dhd_core.wasm')));
const prefix=path.resolve(option('--trace',path.join(root,'build/web/boot')));
const fields=Number(option('--fields','600'));
const traceOff=args.includes('--trace-off'),timing=args.includes('--profile-imports');
// --from F: also report fields/s from field F on (e.g. the race only); --cpu-profile: a V8 CPU
// profile of the same window in build/web/node.cpuprofile.
const from=Number(option('--from','0')),cpuProfile=args.includes('--cpu-profile');
let fromAt=null,session=null;
// --game-speed: every 2 s of wall time in a race, its race clock (60 per game second) against wall
// time: about 60 when the game runs at its own speed, lower in slow motion.
let gameSpeed=null;
if(args.includes('--game-speed')){let at=0,tick=0,updates=0;gameSpeed=()=>{if(bytes.buffer!==memory.buffer)refresh();const now=performance.now(),
  playing=dv.getInt32(0x0829DED8,true)===9&&dv.getInt32(0x0843F348,true)>=2&&dv.getInt32(0x0843F348,true)<=4,t=dv.getInt32(0x0843EEC8,true);
  updates++;if(!playing){at=0;return;}if(!at){at=now;tick=t;updates=0;return;}
  if(now-at>=2000){console.error(`game speed: ${((t-tick)*1000/(now-at)).toFixed(1)} race ticks/s, ${(updates*1000/(now-at)).toFixed(1)} updates/s`);at=now;tick=t;updates=0;}};}
fs.mkdirSync(path.dirname(prefix),{recursive:true});
const env={DHD_ISO:option('--iso',process.env.DHD_ISO||process.env.DHD_TEST_ISO||''),
  DHD_HEADLESS:'1',DHD_FRAME_CLOCK:'1',DHD_SKIP_MOVIES:'1',DHD_MEMCARD:'0',DHD_SND_OFF:'1',
  DHD_RENDER:'native',DHD_SETTINGS:'none',DHD_STATE_TRACE:prefix,DHD_STATE_STOP:String(fields),
  DHD_STATE_EXE:crypto.createHash('sha256').update(fs.readFileSync(modulePath)).digest('hex'),
  DHD_NAV:'0:triangle|cross',DHD_STATE_MASK:path.join(root,'port/tools/state_trace_mask.txt'),
  DHD_STATE_CODEPTRS:path.join(root,'port/tools/state_trace_codeptrs.txt')};
for(let i=0;i<args.length;i++) if(args[i]==='--env') {
  const text=args[++i],n=text.indexOf('=');env[text.slice(0,n)]=text.slice(n+1);
}
if(traceOff)delete env.DHD_STATE_TRACE;
if(typeof WebAssembly.Suspending!=='function')throw Error('Node JSPI required; try --experimental-wasm-jspi');
let core,graphics,queue,gfx,vuThread,memory,bytes,dv,arena=0x07000000,current=65,csr=0x1f80,field=0;
const strings=new Map(),files=new Map(),counts={},importMs={};let nextFile=1,switches=0,sameSwitches=0;
const fibers=new Map(),start=performance.now();
function refresh(){bytes=new Uint8Array(memory.buffer);dv=new DataView(memory.buffer);}
function fold(a){a>>>=0;const low=a&0x1fffffff;
  if(a>=0x38000000&&a<0x3a000000)return a-0x30000000;
  if(a>=0x30400000&&a<0x32000000)return a-0x28000000;
  if((a&0xe0000000)===0||(a&0xe0000000)===0x20000000){
    if(low>=0x08000000&&low<0x0a000000)return low;
    if(low>=0x00400000&&low<0x02000000)return low+0x08000000;
  }return a;
}
function span(a,n){if(bytes.buffer!==memory.buffer)refresh();a=fold(a);n>>>=0;if(a+n>bytes.length)throw Error(`guest span OOB ${a.toString(16)}+${n.toString(16)} / ${bytes.length.toString(16)}`);return bytes.subarray(a,a+n);}
function str(a){if(bytes.buffer!==memory.buffer)refresh();a=fold(a);const end=bytes.indexOf(0,a);if(end<0)throw Error('unterminated guest string');return Buffer.from(bytes.subarray(a,end)).toString();}
function intern(s){if(strings.has(s))return strings.get(s);const b=Buffer.from(s+'\0');const a=arena;arena+=b.length;
  if(arena>0x07800000)throw Error('environment arena exhausted');span(a,b.length).set(b);strings.set(s,a);return a;}
class Exit extends Error{constructor(code){super(`host_exit(${code})`);this.code=code;}}
let fail;
const failure=new Promise((_,reject)=>{fail=reject;});
function save(){const f=fibers.get(current);f.sp=core.__stack_pointer.value;f.csr=getCsr();}
function setCsr(x){csr=x;if(memory){if(bytes.buffer!==memory.buffer)refresh();dv.setUint32(0x07800000,x,true);}}
// The FP control word lives at 0x07800000; the core's host_fp_csr/set_csr go straight to the reactor's.
function getCsr(){if(!memory)return csr;if(bytes.buffer!==memory.buffer)refresh();return dv.getUint32(0x07800000,true);}
function activate(id){const f=fibers.get(id);if(!f)throw Error(`invalid fiber ${id}`);
  current=id;core.__stack_pointer.value=f.sp;setCsr(f.csr);
  if(f.resume){const resume=f.resume;f.resume=null;resume();}
  else if(!f.started){f.started=true;WebAssembly.promising(core.__indirect_function_table.get(f.fn))(f.ctx).then(()=>fail(Error(`fiber ${id} returned`)),fail);}
  else throw Error(`fiber ${id} has no continuation`);
}
const impl={
  host_getenv:a=>env[str(a)]?intern(env[str(a)]):0,
  host_putenv_s:(a,b)=>{env[str(a)]=str(b);},
  host_putenv:a=>{const s=str(a),i=s.indexOf('=');env[s.slice(0,i)]=s.slice(i+1);},
  host_last_error:()=>0,host_exit:code=>{throw new Exit(code);},
  host_error_box:(a,b)=>{throw Error(`${str(a)}: ${str(b)}`);},
  host_exe_path:(a,n)=>{const b=Buffer.from(path.join(root,'build/web/dhd.js')+'\0');span(a,n).set(b.subarray(0,n));return Math.min(n-1,b.length-1);},
  host_display_hz:()=>60,host_monitor_aspect:()=>16/9,
  // --env DHD_FPS_WALL=1: real time, as www/worker.js (the high-rate step follows it); else the
  // deterministic virtual clock.
  host_wall_seconds:env.DHD_FPS_WALL==='1'?()=>performance.now()/1000:()=>field/60,host_cycles:()=>0n,
  host_fp_csr:getCsr,host_fp_set_csr:setCsr,host_fp_mode:x=>{setCsr(x?0xffc0:0x1f80);},
  host_frame_context:(f)=>{field=Number(f);if(gameSpeed)gameSpeed();if(fromAt===null&&field>=from){fromAt=performance.now();if(cpuProfile){session=new (await_inspector()).Session();session.connect();session.post('Profiler.enable');session.post('Profiler.setSamplingInterval',{interval:500});session.post('Profiler.start');}}if(queue)queue.web_q_frame(f);else graphics.web_frame(f);if(field%100===0)console.error(`web field ${field}`);if(traceOff&&field>=fields)throw new Exit(0);},
  host_fiber_main:()=>{if(!fibers.has(65))fibers.set(65,{sp:core.__stack_pointer.value,csr:getCsr(),started:true});return 65;},
  host_fiber_current:()=>current,
  host_callback_fiber:(fn,ctx)=>{if(!core.__indirect_function_table.get(fn))throw Error('null callback');
    for(let id=1;id<=64;id++)if(!fibers.has(id)){
      const sp=0x06000000+id*0x40000;span(sp-0x40000+0x1000,0x3f000).fill(0);
      fibers.set(id,{fn,ctx,sp,csr:0x1f80,started:false});return id;
    }return 0;},
  host_fiber_switch:new WebAssembly.Suspending(id=>{
    if(id===current){sameSwitches++;return;}
    switches++;
    save();const f=fibers.get(current);
    return new Promise(resolve=>{f.resume=resolve;queueMicrotask(()=>{try{activate(id);}catch(e){fail(e);}});});
  }),
  host_fiber_delete:id=>{if(id===current||id===65)throw Error('delete active/root fiber');fibers.delete(id);},
  host_stack_ranges_copy:(out,max)=>{let n=0;for(const id of fibers.keys())if(id!==65){
    if(n<max){dv.setUint32(out+8*n,0x06000000+(id-1)*0x40000,true);dv.setUint32(out+8*n+4,0x06000000+id*0x40000,true);}n++;
  }return Math.min(n,max);},
  host_ram_create:n=>+(n===0x02000000),
  host_ram_view:(at,offset,n)=>+((offset===0&&n===0x02000000&&[0x08000000,0x28000000,0x38000000].includes(at>>>0))||
    (offset===0x00400000&&n===0x01c00000&&[0x00400000,0x20400000,0x30400000].includes(at>>>0))),
  host_mem_commit:(a,n)=>+((a>>>0)+n<=bytes.length),host_mem_reserved:(a,n)=>+((a>>>0)+n<=bytes.length),
  // JS cannot protect individual linear-memory pages. Refuse fault guards so
  // graphics.c uses its existing immediate readback fallback.
  host_mem_protect:(_a,_n,protection)=>+(protection===2),host_callback_fault_guard:()=>0,host_fault_guard_here:()=>0,
  host_file_open:(a,mode)=>{try{const name=str(a);if(mode&8){const id=nextFile++;files.set(id,{fd:null,pos:0});return id;}
    const fd=fs.openSync(name,mode&2?(mode&4?'w+':'r+'):'r');const id=nextFile++;files.set(id,{fd,pos:0});return id;
  }catch{return 0;}},
  host_file_close:id=>{const f=files.get(id);if(f?.fd!==null&&f)fs.closeSync(f.fd);files.delete(id);},
  host_file_read:(id,a,n)=>{const f=files.get(id);if(!f||f.fd===null)return -1n;const got=fs.readSync(f.fd,span(a,n),0,n,f.pos);f.pos+=got;return BigInt(got);},
  host_file_write:(id,a,n)=>{const f=files.get(id);if(!f)return 0;if(f.fd===null)return n;const got=fs.writeSync(f.fd,span(a,n),0,n,f.pos);f.pos+=got;return got;},
  host_file_seek:(id,off)=>{off=BigInt.asUintN(64,off);const f=files.get(id);if(!f||off>BigInt(Number.MAX_SAFE_INTEGER))return -1;f.pos=Number(off);return 0;},
  host_file_size:id=>{const f=files.get(id);return f?.fd!==null&&f?BigInt(fs.fstatSync(f.fd).size):0n;},
  host_file_flush:()=>0,
  host_console_write:(a,n)=>{process.stderr.write(Buffer.from(span(a,n)));},
  host_path_exists:a=>+fs.existsSync(str(a)),host_path_is_dir:a=>{try{return +fs.statSync(str(a)).isDirectory();}catch{return 0;}},
  host_dir_create:a=>{try{fs.mkdirSync(str(a));return 1;}catch{return 0;}},
  host_dir_open:()=>0,host_dir_next:()=>0,host_dir_close:()=>{},
  host_service_host_disc_ready:()=>0,host_service_host_skip_movies:()=>1,
  host_service_host_pad_device:(_,a)=>{span(a,18).fill(0);span(a+2,4).fill(128);},
  host_service_mouse_state:a=>{span(a,36).fill(0);},
  host_service_host_game_options_read:()=>0,host_service_host_setting_locked:()=>0,
  host_service_host_setting_save:()=>0,host_service_movie_update:()=>0,host_service_movie_playing:()=>0,
  host_service_host_poll_dump:()=>0,
  host_service_graphics_threaded:()=>1,host_service_texpack_available:()=>0,host_service_gs_scale:()=>0,
  host_pacing_period:()=>1/60,
};
const noops=`host_sleep_ms host_wait_wall host_dpi_aware host_callbacks_drain host_frame_registers host_frame_video host_capture_name
host_pacing_config host_pacing_wait host_pacing_handoff host_pacing_input host_callback_heartbeat
host_sound_open host_sound_write host_sound_reset host_sound_tick host_sound_log_stack snd_host_bench
host_service_host_init host_service_host_console_init host_service_host_settings_load host_service_host_pad_vibrate
host_service_movie_prepare host_service_movie_start host_service_movie_audio host_service_graphics_wait host_service_graphics_shutdown
host_service_gs_vsync host_service_gs_pump_messages host_service_gs_window_title host_service_gs_native_view
host_service_gs_window_apply host_service_gs_present_options_apply host_service_gs_window_close
host_service_text_input_begin host_service_text_input_end host_service_host_backtrace host_service_presence`.split(/\s+/);
for(const n of noops)impl[n]=()=>{};
impl.host_service_net_offered=()=>0;  // as www/net.js: no online in the browser (finding 143)
for(const n of ['host_service_net_racing','host_service_net_seconds','host_service_net_server_ms','host_heartbeat_eip','host_service_launcher_open'])impl[n]=()=>0;
const metadata=JSON.parse(fs.readFileSync(option('--imports',path.join(root,'build/web/imports.json'))));
const coreBytes=fs.readFileSync(modulePath),threaded=definesSharedMemory(coreBytes);
// As www/worker.js: the threaded core skips the waits that only order queued graphics work
// (port/hle/hle.h); --env DHD_GRAPHICS_ORDERED= keeps them.
if(threaded&&!('DHD_GRAPHICS_ORDERED' in env))env.DHD_GRAPHICS_ORDERED='1';
const mod=await WebAssembly.compile(coreBytes),imports={env:{}};
const graphicsPath=option('--graphics',path.join(root,'build/web/graphics.wasm'));
const gm=await WebAssembly.compile(fs.readFileSync(graphicsPath));
const qm=threaded?await WebAssembly.compile(fs.readFileSync(option('--queue',path.join(path.dirname(graphicsPath),'gqueue.wasm')))):null;
const vm=threaded?await WebAssembly.compile(fs.readFileSync(option('--vu',path.join(path.dirname(graphicsPath),'vu.wasm')))):null;
// As in the browser: core imports the reactor exports as web_x_* are linked through call_indirect
// thunks (no JS frame), the FP control word included; threaded, gqueue.wasm's.
const gnames=new Set(WebAssembly.Module.exports(gm).map(e=>e.name));
const lnames=threaded?new Set(WebAssembly.Module.exports(qm).map(e=>e.name)):gnames;
const direct=WebAssembly.Module.imports(mod).map(e=>e.name).filter(n=>lnames.has('web_x_'+n));
const unqueued=threaded?WebAssembly.Module.imports(mod).map(e=>e.name).filter(n=>gnames.has('web_x_'+n)&&!lnames.has('web_x_'+n)):[];
if(unqueued.length)throw Error(`gqueue.wasm does not queue ${unqueued.join(', ')}`);
const table=new WebAssembly.Table({element:'anyfunc',initial:Math.max(1,direct.length)});
const thunk=direct.length?(await WebAssembly.instantiate(thunks(direct,metadata),{env:{t:table}})).instance.exports:{};
for(const entry of WebAssembly.Module.imports(mod)){
  if(entry.module!=='env'||!metadata[entry.name])throw Error(`unchecked import ${entry.module}.${entry.name}`);
  const name=entry.name;
  if(direct.includes(name)){imports.env[name]=thunk[name];continue;}
  imports.env[name]=impl[name] instanceof WebAssembly.Suspending?impl[name]:(...a)=>{
    counts[name]=(counts[name]??0)+1;
    const t=timing?performance.now():0;
    try {
    const g=graphics?.['web_x_'+name]??graphics?.[name];
    if(g){const saved=getCsr();setCsr(0x1f80);try{return g(...a);}finally{setCsr(saved);}}
    if(impl[name])return impl[name](...a);
    throw Error(`unsupported headless service ${name}(${a})`);
    }finally{if(timing)importMs[name]=(importMs[name]??0)+performance.now()-t;}
  };
}
core=(await WebAssembly.instantiate(mod,imports)).exports;memory=core.memory;refresh();
let gfxError=null;
if(threaded){
  queue=(await WebAssembly.instantiate(qm,{env:{memory}})).exports;
  direct.forEach((n,i)=>table.set(i,queue['web_x_'+n]));
  // the GS thread first: the VU thread's web_vq_init needs its queue
  const thread=(role,module)=>{const t=new Worker(new URL('./headless_graphics.mjs',import.meta.url),{workerData:{memory,module,env,abi:queue.web_q_abi(),
    preopens:{'/':path.parse(root).root},role}});
    return [t,new Promise((resolve,reject)=>{t.on('message',m=>{if(m.type==='ready')resolve();
      else if(m.type==='error'){gfxError=m.text;console.error(role+' thread:',m.text);reject(Error(m.text));fail(Error(m.text));}});
      t.on('error',e=>{gfxError=String(e?.stack||e);reject(e);fail(e);});})];};
  let ready;
  [gfx,ready]=thread('gs',gm);await ready;
  [vuThread,ready]=thread('vu',vm);await ready;
}else{
// The GS reactor has its own C globals, allocator, stack and function table,
// but imports exactly the core's memory. It never runs concurrently with EE.
const wasi=new WASI({version:'preview1',args:[],env,preopens:{'/':path.parse(root).root},returnOnExit:true});
// The browser's GS device ops are never enabled here (no web_gpu_enable): gpu_rust_web.c's
// dhd_render imports and env.web_gpu_await, each a stub that throws if it is ever called.
const noGpu=n=>()=>{throw Error(n+' without WebGPU');};
const renderStubs=Object.fromEntries(WebAssembly.Module.imports(gm).filter(i=>i.module==='dhd_render').map(i=>[i.name,noGpu('dhd_render.'+i.name)]));
const gi=await WebAssembly.instantiate(gm,{env:{memory,host_wall_seconds:impl.host_wall_seconds,
  host_fp_csr:impl.host_fp_csr,host_fp_set_csr:impl.host_fp_set_csr,
  web_gpu_await:noGpu('web_gpu_await')},dhd_render:renderStubs,wasi_snapshot_preview1:wasi.wasiImport});
graphics=gi.exports;
direct.forEach((n,i)=>table.set(i,graphics['web_x_'+n]));
// WASI.initialize requires the imported memory to be re-exported by the reactor.
wasi.initialize(gi);
}
let status=0;
try{await Promise.race([WebAssembly.promising(core.__main_argc_argv)(0,0),failure]);}
catch(e){if(e instanceof Exit)status=e.code;else{console.error(e);status=1;}}
finally{for(const f of files.values())if(f.fd!==null)fs.closeSync(f.fd);}
const end=performance.now(),seconds=(end-start)/1000;
const window=fromAt===null?null:{from,fields:field-from,fields_per_second:(field-from)/((end-fromAt)/1000)};
if(session)await new Promise(r=>session.post('Profiler.stop',(e,res)=>{fs.writeFileSync(path.join(root,'build/web/node.cpuprofile'),JSON.stringify(res.profile));r();}));
const rows=fs.existsSync(prefix+'.tsv')?fs.readFileSync(prefix+'.tsv','utf8').split('\n').filter(x=>/^\d/.test(x)).length:0;
const completed=traceOff?field:rows;
const vu=threaded?await new Promise(r=>{vuThread.on('message',m=>{if(m.type==='report')r(m);});vuThread.postMessage({type:'report'});setTimeout(()=>r({}),5000);})
  :{vu_native:graphics.web_vu_native?.(),vu_steps:graphics.web_vu_stat?.(0),vu_blocks:graphics.web_vu_stat?.(1),vu_ieee_probe:graphics.web_vu_ieee_probe&&[0,1,2,3].map(i=>graphics.web_vu_ieee_probe(i)),probe:[0,1,2,3].map(i=>graphics.web_vu_probe?.(i))};
if(gfx)await gfx.terminate();
if(vuThread)await vuThread.terminate();
const report={vu_native:vu.vu_native,vu_steps:vu.vu_steps,vu_blocks:vu.vu_blocks,vu_ieee_probe:vu.vu_ieee_probe,probe:vu.probe,threaded,graphics_error:gfxError,rows,window,completed,requested:fields,trace:!traceOff,seconds,fields_per_second:completed/seconds,exit:status,switches,sameSwitches,counts,importMs};
fs.writeFileSync(prefix+'-run.json',JSON.stringify(report,null,2));
console.error(JSON.stringify({...report,counts:undefined,importMs:undefined}));
process.exitCode=status||+(completed!==fields);
