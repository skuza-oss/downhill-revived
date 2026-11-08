#!/usr/bin/env python3
"""Install the tracked pre-commit hook (tools/hooks/pre-commit) into this clone's hooks folder.

    python tools/hooks/install.py           # once per clone (worktrees share the hooks folder)
    python tools/hooks/install.py --remove

It writes a two-line `pre-commit` there that runs the tracked script, so updates to the script
apply without reinstalling. It does not set core.hooksPath, which would switch off every other
hook in that folder (a local pre-push guard, for one). An existing pre-commit that isn't this
one is left alone: the script says so and stops.
"""
import os, subprocess, sys

MARK = '# installed by tools/hooks/install.py'
STUB = f'''#!/bin/sh
{MARK}
exec sh "$(git rev-parse --show-toplevel)/tools/hooks/pre-commit" "$@"
'''


def main():
    hooks = subprocess.run(['git', 'rev-parse', '--git-path', 'hooks'], capture_output=True, text=True,
                           check=True).stdout.strip()
    target = os.path.join(hooks, 'pre-commit')
    ours = os.path.exists(target) and MARK in open(target, encoding='utf-8', errors='replace').read()
    if '--remove' in sys.argv:
        if ours:
            os.remove(target)
            print(f'removed {target}')
        else:
            print('no pre-commit of ours to remove')
        return 0
    if os.path.exists(target) and not ours:
        print(f'{target} exists and is not this hook: merge it by hand (call tools/hooks/pre-commit from it)')
        return 1
    os.makedirs(hooks, exist_ok=True)
    with open(target, 'w', encoding='utf-8', newline='\n') as f:
        f.write(STUB)
    os.chmod(target, 0o755)
    print(f'installed {target}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
