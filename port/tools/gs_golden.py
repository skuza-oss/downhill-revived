"""GS goldens: the C renderers' outputs, recorded once per adapter and driver, and the exact compare
every later renderer change is judged by (the Rust renderer plan, sections 4.2 P0-2 and 5.2).

Changes to this file may only add conditions: no check is ever relaxed.

    python port/tools/gs_golden.py key
    python port/tools/gs_golden.py compare KIND [SOURCE] [--golden GOLD] [--capture NAME] [--backend wgpu]
    python port/tools/gs_golden.py adopt --from OLD GROUP[=SOURCE]...
    python port/tools/gs_golden.py selftest

key prints this PC's adapter key (vendor-device-driver-api) and its golden directory,
DHD_DEVCACHES/dhd-gs-golden/<key> ($GOLD).

A golden holds, one directory per group (x86_64 runs of the C renderers, Direct3D 11 and wgpu-native,
every Direct3D 11 and WebGPU result identical where both were compared):

  test-gpu/                     test_gpu's DHD_GS_VRAM_LOG on warp-early0 and hardware, d3d11 and wgpu
  test-gpu-ee/                  the same with DHD_TEST_EE_MXCSR=1 (the EE thread's MXCSR at GS start)
  replay/<cap>/<backend>/       replays of g+0 and g+3600 on d3d11 and wgpu: every field's PPM,
                                vram.txt (per-field VRAM hashes) and vram.bin (the last field's VRAM)
  replay-native/g+3600/wgpu/    the native renderer's replay (Vulkan)
  replay-native-interp/g+3600/wgpu/   the same with DHD_INTERP=1
  native-transfers/<run>/       test_native_transfers dumps (d3d11, wgpu, wgpu-no-dual)
  native-transfers-presented/<run>/   the same with --presented
  native-transfers-stress/<run>/      the same with --stress (test_native_stress.c)
  web/g+0/web-vram.txt          web_gs_replay.mjs on g+0 (per-field VRAM hashes in the browser)
  meta.json                     adapter PCI ids, driver, API, WARP and OS versions, commit, exe
                                SHA-256s, and every item's SHA-256 and size per group

Why a driver update orphans a golden: its directory is keyed on, and compare pins, the adapter's
driver version (and the WARP version for the test-gpu groups), because the C renderers the goldens
come from could differ per driver. The pins stay. A new driver gets its golden by adopt (finding 242):

adopt --from OLD GROUP[=SOURCE]... makes this PC's golden ($GOLD, which must not exist yet) from the
golden OLD (a directory or a key under DHD_DEVCACHES/dhd-gs-golden) recorded on the same adapters
(name, PCI vendor and device) with another driver or WARP. Each GROUP's run (SOURCE as for compare;
test-gpu and the native-transfers groups have their default) is compared with OLD's items exactly as
compare does, the driver and WARP pins aside: they are what is being proven. Only when every named
group is identical are those groups hard-linked (copied where links fail) into $GOLD, retired items
included, with a meta.json naming this PC's drivers, OLD, the commit and the groups left out; the
meta.json is written last, so a stopped adopt leaves no golden. A group that differs refuses the
whole adopt (nothing written): then that group needs a new recording from the oracle (below). Groups
not named are not in $GOLD, so compare refuses them there ('the golden has no group').

The Direct3D 11 and C wgpu-native renderers are gone (finding 088, CL2), and with them the record
command: a golden is re-recorded from tag gs-golden-base (or last-32bit-0.3.1), whose gs_golden.py
record runs them (or frozen executables of your own, --oracle).
The goldens' d3d11 items (RETIRED) are still checked to be intact, but never compared: a run has
none (a run's d3d11 item is a failure), and every other item must equal the golden's.

compare checks SOURCE against GOLD (default: this PC's golden directory) byte for byte; text files
(vram.txt, web-vram.txt) ignore CR-only line-ending differences. There is no tolerance. KIND and
SOURCE:

  all RUN                       RUN is a record directory (with meta.json): every group
  test-gpu [DIR]                DIR has test_gs_wgpu's vram-<device>-wgpu.bin
  test-gpu-ee [DIR]                 (default build/port/gs-wgpu/x86_64)
  replay PREFIX                 replay_gs.py's output prefix (PREFIX-*, taken as --backend, wgpu; the
  replay-native PREFIX          golden's two-backend layout PREFIX-<backend>-* is read too)
  replay-native-interp PREFIX
  native-transfers [DIR]        test_native_transfers' output (default build/port/test-native-transfers;
  native-transfers-presented [DIR]  the dump, dump-presented and dump-stress directories)
  native-transfers-stress [DIR]
  web PREFIX                    web_gs_replay.mjs's PREFIX-web-vram.txt

A SOURCE that is a record directory (has meta.json) is read in the record layout for any KIND.
replay and web find the capture themselves (the one whose golden the run equals; --capture names
it). Native pictures must be identical too (decision D5: 0 differing pixels against the C
wgpu-native goldens).
The meta count a compare must reach leaves out the RETIRED items.
compare refuses (exit 1) when GOLD has no meta.json, a golden file is missing, extra or changed,
this PC's adapter (PCI vendor and device) or driver differs from the golden's, the WARP version
differs (test-gpu groups), or RUN's meta.json names another adapter or driver. It prints
"compared N items (meta count M)" and passes only when N == M and every item is identical.
A source with no items fails.

selftest plants one mutation of each kind in a synthetic golden (no game data) and checks that
compare fails or refuses on each (a retired item in a run, or changed in the golden, included), and
passes on the unchanged copy and on a CR-only difference.
Exit codes: 0 pass, 1 fail or refused, 2 bad usage.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_verdict  # noqa: E402
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches

try:
    import numpy
except ImportError:  # the counts below fall back to plain Python
    numpy = None

ROOT = Path(__file__).resolve().parents[2]
FORMAT = 1
GOLDEN_ROOT = (DEVCACHES / 'dhd-gs-golden')
ARCH = 'x86_64'
EXACT_API = 'dx12'       # the WebGPU backend's API on x86_64 (test_gs_wgpu, test_present)
DEVICES = ('warp-early0', 'hardware')
GOLDEN_BACKENDS = ('d3d11', 'wgpu')  # what the goldens were recorded on
BACKENDS = ('wgpu',)                 # what a run has: the Rust renderer's
RETIRED = ('d3d11',)                 # golden items kept intact, never compared; never in a run
TRANSFER_MODES = {'native-transfers': (None, 'dump'), 'native-transfers-presented': ('presented', 'dump-presented'),
                  'native-transfers-stress': ('stress', 'dump-stress')}
GROUPS = ('test-gpu', 'test-gpu-ee', 'replay', 'replay-native', 'replay-native-interp',
          'native-transfers', 'native-transfers-presented', 'native-transfers-stress', 'web')
REPLAY_GROUPS = ('replay', 'replay-native', 'replay-native-interp')
TEXT_SUFFIXES = ('.txt',)
VRAM_WORDS_LOG = re.compile(r'^vram-[^/]+\.bin$')


class Refused(Exception):
    """compare cannot judge: the golden is unusable or pinned to another adapter or driver."""


# ---------------------------------------------------------------- system: adapters and versions

_SYSTEM = None


def system_info():
    """This PC's display adapters (name, PCI vendor and device, driver), WARP's and Windows' versions."""
    global _SYSTEM
    if _SYSTEM is None:
        script = ('$a = @(Get-CimInstance Win32_VideoController | ForEach-Object { [pscustomobject]@{ '
                  'name = $_.Name; pnp = $_.PNPDeviceID; driver = $_.DriverVersion } }); '
                  '$w = (Get-Item "$env:SystemRoot\\System32\\d3d10warp.dll").VersionInfo.FileVersionRaw.ToString(); '
                  '$o = [System.Environment]::OSVersion.Version.ToString(); '
                  '[pscustomobject]@{ adapters = $a; warp = $w; os = $o } | ConvertTo-Json -Depth 4 -Compress')
        text = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
                              capture_output=True, text=True, timeout=120, check=True).stdout
        data = json.loads(text)
        adapters = []
        for a in data['adapters'] or []:
            m = re.search(r'VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})', a.get('pnp') or '')
            adapters.append({'name': a.get('name') or '', 'vendor': m.group(1).lower() if m else '',
                             'device': m.group(2).lower() if m else '', 'driver': a.get('driver') or ''})
        _SYSTEM = {'adapters': adapters, 'warp': data['warp'], 'os': data['os']}
    return _SYSTEM


def find_adapter(name, system=None):
    """The PCI adapter called name (a 'gs: wgpu ... on NAME' report), else None."""
    for a in (system or system_info())['adapters']:
        if a['vendor'] and a['name'] == name:
            return dict(a)
    return None


def default_adapter(system=None):
    """The adapter the GS uses when no run has reported one: the first PCI adapter whose driver works."""
    for a in (system or system_info())['adapters']:
        if a['vendor']:
            return dict(a)
    raise Refused('no PCI display adapter found')


def adapter_key(adapter, api=EXACT_API):
    return '%s-%s-%s-%s' % (adapter['vendor'], adapter['device'], adapter['driver'], api)


# ---------------------------------------------------------------- files

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            h.update(block)
    return h.hexdigest()


def files_under(directory):
    """{relative posix path: Path} of every file under directory (none if it does not exist)."""
    directory = Path(directory)
    if not directory.is_dir():
        return {}
    return {p.relative_to(directory).as_posix(): p for p in sorted(directory.rglob('*')) if p.is_file()}


def normalise_text(data):
    return data.replace(b'\r\n', b'\n').replace(b'\r', b'\n')


def count_words(a, b, size=4):
    """Differing size-byte units of two equal-length byte strings."""
    n = min(len(a), len(b)) // size * size
    if numpy is not None:
        dtype = {1: numpy.uint8, 3: None, 4: numpy.uint32}.get(size)
        if dtype is not None:
            return int(numpy.count_nonzero(numpy.frombuffer(a[:n], dtype) != numpy.frombuffer(b[:n], dtype)))
    count = 0
    step = 1 << 16
    for start in range(0, n, step):
        x, y = a[start:start + step], b[start:start + step]
        if x != y:
            count += sum(1 for i in range(0, len(x), size) if x[i:i + size] != y[i:i + size])
    return count


def ppm_pixels(data):
    """(width, height, rgb bytes) of a binary PPM, else None."""
    parts = data.split(b'\n', 3)
    if len(parts) != 4 or parts[0] != b'P6':
        return None
    try:
        w, h = (int(v) for v in parts[1].split())
    except ValueError:
        return None
    return w, h, parts[3]


def rgb_pixels_differ(a, b, stride):
    """Pixels (stride bytes each, the first 3 compared) differing in RGB."""
    n = min(len(a), len(b)) // stride * stride
    if numpy is not None:
        x = numpy.frombuffer(a[:n], numpy.uint8).reshape(-1, stride)[:, :3]
        y = numpy.frombuffer(b[:n], numpy.uint8).reshape(-1, stride)[:, :3]
        return int(numpy.count_nonzero((x != y).any(axis=1)))
    return sum(1 for i in range(0, n, stride) if a[i:i + 3] != b[i:i + 3])


def item_difference(rel, golden, run):
    """None when the run's file equals the golden one; else what differs (one line)."""
    da, db = golden.read_bytes(), run.read_bytes()
    if rel.endswith(TEXT_SUFFIXES):
        da, db = normalise_text(da), normalise_text(db)
    if da == db:
        return None
    if len(da) != len(db):
        what = 'size %d, golden %d%s' % (len(db), len(da), ' (truncated)' if len(db) < len(da) else '')
    else:
        what = 'differs'
    name = rel.rsplit('/', 1)[-1]
    if VRAM_WORDS_LOG.match(name):
        details = gs_verdict.compare_vram_logs(golden, run)
        if details:
            return '; '.join(details[:4]) + (' (+%d more)' % (len(details) - 4) if len(details) > 4 else '')
        return what
    if rel.endswith(('.txt',)):
        la, lb = da.split(b'\n'), db.split(b'\n')
        lines = sum(1 for x, y in zip(la, lb) if x != y) + abs(len(la) - len(lb))
        return '%d lines differ (%s)' % (lines, what)
    if rel.endswith('.ppm'):
        pa, pb = ppm_pixels(da), ppm_pixels(db)
        if pa and pb and pa[:2] == pb[:2] and len(pa[2]) == len(pb[2]):
            return '%d pixels differ' % rgb_pixels_differ(pa[2], pb[2], 3)
        return 'picture size or header differs (%s)' % what
    if rel.endswith('.rgba') and len(da) == len(db):
        if da[-8:] != db[-8:]:
            return 'picture size differs'
        return '%d pixels differ' % rgb_pixels_differ(da[:-8], db[:-8], 4)
    if rel.endswith(('.bin', '.vram')) and len(da) == len(db):
        return 'VRAM: %d words differ' % count_words(da, db)
    if len(da) == len(db):
        return '%d bytes differ' % count_words(da, db, 1)
    return what


# ---------------------------------------------------------------- golden and sources

def retired(group, rel):
    """True for a golden item of a RETIRED backend (rel as in the golden's group directory):
    test-gpu*/vram-<device>-d3d11.bin, replay*/<cap>/d3d11/..., native-transfers*/d3d11/..."""
    parts = rel.split('/')
    if group in ('test-gpu', 'test-gpu-ee'):
        return any(parts[-1].endswith('-%s.bin' % b) for b in RETIRED)
    if group in REPLAY_GROUPS:
        return len(parts) > 2 and parts[1] in RETIRED
    if group in TRANSFER_MODES:
        return len(parts) > 1 and parts[0] in RETIRED
    return False


def load_meta(directory):
    path = Path(directory) / 'meta.json'
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding='utf-8'))


def golden_items(golden, meta, group):
    """{rel: Path} of a golden group, after checking it against meta (Refused if not intact)."""
    if group not in meta['groups']:
        raise Refused('the golden has no group %s' % group)
    expected = meta['groups'][group]['items']
    present = files_under(Path(golden) / group)
    missing = sorted(set(expected) - set(present))
    extra = sorted(set(present) - set(expected))
    if missing or extra:
        raise Refused('golden %s is not intact: %d missing %s, %d extra %s'
                      % (group, len(missing), missing[:3], len(extra), extra[:3]))
    for rel, path in present.items():
        info = expected[rel]
        if path.stat().st_size != info['size'] or sha256(path) != info['sha256']:
            raise Refused('golden %s/%s changed since it was recorded (size or SHA-256)' % (group, rel))
    return present


def check_pins(meta, groups, run_meta=None):
    """Refused unless this PC (and RUN, when it has a meta.json) has the golden's adapters and drivers."""
    system = system_info()
    for role, pinned in sorted(meta['adapters'].items()):
        now = find_adapter(pinned['name'], system)
        if now is None:
            raise Refused('adapter %s (%s) of the golden is not on this PC' % (pinned['name'], role))
        for field in ('vendor', 'device', 'driver'):
            if now[field] != pinned[field]:
                raise Refused('adapter %s: %s is %s here, %s in the golden (record a golden for this adapter '
                              'and driver from the frozen oracle exes)' % (pinned['name'], field, now[field], pinned[field]))
    if any(g.startswith('test-gpu') for g in groups) and system['warp'] != meta['warp']:
        raise Refused('WARP is %s here, %s in the golden' % (system['warp'], meta['warp']))
    if run_meta is not None:
        if run_meta.get('adapters') != meta['adapters']:
            raise Refused('the run was recorded on %s, the golden on %s' % (run_meta.get('adapters'), meta['adapters']))
        if run_meta.get('warp') != meta['warp']:
            raise Refused('the run was recorded with WARP %s, the golden with %s' % (run_meta.get('warp'), meta['warp']))


def replay_files(prefix, backend_single):
    """{backend/name: Path} of replay_gs.py's outputs for one prefix (both layouts)."""
    prefix = Path(prefix).resolve()
    out = {}
    both = any(Path('%s-%s-vram.txt' % (prefix, b)).exists() or
               list(prefix.parent.glob('%s-%s-[0-9][0-9][0-9].ppm' % (prefix.name, b))) for b in GOLDEN_BACKENDS)
    for backend in (GOLDEN_BACKENDS if both else (backend_single,)):
        stem = '%s-%s' % (prefix.name, backend) if both else prefix.name
        for p in sorted(prefix.parent.glob(stem + '-[0-9][0-9][0-9].ppm')):
            out['%s/%s' % (backend, p.name[len(stem) + 1:])] = p
        for name in ('vram.txt', 'vram.bin'):
            p = prefix.parent / ('%s-%s' % (stem, name))
            if p.is_file():
                out['%s/%s' % (backend, name)] = p
    return out


def source_items(group, source, golden_rel, capture=None, backend='wgpu'):
    """[(candidate capture or None, {rel: Path})]: the run's items for a group, named as in the golden."""
    source = Path(source) if source is not None else None
    if source is not None and source.is_dir() and (source / 'meta.json').is_file():
        return [(None, files_under(source / group))]
    if group in ('test-gpu', 'test-gpu-ee'):
        directory = source or ROOT / 'build' / 'port' / 'gs-wgpu' / ARCH
        found = {rel: directory / rel for rel in golden_rel if (directory / rel).is_file()}
        found.update({p.name: p for p in sorted(Path(directory).glob('vram-*.bin'))
                      if any(p.name == 'vram-%s-%s.bin' % (d, b) for d in DEVICES for b in GOLDEN_BACKENDS)})
        return [(None, found)]
    if group in TRANSFER_MODES:
        directory = source or ROOT / 'build' / 'port' / 'test-native-transfers'
        return [(None, files_under(Path(directory) / TRANSFER_MODES[group][1]))]
    if source is None:
        raise SystemExit('compare %s needs a SOURCE (the output prefix)' % group)
    captures = sorted(set(rel.split('/', 1)[0] for rel in golden_rel))
    if capture:
        captures = [capture]
    if group in REPLAY_GROUPS:
        files = replay_files(source, backend)
    elif group == 'web':
        p = Path('%s-web-vram.txt' % Path(source).resolve())
        files = {'web-vram.txt': p} if p.is_file() else {}
    else:
        raise SystemExit('unknown group %s' % group)
    return [(c, {'%s/%s' % (c, rel): p for rel, p in files.items()}) for c in captures]


def compare_group(group, golden_files, candidates, out):
    """(items compared, meta count, problems) of one group; with several candidate captures the run
    must equal one of them, and the closest one is reported. golden_files holds no RETIRED item, so
    a run's retired item is extra."""
    best = None
    for capture, run in candidates:
        expected = {rel: p for rel, p in golden_files.items() if capture is None or rel.startswith(capture + '/')}
        if capture is not None:
            # one backend run: the golden's items of that backend only
            backends = set(rel.split('/')[1] for rel in run)
            if len(backends) == 1:
                expected = {rel: p for rel, p in expected.items() if rel.split('/')[1] in backends}
        problems = []
        if not run:
            problems.append('%s: the source has no items' % group)
        missing = sorted(set(expected) - set(run))
        extra = sorted(set(run) - set(expected))
        problems.extend('%s/%s: missing' % (group, rel) for rel in missing)
        problems.extend('%s/%s: %s' % (group, rel, 'retired (d3d11: the Direct3D 11 renderer is gone)'
                                       if retired(group, rel) else 'extra (not in the golden)') for rel in extra)
        compared = 0
        for rel in sorted(set(expected) & set(run)):
            compared += 1
            what = item_difference(rel, expected[rel], run[rel])
            if what:
                problems.append('%s/%s: %s' % (group, rel, what))
        result = (len(problems), capture, compared, len(expected), problems)
        if best is None or result[0] < best[0]:
            best = result
        if not problems:
            break
    _, capture, compared, count, problems = best
    out('%s%s: compared %d items (meta count %d)%s' % (group, '' if capture is None else ' ' + capture, compared, count,
                                                       ': identical' if not problems else ': %d problems' % len(problems)))
    for p in problems[:20]:
        out('  ' + p)
    if len(problems) > 20:
        out('  ... %d more' % (len(problems) - 20))
    return compared, count, problems


def compare(kind, source, golden, capture=None, backend='wgpu', out=print):
    """0 when SOURCE equals the golden, 1 when it does not or the compare is refused."""
    golden = Path(golden)
    try:
        meta = load_meta(golden)
        if meta is None:
            raise Refused('%s has no meta.json (not a golden)' % golden)
        if meta.get('format') != FORMAT:
            raise Refused('golden format %r, expected %d' % (meta.get('format'), FORMAT))
        run_meta = None
        if kind == 'all':
            if source is None:
                raise SystemExit('compare all needs RUN (a record directory)')
            run_meta = load_meta(source)
            if run_meta is None:
                raise Refused('%s has no meta.json (not a complete record)' % source)
            groups = list(meta['groups'])
        else:
            if kind not in GROUPS:
                raise SystemExit('unknown KIND %s (all, %s)' % (kind, ', '.join(GROUPS)))
            if source is not None and Path(source).is_dir() and (Path(source) / 'meta.json').is_file():
                run_meta = load_meta(source)
            groups = [kind]
        check_pins(meta, groups, run_meta)
        intact = {g: golden_items(golden, meta, g) for g in groups}  # every item, the retired ones too
        golden_by_group = {g: {rel: p for rel, p in items.items() if not retired(g, rel)}
                           for g, items in intact.items()}
        retired_count = sum(1 for g in meta['groups'] for rel in meta['groups'][g]['items'] if retired(g, rel))
    except Refused as e:
        out('REFUSED: %s' % e)
        out('VERDICT FAIL refused')
        return 1
    total = expected_total = 0
    problems = []
    for group in groups:
        candidates = source_items(group, source, golden_by_group[group], capture, backend)
        compared, count, group_problems = compare_group(group, golden_by_group[group], candidates, out)
        total += compared
        expected_total += count
        problems.extend(group_problems)
    if kind == 'all':
        expected_total = meta['count'] - retired_count
    if total != expected_total:
        problems.append('compared %d items, the meta count is %d' % (total, expected_total))
    if total == 0:
        problems.append('no items compared')
    out('compared %d items (meta count %d): %s' % (total, expected_total, 'identical' if not problems else 'DIFFERENT'))
    stream = io.StringIO()
    code = gs_verdict.verdict(problems, out=stream)
    for line in stream.getvalue().splitlines():
        out(line)
    return code


# ---------------------------------------------------------------- adopt

def git_head():
    try:
        return subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True,
                              timeout=30, check=True).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ''


def adopt(old, specs, root=GOLDEN_ROOT, system=None, out=print):
    """0 when every named group's run equals OLD's golden and this PC's golden was written, else 1."""
    system = system or system_info()
    old = Path(old) if Path(old).is_dir() else Path(root) / old
    try:
        meta = load_meta(old)
        if meta is None:
            raise Refused('%s has no meta.json (not a golden)' % old)
        if meta.get('format') != FORMAT:
            raise Refused('golden format %r, expected %d' % (meta.get('format'), FORMAT))
        adapters = {}
        for role, pinned in sorted(meta['adapters'].items()):
            now = find_adapter(pinned['name'], system)
            if now is None or (now['vendor'], now['device']) != (pinned['vendor'], pinned['device']):
                raise Refused('adapter %s (%s) of the golden is not on this PC' % (pinned['name'], role))
            adapters[role] = now
        if all(adapters[r]['driver'] == meta['adapters'][r]['driver'] for r in adapters) and                 system['warp'] == meta.get('warp'):
            raise Refused("%s was recorded with this PC's drivers and WARP: compare it" % old)
        key = adapter_key(adapters.get('exact') or default_adapter(system))
        new = Path(root) / key
        if new.resolve() == old.resolve():
            raise Refused("%s is already this PC's golden (%s): compare it" % (old, key))
        if (new / 'meta.json').exists():
            raise Refused('%s already has a golden' % new)
        if not specs:
            raise SystemExit('adopt needs at least one GROUP[=SOURCE]')
        groups = {}
        for spec in specs:
            group, _, source = spec.partition('=')
            if group not in GROUPS:
                raise SystemExit('unknown GROUP %s (%s)' % (group, ', '.join(GROUPS)))
            groups[group] = source or None
        intact = {g: golden_items(old, meta, g) for g in groups}
    except Refused as e:
        out('REFUSED: %s' % e)
        out('VERDICT FAIL refused')
        return 1
    problems = []
    for group, source in groups.items():
        expected = {rel: p for rel, p in intact[group].items() if not retired(group, rel)}
        candidates = source_items(group, source, expected)
        compared, count, group_problems = compare_group(group, expected, candidates, out)
        problems.extend(group_problems)
        if compared != count or compared == 0:
            problems.append('%s: compared %d items, the meta count is %d' % (group, compared, count))
    if problems:
        out('adopt: %d problems: nothing written (a group that differs needs a recording from the oracle)'
            % len(problems))
        stream = io.StringIO()
        gs_verdict.verdict(problems, out=stream)
        for line in stream.getvalue().splitlines():
            out(line)
        return 1
    staging = Path(root) / (key + '.adopting')
    shutil.rmtree(staging, ignore_errors=True)
    for group in groups:
        for rel, path in intact[group].items():
            target = staging / group / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(path, target)
            except OSError:
                shutil.copy2(path, target)
    new_meta = dict(meta)
    new_meta.update(key=key, adapters={r: {f: a[f] for f in ('name', 'vendor', 'device', 'driver')}
                                       for r, a in adapters.items()},
                    warp=system['warp'], os=system['os'], system_adapters=system['adapters'],
                    groups={g: meta['groups'][g] for g in groups},
                    count=sum(meta['groups'][g]['count'] for g in groups),
                    adopted={'from': meta.get('key', old.name), 'from_adapters': meta['adapters'],
                             'from_warp': meta.get('warp'), 'commit': git_head(),
                             'proven': sorted(groups), 'left_out': sorted(set(meta['groups']) - set(groups))})
    (staging / 'meta.json').write_text(json.dumps(new_meta, indent=1), encoding='utf-8')
    if new.exists():
        shutil.rmtree(new)  # no meta.json (checked above): not a golden, a leftover
    staging.rename(new)
    out('adopt: %s from %s, %d items in %s; left out: %s'
        % (key, new_meta['adopted']['from'], new_meta['count'], ', '.join(sorted(groups)),
           ', '.join(new_meta['adopted']['left_out']) or 'none'))
    out('VERDICT PASS')
    return 0


# ---------------------------------------------------------------- selftest

def selftest():
    """Mutations of a synthetic golden (no game data): each must fail or refuse; the copy and a
    CR-only difference must pass."""
    system = system_info()
    adapter = default_adapter(system)
    root = Path(tempfile.mkdtemp(prefix='gs-golden-selftest-', dir=os.environ.get('DHD_SCRATCH')))
    failures = []
    try:
        log = b'step'.ljust(gs_verdict.CHECKPOINT_NAME, b'\0') + bytes(range(256)) * (gs_verdict.VRAM_BYTES // 256)
        # the golden's retired (d3d11) items: checked intact, never compared, never in a run
        retired_files = {
            'test-gpu/vram-hardware-d3d11.bin': log,
            'replay/g+0/d3d11/vram.txt': b'0 0123456789ABCDEF\n1 FEDCBA9876543210\n',
            'native-transfers/d3d11/00.name': b'scene',
        }

        def build(directory, golden):
            files = {
                'test-gpu/vram-hardware-wgpu.bin': log,
                'replay/g+0/wgpu/000.ppm': b'P6\n2 2\n255\n' + bytes(range(12)),
                'replay/g+0/wgpu/vram.txt': b'0 0123456789ABCDEF\n1 FEDCBA9876543210\n',
                'replay/g+0/wgpu/vram.bin': bytes(64),
                'native-transfers/wgpu/00.name': b'scene',
                'native-transfers/wgpu/00.rgba': bytes(16) + (2).to_bytes(4, 'little') + (2).to_bytes(4, 'little'),
            }
            if golden:
                files.update(retired_files)
            meta = {'format': FORMAT, 'golden': golden, 'adapters': {'exact': adapter}, 'warp': system['warp'],
                    'groups': {}, 'count': 0}
            for rel, data in files.items():
                p = directory / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
                group, item = rel.split('/', 1)
                g = meta['groups'].setdefault(group, {'count': 0, 'items': {}})
                g['items'][item] = {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}
                g['count'] += 1
                meta['count'] += 1
            (directory / 'meta.json').write_text(json.dumps(meta), encoding='utf-8')

        golden = root / 'golden'
        build(golden, True)

        def case(name, mutate, kind='all', source=None, expect=1, needle=None):
            run = root / name
            build(run, False)
            if mutate:
                mutate(run)
            lines = []
            code = compare(kind, source(run) if source else run, golden, out=lines.append)
            text = '\n'.join(lines)
            ok = code == expect and (needle is None or needle in text)
            print('selftest %-24s exit %d (expected %d)%s' % (name, code, expect, '' if ok else '  <-- WRONG'))
            if not ok:
                failures.append(name)
                print('\n'.join('    ' + line for line in lines))

        def poke(rel, offset, value=0xFF):
            def f(run):
                p = run / rel
                data = bytearray(p.read_bytes())
                data[offset] ^= value
                p.write_bytes(bytes(data))
            return f

        def edit_meta(run, golden_too=False):
            def f(_):
                for d in ([run, golden] if golden_too else [run]):
                    m = json.loads((d / 'meta.json').read_text())
                    m['adapters']['exact'] = dict(m['adapters']['exact'], driver='0.0.0.0')
                    (d / 'meta.json').write_text(json.dumps(m))
            return f

        case('identical', None, expect=0, needle='compared 6 items (meta count 6): identical')
        case('vram-word', poke('test-gpu/vram-hardware-wgpu.bin', gs_verdict.CHECKPOINT_NAME + 4096), needle='1 words differ')
        case('pixel', poke('replay/g+0/wgpu/000.ppm', 11 + 4), needle='1 pixels differ')
        case('rgba-pixel', poke('native-transfers/wgpu/00.rgba', 1), needle='1 pixels differ')
        case('vram-bin', poke('replay/g+0/wgpu/vram.bin', 9), needle='1 words differ')
        case('missing', lambda run: (run / 'replay/g+0/wgpu/vram.bin').unlink(), needle='missing')
        case('extra', lambda run: (run / 'replay/g+0/wgpu/001.ppm').write_bytes(b'P6\n1 1\n255\n\0\0\0'), needle='extra')
        case('truncated', lambda run: (run / 'replay/g+0/wgpu/vram.bin').write_bytes(bytes(60)), needle='truncated')
        case('cr-only', lambda run: (run / 'replay/g+0/wgpu/vram.txt').write_bytes(
            (run / 'replay/g+0/wgpu/vram.txt').read_bytes().replace(b'\n', b'\r\n')), expect=0)
        case('run-adapter', lambda run: edit_meta(run)(run), needle='REFUSED')
        g = json.loads((golden / 'meta.json').read_text())
        g['adapters']['exact'] = dict(g['adapters']['exact'], driver='0.0.0.0')
        (golden / 'meta.json').write_text(json.dumps(g))
        case('golden-driver', lambda run: edit_meta(run)(run), needle='REFUSED')
        g['adapters']['exact'] = dict(adapter, device='ffff')
        (golden / 'meta.json').write_text(json.dumps(g))
        case('golden-device', None, needle='REFUSED')
        shutil.rmtree(golden)
        build(golden, True)
        case('golden-changed', lambda run: poke('replay/g+0/wgpu/vram.bin', 0)(golden), needle='REFUSED')
        shutil.rmtree(golden)
        build(golden, True)
        case('golden-retired-changed', lambda run: poke('test-gpu/vram-hardware-d3d11.bin', 99)(golden), needle='REFUSED')
        shutil.rmtree(golden)
        build(golden, True)
        case('retired-in-run', lambda run: (run / 'native-transfers/d3d11').mkdir() or
             (run / 'native-transfers/d3d11/00.name').write_bytes(b'scene'), needle='retired')
        case('retired-log-in-run', lambda run: (run / 'test-gpu/vram-hardware-d3d11.bin').write_bytes(log), needle='retired')
        empty = root / 'empty'
        empty.mkdir()
        case('zero-items', None, kind='native-transfers', source=lambda run: empty, needle='no items')
        case('prefix-none', None, kind='replay', source=lambda run: root / 'nothing' / 'p', needle='no items')

        def prefix_run(run):
            for rel in ('000.ppm', 'vram.txt', 'vram.bin'):
                shutil.copy2(run / 'replay/g+0/wgpu' / rel, root / ('p-' + rel))
            return root / 'p'
        case('prefix-single', None, kind='replay', source=prefix_run, expect=0,
             needle='compared 3 items (meta count 3): identical')
        meta = load_meta(golden)
        meta['count'] += 1
        (golden / 'meta.json').write_text(json.dumps(meta))
        case('count-mismatch', None, needle='meta count 7')

        # adopt: a golden pinned to another driver becomes this PC's only when the run equals it
        adopt_root = root / 'adopt'
        old = adopt_root / 'old'
        shutil.rmtree(golden)
        build(golden, True)
        shutil.copytree(golden, old)
        m = json.loads((old / 'meta.json').read_text())
        m['adapters']['exact'] = dict(m['adapters']['exact'], driver='0.0.0.0')
        m['key'], m['warp'] = 'old', '0.0.0.0'  # recorded with another driver and WARP
        (old / 'meta.json').write_text(json.dumps(m))
        run = root / 'adopt-run'
        build(run, False)
        new = adopt_root / adapter_key(adapter)

        def adopt_case(name, specs, expect, needle, sys_info=system):
            lines = []
            code = adopt(old, specs, root=adopt_root, system=sys_info, out=lines.append)
            ok = code == expect and needle in '\n'.join(lines)
            print('selftest %-24s exit %d (expected %d)%s' % (name, code, expect, '' if ok else '  <-- WRONG'))
            if not ok:
                failures.append(name)
                print('\n'.join('    ' + line for line in lines))

        def compare_case(name, kind, expect, needle):
            lines = []
            code = compare(kind, run, new, out=lines.append)
            ok = code == expect and needle in '\n'.join(lines)
            print('selftest %-24s exit %d (expected %d)%s' % (name, code, expect, '' if ok else '  <-- WRONG'))
            if not ok:
                failures.append(name)
                print('\n'.join('    ' + line for line in lines))

        poke('replay/g+0/wgpu/vram.bin', 3)(run)
        adopt_case('adopt-differs', ['test-gpu=%s' % run, 'replay=%s' % run], 1, 'nothing written')
        if new.exists():
            failures.append('adopt-differs wrote a golden')
        poke('replay/g+0/wgpu/vram.bin', 3)(run)
        adopt_case('adopt-other-device', ['test-gpu=%s' % run], 1, 'REFUSED',
                   dict(system, adapters=[dict(adapter, device='ffff')]))
        adopt_case('adopt-same-driver', ['test-gpu=%s' % run], 1, "this PC's drivers",
                   dict(system, adapters=[dict(adapter, driver='0.0.0.0')], warp='0.0.0.0'))
        adopt_case('adopt', ['test-gpu=%s' % run, 'replay=%s' % run], 0, 'left out: native-transfers')
        if not (new / 'test-gpu/vram-hardware-d3d11.bin').is_file():
            failures.append('adopt kept no retired item')
        compare_case('adopted-compare', 'replay', 0, 'compared 3 items (meta count 3): identical')
        compare_case('adopted-left-out', 'native-transfers', 1, 'the golden has no group native-transfers')
        adopt_case('adopt-exists', ['test-gpu=%s' % run], 1, 'already has a golden')
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print('selftest: %s' % ('PASS' if not failures else 'FAIL ' + ', '.join(failures)))
    return 1 if failures else 0


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('key', help="this PC's adapter key and golden directory")
    cmp_ = sub.add_parser('compare', help='compare outputs with the golden')
    cmp_.add_argument('kind', choices=('all',) + GROUPS)
    cmp_.add_argument('source', nargs='?')
    cmp_.add_argument('--golden', help="the golden directory (default: this PC's)")
    cmp_.add_argument('--capture', help='replay and web: the capture (g+0, g+3600); found by default')
    cmp_.add_argument('--backend', choices=BACKENDS, default='wgpu',
                      help='replay groups: the backend of a one-backend run (wgpu, the only one)')
    ad = sub.add_parser('adopt', help="this PC's golden from one recorded with another driver, proven equal")
    ad.add_argument('--from', dest='old', required=True, help='the old golden (directory or key)')
    ad.add_argument('groups', nargs='+', metavar='GROUP[=SOURCE]')
    sub.add_parser('selftest', help='planted mutations on a synthetic golden')
    args = ap.parse_args(argv)
    if args.command == 'key':
        key = adapter_key(default_adapter())
        print(key)
        print(GOLDEN_ROOT / key)
        return 0
    if args.command == 'selftest':
        return selftest()
    if args.command == 'adopt':
        return adopt(args.old, args.groups)
    golden = Path(args.golden) if args.golden else GOLDEN_ROOT / adapter_key(default_adapter())
    return compare(args.kind, args.source, golden, args.capture, args.backend)


if __name__ == '__main__':
    sys.exit(main())
