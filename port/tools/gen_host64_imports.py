"""Generate the native boundary from host_api.h and WABT 1.0.42's module header.

No guessed ABI: unknown names, namespaces and signature mismatches are reported
and rejected. The deliberately small C declaration parser fails closed.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Native host services that take host C function pointers or return host pointer
# arrays. The module calls their guest-safe forms (host_callback_*, host_stack_ranges_copy).
NATIVE_ONLY = {'host_fiber_create', 'host_thread_detach', 'host_fault_guard',
               'host_memory_regions', 'host_heartbeat_start', 'host_stack_ranges'}


def split_args(text):
    result, start, depth = [], 0, 0
    for i, ch in enumerate(text):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch == ',' and depth == 0:
            result.append(text[start:i].strip()); start = i + 1
    result.append(text[start:].strip())
    return [] if result == ['void'] or result == [''] else result


def api_declarations(path):
    text = re.sub(r'/\*.*?\*/', '', path.read_text(), flags=re.S)
    text = re.sub(r'^\s*#.*$', '', text, flags=re.M)
    result = {}
    for match in re.finditer(r'([\w\s*]+?)\b((?:host_|snd_host_)\w+)\s*\((.*?)\)\s*;', text, re.S):
        ret, name, args = match.groups()
        result[name] = (ret.strip(), split_args(args))
    declared = set(re.findall(r'\b((?:host_|snd_host_)\w+)\s*\(', text))
    if declared != result.keys():
        raise ValueError(f'unparsed host declarations: {declared - result.keys()}')
    return result


def ctype(decl):
    if re.search(r'\(\s*\*\s*\w+\s*\)\s*\(', decl):
        return 'callback'
    return re.sub(r'\b\w+\s*$', '', decl).strip()


def wasm_type(typ):
    if '*' in typ or typ == 'callback':
        return 'u32'
    typ = ' '.join(typ.split())
    if typ in ('int', 'unsigned', 'unsigned long', 'uint32_t', 'uintptr_t',
               'size_t', 'HostFile', 'HostDir', 'HostFiber'):
        return 'u32'
    if typ in ('int64_t', 'uint64_t'):
        return 'u64'
    if typ in ('void', 'double', 'float'):
        return {'void': 'void', 'double': 'f64', 'float': 'f32'}[typ]
    raise ValueError(f'unsupported boundary type {typ!r}')


def module_declarations(path):
    text = path.read_text()
    imports = []
    for m in re.finditer(r"/\* import: '([^']+)' '([^']+)' \*/\s*(\w+) (\w+)\(([^;]+)\);", text):
        namespace, name, ret, symbol, params = m.groups()
        imports.append((namespace, name, ret, symbol, split_args(params)))
    if len(imports) != len(re.findall(r'/\* import:', text)):
        raise ValueError('non-function or unparsed wasm2c import; only env functions are supported')
    exports = []
    for m in re.finditer(r"/\* export: '([^']+)' \*/\s*([\w*]+) (\w+)\(([^;]+)\);", text):
        name, ret, symbol, params = m.groups()
        exports.append((name, ret, symbol, split_args(params)))
    if len(exports) != len(re.findall(r'/\* export:', text)):
        raise ValueError('unparsed wasm2c export declaration')
    return imports, exports


def generate(header, out, api=ROOT / 'port/host/host_api.h', linux=False):
    out.mkdir(parents=True, exist_ok=True)
    declarations = api_declarations(api)
    imports, exports = module_declarations(header)
    rows = []
    for ns, name, ret, sym, params in imports:
        status, expected = 'matched', None
        if ns != 'env' or name not in declarations:
            status = 'unknown import'
        else:
            r, args = declarations[name]
            expected = [wasm_type(r), *[wasm_type(ctype(a)) for a in args]]
            if [ret, *params[1:]] != expected:
                status = 'signature mismatch'
            # A C function pointer has no wasm32 meaning on a 64-bit host. The module
            # uses the guest-safe host_callback_* contracts (table index + guest ctx).
            if name in NATIVE_ONLY or any(ctype(arg) == 'callback' for arg in args):
                status = 'function-pointer import; the module must call host_callback_*'
        rows.append(dict(namespace=ns, name=name, actual=[ret, *params[1:]],
                         expected=expected, status=status))
    (out / 'imports.json').write_text(json.dumps(rows, indent=2) + '\n')
    report = ['| Namespace | Import | Module signature | Header signature | Status |',
              '|---|---|---|---|---|']
    report += [f"| {r['namespace']} | {r['name']} | {r['actual']} | {r['expected']} | {r['status']} |" for r in rows]
    (out / 'imports.md').write_text('\n'.join(report) + '\n')
    if any(r['status'] != 'matched' for r in rows):
        raise ValueError(f'unmatched imports; see {out / "imports.md"}')
    # The exported table is the sole route for callbacks. Preserve the entry's
    # own module_instance, and validate the complete WABT type hash first.
    names = {name: symbol for name, _, symbol, _ in exports}
    if any(name=='host_service_host_disc_ready' for _,name,*_ in imports) and 'port_disc_validate' not in names:
        raise ValueError('disc chooser requires the port_disc_validate module export')
    callbacks = any(name.startswith('host_callback_') for _, name, *_ in imports)
    if callbacks and '__indirect_function_table' not in names:
        raise ValueError('callback imports require --export-table')
    table = ['/* Generated by gen_host64_imports.py; do not edit. */',
             '#ifndef DHD_HOST64_EXPORTS_GEN_H', '#define DHD_HOST64_EXPORTS_GEN_H',
             '#include "core.h"', 'struct host64_exports {']
    for _, ret, sym, params in exports:
        table.append(f'    {ret} (*{sym})(%s);' % ', '.join(params))
    table += ['};', 'struct w2c_env { w2c_core *core; struct host64_exports exports; };',
              'static inline void host64_exports_bind(struct w2c_env *e, w2c_core *core) {',
              '    e->core = core;']
    table += [f'    e->exports.{sym} = {sym};' for _, _, sym, _ in exports]
    table += ['}']
    main = next((x for x in exports if x[0] == 'main'), None)
    if main is None:
        main = next((x for x in exports if x[0] == '__main_argc_argv'), None)
    if main:
        _, ret, sym, params = main
        if ret != 'u32' or params[1:] not in ([], ['u32', 'u32']):
            raise ValueError(f'unsupported main export signature: {main}')
        actual = ', 0, 0' if len(params) == 3 else ''
        table += ['static inline int host64_module_main(struct w2c_env *e) {',
                  f'    return (int)e->exports.{sym}(e->core{actual});', '}']
    table += ['#endif', '']
    (out / 'exports_gen.h').write_text('\n'.join(table))
    if linux:
        code = ['/* Generated by gen_host64_imports.py from host_api.h; do not edit. */',
                '#include "exports_gen.h"', '#include "bridge.h"',
                '#include "../host/host_api.h"', '#include "../hle/hle.h"',
                '#include "../net/net.h"', '#include <stdlib.h>', '#include <string.h>',
                '#include <pthread.h>',
                'static pthread_t reverse_owner; static int reverse_have;',
                'static int reverse_is_owner(void)',
                '{ return reverse_have && pthread_equal(reverse_owner, pthread_self()); }',
                'static void reverse_claim(void)',
                '{ reverse_have = 1; reverse_owner = pthread_self(); }']
    else:
        code = ['/* Generated by gen_host64_imports.py from host_api.h; do not edit. */',
                '#include "exports_gen.h"', '#include "bridge.h"',
                '#include "../host/host_api.h"', '#include "../hle/hle.h"',
                '#include "../net/net.h"', '#include <stdlib.h>', '#include <string.h>',
                '#define WIN32_LEAN_AND_MEAN', '#include <windows.h>']
    if 'port_disc_validate' in names:
        disc=next(x for x in exports if x[0]=='port_disc_validate')
        if disc[1]!='u32' or disc[3][1:]!=['u32','u32','u32']:
            raise ValueError('invalid port_disc_validate export signature')
        if linux:
            code += ['static struct w2c_env *reverse_env;',
                     'const char *game_data_read(const char *path, uint8_t **elf) {',
                     '    if (!reverse_env || !reverse_is_owner() || elf) wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);']
        else:
            code += ['static struct w2c_env *reverse_env;',
                     'static DWORD reverse_owner;',
                     'const char *game_data_read(const char *path, uint8_t **elf) {',
                     '    if (!reverse_env || reverse_owner!=GetCurrentThreadId() || elf) wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);']
        code += ['    size_t size=strlen(path)+1; if (size>UINT32_MAX) wasm_rt_trap(WASM_RT_TRAP_OOB);',
                 '    u32 p=host64_bridge_alloc((u32)size,1), error=host64_bridge_alloc(512,1);',
                 '    memcpy(host64_bridge_pointer(p,(u32)size),path,size);',
                 f'    u32 failed=reverse_env->exports.{names["port_disc_validate"]}(reverse_env->core,p,error,512);',
                 '    static char message[512];',
                 '    if (!failed) return NULL;',
                 '    const char *text=host64_bridge_string(error);',
                 '    size_t length=strlen(text); if (length>=sizeof message) wasm_rt_trap(WASM_RT_TRAP_OOB);',
                 '    memcpy(message,text,length+1); return message;', '}']
    if callbacks:
        code += [f'''static wasm_rt_funcref_t callback(struct w2c_env *e, u32 index,
        wasm_rt_func_type_t type) {{
    wasm_rt_funcref_table_t *t = e->exports.{names['__indirect_function_table']}(e->core);
    if (!type || index >= t->size || !t->data[index].func ||
        !t->data[index].func_type || memcmp(t->data[index].func_type,type,32))
        wasm_rt_trap(WASM_RT_TRAP_CALL_INDIRECT);
    return t->data[index];
}}''']
        # All callback shapes in host_api.h are declared here, and validated
        # against the actual table at invocation (including after mutation).
        # Every callback gets the guest ctx first (host_api.h, host_callback_*).
        shapes = [('void_arg', 'void', ['u32']), ('fault', 'u32', ['u32', 'u32']),
                  ('region', 'void', ['u32', 'u32', 'u32', 'u32'])]
        for label, ret, args in shapes:
            sig = ', '.join(['struct w2c_env *e', 'u32 index'] + [f'{t} a{i}' for i, t in enumerate(args)])
            types = ', '.join(['WASM_RT_I32'] * (len(args) + (ret != 'void')))
            code += [f'static {ret} call_{label}({sig}) {{',
                     f'    wasm_rt_funcref_t f = callback(e,index,wasm2c_core_get_func_type({len(args)},{int(ret != "void")}{", " if types else ""}{types}));',
                     f'    {"return " if ret != "void" else ""}(({ret} (*)(void*{", " if args else ""}{", ".join(args)}))f.func)(f.module_instance{", " if args else ""}{", ".join(f"a{i}" for i in range(len(args)))});', '}']
        if linux:
            lock_decl = 'static pthread_mutex_t pending_lock=PTHREAD_MUTEX_INITIALIZER;'
            lock_acquire, lock_release = 'pthread_mutex_lock(&pending_lock);', 'pthread_mutex_unlock(&pending_lock);'
        else:
            lock_decl = 'static SRWLOCK pending_lock=SRWLOCK_INIT;'
            lock_acquire, lock_release = 'AcquireSRWLockExclusive(&pending_lock);', 'ReleaseSRWLockExclusive(&pending_lock);'
        code += ['''typedef struct { struct w2c_env *env; u32 index, arg; } Start;
static void start_callback(void *p) {
    Start s=*(Start *)p; free(p); call_void_arg(s.env,s.index,s.arg);
}
static struct w2c_env *fault_env, *beat_env, *region_env;
static u32 fault_index, beat_index, region_index, fault_ctx, beat_ctx, region_ctx;
static int fault_callback(uint32_t at) { return call_fault(fault_env,fault_index,fault_ctx,at); }
/* Producers never enter wasm, touch its stack pointer, or validate its table.
   Capture only immutable callback identity; table validation happens on drain. */
typedef struct Pending { struct Pending *next; struct w2c_env *env;
    u32 index, arg; HostFiber resume; } Pending;
''' + lock_decl + '''
static Pending *pending_first, *pending_last;
static void enqueue(Pending *p) {
    p->next=NULL;
    ''' + lock_acquire + '''
    if (pending_last) pending_last->next=p; else pending_first=p;
    pending_last=p;
    ''' + lock_release + '''
}
static void post_start(void *p) { enqueue(p); }
static void beat_callback(void) {
    Pending *p=calloc(1,sizeof *p); if (!p) abort();
    p->env=beat_env; p->index=beat_index; p->arg=beat_ctx; enqueue(p);
}
static void pending_run(void *data) {
    Pending *p=data;
    call_void_arg(p->env,p->index,p->arg);
    host_fiber_switch(p->resume); abort();
}
static void drain_pending(void) {
    static int draining;
    if (draining) return;
    draining=1;
    /* Detach a finite batch, so producers cannot starve the EE scheduler. */
    ''' + lock_acquire + '''
    Pending *p=pending_first; pending_first=pending_last=NULL;
    ''' + lock_release + '''
    while (p) {
        Pending *next=p->next;
        p->resume=host_fiber_current();
        HostFiber f=host_fiber_create(pending_run,p);
        if (!f) wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
        host_fiber_switch(f); host_fiber_delete(f); free(p); p=next;
    }
    draining=0;
}
static void region_callback(uintptr_t lo, uintptr_t hi, int host) {
    /* Host process addresses have no wasm32 representation. Reject rather
       than silently truncate diagnostic values. */
    if (lo>UINT32_MAX || hi>UINT32_MAX) wasm_rt_trap(WASM_RT_TRAP_OOB);
    call_region(region_env,region_index,region_ctx,(u32)lo,(u32)hi,(u32)host);
}''']
    for _, name, ret, sym, params in imports:
        r, args = declarations[name]
        signature = ', '.join([params[0] + ' e'] + [f'{t} a{i}' for i, t in enumerate(params[1:])])
        code.append(f'{ret} {sym}({signature}) {{')
        code.append('    (void)e;')
        if 'port_disc_validate' in names:
            if linux:
                code += ['    if (reverse_have && !reverse_is_owner()) wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);',
                         '    reverse_env=e; reverse_claim();']
            else:
                code += ['    if (reverse_owner && reverse_owner!=GetCurrentThreadId()) wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);',
                         '    reverse_env=e; reverse_owner=GetCurrentThreadId();']
        # Explicit policies for the new address-only API. Never treat an i32
        # guest address as a native pointer without checking its entire span.
        strings = {
            'host_service_host_setting_save': (0,1),
            'host_service_host_setting_locked': (0,),
            'host_service_gs_window_title': (0,),
            'host_service_movie_start': (0,), 'host_service_text_input_edit': (2,),
            'host_service_net_init': (0,1,2,3), 'host_service_net_join_room': (0,),
            'host_service_gs_capture_request': (0,),
            'host_service_disc_check_start': (0,1), 'host_service_clipboard_set': (0,),
        }
        for i in strings.get(name, ()):
            code.append(f'    (void)host64_bridge_string(a{i});')
        spans = {
            'host_service_host_pad_device': [(1,'sizeof(HostPad)')],
            'host_service_mouse_state': [(0,'sizeof(HostMouse)')],
            'host_service_presence': [(0,'sizeof(HostPresence)')],
            'host_service_touch_field': [(0,'sizeof(HostTouch)')],
            'host_service_movie_audio': [(0,'host64_bridge_array_bytes((int32_t)a1,8)')],
            'host_service_vif_feed': [(1,'host64_bridge_array_bytes((int32_t)a2,4)')],
            'host_service_graphics_feed': [(1,'host64_bridge_array_bytes((int32_t)a2,4)')],
            'host_service_net_get_lobby': [(0,'sizeof(NetLobby)')],
            'host_service_net_rotation': [(0,'host64_bridge_array_bytes((int32_t)a1,1)')],
            'host_service_net_set_room': [(0,'28')],
            'host_service_net_send_frame': [(1,'a2')],
            'host_service_net_receive_frame': [(0,'a1')],
            'host_service_net_list_lobbies': [(0,'host64_bridge_array_bytes((int32_t)a1,sizeof(NetLobbyInfo))')],
            'host_service_disc_check_poll': [(0,'32'),(1,'a2')],
            'host_service_net_list_lobbies_poll': [(0,'host64_bridge_array_bytes((int32_t)a1,sizeof(NetLobbyInfo))')],
            'host_service_net_hash_ranges': [(0,'host64_bridge_array_bytes((int32_t)a1,8)')],
            'host_service_gs_read_image': [(0,'a1')],
            'host_service_gs_defer_read_image': [(0,'a1')],
            'host_service_gs_store_image': [(0,'112'),(1,'a2')],
            'host_service_gs_snapshot': [(0,'a1')],
            'host_service_gs_trace_pop': [(0,'host_service_gs_trace_size()')],
            'host_frame_registers': [(0,'20')], 'host_console_write': [(0,'a1')],
            'host_service_host_game_options_read': [(0,'a1')],
            'host_service_text_input_edit': [(0,'host64_bridge_array_bytes((int32_t)a1,1)')],
            'host_vu1_snapshot': [(0,'2096')], 'host_vu1_file': [(0,'1024')],
            'host_service_controls_layout': [(1,'52')],
            'host_service_controls_change': [(5,'host64_bridge_array_bytes((int32_t)a6,1)')],
            'host_service_controls_map': [(1,'16'),(2,'sizeof(HostPad)')],
        }
        for i, length in spans.get(name, []):
            code.append(f'    (void)host64_bridge_pointer(a{i},{length});')
        if name == 'host_capture_name':
            code.append('    if (a1) (void)host64_bridge_string(a0);')
        if name == 'host_service_net_set_room':
            code += ['    u32 room=*((u32 *)host64_bridge_pointer(a0,28)+6);',
                     '    if (room) (void)host64_bridge_string(room);']
        if name == 'host_service_net_hash_ranges':
            code += ['    const u32 *ranges=host64_bridge_pointer(a0,host64_bridge_array_bytes((int32_t)a1,8));',
                     '    for (u32 i=0;i<a1;i++) (void)host64_bridge_pointer(ranges[i*2],ranges[i*2+1]);']
        if name == 'host_callbacks_drain':
            code += ['    drain_pending();' if callbacks else '    /* no posted callbacks */']
        elif name == 'host_callback_detach':
            code += ['    (void)callback(e,a0,wasm2c_core_get_func_type(1,0,WASM_RT_I32));',
                     '    Pending *p=calloc(1,sizeof *p); if (!p) return 0;',
                     '    p->env=e; p->index=a0; p->arg=a1;',
                     '    int ok=host_thread_detach(post_start,p); if (!ok) free(p); return ok;']
        elif name == 'host_callback_fiber':
            code += ['    (void)callback(e,a0,wasm2c_core_get_func_type(1,0,WASM_RT_I32));',
                     '    Start *s=malloc(sizeof *s); if (!s) return 0;',
                     '    *s=(Start){e,a0,a1};',
                     '    int id=host_fiber_create(start_callback,s); if (!id) free(s); return (u32)id;']
        elif name in ('host_callback_fault_guard', 'host_callback_heartbeat', 'host_callback_memory_regions'):
            label, native, shape = {
                'host_callback_fault_guard': ('fault', 'host_fault_guard', '2,1,WASM_RT_I32,WASM_RT_I32,WASM_RT_I32'),
                'host_callback_heartbeat': ('beat', 'host_heartbeat_start', '1,0,WASM_RT_I32'),
                'host_callback_memory_regions': ('region', 'host_memory_regions',
                                                 '4,0,WASM_RT_I32,WASM_RT_I32,WASM_RT_I32,WASM_RT_I32')}[name]
            # Validate now (the type, the index), and again at every invocation.
            code += [f'    (void)callback(e,a0,wasm2c_core_get_func_type({shape}));',
                     f'    {label}_env=e; {label}_index=a0; {label}_ctx=a1;',
                     f'    {"return (u32)" if ret != "void" else ""}{native}({label}_callback);']
        elif name == 'host_code_range':
            if '__indirect_function_table' not in names:
                raise ValueError('code range requires the module table export')
            code += ['    /* Guest code addresses are table indices, never native pointers. */',
                     '    u32 *lo=host64_bridge_pointer(a0,4), *hi=host64_bridge_pointer(a1,4);',
                     '    *lo=1;',
                     f'    *hi=e->exports.{names["__indirect_function_table"]}(e->core)->size;']
        elif name == 'host_stack_ranges_copy':
            code += ['    uintptr_t (*ranges)[2]; int n=host_stack_ranges(&ranges);',
                     '    if ((int32_t)a1<0) wasm_rt_trap(WASM_RT_TRAP_OOB);',
                     '    if (n>(int32_t)a1) n=(int32_t)a1;',
                     '    u32 *out=host64_bridge_pointer(a0,host64_bridge_array_bytes(n,8));',
                     '    for (int i=0;i<n;i++) for (int j=0;j<2;j++) {',
                     '        if (ranges[i][j]>UINT32_MAX) wasm_rt_trap(WASM_RT_TRAP_OOB);',
                     '        out[i*2+j]=(u32)ranges[i][j];', '    }',
                     '    return (u32)n;']
        else:
            actual = []
            for i, a in enumerate(args):
                typ = ctype(a)
                if '*' in typ:
                    if typ == 'const char *':
                        actual.append(f'host64_bridge_string(a{i})')
                        continue
                    length = {
                        ('host_file_read', 1): 'a2', ('host_file_write', 1): 'a2',
                        ('host_exe_path', 0): 'a1',
                        ('host_sound_write', 0): 'host64_bridge_pcm_bytes((int32_t)a1)',
                        ('host_dir_open', 1): 'sizeof(HostDirEntry)',
                        ('host_dir_next', 1): 'sizeof(HostDirEntry)',
                    }.get((name, i))
                    if length is None:
                        raise ValueError(f'pointer argument needs an explicit span policy: {name} argument {i}')
                    actual.append(f'({typ})host64_bridge_pointer(a{i},{length})')
                else:
                    actual.append(f'({typ})a{i}')
            call = f'{name}({", ".join(actual)})'
            if '*' in r:
                if name != 'host_getenv':
                    raise ValueError(f'pointer return needs explicit lifetime policy: {name}')
                # The arena is never freed: hand out one copy per distinct value
                # (the module asks for the same settings every field).
                code += [f'    const char *value={call};', '    if (!value) return 0;',
                         '    static u32 seen[1024]; static unsigned count;',
                         '    size_t n=strlen(value)+1;',
                         '    for (unsigned i=0;i<count;i++)',
                         '        if (!strcmp(host64_bridge_string(seen[i]),value)) return seen[i];',
                         '    u32 p=host64_bridge_alloc((u32)n,1);',
                         '    memcpy(host64_bridge_pointer(p,(u32)n),value,n);',
                         '    if (count<1024) seen[count++]=p;', '    return p;']
            elif name == 'host_heartbeat_eip':
                code.append('    return 0; /* owner-thread drain cannot suspend itself; native PC is unknown */')
            else:
                code.append(f'    {"return (" + ret + ")" if ret != "void" else ""}{call};')
        code += ['}', '']
    (out / 'imports_gen.c').write_text('\n'.join(code))
    return rows, names


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('header', type=Path)
    p.add_argument('--out', type=Path, default=ROOT / 'port/host64')
    p.add_argument('--linux', action='store_true', help='POSIX (pthread) guards instead of Win32')
    a = p.parse_args()
    generate(a.header, a.out, linux=a.linux)
