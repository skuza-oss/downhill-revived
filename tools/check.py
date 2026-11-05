#!/usr/bin/env python3
"""One entry point for the checks, in tiers. Each step prints one line (OK/FAIL, seconds, log);
its full output goes to build/check/<tier>-<step>.log.

    python tools/check.py docs       # docs linter + never-commit check of the tree (seconds)
    python tools/check.py quick      # docs, then the game core (module cache) and the unit tests
    python tools/check.py port       # quick, then the native game, the renderer and network tests
    python tools/check.py matching   # the retail image: configure, build, byte-for-byte check
    python tools/check.py server     # the online server's tests and type check (npm)
    options: -j N (parallel compiles for the build steps), --keep-going (run every step)

Which tier a change needs: AGENTS.md, "Done means". The tiers wrap the existing tools; nothing
here runs the game, a phone or a benchmark. Several worktrees on one machine: set DHD_BUILD_SLOTS
(docs/TESTING.md), the build scripts honour it.
"""
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
NPM = 'npm.cmd' if os.name == 'nt' else 'npm'


def steps(tier, jobs):
    j = ['-j', str(jobs)] if jobs else []
    docs = [('check_docs', [PY, 'tools/check_docs.py', '--quiet'], ROOT),
            ('check_tree', [PY, 'tools/check_tree.py'], ROOT)]
    quick = docs + [('build_wasm', [PY, 'port/tools/build_wasm.py'] + j, ROOT),
                    ('test_runtime', [PY, 'port/tools/test_runtime.py'], ROOT)]
    port = quick + [('build_game', [PY, 'port/tools/build.py', '--no-core'] + j, ROOT),
                    ('net_tests', [PY, 'port/net/build_tests.py'], ROOT),
                    ('test_gs_wgpu', [PY, 'port/tools/test_gs_wgpu.py', '--skip-hardware'], ROOT),
                    ('test_wasm2c_chunks', [PY, 'port/tools/test_wasm2c_chunks.py'], ROOT)]
    matching = [('configure', [PY, 'tools/configure.py'], ROOT),
                ('ninja', [PY, '-m', 'ninja'] + j, ROOT),
                ('check_image', [PY, 'tools/check_image.py', 'build/dhd.elf'], ROOT)]
    server_dir = os.path.join(ROOT, 'online', 'server')
    server = [('npm_test', [NPM, 'test'], server_dir),
              ('npm_typecheck', [NPM, 'run', 'typecheck'], server_dir)]
    return {'docs': docs, 'quick': quick, 'port': port, 'matching': matching, 'server': server}.get(tier)


def main(argv):
    args = [a for a in argv if not a.startswith('-')]
    jobs = None
    if '-j' in argv:
        at = argv.index('-j') + 1
        if at >= len(argv) or not argv[at].isdigit():
            print(__doc__)
            return 2
        jobs = int(argv[at])
        args.remove(argv[at])
    tier = args[0] if args else ''
    plan = steps(tier, jobs)
    if plan is None:
        print(__doc__)
        return 2
    logs = os.path.join(ROOT, 'build', 'check')
    os.makedirs(logs, exist_ok=True)
    failed = 0
    for name, cmd, cwd in plan:
        log = os.path.join(logs, f'{tier}-{name}.log')
        start = time.time()
        with open(log, 'w', encoding='utf-8', errors='replace') as out:
            try:
                rc = subprocess.run(cmd, cwd=cwd, stdout=out, stderr=subprocess.STDOUT).returncode
            except FileNotFoundError as e:
                out.write(f'{e}\n')
                rc = 127
        took = time.time() - start
        rel = os.path.relpath(log, ROOT).replace(os.sep, '/')
        print(f'{"OK  " if rc == 0 else "FAIL"} {name:14} {took:6.1f} s  {rel}', flush=True)
        if rc:
            failed += 1
            with open(log, encoding='utf-8', errors='replace') as f:
                tail = [l.rstrip() for l in f.readlines()[-3:] if l.strip()]
            for line in tail:
                print(f'       {line[:160]}')
            if '--keep-going' not in argv:
                break
    print(f'check {tier}: {"FAIL" if failed else "OK"}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
