"""Web-only effective-address rewrite. Existing functions/table indices stay fixed."""
import argparse, collections, json, os, re, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'port/tools'))
from wasm_postlink import Module,uleb,sleb,enc_u,enc_s,instructions,I32,I64,F32,F64,V128
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches
U=enc_u
def exports(m):
    p=m.section(7)[1]; n,i=uleb(p,0); out={}
    for _ in range(n):
        size,i=uleb(p,i); name=p[i:i+size].decode();i+=size
        kind=p[i];idx,i=uleb(p,i+1)
        out[name]=(kind,idx)
    return out
def append_helpers(m,h,stack=0x07920000):
    if h.imported_funcs: raise ValueError('helper has unresolved function imports')
    base=len(m.func_types); types=len(m.types)
    # LLVM leaf functions may use locals below __stack_pointer without publishing
    # a lowered global SP. Inserting a call to a helper that uses that SP would
    # overwrite the leaf's live locals. Helpers therefore have a private stack.
    # They import no functions, cannot yield, and finish before any fiber switch.
    gs=m.section(6)
    if gs is None:raise ValueError('expected module globals')
    global_count,pos=uleb(gs[1],0)
    helper_sp=global_count
    gs[1]=U(global_count+1)+gs[1][pos:]+b'\x7f\x01\x41'+enc_s(stack)+b'\x0b'
    def addvec(sid,items):
        s=m.section(sid);n,i=uleb(s[1],0);s[1]=U(n+len(items))+s[1][i:]+b''.join(items)
    addvec(1,[b'\x60'+U(len(p))+bytes(p)+U(len(r))+bytes(r) for p,r in h.types])
    m.types.extend(h.types)
    addvec(3,[U(t+types) for t in h.func_types])
    # Helpers use only scalar locals, no table, globals, data, or start function.
    if h.section(11) or h.section(8): raise ValueError('helper has data/start')
    bodies=[]
    for k in range(len(h.bodies)):
        b,start=h.body_code(k); out=bytearray(b[:start])
        for at,op,sub,imm,end in instructions(b,start,len(b)):
            if op==0x10:
                f,_=uleb(b,imm);out+=b'\x10'+U(base+f)
            elif op in (0x23,0x24):
                g,_=uleb(b,imm)
                if g!=exports(h)['__stack_pointer'][1]:raise ValueError('helper uses non-stack global')
                out+=bytes([op])+U(helper_sp)
            else:out+=b[at:end]
        bodies.append(bytes(out))
    return base,bodies,{n:base+i for n,(kind,i) in exports(h).items() if kind==0}
def rewrite(src,dst,helper,float_ops=True,fast_memory=True,debug_memory=False,memory=True,helper_stack=0x07920000,inline_memory=True):
    # memory=False: a module that only ever sees canonical addresses (the web GS reactor, whose
    # entry points fold their guest-address arguments) keeps its native accesses, its atomics
    # (the threaded build's queue, port/web/web_queue.c) included.
    # helper_stack: the helpers' private stack top; a module that runs on another thread than
    # the core (build_graphics.py --threads) needs its own.
    # inline_memory (with fast_memory): each access tests its base against the alias intervals
    # inline and stays a native access when canonical (98.7% of the race's accesses); only alias
    # bases call the access's wrapper. Without it every access calls its wrapper (finding 087).
    m=Module(Path(src).read_bytes());h=Module(Path(helper).read_bytes())
    base,extra,func=append_helpers(m,h,helper_stack)
    wrappers={};count=collections.Counter(); original=len(m.bodies)
    def wrapper(op,sub,tail,align,width,store,typ,offset):
        key=(op,sub,tail,align,width,store,typ,offset if fast_memory else 0)
        if key in wrappers:return wrappers[key]
        params=(I32,typ,I32) if store else ((I32,V128,I32) if sub is not None and 84<=sub<=87 else (I32,I32))
        result=() if store else (V128 if sub is not None else typ,)
        sig=(params,result)
        try:t=m.types.index(sig)
        except ValueError:
            t=len(m.types);m.types.append(sig)
            s=m.section(1);n,i=uleb(s[1],0);s[1]=U(n+1)+s[1][i:]+b'\x60'+U(len(params))+bytes(params)+U(len(result))+bytes(result)
        idx=base+len(extra)
        s=m.section(3);n,i=uleb(s[1],0);s[1]=U(n+1)+s[1][i:]+U(t)
        offset_index=len(params)-1
        body=b'\x00\x20\x00\x20'+U(offset_index)+b'\x41'+enc_s(width)+b'\x10'+U(func['web_prepare'])
        if len(params)==3:body+=b'\x20\x01'
        body+=bytes([op])+(U(sub) if sub is not None else b'')+U(0)+U(0)+tail
        if store:
            body+=b'\x20\x00\x20'+U(offset_index)+b'\x41'+enc_s(width)+b'\x10'+U(func['web_finish'])
        if fast_memory:
            # These intervals contain no alias or alias boundary. Check the
            # base, not a wrapping effective address; retain the native memarg
            # and its OOB trap. Unknown/mirror pointers keep the complete helper.
            def const(x):return b'\x41'+enc_s(x if x<0x80000000 else x-0x100000000)
            def interval(lo,hi):
                lower=max(0,lo-offset);upper=hi-offset-width
                if upper<lower:return b''
                return b'\x20\x00'+const(lower)+b'\x6b'+const(upper-lower)+b'\x4d'
            ranges=[r for r in (interval(0,0x00400000),interval(0x02000000,0x20400000),interval(0x3a000000,0xffffffff)) if r]
            cond=b'\x41\x00' if not ranges else ranges[0]+b''.join(r+b'\x72' for r in ranges[1:])
            native=b'\x20\x00'+(b'\x20\x01' if len(params)==3 else b'')
            native+=bytes([op])+(U(sub) if sub is not None else b'')+U(align)+U(offset)+tail
            if debug_memory:
                native=b'\x20\x00'+const(offset)+const(width)+b'\x10'+U(func['web_assert_unfolded'])+native
            if inline_memory:
                # The call site has already tested the canonical intervals (inline_test), so only
                # alias addresses arrive here. A span wholly inside the low view or the complete
                # 0x38000000 window (the two the game uses: 1.1% and 0.3% of the race's accesses)
                # folds by a constant: one native access at a+offset+delta, memarg offset 0 (the
                # sum stays in RAM, no wrap). Everything else keeps the complete checked helper.
                def folded(lo,hi,delta):
                    test=interval(lo,hi)
                    if not test:return b'',b''
                    acc=b'\x20\x00'+const((offset+delta)&0xffffffff)+b'\x6a'
                    if debug_memory:
                        acc=b'\x20\x00'+const(offset)+const(width)+const(delta&0xffffffff)+b'\x10'+U(func['web_assert_folded'])+acc
                    acc+=(b'\x20\x01' if len(params)==3 else b'')+bytes([op])+(U(sub) if sub is not None else b'')+U(align)+U(0)+tail
                    return test+b'\x04'+(bytes(result) if result else b'\x40')+acc+b'\x05',b'\x0b'
                heads,ends=zip(folded(0x00400000,0x02000000,0x08000000),folded(0x38000000,0x3a000000,-0x30000000))
                body=b'\x00'+b''.join(heads)+body[1:]+b''.join(ends)
            else:
                body=b'\x00'+cond+b'\x04'+(bytes(result) if result else b'\x40')+native+b'\x05'+body[1:]+b'\x0b'
        extra.append(body+b'\x0b');wrappers[key]=idx;return idx
    def inline_test(offset,width):
        """(lower, span) of each base interval whose access would touch an alias window: the
        complement of wrapper()'s canonical intervals [0,4M), [32M,0x20400000), [0x3a000000,4G).
        A base outside all of them is canonical; the access stays native, with its own OOB trap."""
        tests=[]
        for lo,hi in ((0x00400000,0x02000000),(0x20400000,0x3a000000)):
            lower=max(0,lo-offset-width+1);upper=hi-offset-1
            if upper>=lower:tests.append((lower,upper-lower))
        return tests
    types=[I32,I64,F32,F64,I32,I32,I32,I32,I64,I64,I64,I64,I64,I64,I32,I64,F32,F64,I32,I32,I64,I64,I64]
    widths=[4,8,4,8,1,1,2,2,1,1,2,2,4,4,4,8,4,8,1,2,1,2,4]
    rewritten=[]
    float_names={0x91:'sqrt',0x92:'add',0x93:'sub',0x94:'mul',0x95:'div',
                 0xb2:'i32',0xb3:'u32',0xb4:'i64',0xb5:'u64',0xb6:'demote'}
    vector_names={0xe3:'sqrt',0xe4:'add',0xe5:'sub',0xe6:'mul',0xe7:'div',0xfa:'i32',0xfb:'u32'}
    double_names={0x9f:'dsqrt',0xa0:'dadd',0xa1:'dsub',0xa2:'dmul',0xa3:'ddiv',0xb9:'di64',0xba:'du64',0xbb:'promote'}
    for k in range(original):
        b,start=m.body_code(k);out=bytearray()
        decoded=list(instructions(b,start,len(b)))
        # Scratch locals of the inlined canonical tests, appended after the function's own: one
        # i32 for the base, one per type of stored (or lane) value. Each test is used up within
        # its own access, so one local per type serves the whole function.
        groups,p=uleb(b,0);own=len(m.types[m.func_types[m.imported_funcs+k]][0])
        for _ in range(groups):
            c,p=uleb(b,p);own+=c;p+=1
        scratch={}
        def local(t):  # t: 'base' or the value type
            if t not in scratch:scratch[t]=own+len(scratch)
            return U(scratch[t])
        for position,(at,op,sub,imm,end) in enumerate(decoded):
            if memory and (0x28<=op<=0x3e or op==0xfd and (sub<=11 or 84<=sub<=93)):
                align,p=uleb(b,imm)
                if align&0x40: raise ValueError('multi-memory unsupported')
                offset,p=uleb(b,p);tail=b[p:end]
                if offset>0xffffffff:raise ValueError('memory64 offset')
                if op==0xfd:
                    store=sub==11 or 88<=sub<=91
                    width=([16,8,8,8,8,8,8,1,2,4,8,16][sub] if sub<=11 else (1<<(sub-84)%4 if sub<=91 else (4 if sub==92 else 8)))
                    typ=V128
                else:store=op>=0x36;typ=types[op-0x28];width=widths[op-0x28]
                # Local proof, requiring no assumptions about game pointer
                # creation or stored pointer bits: literal address, with at
                # most one independent value push for a scalar store.
                prior=position-1-(1 if store else 0)
                literal=None
                if fast_memory and op!=0xfd and prior>=0 and decoded[prior][1]==0x41:
                    if not store or decoded[position-1][1] in (0x20,0x23,0x41,0x42,0x43,0x44):
                        literal=sleb(b,decoded[prior][3])[0]&0xffffffff
                def canonical(a):
                    e=a+offset
                    return (e+width<=0x00400000) or (e>=0x02000000 and e+width<=0x20400000) or (e>=0x3a000000 and e+width<=0xffffffff)
                if literal is not None and canonical(literal):
                    if debug_memory:
                        out+=b'\x41'+enc_s(literal if literal<0x80000000 else literal-0x100000000)+b'\x41'+enc_s(offset)+b'\x41'+enc_s(width)+b'\x10'+U(func['web_assert_unfolded'])
                    out+=b[at:end];count['constant_bypass']+=1
                    continue
                inline=fast_memory and inline_memory
                tests=inline_test(offset,width) if inline else None
                if inline and not tests:
                    # No base reaches an alias window with this offset and width.
                    out+=b[at:end];count['offset_bypass']+=1;continue
                call=b'\x41'+enc_s(offset if offset<0x80000000 else offset-0x100000000)+b'\x10'+U(wrapper(op,sub,tail,align,width,store,typ,offset))
                count['memory_accesses']+=1
                if not inline:
                    out+=call;continue
                # if base is canonical: the original access (native memarg, native OOB trap);
                # else: the wrapper, which folds or takes the checked helper.
                value=store or (op==0xfd and 84<=sub<=87)
                a=local('base');v=local(typ) if value else b''
                if value:out+=b'\x21'+v
                out+=b'\x22'+a
                for n,(lower,span) in enumerate(tests):
                    if n:out+=b'\x20'+a
                    if lower:out+=b'\x41'+enc_s(lower)+b'\x6b'
                    out+=b'\x41'+enc_s(span if span<0x80000000 else span-0x100000000)+b'\x4b'
                    if n:out+=b'\x71'
                push=b'\x20'+a+(b'\x20'+v if value else b'')
                native=push+b[at:end]
                if debug_memory:
                    native=b'\x20'+a+b'\x41'+enc_s(offset if offset<0x80000000 else offset-0x100000000)+b'\x41'+enc_s(width)+b'\x10'+U(func['web_assert_unfolded'])+native
                out+=b'\x04'+(b'\x40' if store else bytes([V128 if op==0xfd else typ]))+native+b'\x05'+push+call+b'\x0b'
                count['inline_tests']+=1
            elif float_ops and op in float_names:
                out+=b'\x10'+U(func['web_'+float_names[op]]);count['float_ops']+=1
            elif float_ops and op in double_names:
                out+=b'\x10'+U(func['web_'+double_names[op]]);count['double_ops']+=1
            elif float_ops and op==0xfd and sub in vector_names:
                out+=b'\x10'+U(func['web_v'+vector_names[sub]]);count['vector_float_ops']+=1
            elif memory and op==0xfc and sub in (10,11):
                out+=b'\x10'+U(func['web_copy' if sub==10 else 'web_fill']);count['bulk']+=1
            elif op==0xfe and memory or op==0xfc and sub==8:
                raise ValueError('atomic/memory.init needs an explicit web lowering')
            else:out+=b[at:end]
        _,first=uleb(b,0)
        added=b''.join(U(1)+bytes([I32 if t=='base' else t]) for t,_ in sorted(scratch.items(),key=lambda x:x[1]))
        rewritten.append(U(groups+len(scratch))+b[first:start]+added+bytes(out))
    all_bodies=rewritten+extra
    m.section(10)[1]=U(len(all_bodies))+b''.join(U(len(b))+b for b in all_bodies)
    # Keep helper names in sampling profiles; Binaryen remaps this section.
    names=dict(m.names)
    names.update({base+k:'web_helper_'+h.name(k) for k in range(len(h.bodies))})
    names.update({i:'web_access_'+str(i) for i in wrappers.values()})
    entries=U(len(names))+b''.join(U(i)+U(len(n.encode()))+n.encode() for i,n in sorted(names.items()))
    m.sections=[s for s in m.sections if not(s[0]==0 and s[2]=='name')]
    m.sections.append([0,b'\x04name\x01'+U(len(entries))+entries,'name'])
    Path(dst).write_bytes(m.encode(drop_debug=True))
    report=dict(count,wrappers=len(wrappers),input_bytes=Path(src).stat().st_size,output_bytes=Path(dst).stat().st_size)
    Path(str(dst)+'.json').write_text(json.dumps(report,indent=2));print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('input');p.add_argument('output');p.add_argument('--no-float',action='store_true');p.add_argument('--legacy-memory',action='store_true');p.add_argument('--debug-memory',action='store_true');p.add_argument('--no-inline-memory',action='store_true');a=p.parse_args()
    out=ROOT/'build/web';out.mkdir(parents=True,exist_ok=True)
    compiler=str(DEVCACHES / 'wasi-sdk/wasi-sdk-34.0-x86_64-windows/bin/clang.exe')
    helper=out/'memory.wasm'
    builtins=next(Path(compiler).parents[1].glob('lib/clang/*/lib/wasm32-unknown-wasi/libclang_rt.builtins.a'))
    import objcache  # the helper module through the object cache (objcache.py, "Whole modules")
    env=dict(os.environ,TEMP=str(out),TMP=str(out))
    cmd=[compiler,'-O2','-msimd128','-mbulk-memory','-ffp-contract=off','-nostdlib','-Wl,--no-entry','-Wl,--export-all','-Wl,--import-memory',*[str(Path(__file__).with_name(n+'.c')) for n in ('memory','float','double')],str(builtins),'-o',str(helper)]
    if objcache.compile(cmd,helper,lambda c:subprocess.run(c,env=env),extra=objcache.toolchain(compiler),env=env).returncode:sys.exit('postlink: %s: build failed'%helper)
    rewrite(a.input,a.output,helper,not a.no_float,not a.legacy_memory,a.debug_memory,inline_memory=not a.no_inline_memory)
