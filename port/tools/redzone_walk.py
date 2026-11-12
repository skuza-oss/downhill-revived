#!/usr/bin/env python3
"""The red-zone walk (finding 213): a game with red zones around every address-taken local
(DHD_CORE_REDZONE=1, port/tools/redzone.py) driven through what the harness reaches, then every
`redzone:` line of its logs, once per function and local.

    python port/tools/redzone_walk.py --build      # build the red-zone game into build/redzone/,
                                                   # then this checkout's players' build again
    python port/tools/redzone_walk.py              # the walk: the state-trace recipes, OPTIONS
    python port/tools/redzone_walk.py --recipes boot,race --no-options

The recipes run headless (state_trace_run.py, deterministic, no window); the OPTIONS walk is
options_walk.py's four scenarios on the hidden desktop. Both take the game lock untimed. A hit
names the function, the local and the 16 bytes after (or before) it that were written: an access
past that local the EE frame would have put on its neighbour. Exit 1 when there is a hit.
"""
import argparse, os, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable
OUT = ROOT / 'build' / 'redzone'
RECIPES = ['boot', 'race', 'race2', 'race3', 'race4', 'race5', 'race6', 'split', 'memcard']
HIT = re.compile(r'^redzone: .*$', re.M)


def build(jobs):
    env = dict(os.environ, DHD_CORE_REDZONE='1')
    if subprocess.run([PY, str(ROOT / 'port/tools/build.py'), '-j', str(jobs)], cwd=ROOT, env=env).returncode:
        sys.exit('redzone_walk: the red-zone build failed')
    OUT.mkdir(parents=True, exist_ok=True)
    src = ROOT / 'build' / 'host64-exe'
    for name in ('dhd64.exe', 'dhd64.map'):
        shutil.copy2(src / name, OUT / name)
    tsv = ROOT / 'build' / 'wasm' / 'redzone.tsv'
    if tsv.exists():
        shutil.copy2(tsv, OUT / 'redzone.tsv')
    env.pop('DHD_CORE_REDZONE')
    if subprocess.run([PY, str(ROOT / 'port/tools/build.py'), '-j', str(jobs)], cwd=ROOT, env=env).returncode:
        sys.exit('redzone_walk: the players\' build again failed')
    print('red-zone game: %s' % (OUT / 'dhd64.exe'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build', action='store_true')
    ap.add_argument('-j', type=int, default=6)
    ap.add_argument('--exe', type=Path, default=OUT / 'dhd64.exe')
    ap.add_argument('--recipes', default=','.join(RECIPES))
    ap.add_argument('--no-options', action='store_true')
    ap.add_argument('--env', action='append', default=[], help='extra NAME=VALUE for the recipes')
    a = ap.parse_args()
    if a.build:
        build(a.j)
        return
    if not a.exe.exists():
        sys.exit('redzone_walk: no %s (run with --build first)' % a.exe)
    logs, failed = [], []
    for recipe in [r for r in a.recipes.split(',') if r]:
        name = 'redzone-' + recipe
        cmd = [PY, str(ROOT / 'port/tools/state_trace_run.py'), recipe, '--name', name, '--exe', str(a.exe)]
        for e in a.env:
            cmd += ['--env', e]
        r = subprocess.run(cmd, cwd=ROOT)
        if r.returncode:
            failed.append(recipe)
        logs.append(ROOT / 'build' / 'state-trace' / (name + '.log'))
    if not a.no_options:
        out = OUT / 'options-walk'
        r = subprocess.run([PY, str(ROOT / 'port/tools/options_walk.py'), '--exe', str(a.exe), '--out', str(out)],
                           cwd=ROOT)
        if r.returncode:
            failed.append('options_walk')
        logs += sorted(out.rglob('*.log'))
    hits = {}
    for log in logs:
        if log.exists():
            for line in HIT.findall(log.read_text(errors='replace')):
                hits.setdefault(line.strip(), log.relative_to(ROOT).as_posix())
    for line, where in sorted(hits.items()):
        print('%s   (%s)' % (line, where))
    print('redzone_walk: %d log(s), %d hit(s)%s' % (len(logs), len(hits),
                                                   ('; runs that failed: ' + ', '.join(failed)) if failed else ''))
    sys.exit(1 if hits or failed else 0)


if __name__ == '__main__':
    main()
