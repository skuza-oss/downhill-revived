#!/usr/bin/env python3
"""One entry point for the checks, in tiers. Each step prints one line (OK/FAIL, seconds, log);
its full output goes to build/check/<tier>-<step>.log.

    python tools/check.py docs       # docs linter + never-commit check of the tree + the launcher's strings (seconds)
    python tools/check.py quick      # docs, then the game core (module cache) and the unit tests
    python tools/check.py port       # quick, then the native game (and its retail-data scan), the renderer and network tests
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
            ('check_tree', [PY, 'tools/check_tree.py'], ROOT),
            # the launcher's string table: a language file's stale keys, the page's keys (finding 225)
            ('lang_check', [PY, 'tools/lang_check.py'], ROOT)]
    quick = docs + [('build_wasm', [PY, 'port/tools/build_wasm.py'] + j, ROOT),
                    ('test_runtime', [PY, 'port/tools/test_runtime.py'], ROOT),
                    # the touch overlay's action pictures: the shader is what their shapes give (finding 229)
                    ('touch_icons', [PY, 'port/tools/touch_icons.py', '--check'], ROOT),
                    # the iOS build and packaging rules, no Mac needed (finding 224; the Mac builds the app)
                    ('ios_tests', [PY, '-m', 'unittest', 'discover', '-s', 'port/ios/tests'], ROOT)]
    # The retail-data scan package_release.py runs on dhd.exe (scan_retail.py: no copy of the retail
    # ELF's code or data beyond the source's own literals), run on the game just built, so a copy
    # shows here and not first at packaging.
    port = quick + [('build_game', [PY, 'port/tools/build.py', '--no-core'] + j, ROOT),
                    ('scan_retail', [PY, 'port/tools/scan_retail.py', 'build/host64-exe/dhd64.exe'], ROOT),
                    ('net_tests', [PY, 'port/net/build_tests.py'], ROOT),
                    ('test_gs_wgpu', [PY, 'port/tools/test_gs_wgpu.py', '--skip-hardware'], ROOT),
                    # dhd.ini's own tests: saves at once, the Windows retries (findings 101, 228)
                    ('settings_tests', [PY, 'port/tools/rustgs.py', 'test', '--', '-p', 'dhd_settings'], ROOT),
                    # the launcher page's logic: strings, the phones' bridge, the disc import, bindings (finding 225)
                    ('test_launcher_ui', ['node', 'port/launcher/test_launcher_ui.js'], ROOT),
                    ('test_launcher_handoff', ['node', 'port/launcher/test_handoff.js'], ROOT),
                    ('test_launcher_controls', ['node', 'port/launcher/test_controls.js'], ROOT),
                    ('test_lang_check', [PY, 'tools/test_lang_check.py'], ROOT),
                    ('test_wasm2c_chunks', [PY, 'port/tools/test_wasm2c_chunks.py'], ROOT),
                    # stack locals the EE frame makes one object (finding 213): unreviewed overruns fail
                    ('test_stack_escapes', [PY, 'port/tools/test_stack_escapes.py'], ROOT),
                    # the OPTIONS walk's evidence rules on a scripted stand-in (finding 219)
                    ('test_options_walk', [PY, 'port/tools/test_options_walk.py'], ROOT),
                    ('stack_escapes', [PY, 'port/tools/stack_escapes.py', '--check'] + j, ROOT),
                    # the gate itself: a unit that does not compile fails the check (finding 219)
                    ('test_stack_gate', [PY, 'port/tools/test_stack_escapes_gate.py', '-j', str(jobs or 6)], ROOT)]
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
