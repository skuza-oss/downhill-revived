"""iOS's guest memory (findings 258, 267) in an address space with no 2 GiB range left, on a Mac or Linux:
port/host64/test_guest_space.c with guest_memory.c's bounds-checked layout, as the iOS builds have
it. The reservation 0.8.5 made fails there in every mode; the two parts start when they are
reserved at the start or when two holes are free at the game's start, and the game's start says
why when there is no room at all; in the player's iPhone XR address space (finding 267) 0.8.9's one
base fits nowhere and the two parts at their own addresses start. With the guest in place, accesses outside its two parts (into a
readable, writable mapping in the gap and one above the high part, through the core's access
paths and GUEST()) end their process by SIGSEGV with the fault reporter's line and leave the
mapping untouched. Needs a C compiler named cc and WABT's wasm-rt.h (WABT_DIR, as
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
                        '-I' + str(HOST64), '-I' + str(ROOT / 'port/include'), '-I' + str(WABT / 'include'),
                        *[str(HOST64 / s) for s in sources], '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_mode(self, mode):
        return subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=600)   # filling the space takes seconds, minutes on a busy Mac

    WILD = 12   # test_guest_space.c's wild_accesses

    def assert_wild(self, r):
        self.assertEqual(r.stdout.count(': ended by SIGSEGV'), self.WILD, r.stdout)
        self.assertGreaterEqual(r.stdout.count('ended by SIGSEGV, the mapping there untouched'), self.WILD - 1, r.stdout)
        self.assertIn('wild plain access in the gap at 0x50000000', r.stdout)
        self.assertIn('host64 memory: access of uncommitted guest address 0x050000000\n', r.stderr)   # run_record.c's line
        self.assertIn('host64 memory: access of uncommitted guest address 0x039ffffe0\n', r.stderr)

    def test_reserved_at_the_start(self):
        r = self.run_mode('start')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('the single 2 GiB reservation (0.8.5): fails', r.stdout)
        self.assertIn('reserved at the start', r.stderr)
        self.assertIn('room to grow to 2048 MiB', r.stderr)
        self.assertIn('PASS guest space start', r.stdout)
        self.assert_wild(r)

    def test_two_holes_at_the_game_start(self):
        r = self.run_mode('late')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("the start's reservation failed", r.stderr)
        self.assertIn('reserved now', r.stderr)
        self.assertIn('PASS guest space late', r.stdout)
        self.assert_wild(r)
        largest = int(r.stdout.split('largest free range: ')[1].split(' ')[0])
        self.assertLess(largest, 1024)   # the 960 MiB hole: no 2 GiB range anywhere

    def test_the_players_iphone_xr(self):
        r = self.run_mode('phone')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('the one base with the high part 1792 MiB above (0.8.9): fails', r.stdout)
        self.assertIn("the start's reservation failed", r.stderr)
        self.assertIn('reserved now', r.stderr)
        self.assertIn('room to grow to 2048 MiB', r.stderr)   # the high part in the 384 MiB at the top
        self.assertIn('PASS guest space phone', r.stdout)
        self.assert_wild(r)

    def test_no_room_says_why(self):
        r = self.run_mode('none')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("host64 memory: no room for the guest's memory: it needs 928 MiB free at one address", r.stderr)
        self.assertIn('host64 memory: reserve guest memory (12: ', r.stderr)   # run_record.c's line
        self.assertIn("error box: Downhill Revived: The game can't get the memory it needs on this device", r.stderr)
        self.assertNotIn('PASS', r.stdout)


if __name__ == '__main__':
    unittest.main()
