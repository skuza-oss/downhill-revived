"""game_lock.py's rules, with real holder processes on a lock folder of their own (under DHD_SCRATCH):
two untimed holders together (DHD_GAME_SLOTS=2), a timed one waiting for them, an untimed one
waiting for a timed one (also while that only waits), one place with the default of 1, and a killed
holder freeing its place at once.

    python port/tools/test_game_lock.py"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def child(mode, name, events, release):
    """A holder: takes the lock, writes `held <name> <time>`, waits for its release file, then
    writes `free <name> <time>` and frees it."""
    sys.path.insert(0, str(HERE))
    import game_lock
    game_lock.acquire(name, timed=mode == 'timed', poll=0.05, say=lambda *_: None)
    with open(events, 'a') as f:
        f.write(f'held {name} {time.time():.3f}\n')
    while not os.path.exists(release):
        time.sleep(0.02)
    with open(events, 'a') as f:
        f.write(f'free {name} {time.time():.3f}\n')
    game_lock.release()


class GameLock(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='game-lock-test-', dir=os.environ.get('DHD_SCRATCH')))
        self.events = self.dir / 'events.txt'
        self.events.write_text('')
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        shutil.rmtree(self.dir, ignore_errors=True)

    def start(self, mode, name, slots=1):
        env = dict(os.environ, DHD_SCRATCH=str(self.dir), DHD_GAME_SLOTS=str(slots))
        p = subprocess.Popen([sys.executable, __file__, 'child', mode, name, str(self.events),
                              str(self.dir / f'release-{name}')], env=env)
        self.procs.append(p)
        return p

    def free(self, name):
        (self.dir / f'release-{name}').write_text('')

    def held(self):
        """{name: time} of every holder that has taken the lock so far."""
        return {w[1]: float(w[2]) for w in (l.split() for l in self.events.read_text().splitlines()) if w[0] == 'held'}

    def wait_for(self, names, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            if set(names) <= set(self.held()):
                return
            time.sleep(0.05)
        self.fail(f'{names} not all held after {timeout} s (held: {sorted(self.held())})')

    def stays_out(self, name, seconds=1.5):
        time.sleep(seconds)
        self.assertNotIn(name, self.held(), f'{name} took the lock while it should wait')

    def test_two_untimed_together(self):
        self.start('untimed', 'a', slots=2)
        self.start('untimed', 'b', slots=2)
        self.wait_for(['a', 'b'])
        self.start('untimed', 'c', slots=2)  # a third waits for a place
        self.stays_out('c')
        self.free('a')
        self.wait_for(['c'])

    def test_default_one_at_a_time(self):
        self.start('untimed', 'a')
        self.wait_for(['a'])
        self.start('untimed', 'b')
        self.stays_out('b')
        self.free('a')
        self.wait_for(['b'])

    def test_timed_waits_for_untimed_and_blocks_new_ones(self):
        self.start('untimed', 'a', slots=3)
        self.start('untimed', 'b', slots=3)
        self.wait_for(['a', 'b'])
        self.start('timed', 't', slots=3)
        self.stays_out('t')
        self.start('untimed', 'c', slots=3)  # a free place, but the timed run waits: c waits too
        self.stays_out('c')
        self.free('a')
        self.stays_out('t', 0.8)  # b still holds
        self.free('b')
        self.wait_for(['t'])
        self.stays_out('c')
        self.free('t')
        self.wait_for(['c'])
        held = self.held()
        self.assertGreater(held['c'], held['t'])

    def test_untimed_waits_for_timed(self):
        self.start('timed', 't', slots=2)
        self.wait_for(['t'])
        self.start('untimed', 'a', slots=2)
        self.start('timed', 'u', slots=2)  # timed runs queue for each other
        self.stays_out('a')
        self.assertNotIn('u', self.held())
        self.free('t')
        end = time.time() + 20
        while time.time() < end and not ({'a', 'u'} & set(self.held())):
            time.sleep(0.05)
        first = set(self.held()) - {'t'}
        self.assertEqual(len(first), 1, f'exactly one of a, u after t, got {first}')
        self.stays_out(({'a', 'u'} - first).pop(), 1.0)
        self.free(first.pop())
        self.wait_for(['a', 'u'])

    def test_killed_holder_frees_its_place(self):
        for mode in ('untimed', 'timed'):
            with self.subTest(mode=mode):
                self.events.write_text('')
                p = self.start(mode, 'k' + mode)
                self.wait_for(['k' + mode])
                self.start('untimed', 'w' + mode)
                self.stays_out('w' + mode, 0.8)
                p.kill()
                p.wait()
                start = time.time()
                self.wait_for(['w' + mode], timeout=5)
                self.assertLess(self.held()['w' + mode] - start, 2.0)
                self.free('w' + mode)

    def test_status(self):
        self.start('untimed', 'a', slots=2)
        self.wait_for(['a'])
        out = subprocess.run([sys.executable, str(HERE / 'game_lock.py')], capture_output=True, text=True,
                             env=dict(os.environ, DHD_SCRATCH=str(self.dir))).stdout
        self.assertIn(' a', out)
        self.free('a')
        self.procs[0].wait(10)
        out = subprocess.run([sys.executable, str(HERE / 'game_lock.py')], capture_output=True, text=True,
                             env=dict(os.environ, DHD_SCRATCH=str(self.dir))).stdout
        self.assertEqual(out.strip(), 'free')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'child':
        child(*sys.argv[2:6])
    else:
        unittest.main()
