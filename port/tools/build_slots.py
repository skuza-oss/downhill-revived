"""At most DHD_BUILD_SLOTS heavy builds at once on this machine (build_wasm.py, build_host64.py).

Several checkouts (worktrees) building at the same time share the CPU and RAM: each core build's
largest translated files take about 4 GB in clang, and too many jobs only make every build
slower. With DHD_BUILD_SLOTS=N set, a build first takes one of N slots (a lock file in
DHD_BUILD_SLOT_DIR, default <DHD_DEVCACHES>/build-slots) and waits while all are taken; the
operating system frees a slot when its build ends, however it ends. Unset, nothing waits.

    with build_slots.slot('build_wasm'):
        ...
"""
import contextlib
import os
import sys
import time
from pathlib import Path

if os.name == 'nt':
    import msvcrt
else:
    import fcntl


def _lock(f):
    try:
        if os.name == 'nt':
            f.seek(0)  # msvcrt locks from the file position: every build must lock the same byte
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


@contextlib.contextmanager
def slot(what):
    count = int(os.environ.get('DHD_BUILD_SLOTS') or 0)
    if count <= 0:
        yield
        return
    folder = Path(os.environ.get('DHD_BUILD_SLOT_DIR') or
                  Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches') / 'build-slots')
    folder.mkdir(parents=True, exist_ok=True)
    start, said = time.time(), False
    while True:
        for i in range(count):
            f = open(folder / ('slot%d.lock' % i), 'a+')
            if _lock(f):
                waited = time.time() - start
                if said or waited > 1:
                    print('%s: build slot %d of %d after %.0f s' % (what, i + 1, count, waited), flush=True)
                try:
                    yield
                finally:
                    f.close()  # the lock goes with the handle
                return
            f.close()
        if not said:
            print('%s: all %d build slots taken (DHD_BUILD_SLOTS); waiting' % (what, count), file=sys.stderr, flush=True)
            said = True
        time.sleep(5)
