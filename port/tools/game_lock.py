"""The PC's game lock for test runs that start the game: untimed checks share the PC, timed runs
have it to themselves.

    with game_lock.held('my-check'):              # untimed: captures, logic, online tests, state traces
        ...start the game, run the check...
    with game_lock.held('bench', timed=True):     # timed: benchmarks, anything measuring fps or GPU time
        ...

Rules:
- Untimed holders: at most DHD_GAME_SLOTS at once on the PC (default 1: one game at a time, as
  before). Set the same value for every lane (a user environment variable): each holder counts
  against its own value.
- A timed holder has the whole PC: it waits until every untimed holder is done, and from the
  moment it starts waiting no new untimed holder starts. Timed holders queue for each other.
- A holder that dies (crash, kill, Ctrl+C) frees its place at once: every place is an operating
  system lock on a file (msvcrt / flock), which goes with the process's handles however it ends.
- Nested holds in one process share the outer hold (a timed hold inside an untimed one stays
  untimed: take the timed one outermost).

The files are in DHD_SCRATCH/dhd-workers/game-slots (the system temp folder without DHD_SCRATCH):
gate.lock (held by the timed run that waits or runs; untimed holders pass it to take a place) and
slot<N>.lock for N below MAX_SLOTS (an untimed holder holds one of the first DHD_GAME_SLOTS, a
timed one holds them all), each with a .owner text, `pid=<pid> <name>`, for the status line.
`python port/tools/game_lock.py` prints the holders, or `free`."""
import contextlib
import os
import sys
import tempfile
import time
from pathlib import Path

if os.name == 'nt':
    import msvcrt
else:
    import fcntl

MAX_SLOTS = 16       # a timed hold takes this many places: DHD_GAME_SLOTS is clamped to it
_held = []           # this process's lock files, while it holds
_depth = [0]
_timed = [False]     # this process holds the gate


def folder():
    return Path(os.environ.get('DHD_SCRATCH') or tempfile.gettempdir()) / 'dhd-workers' / 'game-slots'


def slots():
    try:
        n = int(os.environ.get('DHD_GAME_SLOTS') or 1)
    except ValueError:
        n = 1
    return max(1, min(n, MAX_SLOTS))


def _try(path):
    """The open, locked file at path, or None when another process holds it."""
    f = open(path, 'a+')
    try:
        if os.name == 'nt':
            f.seek(0)  # msvcrt locks from the file position: every holder must lock the same byte
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return f
    except OSError:
        f.close()
        return None


def _owner(lock, text):
    try:
        lock.with_suffix('.owner').write_text(text)
    except OSError:
        pass


def _read_owner(lock):
    try:
        return lock.with_suffix('.owner').read_text().strip()
    except OSError:
        return ''


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


def _timed_owner():
    """The owner text of the timed run that holds the gate, or '' (gate.owner exists only while a
    timed run holds it, or after one died: its pid tells)."""
    text = _read_owner(folder() / 'gate.lock')
    try:
        return text if text.startswith('pid=') and alive(int(text.split()[0][4:])) else ''
    except ValueError:
        return ''


def holders():
    """[(place, owner text)] of every place held now; place is 'timed' for the gate."""
    d, found = folder(), []
    if not d.is_dir():
        return found
    for name in ['gate'] + ['slot%d' % i for i in range(MAX_SLOTS)]:
        lock = d / (name + '.lock')
        if not lock.exists():
            continue
        f = _try(lock)
        if f:
            f.close()
            continue
        found.append(('timed' if name == 'gate' else name, _read_owner(lock)))
    return found


def _describe(found):
    timed = [o for p, o in found if p == 'timed']
    if timed:
        return 'a timed run (%s)' % (timed[0] or 'no owner text')
    return ', '.join(o or p for p, o in found) or 'nothing'


def acquire(name, timed=False, poll=5, say=print):
    if _depth[0]:
        _depth[0] += 1
        return
    d = folder()
    d.mkdir(parents=True, exist_ok=True)
    me = f'pid={os.getpid()} {name}' + (' (timed)' if timed else '')
    said = False
    if timed:
        # The gate first: from now on no untimed holder starts; then every place, as they free.
        while True:
            gate = _try(d / 'gate.lock')
            if gate:
                break
            if not said:
                say(f'game lock: waiting for {_describe(holders())}')
                said = True
            time.sleep(poll)
        _owner(d / 'gate.lock', me)
        _held.append(gate)
        _timed[0] = True
        try:
            for i in range(MAX_SLOTS):
                lock = d / f'slot{i}.lock'
                while True:
                    f = _try(lock)
                    if f:
                        break
                    if not said:
                        others = [h for h in holders() if not h[1].startswith(f'pid={os.getpid()} ')]
                        say(f'game lock: timed run waiting for {_describe(others)} to finish')
                        said = True
                    time.sleep(poll)
                _owner(lock, me)
                _held.append(f)
        except BaseException:
            _release_files()
            raise
        _depth[0] = 1
        return
    n = slots()
    while True:
        gate = _try(d / 'gate.lock')
        if gate:
            got = None
            try:
                for i in range(n):
                    got = _try(d / f'slot{i}.lock')
                    if got:
                        _owner(d / f'slot{i}.lock', me)
                        break
            finally:
                gate.close()
            if got:
                _held.append(got)
                _depth[0] = 1
                return
            wait = poll  # every place taken
        else:
            # A timed run holds the gate (it waits or runs), or another untimed holder passes it
            # for a moment: then try again soon.
            wait = poll if _timed_owner() else min(poll, 0.2)
        if not said and wait == poll:
            say(f'game lock: waiting for {_describe(holders())} (DHD_GAME_SLOTS={n})')
            said = True
        time.sleep(wait)


def _release_files():
    if _timed[0]:
        try:
            (folder() / 'gate.owner').unlink()  # still under the gate: no other timed run wrote it
        except OSError:
            pass
        _timed[0] = False
    while _held:
        f = _held.pop()
        try:
            f.close()  # the lock goes with the handle
        except OSError:
            pass


def release():
    if _depth[0] == 0:
        return
    _depth[0] -= 1
    if _depth[0] == 0:
        _release_files()


@contextlib.contextmanager
def held(name, timed=False, poll=5):
    acquire(name, timed=timed, poll=poll)
    try:
        yield
    finally:
        release()


if __name__ == '__main__':
    found = holders()
    print('free' if not found else '\n'.join(f'{p}: {o or "(no owner text)"}' for p, o in found))
    sys.exit(0)
