"""Names the functions of a crash stack in a player's dhd.log (Windows).

The game writes a guest memory fault as "host64 memory: ... at rip ... (image base ...)" and then
"  at rva 0x..." lines (port/host64/guest_memory_win32.c). The rvas are offsets into that build's
dhd.exe; its lld-link map (build_host64.py's dhd64.map, kept beside the release zip by
package_release.py as DownhillRevived-<version>-win.map) names them. Check that dhd.log's first
line (the exe's sha256, first 16 digits) is the build the map came from.

    python port/tools/crash_stack.py --map DownhillRevived-0.7.3-win.map dhd.log [more logs]

Game functions show as w2c_core_func_XXXXXXXX (the PS2 address, src/), the asm blocks the
runtime runs as w2c_core_ps2a_* (port/runtime/ps2asm.c).
"""
import argparse
import bisect
import re
import sys
from pathlib import Path

BASE = 0x140000000  # lld-link's preferred load address for x64 exes (the map's VAs)
SYMBOL = re.compile(r'^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(\S.*)$')


def read_map(path):
    """Address -> (name, object); the shortest name where aliases share an address."""
    symbols = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = SYMBOL.match(line)
            if m:
                at, name = int(m.group(2), 16), m.group(1)
                if at not in symbols or len(name) < len(symbols[at][0]):
                    symbols[at] = (name, m.group(3).strip())
    return symbols


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--map', type=Path, required=True, help="the build's dhd64.map")
    ap.add_argument('logs', type=Path, nargs='+', help="players' dhd.log files")
    a = ap.parse_args()
    symbols = read_map(a.map)
    addresses = sorted(symbols)
    for log in a.logs:
        lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
        print('%s: %s' % (log, lines[0] if lines else 'empty'))
        for line in lines:
            if line.startswith('host64 memory:'):
                print('  ' + line)
            m = re.match(r'\s*at rva 0x([0-9a-f]+)', line)
            if m:
                at = BASE + int(m.group(1), 16)
                i = bisect.bisect_right(addresses, at) - 1
                name, unit = symbols[addresses[i]] if i >= 0 else ('?', '')
                print('    0x%-8s %s+0x%x  (%s)' % (m.group(1), name, at - addresses[max(i, 0)], unit))
    return 0


if __name__ == '__main__':
    sys.exit(main())
