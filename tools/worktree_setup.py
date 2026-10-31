"""Make a git worktree of this repository ready to build and test, and list or clean up worktrees.

    python tools/worktree_setup.py [WORKTREE]        # set up WORKTREE (default: the one you are in)
    python tools/worktree_setup.py --list            # every worktree: branch, merged or not, drive, heavy folders
    python tools/worktree_setup.py --remove NAME     # after `git worktree remove`: delete NAME's scratch folders

Setting up does, each step only when needed:
- The game files a fresh worktree lacks (never in git; AGENTS.md): asm/, assets/ and iso/SCUS_971.77
  are copied from the main checkout (the first worktree of `git worktree list`, or --from, which
  may also be a separate clone). tools/compilers and tools/ext (the decomp toolchains, read-only)
  become junctions to the main checkout's, or copies where a junction can't be made.
- On the system drive (C: on Windows: T3's worktrees live there), the heavy folders become junctions
  to DHD_SCRATCH/worktrees/<name>/: build/, online/server/node_modules, port/launcher/node_modules.
  A real folder already there is left alone (reported); --move moves its contents first.
Nothing is ever taken from or written to the main checkout."""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

GAME_FILES = ('asm', 'assets', 'iso/SCUS_971.77')   # copied
SHARED = ('tools/compilers', 'tools/ext')          # junctioned to the main checkout's (copied if that fails)
HEAVY = ('build', 'online/server/node_modules', 'port/launcher/node_modules')
WINDOWS = os.name == 'nt'


def git(*args, cwd=None):
    return subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, check=True).stdout


def worktrees(cwd):
    """[(path, branch or '', head)] from `git worktree list --porcelain`, the main checkout first."""
    found, cur = [], {}
    for line in git('worktree', 'list', '--porcelain', cwd=cwd).splitlines() + ['']:
        if not line:
            if cur:
                found.append((Path(cur['worktree']), cur.get('branch', '').removeprefix('refs/heads/'), cur.get('HEAD', '')))
            cur = {}
        else:
            key, _, value = line.partition(' ')
            cur[key] = value
    return found


def system_drive(path):
    if WINDOWS:
        return Path(path).drive.upper() == os.environ.get('SystemDrive', 'C:').upper()
    return False


def is_link(path):
    return path.is_symlink() or path.is_junction()


def link(path, target):
    """A directory junction (Windows) or symlink at path pointing at target; None, or why it failed."""
    target.mkdir(parents=True, exist_ok=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    if WINDOWS:
        r = subprocess.run(['cmd', '/c', 'mklink', '/J', str(path), str(target)], capture_output=True, text=True)
        return (r.stdout + r.stderr).strip() or 'mklink failed' if r.returncode else None
    try:
        path.symlink_to(target, target_is_directory=True)
    except OSError as e:
        return str(e)


def scratch_root():
    root = os.environ.get('DHD_SCRATCH')
    if not root:
        sys.exit('worktree_setup: set DHD_SCRATCH to a scratch folder off the system drive')
    if system_drive(root):
        sys.exit('worktree_setup: DHD_SCRATCH is on the system drive')
    return Path(root) / 'worktrees'


def setup(tree, main, move):
    tree = tree.resolve()
    print(f'worktree {tree} (main checkout {main})')
    for rel in GAME_FILES:
        src, dst = main / rel, tree / rel
        if dst.exists():
            continue
        if not src.exists():
            print(f'  {rel}: not in the main checkout either (see AGENTS.md / docs/BUILDING.md)')
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        (shutil.copytree if src.is_dir() else shutil.copy2)(src, dst)
        print(f'  {rel}: copied')
    for rel in SHARED:
        src, dst = main / rel, tree / rel
        if not dst.exists() and src.exists():
            failed = link(dst, src)
            if failed:
                shutil.copytree(src, dst)
            print(f'  {rel}: ' + (f'copied (no junction: {failed})' if failed else 'junction to the main checkout'))
    if not system_drive(tree):
        print('  not on the system drive: build folders stay in the worktree')
        return
    base = scratch_root() / tree.name
    for rel in HEAVY:
        path, target = tree / rel, base / rel.replace('/', '_')
        if not path.parent.exists():
            continue
        if path.exists() and is_link(path):
            continue
        if path.exists():
            if not move:
                print(f'  {rel}: a real folder on the system drive (--move moves it to {target})')
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(path), str(target))
            print(f'  {rel}: moved to {target}')
        failed = link(path, target)
        print(f'  {rel}: ' + (f'no junction ({failed})' if failed else f'junction to {target}'))


def listing(cwd):
    trees = worktrees(cwd)
    main_branch = trees[0][1] or 'main'
    merged = set(git('branch', '--merged', main_branch, '--format=%(refname:short)', cwd=cwd).split())
    for path, branch, head in trees:
        heavy = [rel for rel in HEAVY if (path / rel).exists() and not is_link(path / rel)]
        flags = ['merged' if branch in merged and branch != main_branch else ''] + \
                ['system drive' if system_drive(path) else ''] + \
                ([] if path.exists() else ['missing']) + [f'real {h}' for h in heavy if system_drive(path)]
        print(f'{branch or head[:9]:42} {", ".join(f for f in flags if f):40} {path}')


def remove(name, cwd):
    target = scratch_root() / name
    if any(path.name == name for path, _, _ in worktrees(cwd)):
        sys.exit(f'worktree_setup: worktree {name} still exists: `git worktree remove` it first')
    if target.exists():
        shutil.rmtree(target)
        print(f'deleted {target}')
    else:
        print(f'nothing at {target}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('worktree', nargs='?', type=Path)
    p.add_argument('--from', dest='main', type=Path, help='the checkout to copy the game files from')
    p.add_argument('--move', action='store_true', help='move real heavy folders off the system drive')
    p.add_argument('--list', action='store_true')
    p.add_argument('--remove', metavar='NAME', help="a removed worktree's folder name")
    a = p.parse_args()
    here = Path(a.worktree or Path.cwd())
    if a.remove:
        return remove(a.remove, here)
    if a.list:
        return listing(here)
    tree = Path(git('rev-parse', '--show-toplevel', cwd=here).strip())
    main_tree = (a.main or worktrees(here)[0][0]).resolve()
    if main_tree == tree.resolve():
        sys.exit('worktree_setup: this is the main checkout; name a worktree (or run it inside one)')
    setup(tree, main_tree, a.move)


if __name__ == '__main__':
    main()
