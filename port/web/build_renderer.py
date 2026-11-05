#!/usr/bin/env python3
"""Build the browser's Rust renderer module (port/render, crate dhd_render) into build/web/renderer.

    python port/web/build_renderer.py [--profile release|fixloop] [--no-opt]

Steps (plan section 3.3):
1. `port/tools/rustgs.py build --target wasm32-unknown-unknown` (the cdylib, then wasm-bindgen
   --target web into build/web/renderer: dhd_render.js and dhd_render_bg.wasm). rustgs owns every
   flag, the version gate, the cache and check_rust_lib.py's refusals on the built module.
2. The glue import module is renamed from `./dhd_render_bg.js` to `wbg` in both files (finding 085,
   decision b; the layout checker's rules M1 and R1 want `wbg`). The build stops if the module
   imports from anything but that one module, or if the glue's import-object key is not there
   exactly once. A module already named `wbg` (with its key) is left as it is.
3. Optional wasm-opt -O3 (skipped with --no-opt), a pass of its own, separate from build.py's pass
   over the core and the graphics reactor.
4. build/web/renderer-manifest.json: the sha256 of the two served files and their sizes (raw and
   gzip, before and after wasm-opt), plus rustgs's cache key and raw module hash.

serve.mjs serves exactly /build/renderer/dhd_render.js and /build/renderer/dhd_render_bg.wasm, so a
glue that needs anything else (wasm-bindgen `snippets/`, a second module) is refused here.
Nothing written here is game data; all of it stays in build/web (never committed).
"""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'port/web'))
import check_web_layout as layout  # noqa: E402  (read-only use of the P0-6 checker's wasm parser)
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches

OUT = ROOT / 'build/web'
RENDERER = OUT / 'renderer'
OUT_NAME = 'dhd_render'
GLUE_MODULE = 'wbg'
SERVED = (OUT_NAME + '.js', OUT_NAME + '_bg.wasm')   # == the serve.mjs allowlist under /build/renderer/
BINARYEN = (DEVCACHES / 'binaryen/binaryen-version_133/bin/wasm-opt.exe')
WASM = 'wasm32-unknown-unknown'


def die(msg):
    sys.exit('build_renderer: ' + msg)


def run(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        die('command failed (exit %d): %s' % (r.returncode, cmd[0]))
    return r


def rename_import_module(src, dst, old, new):
    """Copy of a wasm with import module `old` renamed `new` (src may be dst). From the spike (finding 085)."""
    data = Path(src).read_bytes()
    out = bytearray(data[:8])
    pos = 8
    while pos < len(data):
        r = layout.Reader(data, pos, len(data))
        sid = r.byte()
        size = r.uleb()
        start, end = r.i, r.i + size
        body = data[start:end]
        if sid == 2:
            s = layout.Reader(data, start, end)
            entries = []
            for _ in range(s.uleb()):
                mod = s.name()
                b = s.i
                s.name()
                kind = s.byte()
                if kind == 0:
                    s.uleb()
                elif kind == 1:
                    s.byte(); s.limits()
                elif kind == 2:
                    s.limits()
                elif kind == 3:
                    s.byte(); s.byte()
                elif kind == 4:
                    s.byte(); s.uleb()
                else:
                    die('%s: unknown import kind %d' % (src, kind))
                rest = data[b:s.i]
                entries.append(layout._name(new if mod == old else mod) + rest)
            body = layout._vec(entries)
        out += layout._sec(sid, body)
        pos = end
    Path(dst).write_bytes(bytes(out))


def import_modules(wasm):
    return {mod for mod, _, _ in layout.parse('renderer', wasm).imports}


def name_glue_module(pkg, out_name=OUT_NAME, new=GLUE_MODULE):
    """Rename wasm-bindgen's glue import module ./<out_name>_bg.js to `new` in the module and the
    glue's one import-object key. Returns True if renamed, False if it already was."""
    old = './%s_bg.js' % out_name
    wasm, js = pkg / (out_name + '_bg.wasm'), pkg / (out_name + '.js')
    text = js.read_text(encoding='utf-8')
    mods = import_modules(wasm)
    old_key, new_key = '"%s": import0,' % old, '"%s": import0,' % new
    if mods == {new} and text.count(new_key) == 1 and old not in text:
        return False
    if mods != {old}:
        die('%s imports from %s, expected only %s' % (wasm, sorted(mods), old))
    if text.count(old_key) != 1 or text.count(old) != 1:
        die('%s: expected exactly one import-object key %s' % (js, old_key))
    rename_import_module(wasm, wasm, old, new)
    if import_modules(wasm) != {new}:
        die('renaming the import module of %s failed' % wasm)
    js.write_text(text.replace(old_key, new_key), encoding='utf-8', newline='')
    return True


def check_served_only(pkg, out_name=OUT_NAME):
    """The glue may load nothing the server does not serve."""
    text = (pkg / (out_name + '.js')).read_text(encoding='utf-8')
    if './snippets/' in text:
        die('%s imports wasm-bindgen snippets; serve.mjs serves only %s' % (pkg / (out_name + '.js'), ', '.join(SERVED)))
    if (pkg / 'snippets').exists():
        die('%s exists; serve.mjs serves only %s' % (pkg / 'snippets', ', '.join(SERVED)))
    for name in SERVED:
        if not (pkg / name).is_file():
            die('%s was not built' % (pkg / name))


def sizes(path):
    b = Path(path).read_bytes()
    return {'raw': len(b), 'gzip': len(gzip.compress(b, 9))}


def build(profile='release', opt=True, out=OUT):
    """Build the renderer into out/renderer; returns the manifest dict (also written to
    out/renderer-manifest.json)."""
    pkg = out / 'renderer'
    if pkg.resolve() != RENDERER.resolve():
        die('rustgs writes the wasm glue to %s only' % RENDERER)
    out.mkdir(parents=True, exist_ok=True)
    report = out / 'renderer-rustgs.json'
    if report.exists():
        report.unlink()
    env = dict(os.environ, TEMP=str(out), TMP=str(out), TMPDIR=str(out))
    run([sys.executable, ROOT / 'port/tools/rustgs.py', 'build', '--target', WASM, '--profile', profile,
         '--report', report], cwd=ROOT, env=env)
    if not report.exists():
        die('rustgs wrote no report')
    rep = json.loads(report.read_text())
    if Path(rep['lib']).resolve() != (pkg / SERVED[1]).resolve():
        die('rustgs put the module at %s, expected %s' % (rep['lib'], pkg / SERVED[1]))
    check_served_only(pkg)
    renamed = name_glue_module(pkg)
    print('build_renderer: glue import module %s %s' % (GLUE_MODULE, 'renamed' if renamed else 'already set'), flush=True)
    before = {n: sizes(pkg / n) for n in SERVED}
    if opt:
        bg = pkg / SERVED[1]
        tmp = out / 'dhd_render_bg-optimized.wasm'
        # The same feature set as build.py's pass (compact imports: unsupported by Node 24).
        sys.path.insert(0, str(ROOT / 'port/tools'))
        import objcache  # the same input's output from the object cache (objcache.tool_output)
        cmd = [BINARYEN, bg, '-O3', '--all-features', '--disable-compact-imports', '-o', tmp]
        objcache.tool_output(cmd, bg, tmp, lambda c: run(c, cwd=ROOT, env=env))
        if import_modules(tmp) != {GLUE_MODULE}:
            die('wasm-opt changed the import modules of %s' % bg)
        shutil.copyfile(tmp, bg)
        tmp.unlink()
    manifest = {
        'profile': profile, 'optimized': opt, 'glue_module': GLUE_MODULE,
        'rustgs': {k: rep.get(k) for k in ('key', 'cache', 'sha256', 'versions')},
        'sha256': {str(pkg / n): hashlib.sha256((pkg / n).read_bytes()).hexdigest() for n in SERVED},
        'sizes': {n: sizes(pkg / n) for n in SERVED},
        'sizes_before_opt': before,
    }
    (out / 'renderer-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for n in SERVED:
        s = manifest['sizes'][n]
        print('build_renderer: %-20s %8d bytes, %7d gzip' % (n, s['raw'], s['gzip']), flush=True)
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--profile', choices=('release', 'fixloop'), default='release')
    p.add_argument('--no-opt', action='store_true', help='skip wasm-opt')
    a = p.parse_args()
    build(a.profile, not a.no_opt)
    print('build_renderer: OK ->', RENDERER)


if __name__ == '__main__':
    main()
