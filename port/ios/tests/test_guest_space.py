"""iOS's guest memory (finding 258) in an address space with no 2 GiB range left, on a Mac or Linux:
port/host64/test_guest_space.c with guest_memory.c's bounds-checked layout, as the iOS builds have
it. The reservation 0.8.5 made fails there in every mode; the two parts start when they are
reserved at the start or when two holes are free at the game's start, and the game's start says
why when there is no room at all. Needs a C compiler named cc and WABT's wasm-rt.h (WABT_DIR, as
build_host64.py); skipped elsewhere (Windows: guest_memory_win32.c has no such layout).
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CC = shutil.which('cc')
WABT = Path(os.environ.get('WABT_DIR', '/opt/wabt'))
HOST64 = ROOT / 'port/host64'


@unittest.skipUnless(CC and sys.platform != 'win32' and (WABT / 'include/wasm-rt.h').exists(),
                     'requires cc, a POSIX host and WABT (WABT_DIR)')
class GuestSpace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.dir.name) / 'test_guest_space'
        sources = ['test_guest_space.c', 'guest_memory.c', 'guest_memory_posix.c', 'guest_place.c']  # the test first
        subprocess.run([CC, '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function',
                        '-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1', '-DWASM_RT_SKIP_BOUNDS_CHECKS=1',
                        '-I' + str(HOST64), '-I' + str(WABT / 'include'),
                        *[str(HOST64 / s) for s in sources], '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_mode(self, mode):
        return subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=120)

    def test_reserved_at_the_start(self):
        r = self.run_mode('start')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('the single 2 GiB reservation (0.8.5): fails', r.stdout)
        self.assertIn('reserved at the start', r.stderr)
        self.assertIn('room to grow to 2048 MiB', r.stderr)
        self.assertIn('PASS guest space start', r.stdout)

    def test_two_holes_at_the_game_start(self):
        r = self.run_mode('late')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("the start's reservation failed", r.stderr)
        self.assertIn('reserved now', r.stderr)
        self.assertIn('PASS guest space late', r.stdout)
        largest = int(r.stdout.split('largest free range: ')[1].split(' ')[0])
        self.assertLess(largest, 1024)   # the 960 MiB hole: no 2 GiB range anywhere

    def test_no_room_says_why(self):
        r = self.run_mode('none')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("host64 memory: no room for the guest's memory: it needs 928 MiB free at one address", r.stderr)
        self.assertIn('host64 memory: reserve guest memory (12: ', r.stderr)   # run_record.c's line
        self.assertNotIn('PASS', r.stdout)


if __name__ == '__main__':
    unittest.main()
