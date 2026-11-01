"""Checks that a file holds no verbatim copy of the retail game: every run of 64 bytes or more
that also appears in the retail ELF (iso/SCUS_971.77, your own, never committed) is reported.
Windows of the ELF with fewer than 8 different byte values (zero fill, padding, repeated
words) or that only count up as 32-bit words (an index table, 0, 1, 2, ...) are not counted:
they are no game data. The few runs the decompiled source compiles to
(its string literals; SOURCE) are listed but allowed.

The release exe must pass it (port/tools/package_release.py runs it): the game's data image and
the VU microcode come from the player's disc at start (port/hle/host.c, finding 053), so dhd.exe
holds only the translated code.
usage: python port/tools/scan_retail.py FILE [--elf iso/SCUS_971.77] [--window 64]"""
import argparse
import functools
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


# Runs the decompiled source itself compiles to (PS2 address, length, unit): string literals of
# its functions, and the CRC-32 table clang computes from func_00234F38's loop. They are code of
# the decompilation, not data copied from the disc; any other run is. A window may start at the NULs
# before its literal: the wasm module packs literals with their terminators, so a run there can
# begin one to three bytes early (finding 082, x64 package).
SOURCE = [
    (0x288A2F, 73, 'src/unsorted/00118898.c: getNextPoint format string, from the NUL before it'),
    (0x288B6F, 136, 'src/unsorted/0011D048.c: both AnimalHerd::addShadow messages'),
    (0x288EAF, 122, 'src/unsorted/00127F20.c: "Stealing highestPed" messages'),
    (0x296A70, 113, 'src/unsorted/001EB1D8.cpp: uiScreenInit messages, "PRESS ... TO START"'),
    (0x296BEE, 126, 'src/unsorted/001EB1D8.cpp: float constants of its menus'),
    (0x2984FF, 67, 'src/unsorted/00218FA8.c: "CLUTs Loaded" message'),
    (0x2985C7, 64, 'src/unsorted/002193E8.c: messages'),
    (0x298687, 93, 'src/unsorted/002193E8.c: messages'),
    (0x2986E8, 80, 'src/unsorted/002193E8.c: messages'),
    (0x298817, 76, 'src/unsorted/0021A008.c: messages'),
    (0x298868, 83, 'src/unsorted/0021A008.c: messages'),
    (0x2988C0, 128, 'src/unsorted/0021A008.c: messages'),
    (0x298B45, 283, 'src/unsorted/0021F400.c: IOP module paths (cdrom0:\\MOD\\...), from the NULs before them'),
    (0x2995B4, 2052, 'src/unsorted/00234F38.c: the CRC-32 table, computed by clang from the loop'),
]
# Public tables of a standard that the game's libraries and the port's both carry, with the zero
# fill the run can take in on either side. The bytes of a linked library are allowed already
# (package_release.game_data_runs), but there the linker's padding around the table differs.
PUBLIC = [
    (0x299535, 71, "DEFLATE's distance base table (RFC 1951; 1, 2, 3, 4, 5, 7 ... 24577 as 16-bit words, "
                   "60 bytes at 0x299538) of the game's zlib; the png crate's inflater (texture packs, "
                   "finding 057) has the same table"),
    (0x281D44, 105, "the game's C library digit string '0123456789abcdefghijklmnopqrstuvwxyz' (at 0x281D80, for "
                    "every base up to 36) with the 60 bytes of zero fill before it and the 9 after; serde_json "
                    "(dhd_presence, finding 106) carries the hexadecimal digits after zero padding in the Linux "
                    "build, and the Linux AppImage's WebKitGTK and Kerberos libraries (finding 125) the whole "
                    "string between zeros"),
]


def elf_address(elf, offset):
    """The PS2 address of an ELF file offset in its loadable segment, else None."""
    import struct
    phoff, = struct.unpack_from('<I', elf, 0x1C)
    _, start, vaddr, _, size, _, _, _ = struct.unpack_from('<8I', elf, phoff)
    return vaddr + offset - start if start <= offset < start + size else None


def allowed(elf, offset, length):
    """Whether the run at ELF offset is one of SOURCE or PUBLIC."""
    address = elf_address(elf, offset)
    return address is not None and any(a <= address and address + length <= a + n for a, n, _ in SOURCE + PUBLIC)


def counting(w):
    """Whether w is 32-bit words counting up by 0 or 1 (0, 0, 1, 2, ... 14), at any alignment: an index
    table any program may hold (dxcompiler.dll, the macOS launcher's Rust code), no game data."""
    if w.count(0) < len(w) * 3 // 4:
        return False
    for s in range(4):
        low = w[s::4]
        if all(w[k] == 0 for k in range(len(w)) if (k - s) % 4) and all(0 <= b - a <= 1 for a, b in zip(low, low[1:])):
            return True
    return False


@functools.lru_cache(maxsize=4)
def windows(elf, size):
    """{hash: [elf offsets]} of the ELF's informative windows (step 1); built once per ELF (a package
    scans hundreds of files against the same one: the AppImage's libraries)."""
    out = {}
    for i in range(len(elf) - size + 1):
        w = elf[i:i + size]
        if len(set(w)) < 8 or counting(w):
            continue
        out.setdefault(hash(w), []).append(i)
    return out


def scan(data, elf, size=64):
    """[(file offset, elf offset, length)] of the runs found, merged."""
    table = windows(elf, size)
    runs, i = [], 0
    while i <= len(data) - size:
        hit = None
        for e in table.get(hash(data[i:i + size]), ()):
            if elf[e:e + size] == data[i:i + size]:
                hit = e
                break
        if hit is None:
            i += 1
            continue
        n = size
        while i + n < len(data) and hit + n < len(elf) and data[i + n] == elf[hit + n]:
            n += 1
        runs.append((i, hit, n))
        i += n
    return runs


def elf_place(elf, offset):
    """'.section+0x...' of a file offset in the ELF, from its section headers."""
    import struct
    shoff, = struct.unpack_from('<I', elf, 0x20)
    shnum, shstr = struct.unpack_from('<HH', elf, 0x30)
    sh = [struct.unpack_from('<10I', elf, shoff + 40 * k) for k in range(shnum)]
    names = sh[shstr][4]
    for s in sh:
        if s[1] != 8 and s[4] <= offset < s[4] + s[5]:
            name = elf[names + s[0]:elf.index(b'\0', names + s[0])].decode()
            return '%s+0x%X (PS2 0x%08X)' % (name, offset - s[4], s[3] + offset - s[4]) if s[3] else \
                '%s+0x%X' % (name, offset - s[4])
    return 'offset 0x%X' % offset


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('file', type=Path)
    ap.add_argument('--elf', type=Path, default=ROOT / 'iso' / 'SCUS_971.77')
    ap.add_argument('--window', type=int, default=64)
    a = ap.parse_args()
    elf, data = a.elf.read_bytes(), a.file.read_bytes()
    runs = scan(data, elf, a.window)
    bad = 0
    for at, e, n in runs:
        ok = allowed(elf, e, n)
        bad += not ok
        print('%s +0x%X: %d bytes = ELF %s%s' % (a.file.name, at, n, elf_place(elf, e),
                                                 ' (decompiled source, SOURCE)' if ok else ''))
    print('%s: %d retail run(s) of %d+ bytes, %d bytes in all; %d not from the decompiled source' % (
        a.file.name, len(runs), a.window, sum(n for _, _, n in runs), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
