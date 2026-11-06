"""Sampling CPU profiler for a running Windows x64 process (no admin, no WPR).

    python port/tools/sample_profile.py --pid N --seconds 30 --out build/prof/x.json
    python port/tools/sample_profile.py --run EXE --env K=V ... --seconds 30 --out ...

Every --interval ms each thread of the process that used CPU cycles since the previous tick
(QueryThreadCycleTime) is suspended, its instruction pointer read (GetThreadContext) and
resumed. Addresses are grouped by module; addresses inside the main exe are symbolized
with llvm-symbolizer (needs a PDB beside the exe: build_host64.py --debug). The report lists
self samples per function and per thread, and the inlined frames (llvm-symbolizer --inlines)
are kept so a function inlined into its caller is still visible. Only the process given (or
started here, through owned_process, which ends it and what it started) is touched.

--stacks THREAD (e.g. game) walks the whole call stack of that thread's samples instead (dbghelp's
StackWalk64 over the modules' x64 unwind data, the thread suspended while it is walked): the report
gains "stacks" (outermost frame first, inlined frames expanded) and "inclusive" (samples in which a
function is anywhere on the stack). The walk costs more per sample than a pc read, so the sampled
thread runs slower; use the shares, not the times.
"""
import argparse
import collections
import contextlib
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock
import owned_process

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
winmm = ctypes.WinDLL('winmm')

TH32CS_SNAPTHREAD = 4
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040 | 0x0800  # suspend/resume, get context, query, query limited
PROCESS_ACCESS = 0x0400 | 0x0010  # query information, vm read
CONTEXT_AMD64_CONTROL = 0x00100001


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ThreadID', wintypes.DWORD),
                ('th32OwnerProcessID', wintypes.DWORD), ('tpBasePri', wintypes.LONG),
                ('tpDeltaPri', wintypes.LONG), ('dwFlags', wintypes.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [('lpBaseOfDll', ctypes.c_void_p), ('SizeOfImage', wintypes.DWORD), ('EntryPoint', ctypes.c_void_p)]


k32.OpenThread.restype = wintypes.HANDLE
k32.OpenProcess.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.SuspendThread.argtypes = [wintypes.HANDLE]
k32.ResumeThread.argtypes = [wintypes.HANDLE]
k32.GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
k32.QueryThreadCycleTime.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_ulonglong)]
k32.GetThreadDescription.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                       ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, wintypes.HMODULE, ctypes.POINTER(MODULEINFO), wintypes.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32(); entry.dwSize = ctypes.sizeof(entry)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            out.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return out


def modules_of(process):
    arr = (wintypes.HMODULE * 1024)(); need = wintypes.DWORD()
    psapi.EnumProcessModulesEx(process, arr, ctypes.sizeof(arr), ctypes.byref(need), 3)
    out = []
    for i in range(need.value // ctypes.sizeof(wintypes.HMODULE)):
        info = MODULEINFO(); name = ctypes.create_unicode_buffer(260)
        if psapi.GetModuleInformation(process, arr[i], ctypes.byref(info), ctypes.sizeof(info)):
            psapi.GetModuleBaseNameW(process, arr[i], name, 260)
            out.append((info.lpBaseOfDll or 0, info.SizeOfImage, name.value))
    return sorted(out)


class Thread:
    def __init__(self, tid):
        self.tid = tid
        self.handle = k32.OpenThread(THREAD_ACCESS, False, tid)
        self.cycles = 0
        self.name = ''
        self.sp = 0
        if self.handle:
            p = ctypes.c_wchar_p()
            if k32.GetThreadDescription(self.handle, ctypes.byref(p)) >= 0 and p.value:
                self.name = p.value
        self.buf = ctypes.create_string_buffer(1232 + 16)
        self.ctx = (ctypes.addressof(self.buf) + 15) & ~15

    def busy(self):
        c = ctypes.c_ulonglong()
        if not k32.QueryThreadCycleTime(self.handle, ctypes.byref(c)):
            return None
        d = c.value - self.cycles; self.cycles = c.value
        return d

    def pc(self):
        if k32.SuspendThread(self.handle) == 0xFFFFFFFF:
            return None
        try:
            ctypes.memset(self.ctx, 0, 1232)
            ctypes.c_uint32.from_address(self.ctx + 0x30).value = CONTEXT_AMD64_CONTROL
            if not k32.GetThreadContext(self.handle, self.ctx):
                return None
            self.sp = ctypes.c_uint64.from_address(self.ctx + 0x98).value  # Rsp
            return ctypes.c_uint64.from_address(self.ctx + 0xF8).value  # Rip
        finally:
            k32.ResumeThread(self.handle)


k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def return_chain(process, sp, exe_base, exe_size, cache, depth=4):
    """Up to depth stack words above sp that look like return addresses: inside the exe and just
    after a call instruction (E8 rel32, FF /2 with a register, or FF 15/FF 14 24-style forms). A
    heuristic: stale words can appear, so read the result as a statistical caller chain."""
    buf = (ctypes.c_uint64 * 512)(); got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(process, ctypes.c_void_p(sp), buf, ctypes.sizeof(buf), ctypes.byref(got)):
        return ()
    out = []
    for q in buf[:got.value // 8]:
        if not exe_base + 8 <= q < exe_base + exe_size:
            continue
        if q not in cache:
            code = (ctypes.c_ubyte * 8)()
            ok = k32.ReadProcessMemory(process, ctypes.c_void_p(q - 8), code, 8, ctypes.byref(got))
            b = bytes(code) if ok else b''
            cache[q] = bool(b) and (b[3] == 0xE8 or (b[6] == 0xFF and (b[7] & 0x38) == 0x10)
                                    or (b[5] == 0xFF and (b[6] & 0x38) == 0x10)
                                    or (b[2] == 0xFF and (b[3] & 0x38) == 0x10))
        if cache[q]:
            out.append(q)
            if len(out) == depth:
                break
    return tuple(out)


class ADDRESS64(ctypes.Structure):
    _fields_ = [('Offset', ctypes.c_uint64), ('Segment', ctypes.c_uint16), ('Mode', ctypes.c_uint32)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [('AddrPC', ADDRESS64), ('AddrReturn', ADDRESS64), ('AddrFrame', ADDRESS64),
                ('AddrStack', ADDRESS64), ('AddrBStore', ADDRESS64), ('FuncTableEntry', ctypes.c_void_p),
                ('Params', ctypes.c_uint64 * 4), ('Far', wintypes.BOOL), ('Virtual', wintypes.BOOL),
                ('Reserved', ctypes.c_uint64 * 3), ('KdHelp', ctypes.c_uint64 * 32)]  # KDHELP64, with room


class StackWalker:
    """Whole call stacks of a suspended thread through dbghelp's StackWalk64 (the x64 unwind data of
    every module), for --stacks. The thread stays suspended while it is walked."""
    CONTEXT_FULL = 0x10000B

    def __init__(self, process, depth):
        self.process, self.depth = process, depth
        self.dbghelp = ctypes.WinDLL('dbghelp', use_last_error=True)
        self.dbghelp.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
        self.dbghelp.StackWalk64.argtypes = [wintypes.DWORD, wintypes.HANDLE, wintypes.HANDLE,
                                             ctypes.POINTER(STACKFRAME64), ctypes.c_void_p, ctypes.c_void_p,
                                             ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        self.dbghelp.SymSetOptions(0x00000002 | 0x00000004)  # deferred loads, no symbols needed: unwind data only
        if not self.dbghelp.SymInitializeW(process, None, True):
            raise OSError(ctypes.get_last_error(), 'SymInitialize')
        self.table = ctypes.cast(self.dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
        self.base = ctypes.cast(self.dbghelp.SymGetModuleBase64, ctypes.c_void_p)

    def refresh(self):
        self.dbghelp.SymRefreshModuleList(self.process)

    def walk(self, thread):
        """Return addresses, innermost first (the pc, then each return address), or None."""
        if k32.SuspendThread(thread.handle) == 0xFFFFFFFF:
            return None
        try:
            ctypes.memset(thread.ctx, 0, 1232)
            ctypes.c_uint32.from_address(thread.ctx + 0x30).value = self.CONTEXT_FULL
            if not k32.GetThreadContext(thread.handle, thread.ctx):
                return None
            frame = STACKFRAME64()
            frame.AddrPC.Offset = ctypes.c_uint64.from_address(thread.ctx + 0xF8).value   # Rip
            frame.AddrStack.Offset = ctypes.c_uint64.from_address(thread.ctx + 0x98).value  # Rsp
            frame.AddrFrame.Offset = ctypes.c_uint64.from_address(thread.ctx + 0xA0).value  # Rbp
            frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = 3  # AddrModeFlat
            out = []
            while len(out) < self.depth and self.dbghelp.StackWalk64(
                    0x8664, self.process, thread.handle, ctypes.byref(frame), ctypes.c_void_p(thread.ctx),
                    None, self.table, self.base, None):
                pc = frame.AddrPC.Offset
                if not pc or (out and pc == out[-1]):
                    break
                out.append(pc)
            return out
        finally:
            k32.ResumeThread(thread.handle)


def sample(pid, seconds, interval, alive, minimize=False, callers=None, stacks=None, depth=96):
    process = k32.OpenProcess(PROCESS_ACCESS, False, pid)
    threads = {}
    counts = collections.Counter()  # (thread name or id, pc)
    caller_counts = collections.Counter()  # (callee rva, caller return rva)
    stack_counts = collections.Counter()  # (thread name, (pc, return addresses...)) for --stacks threads
    walker = StackWalker(process, depth) if stacks else None
    mods = []
    exe_base = exe_size = 0
    targets = set(); ret_cache = {}
    if callers:
        exe_base, exe_size, _ = next((m for m in modules_of(process) if m[2].lower().endswith('.exe')), (0, 0, ''))
        targets = {exe_base + (c - 0x140000000) for c, _ in callers}
    winmm.timeBeginPeriod(1)
    sample.utc = [time.time(), None]
    end = time.perf_counter() + seconds
    last_scan = 0
    ticks = 0
    try:
        while time.perf_counter() < end and alive():
            now = time.perf_counter()
            if now - last_scan > 0.5:
                if minimize:
                    owned_process.minimize_windows([pid])
                fresh = False
                for tid in threads_of(pid):
                    if tid not in threads:
                        t = Thread(tid)
                        if t.handle:
                            t.busy(); threads[tid] = t; fresh = True
                if walker and fresh:
                    walker.refresh()
                if fresh:
                    mods = modules_of(process) or mods  # while the process lives (it may exit before the end)
                last_scan = now
            for t in list(threads.values()):
                d = t.busy()
                if d is None:
                    del threads[t.tid]; continue
                if d < 100000:  # under ~20 us of cycles since the last tick: idle
                    continue
                if walker and t.name in stacks:
                    chain = walker.walk(t)
                    if chain:
                        stack_counts[(t.name, tuple(chain))] += 1
                        counts[(t.name, chain[0])] += 1
                    continue
                pc = t.pc()
                if pc is not None:
                    if not t.name:  # names are set after start
                        p = ctypes.c_wchar_p()
                        if k32.GetThreadDescription(t.handle, ctypes.byref(p)) >= 0 and p.value:
                            t.name = p.value
                    counts[(t.name or 'tid%d' % t.tid, pc)] += 1
                    for start, size in callers or ():
                        a = exe_base + (start - 0x140000000)
                        if a <= pc < a + size:
                            chain = return_chain(process, t.sp, exe_base, exe_size, ret_cache)
                            caller_counts[(start, tuple(r - exe_base + 0x140000000 for r in chain))] += 1
            ticks += 1
            time.sleep(interval / 1000)
        mods = modules_of(process) or mods
    finally:
        winmm.timeEndPeriod(1)
    sample.utc[1] = time.time()
    sample.callers = caller_counts
    sample.stacks = stack_counts
    return counts, mods, ticks


def symbolize(exe, addresses, llvm):
    """address -> list of frames (innermost first), via llvm-symbolizer --inlines."""
    if not addresses:
        return {}
    tool = Path(llvm) / 'llvm-symbolizer.exe'
    text = '\n'.join('0x%x' % a for a in addresses) + '\n'
    r = subprocess.run([str(tool), '--obj=' + str(exe), '--inlines', '--functions=linkage', '--no-demangle'],
                       input=text, capture_output=True, text=True)
    blocks = r.stdout.strip('\n').split('\n\n')
    out = {}
    for a, block in zip(addresses, blocks):
        lines = block.split('\n')
        out[a] = [lines[i] for i in range(0, len(lines), 2)]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pid', type=int)
    ap.add_argument('--run', type=Path, help='start this exe (cwd = its folder) and profile it')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for --run')
    ap.add_argument('--delay', type=float, default=0, help='seconds before sampling starts')
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--interval', type=float, default=1.0, help='ms between ticks')
    ap.add_argument('--exe', type=Path, help='exe to symbolize (default: --run)')
    ap.add_argument('--llvm', default=str(Path.home() / 'scoop/apps/llvm/current/bin'))
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--minimize', action='store_true', help='keep the windows of the process minimised')
    ap.add_argument('--no-sample', action='store_true',
                    help='only run (minimised) until exit or --seconds: a timing run without the sampler')
    ap.add_argument('--callers', action='append', default=[],
                    help='ADDR:SIZE (preferred-base VA of a function): attribute its samples to direct callers')
    ap.add_argument('--stacks', action='append', default=[],
                    help='thread name (e.g. game): take whole call stacks of its samples (StackWalk64), for '
                         'inclusive times; the report gains "stacks" and "inclusive"')
    ap.add_argument('--depth', type=int, default=96, help='--stacks: frames per stack at most')
    ap.add_argument('--top', type=int, default=60)
    a = ap.parse_args()
    with contextlib.ExitStack() as owned:   # a started process ends with it
        proc = None
        if a.run:
            env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
            for item in a.env:
                k, _, v = item.partition('='); env[k] = v
            a.out.parent.mkdir(parents=True, exist_ok=True)
            owned.enter_context(game_lock.held('sample_profile', timed=True))  # samples and timing: the PC to itself
            log = owned.enter_context(open(str(a.out) + '.log', 'wb'))
            proc = owned.enter_context(owned_process.start([str(a.run.resolve())], cwd=str(a.out.parent.resolve()),
                                                           env=env, stdout=log, stderr=subprocess.STDOUT,
                                                           minimized=True))
            pid = proc.pid
        else:
            pid = a.pid
        alive = (lambda: proc.poll() is None) if proc else (lambda: True)
        # the delay, then with --no-sample the run itself (minimised)
        for seconds in (a.delay, a.seconds if a.no_sample else 0):
            end = time.perf_counter() + seconds
            while time.perf_counter() < end and alive():
                if a.minimize:
                    owned_process.minimize_windows([pid])
                time.sleep(0.2)
        if a.no_sample:
            if proc:
                proc.stop()
            print('exit', proc.returncode if proc else None)
            return
        callers = [tuple(int(x, 0) for x in c.split(':')) for c in a.callers]
        counts, mods, ticks = sample(pid, a.seconds, a.interval, alive, a.minimize, callers, set(a.stacks), a.depth)
    exe = (a.exe or a.run).resolve()
    exe_mod = next((m for m in mods if m[2].lower() == exe.name.lower()), None)
    preferred = 0x140000000
    in_exe = sorted({pc for (_, pc) in counts if exe_mod and exe_mod[0] <= pc < exe_mod[0] + exe_mod[1]})
    syms = symbolize(exe, [pc - exe_mod[0] + preferred for pc in in_exe], a.llvm) if exe_mod else {}
    self_fn = collections.Counter(); incl = collections.Counter(); per_thread = collections.Counter()
    per_mod = collections.Counter(); outer = collections.Counter()  # outer: the non-inlined function
    total = sum(counts.values())
    for (thread, pc), n in counts.items():
        per_thread[thread] += n
        if exe_mod and exe_mod[0] <= pc < exe_mod[0] + exe_mod[1]:
            frames = syms.get(pc - exe_mod[0] + preferred) or ['??']
            self_fn[(thread, frames[0])] += n
            for f in set(frames):
                incl[(thread, f)] += n
            outer[(thread, frames[-1])] += n
            per_mod[exe.name] += n
        else:
            m = next((m for m in mods if m[0] <= pc < m[0] + m[1]), None)
            name = m[2] if m else '?'
            per_mod[name] += n
            self_fn[(thread, '[' + name + ']')] += n
    report = {'pid': pid, 'ticks': ticks, 'samples': total, 'utc': getattr(sample, 'utc', None),
              'threads': per_thread.most_common(), 'modules': per_mod.most_common(),
              'self': [[t, f, n] for (t, f), n in self_fn.most_common()],
              'outer': [[t, f, n] for (t, f), n in outer.most_common(400)],
              'inlined_frames': [[t, f, n] for (t, f), n in incl.most_common(400)],
              'hot_pcs': [[t, '0x%x' % (pc - exe_mod[0] + preferred), syms.get(pc - exe_mod[0] + preferred), n]
                          for (t, pc), n in counts.most_common(400)
                          if exe_mod and exe_mod[0] <= pc < exe_mod[0] + exe_mod[1]]}
    cc = getattr(sample, 'callers', {})
    if cc:
        rets = sorted({r for (_, chain) in cc for r in chain})
        rsyms = symbolize(exe, rets, a.llvm)
        name = lambda r: (rsyms.get(r) or ['?'])[-1]
        named = collections.Counter()
        for (c, chain), n in cc.items():
            named[('0x%x' % c,) + tuple(name(r) for r in chain)] += n
        report['callers'] = [list(k) + [n] for k, n in named.most_common(300)]
        for k, n in named.most_common(40):
            print('caller %6d  %s <- %s' % (n, k[0], ' <- '.join(k[1:])))
    st = getattr(sample, 'stacks', {})
    if st and exe_mod:
        # A return address is symbolized one byte back (inside its call); each address expands to its
        # inlined frames. Stacks are written outermost first; a function counts once per sample in
        # "inclusive" however often it appears in the stack.
        def rva(pc, first):
            return pc - exe_mod[0] + preferred - (0 if first else 1)
        wanted = sorted({rva(pc, i == 0) for (_, chain) in st for i, pc in enumerate(chain)
                         if exe_mod[0] <= pc < exe_mod[0] + exe_mod[1]})
        ssyms = symbolize(exe, wanted, a.llvm)
        def names(pc, first):
            if exe_mod[0] <= pc < exe_mod[0] + exe_mod[1]:
                return ssyms.get(rva(pc, first)) or ['??']
            m = next((m for m in mods if m[0] <= pc < m[0] + m[1]), None)
            return ['[' + (m[2] if m else '?') + ']']
        stacks_out = []; inclusive = collections.Counter(); totals = collections.Counter()
        for (thread, chain), n in st.items():
            frames = [f for i, pc in enumerate(chain) for f in names(pc, i == 0)]
            stacks_out.append([thread, n, frames[::-1]])
            totals[thread] += n
            for f in set(frames):
                inclusive[(thread, f)] += n
        stacks_out.sort(key=lambda s: -s[1])
        report['stacks'] = stacks_out
        report['inclusive'] = [[t, f, n] for (t, f), n in inclusive.most_common(600)]
        for (t, f), n in inclusive.most_common(a.top):
            print('incl %6.2f%%  %-10s %s' % (100 * n / max(totals[t], 1), t[:10], f))
    a.out.write_text(json.dumps(report, indent=1))
    print('samples %d in %d ticks' % (total, ticks))
    for t, n in per_thread.most_common():
        print('thread %-24s %6.2f%%' % (t, 100 * n / max(total, 1)))
    for m, n in per_mod.most_common(8):
        print('module %-24s %6.2f%%' % (m, 100 * n / max(total, 1)))
    for (t, f), n in self_fn.most_common(a.top):
        print('%6.2f%%  %-14s %s' % (100 * n / max(total, 1), t[:14], f))


if __name__ == '__main__':
    main()
