"""The PC's one-game lock for test runs that time or watch a game (one game at a time on this PC).

    with game_lock.held('my-check'):
        ...start the game, run the check...

The lock is a folder, DHD_SCRATCH/dhd-workers/game.lock (the system temp folder without DHD_SCRATCH),
made atomically; its owner file names the holder: `pid=<pid> <name>`. A waiting tool takes the lock
over as soon as that process is gone (a crashed or killed run frees it at once), and an owner file
without a pid (older scratch tools) after 40 minutes, as those tools did. Nested holds in one
process share the lock. `python port/tools/game_lock.py` prints the holder, or `free`."""
import contextlib
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

LOCK = Path(os.environ.get('DHD_SCRATCH') or tempfile.gettempdir()) / 'dhd-workers' / 'game.lock'
OLD_STYLE_AGE = 40 * 60
_depth = [0]


def alive(pid):
    if os.name == 'nt':
        import ctypes
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.OpenProcess.restype = ctypes.c_void_p
        h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return ctypes.get_last_error() == 5  # access denied: it exists
        code = ctypes.c_ulong()
        ok = k32.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code))
        k32.CloseHandle(ctypes.c_void_p(h))
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def holder():
    """(owner text, stale) of the current lock, or None when it is free."""
    try:
        text = (LOCK / 'owner').read_text().strip()
    except OSError:
        if not LOCK.exists():
            return None
        text = ''
    if text.startswith('pid='):
        try:
            return text, not alive(int(text.split()[0][4:]))
        except ValueError:
            pass
    try:
        return text, time.time() - LOCK.stat().st_mtime > OLD_STYLE_AGE
    except OSError:
        return None


def acquire(name, poll=5, say=print):
    if _depth[0]:
        _depth[0] += 1
        return
    waited = False
    while True:
        try:
            LOCK.parent.mkdir(parents=True, exist_ok=True)
            LOCK.mkdir()
            (LOCK / 'owner').write_text(f'pid={os.getpid()} {name}')
            _depth[0] = 1
            return
        except FileExistsError:
            h = holder()
            if h and h[1]:
                say(f'game lock: taking over from a holder that is gone ({h[0] or "no owner file"})')
                shutil.rmtree(LOCK, ignore_errors=True)
                continue
            if not waited and h:
                say(f'game lock: waiting for {h[0]}')
                waited = True
            time.sleep(poll)


def release():
    if _depth[0] == 0:
        return
    _depth[0] -= 1
    if _depth[0] == 0:
        try:
            if (LOCK / 'owner').read_text().startswith(f'pid={os.getpid()} '):
                shutil.rmtree(LOCK, ignore_errors=True)
        except OSError:
            pass


@contextlib.contextmanager
def held(name):
    acquire(name)
    try:
        yield
    finally:
        release()


if __name__ == '__main__':
    h = holder()
    print('free' if h is None else f'{h[0] or "(no owner file)"}{" (stale)" if h[1] else ""}')
    sys.exit(0)
