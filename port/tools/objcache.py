"""Object cache shared by every PC build on this machine (finding 082, "Object cache"): build_wasm.py
(wasm32) and build_host64.py (x64) look an object up before they compile it, so a unit already
compiled in any worktree is copied instead of compiled again.

Key: SHA-256 of the compiler (resolved path, size, mtime, --version), the command line without the
output path, the working directory, the PREPROCESSED source (the same command with -E, or /E for
clang-cl: every header's text and every macro is in it) and the caller's extra inputs (the
Python IR rewriters of the two-step game units). A change to any of them gives another key; there
is no partial match. The worktree root (and a caller's other build folders) are written as tokens
in the key, so worktrees share entries; an object whose bytes name one of those folders is never
stored (it would carry another checkout's path).

Whole modules: the browser build (port/web) compiles and links each of its wasm modules in one
clang call; such a call goes through compile() as it is, the preprocessed text of all its sources
standing for the sources, and toolchain() adding the linker, the sysroot's libraries and the
compiler runtime to the key. Its entry is the linked module.

Entries: DIR/<2 hex>/<sha256>.o, written to a temp file and renamed, so a parallel reader sees a
whole object or none. A hit is copied, not hardlinked: build_wasm.py renames symbols in place
in its objects. Compiler messages (warnings) are not stored: a hit prints none.
At the end of a build that stored objects, at most once in DHD_OBJCACHE_PRUNE_HOURS (default 24)
on the machine, the cache is cut back to 90% of DHD_OBJCACHE_GB (default 30) by dropping the least
recently used entries (a hit touches its entry).

    DHD_OBJCACHE=0         compile everything, read and write nothing
    DHD_OBJCACHE_DIR=PATH  another cache folder (default DHD_DEVCACHES\\dhd-objcache, DHD_DEVCACHES defaulting to ~/DevCaches; never C:)"""
import atexit, hashlib, os, re, shutil, subprocess, sys, threading, time
from pathlib import Path
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIR = os.environ.get('DHD_OBJCACHE_DIR', str(DEVCACHES / 'dhd-objcache'))
ENABLED = os.environ.get('DHD_OBJCACHE', '1') != '0'
LIMIT = int(float(os.environ.get('DHD_OBJCACHE_GB', '30')) * 2**30)
VERSION = b'dhd-objcache 1'
if os.path.splitdrive(os.path.abspath(DIR))[0].upper() == 'C:' or (os.name != 'nt' and 'DHD_OBJCACHE_DIR' not in os.environ):
    ENABLED = False  # the system drive stays free (global rule): no cache rather than one on C:; off Windows only a named one

_lock = threading.Lock()
_stats = {'hit': 0, 'miss': 0, 'uncached': 0}
_compilers = {}


class Result:  # what a caller's compile step returns on a hit (subprocess.CompletedProcess's fields)
    returncode, stdout, stderr = 0, '', ''


def _count(what):
    with _lock:
        _stats[what] += 1


def _compiler(exe):
    exe = str(exe)
    with _lock:
        if exe in _compilers:
            return _compilers[exe]
    path = os.path.realpath(shutil.which(exe) or exe)
    st = os.stat(path)
    version = subprocess.run([path, '--version'], capture_output=True).stdout
    ident = b'%s\0%d\0%d\0%s' % (os.path.basename(exe).lower().encode(), st.st_size, st.st_mtime_ns, version)
    with _lock:
        _compilers[exe] = ident
    return ident


def _pattern(path):
    """A folder in any spelling: either slash, doubled backslashes (C strings), any case."""
    parts = [re.escape(p.encode()) for p in re.split(r'[\\/]+', os.path.abspath(str(path))) if p]
    return re.compile(rb'(?:\\\\|\\|/)+'.join(parts) + rb'(?![\w-])', re.I)


def _folders(roots):
    """[(pattern, token, path)]: the worktree root and a caller's other build folders."""
    return [(_pattern(p), t.encode(), os.path.abspath(str(p)))
            for p, t in [(ROOT, '<root>')] + [(str(p), '<%s>' % t) for p, t in roots]]


def _norm(data, folders):
    if isinstance(data, str):
        data = data.encode('utf-8', 'surrogateescape')
    for pattern, token, _ in folders:
        data = pattern.sub(lambda m: token, data)
    return data


def preprocess_command(cmd, out):
    """The compile command as a preprocessor run to stdout: no -c/-S/-emit-llvm, no output path."""
    cl = os.path.basename(str(cmd[0])).lower().startswith('clang-cl')
    pp, skip = [], 0
    for i, arg in enumerate(map(str, cmd)):
        if skip:
            skip -= 1
            continue
        if arg in ('-c', '/c', '-S', '-emit-llvm') or arg.startswith(('/Fo', '-mexec-model=')):
            continue  # -mexec-model: the link's start file only (wasm), and an error with -E
        if arg == '-Xclang' and i + 1 < len(cmd) and str(cmd[i + 1]) == '-disable-llvm-passes':
            skip = 1
            continue
        if arg == '-o':
            skip = 1
            continue
        pp.append(arg)
    return pp + ['/E' if cl else '-E']


def key(cmd, out, content, extra=(), roots=(), cwd=None):
    """The entry name for compiling `content` (preprocessed source or IR) with `cmd` into `out`."""
    folders = _folders(roots)
    h = hashlib.sha256(VERSION)
    def part(data):
        data = _norm(data, folders)
        h.update(b'%d:' % len(data)); h.update(data)
    part(_compiler(cmd[0]))
    outs = {str(out), '/Fo' + str(out)}
    part('\0'.join(a for a in map(str, cmd[1:]) if a not in outs))
    part(os.path.abspath(cwd or os.getcwd()))
    part(sys.version)
    for e in extra:
        part(e)
    part(content)
    return h.hexdigest()


def file_hashes(*paths):
    """Extra key input: the text of the tools that rewrite a unit between its compile steps."""
    return [hashlib.sha256(open(p, 'rb').read()).hexdigest() + ' ' + os.path.basename(p) for p in paths]


def _entry(k):
    return os.path.join(DIR, k[:2], k + '.o')


def fetch(k, out):
    """Copy entry k to out; False when there is none."""
    path = _entry(k)
    try:
        tmp = '%s.%d.%d.tmp' % (out, os.getpid(), threading.get_ident())
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        shutil.copyfile(path, tmp)
        os.replace(tmp, out)
    except OSError:
        _count('miss')
        return False
    try:
        os.utime(path)  # recently used: pruned last
    except OSError:
        pass
    _count('hit')
    return True


def store(k, out, roots=()):
    """Add out as entry k (after a successful compile); an object naming a build folder is not."""
    try:
        data = open(out, 'rb').read()
    except OSError:
        return
    if any(p.search(data) for p, _, _ in _folders(roots)):
        _count('uncached')
        return
    path = _entry(k)
    tmp = '%s.%d.%d.tmp' % (path, os.getpid(), threading.get_ident())
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(tmp, 'wb') as f:
            f.write(data)
        os.replace(tmp, path)
    except OSError:  # another builder holds that entry open: theirs is the same object
        try:
            os.remove(tmp)
        except OSError:
            pass


# Direct mode: a manifest per command and source text (DIR/m/) lists every file the preprocessor
# read, with its SHA-256, and the paths on earlier include directories that must stay absent (a new
# header there would shadow one of them). When all still hold, the manifest gives the key without
# running the preprocessor; anything else falls back to it (and writes the manifest again).
DIRECT = os.environ.get('DHD_OBJCACHE_DIRECT', '1') != '0'
MARKER = re.compile(rb'^#(?:line)? \d+ "((?:[^"\\]|\\.)*)"', re.M)
INCLUDE_ENV = ('INCLUDE', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'EXTERNAL_INCLUDE')
_hashes = {}


def _file_sha(path):
    try:
        st = os.stat(path)
    except OSError:
        return None
    with _lock:
        known = _hashes.get(path)
    if known and known[:2] == (st.st_size, st.st_mtime_ns):
        return known[2]
    sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    with _lock:
        _hashes[path] = (st.st_size, st.st_mtime_ns, sha)
    return sha


def _search_dirs(cmd, cwd):
    """The -I/-isystem/-iquote//imsvc folders in search order."""
    dirs, args = [], list(map(str, cmd))
    for i, arg in enumerate(args):
        for flag in ('-isystem', '-iquote', '/imsvc', '-imsvc', '-I', '/I'):
            if arg == flag and i + 1 < len(args):
                dirs.append(args[i + 1])
                break
            if arg.startswith(flag) and len(arg) > len(flag):
                dirs.append(arg[len(flag):])
                break
    return [os.path.normcase(os.path.abspath(os.path.join(cwd or os.getcwd(), d))) for d in dirs]


def _manifest_key(cmd, out, extra, folders, cwd, env):
    h = hashlib.sha256(VERSION + b' manifest')
    def part(data):
        data = _norm(data, folders)
        h.update(b'%d:' % len(data)); h.update(data)
    part(_compiler(cmd[0]))
    outs = {str(out), '/Fo' + str(out)}
    args = [a for a in map(str, cmd[1:]) if a not in outs]
    part('\0'.join(args))
    part(os.path.abspath(cwd or os.getcwd()))
    part(sys.version)
    for e in extra:
        part(e)
    for name in INCLUDE_ENV:
        part((env if env is not None else os.environ).get(name, ''))
    for a in args:  # the source and -include files: their text picks the manifest
        path = os.path.join(cwd or os.getcwd(), a)
        if os.path.isfile(path):
            part(_file_sha(path) or '')
    return h.hexdigest()


def _manifest_path(mk):
    return os.path.join(DIR, 'm', mk[:2], mk + '.txt')


def _denorm(text, folders):
    for _, token, path in folders:
        text = text.replace(token.decode(), path)
    return text


def _direct(mk, folders):
    try:
        lines = open(_manifest_path(mk), encoding='utf-8').read().splitlines()
    except OSError:
        return None
    if not lines or len(lines[0]) != 64:
        return None
    for line in lines[1:]:
        kind, _, rest = line.partition(' ')
        if kind == 'f':
            sha, _, path = rest.partition(' ')
            if _file_sha(_denorm(path, folders)) != sha:
                return None
        elif kind == 'a':
            if os.path.exists(_denorm(rest, folders)):
                return None
        else:
            return None
    return lines[0]


def _write_manifest(mk, k, preprocessed, cmd, cwd, folders):
    base = cwd or os.getcwd()
    files = []
    for m in MARKER.finditer(preprocessed):
        name = re.sub(r'\\(.)', r'\1', m.group(1).decode('utf-8', 'surrogateescape'))
        if name.startswith('<'):
            continue
        path = os.path.normcase(os.path.abspath(os.path.join(base, name)))
        if path not in files:
            files.append(path)
    dirs = _search_dirs(cmd, cwd)
    lines, absent = [k], []
    for path in files:
        sha = _file_sha(path)
        if sha is None:
            return
        lines.append('f %s %s' % (sha, _norm(path, folders).decode('utf-8', 'surrogateescape')))
        for i, d in enumerate(dirs):  # every folder it could have been found through
            if path.startswith(d + os.sep):
                rel = path[len(d) + 1:]
                absent += [p for p in (os.path.join(e, rel) for e in dirs[:i]) if p not in absent and p != path]
    lines += ['a ' + _norm(p, folders).decode('utf-8', 'surrogateescape') for p in absent]
    path = _manifest_path(mk)
    tmp = '%s.%d.%d.tmp' % (path, os.getpid(), threading.get_ident())
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        os.replace(tmp, path)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def source_key(cmd, out, extra=(), roots=(), cwd=None, env=None):
    """key() of a source compile, by preprocessing it (or from its manifest, direct mode); None
    when the cache is off or the preprocessor fails."""
    if not ENABLED:
        return None
    folders = _folders(roots)
    mk = _manifest_key(cmd, out, extra, folders, cwd, env) if DIRECT else None
    k = mk and _direct(mk, folders)
    if k:
        return k
    p = subprocess.run([shutil.which(str(cmd[0])) or str(cmd[0])] + preprocess_command(cmd, out)[1:],
                       capture_output=True, cwd=cwd, env=env)
    if p.returncode:
        return None
    k = key(cmd, out, p.stdout, extra, roots, cwd)
    if mk:
        _write_manifest(mk, k, p.stdout, cmd, cwd, folders)
    return k


def toolchain(compiler):
    """Extra key input for a command that also links (clang ... -o module.wasm): every file of the
    compiler's toolchain by name, size and mtime (the linker beside it, the sysroot's libraries
    and start files, the compiler runtime). The link's own flags are in the command already."""
    top = os.path.dirname(os.path.dirname(os.path.realpath(shutil.which(str(compiler)) or str(compiler))))
    h = hashlib.sha256()
    for sub in ('bin', 'lib', os.path.join('share', 'wasi-sysroot', 'lib')):
        for d, dirs, names in os.walk(os.path.join(top, sub)):
            dirs.sort()
            for n in sorted(names):
                try:
                    st = os.stat(os.path.join(d, n))
                except OSError:
                    continue
                h.update(b'%s\0%d\0%d\n' % (os.path.relpath(os.path.join(d, n), top).encode('utf-8', 'replace'),
                                             st.st_size, st.st_mtime_ns))
    return ['toolchain ' + h.hexdigest()]


def compile(cmd, out, run, extra=(), roots=(), cwd=None, env=None, hit=None):
    """run(cmd) unless the object is cached; a miss that leaves out behind is stored.
    run's result on a compile, `hit` (default a Result with returncode 0) on a hit."""
    k = source_key(cmd, out, extra, roots, cwd, env)
    if k is None:
        if ENABLED:
            _count('uncached')
        return run(cmd)
    if fetch(k, str(out)):
        return Result() if hit is None else hit
    try:
        if os.path.exists(str(out)):
            os.remove(str(out))  # a failed compile must not leave the old object to be stored
    except OSError:
        pass
    r = run(cmd)
    if getattr(r, 'returncode', 0) == 0 and os.path.exists(str(out)):
        store(k, str(out), roots)
    return r


def tool_output(cmd, src, out, run, roots=()):
    """A deterministic tool's run from one input file to one output file (the browser build's
    wasm-opt passes, finding 151): run(cmd) unless an earlier run of the same tool (resolved path,
    size, mtime, --version), the same arguments (src and out as tokens) and src's same bytes left
    its output in the cache. Returns what compile() returns."""
    if not ENABLED:
        return run(cmd)
    h = hashlib.sha256(VERSION + b' tool')
    def part(data):
        data = _norm(data, _folders(roots))
        h.update(b'%d:' % len(data)); h.update(data)
    part(_compiler(cmd[0]))
    tokens = {os.path.abspath(str(src)): '<in>', os.path.abspath(str(out)): '<out>'}
    part('\0'.join(tokens.get(os.path.abspath(a), a) for a in map(str, cmd[1:])))
    part(hashlib.sha256(open(src, 'rb').read()).hexdigest())
    k = h.hexdigest()
    if fetch(k, str(out)):
        return Result()
    r = run(cmd)
    if getattr(r, 'returncode', 0) == 0 and os.path.exists(str(out)):
        store(k, str(out), roots)
    return r


def prune(limit=LIMIT):
    """Drop the least recently used entries until the cache is under 90% of limit."""
    entries = []
    for d, _, names in os.walk(DIR):
        for n in names:
            p = os.path.join(d, n)
            try:
                st = os.stat(p)
            except OSError:
                continue
            entries.append((st.st_mtime, st.st_size, p))
    total = sum(s for _, s, _ in entries)
    if total <= limit:
        return 0
    dropped = 0
    for _, size, p in sorted(entries):
        if total <= limit * 0.9:
            break
        try:
            os.remove(p)
            total -= size
            dropped += 1
        except OSError:
            pass
    return dropped


# The walk over every entry (460,000 files, 25 GB: 30 to 65 s) ran at the end of every build that
# stored an object; now at most once in PRUNE_HOURS on the machine (DIR/pruned.stamp). Between two
# walks the cache can grow past the limit by what builds store meanwhile (finding 151).
PRUNE_HOURS = float(os.environ.get('DHD_OBJCACHE_PRUNE_HOURS', '24'))


def prune_due():
    stamp = os.path.join(DIR, 'pruned.stamp')
    try:
        if time.time() - os.path.getmtime(stamp) < PRUNE_HOURS * 3600:
            return False
    except OSError:
        pass
    try:
        os.makedirs(DIR, exist_ok=True)
        open(stamp, 'w').close()  # the next build's walk waits PRUNE_HOURS, even if this one fails
    except OSError:
        pass
    return True


def summary():
    if not ENABLED or not any(_stats.values()):
        return
    dropped = prune() if _stats['miss'] and prune_due() else 0
    print('objcache: %d hits, %d misses, %d not cacheable%s (%s)' % (
        _stats['hit'], _stats['miss'], _stats['uncached'],
        ', %d old entries dropped' % dropped if dropped else '', DIR), flush=True)


atexit.register(summary)


def pe_sha(path):
    """SHA-256 of a PE file with its link time zeroed (the COFF header's TimeDateStamp, the
    checksum and the debug directories' stamps): two links of the same objects compare equal."""
    import struct
    d = bytearray(open(path, 'rb').read())
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    sections, opt_size = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    struct.pack_into('<I', d, pe + 8, 0)
    struct.pack_into('<I', d, opt + 64, 0)  # CheckSum (PE32 and PE32+)
    dirs = opt + (96 if struct.unpack_from('<H', d, opt)[0] == 0x10b else 112)
    rva, size = struct.unpack_from('<II', d, dirs + 6 * 8)  # IMAGE_DIRECTORY_ENTRY_DEBUG
    for s in range(sections):
        sec = opt + opt_size + 40 * s
        vsize, vaddr, _, raw = struct.unpack_from('<IIII', d, sec + 8)
        if size and vaddr <= rva < vaddr + vsize:
            for e in range(size // 28):
                struct.pack_into('<I', d, raw + rva - vaddr + 28 * e + 4, 0)
    return hashlib.sha256(d).hexdigest()


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='the shared object cache: size, pruning, PE comparison')
    ap.add_argument('what', choices=('stats', 'prune', 'pe-sha'))
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    if a.what == 'pe-sha':
        for f in a.files:
            print(pe_sha(f), f)
    elif a.what == 'prune':
        print('%d entries dropped' % prune())
    else:
        n = size = 0
        for d, _, names in os.walk(DIR):
            for name in names:
                n += 1
                size += os.path.getsize(os.path.join(d, name))
        print('%s: %d files, %.2f GB (limit %.0f GB)' % (DIR, n, size / 2**30, LIMIT / 2**30))
