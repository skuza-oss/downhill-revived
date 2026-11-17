"""No instruction above the iOS build's CPU floor (finding 267): the A12 (ARMv8.3-A with crypto and
FP16, LLVM's apple-a12), docs/PLAYING.md's minimum and build_host64.py's IOS_CPU.

0.8.9's iOS core was built with -mcpu=apple-m1, and clang used the SHA3 EOR3 on its own in the VU0
code: every A12 (iPhone XS/XR, iPad 8th gen, iPad Air 3, iPad mini 5) closed the game with SIGILL at
the first VU0 call. This scans the code of every arm64 Mach-O slice (sections marked as holding
instructions: the program, the Rust library and SDL inside it) for the encodings below and names
each one found.

The list: every user-mode A64 instruction of the features LLVM's apple-a13 and apple-a14 (= M1) add
to apple-a12 (llvm/lib/Target/AArch64/AArch64Processors.td: SHA3, SHA512, FP16FML, DotProd, FlagM,
AltFPCmp (FlagM2), FRInt3264, RCPC-immo, SB) and of the ones the M2 adds (BF16, I8MM), so that a
later -mcpu raise is caught as well; plus SM3/SM4, which no Apple chip has (the rest of the 0xCE
block, the ARMv8.2 crypto extensions). The system instructions of the same features (SSBS, DIT,
PredRes, MTE) a compiler never emits unasked. What a scan can't see: LSE2 (ARMv8.4) adds no
instruction but lets LLVM treat a 16-byte aligned LDP/STP as atomic; the cure for that is the
-mcpu, which build_host64.py sets.

Two kinds. A compiler picks most of these by itself once the -mcpu allows them (EOR3 for a ^ b ^ c,
DOT in a vectorized sum, FRINT32 in a conversion, STLUR for an atomic store): refused anywhere.
SHA512, SM3 and SM4 come only from intrinsics or assembly that asks for them; in this app that is
the crypto of the network library (ring's CRYPTOGAMS assembly and the sha2 crate), which uses them
only after asking the CPU at run time (sysctl hw.optional.armv8_2_sha512): reported, not refused.
A word inside a run of 16 or more bytes of printable text is text in the code section (ring's
assembly carries its credit string there), not an instruction.

Each mask was checked against clang's own encodings (port/ios/tests/test_cpu_floor.py).
Pure Python, any machine. GPL-3.0-or-later."""
import struct
import sys

# (mask, value, name): an instruction word w is name when w & mask == value. LIBRARY: the run-time
# chosen kind (above); every other name is refused.
PATTERNS = (
    (0xFFE08000, 0xCE000000, 'eor3 (SHA3)'),
    (0xFFE08000, 0xCE200000, 'bcax (SHA3)'),
    (0xFFE0FC00, 0xCE608C00, 'rax1 (SHA3)'),
    (0xFFE00000, 0xCE800000, 'xar (SHA3)'),
    (0xFFE0F000, 0xCE608000, 'sha512h/sha512h2/sha512su1 (SHA512)'),
    (0xFFFFFC00, 0xCEC08000, 'sha512su0 (SHA512)'),
    (0xFF000000, 0xCE000000, 'SM3/SM4 (ARMv8.2 crypto extensions)'),   # after the named ones
    (0xBFE0FC00, 0x0E20EC00, 'fmlal (FP16FML)'),
    (0xBFE0FC00, 0x0EA0EC00, 'fmlsl (FP16FML)'),
    (0xBFE0FC00, 0x2E20CC00, 'fmlal2 (FP16FML)'),
    (0xBFE0FC00, 0x2EA0CC00, 'fmlsl2 (FP16FML)'),
    (0xBFC0F400, 0x0F800000, 'fmlal by element (FP16FML)'),
    (0xBFC0F400, 0x0F804000, 'fmlsl by element (FP16FML)'),
    (0xBFC0F400, 0x2F808000, 'fmlal2 by element (FP16FML)'),
    (0xBFC0F400, 0x2F80C000, 'fmlsl2 by element (FP16FML)'),
    (0xBFE0FC00, 0x0E809400, 'sdot (DotProd)'),
    (0xBFE0FC00, 0x2E809400, 'udot (DotProd)'),
    (0xBFC0F400, 0x0F80E000, 'sdot by element (DotProd)'),
    (0xBFC0F400, 0x2F80E000, 'udot by element (DotProd)'),
    (0xFFFFFFFF, 0xD500401F, 'cfinv (FlagM)'),
    (0xFFE07C10, 0xBA000400, 'rmif (FlagM)'),
    (0xFFFFFC1F, 0x3A00080D, 'setf8 (FlagM)'),
    (0xFFFFFC1F, 0x3A00480D, 'setf16 (FlagM)'),
    (0xFFFFFFFF, 0xD500405F, 'axflag (FlagM2)'),
    (0xFFFFFFFF, 0xD500403F, 'xaflag (FlagM2)'),
    (0xFFBE7C00, 0x1E284000, 'frint32z/frint32x/frint64z/frint64x (FRInt3264)'),
    (0x9FBFEC00, 0x0E21E800, 'frint32z/frint32x/frint64z/frint64x vector (FRInt3264)'),
    (0x3F200C00, 0x19000000, 'stlur/ldapur (RCPC-immo, ARMv8.4)'),
    (0xFFFFFFFF, 0xD50330FF, 'sb (SB)'),
    (0xBFE0FC00, 0x2E40FC00, 'bfdot (BF16)'),
    (0xBFC0F400, 0x0F40F000, 'bfdot by element (BF16)'),
    (0xFFE0FC00, 0x6E40EC00, 'bfmmla (BF16)'),
    (0xBFE0FC00, 0x2EC0FC00, 'bfmlalb/bfmlalt (BF16)'),
    (0xBFC0F400, 0x0FC0F000, 'bfmlalb/bfmlalt by element (BF16)'),
    (0xFFFFFC00, 0x1E634000, 'bfcvt (BF16)'),
    (0xBFFFFC00, 0x0EA16800, 'bfcvtn (BF16)'),
    (0xFFE0FC00, 0x4E80A400, 'smmla (I8MM)'),
    (0xFFE0FC00, 0x6E80A400, 'ummla (I8MM)'),
    (0xFFE0FC00, 0x4E80AC00, 'usmmla (I8MM)'),
    (0xBFE0FC00, 0x0E809C00, 'usdot (I8MM)'),
    (0xBFC0F400, 0x0F00F000, 'sudot by element (I8MM)'),
    (0xBFC0F400, 0x0F80F000, 'usdot by element (I8MM)'),
)
LIBRARY = {'sha512h/sha512h2/sha512su1 (SHA512)', 'sha512su0 (SHA512)', 'SM3/SM4 (ARMv8.2 crypto extensions)'}
_BY_TOP = {}
for _mask, _value, _name in PATTERNS:   # the top byte first: most words need no mask test at all
    _tops = [t for t in range(256) if (t << 24) & _mask == _value & _mask & 0xFF000000]
    for _t in _tops:
        _BY_TOP.setdefault(_t, []).append((_mask, _value, _name))

CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400


def name_of(word):
    """The instruction above the floor that word encodes, or None."""
    for mask, value, name in _BY_TOP.get(word >> 24, ()):
        if word & mask == value:
            return name
    return None


def _code_sections(data, base):
    """(name, file offset, size, address) of each instruction section of the thin arm64 Mach-O at base."""
    magic, cputype = struct.unpack_from('<II', data, base)
    if magic != 0xFEEDFACF:
        raise ValueError('not a 64-bit Mach-O')
    if cputype != CPU_TYPE_ARM64:
        return []
    ncmds, sizeofcmds = struct.unpack_from('<II', data, base + 16)
    at, end, out = base + 32, base + 32 + sizeofcmds, []
    for _ in range(ncmds):
        if at + 8 > end:
            raise ValueError('a cut load command')
        cmd, size = struct.unpack_from('<II', data, at)
        if size < 8 or at + size > end:
            raise ValueError('a load command of a wrong size')
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            for k in range(nsects):
                s = at + 72 + 80 * k
                sect = data[s:s + 16].rstrip(b'\0').decode('ascii', 'replace')
                seg = data[s + 16:s + 32].rstrip(b'\0').decode('ascii', 'replace')
                addr, ssize, offset = struct.unpack_from('<QQI', data, s + 32)
                flags = struct.unpack_from('<I', data, s + 64)[0]
                if flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS) and offset:
                    if base + offset + ssize > len(data):
                        raise ValueError('a section past the end')
                    out.append(('%s,%s' % (seg, sect), base + offset, ssize, addr))
        at += size
    return out


def slices(data):
    """File offsets of the thin Mach-O slices in data (one for a thin file); [] if not a Mach-O."""
    magic = data[:4]
    if magic == b'\xcf\xfa\xed\xfe':
        return [0]
    if magic in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        wide = magic == b'\xca\xfe\xba\xbf'
        count = struct.unpack_from('>I', data, 4)[0]
        entry = 32 if wide else 20
        if not 0 < count <= 32 or 8 + count * entry > len(data):   # as ipa_audit.py (a Java class shares the magic)
            return []
        return [struct.unpack_from('>Q' if wide else '>I', data, 8 + k * entry + 8)[0] for k in range(count)]
    return []


def _in_text(data, offset, lo, hi):
    """Whether the word at offset lies in a run of 16 or more printable bytes within [lo, hi)."""
    printable = lambda b: 0x20 <= b < 0x7F or b in (9, 10, 13)
    a = offset
    while a > lo and printable(data[a - 1]):
        a -= 1
    b = offset
    while b < hi and printable(data[b]):
        b += 1
    return b - a >= 16 and b >= offset + 4


def scan(data, limit=20):
    """(code bytes scanned, {name: count}, [(address, word, name)] of the first `limit` found) over
    every arm64 slice of a Mach-O; text inside the code is skipped."""
    scanned, counts, first = 0, {}, []
    for base in slices(data):
        for _sect, offset, size, addr in _code_sections(data, base):
            words = memoryview(data)[offset:offset + size - size % 4].cast('I')
            scanned += size
            tops = _BY_TOP
            for i, w in enumerate(words):
                if (w >> 24) in tops:
                    name = name_of(w)
                    if name and not _in_text(data, offset + 4 * i, offset, offset + size):
                        counts[name] = counts.get(name, 0) + 1
                        if len(first) < limit and name not in LIBRARY:
                            first.append((addr + 4 * i, w, name))
    return scanned, counts, first


def refused(counts):
    return {n: c for n, c in counts.items() if n not in LIBRARY}


def problems(name, data):
    """What makes a member of an iOS app unfit: an arm64 Mach-O with code above the A12."""
    if not slices(data):
        return []
    try:
        _scanned, counts, first = scan(data, 3)
    except (ValueError, struct.error) as e:
        return ["%s: a Mach-O whose code can't be scanned for the CPU floor (%s)" % (name, e)]
    bad = refused(counts)
    if not bad:
        return []
    found = ', '.join('%s x%d' % (n, c) for n, c in sorted(bad.items(), key=lambda x: -x[1]))
    where = ', '.join('0x%x' % a for a, _w, _n in first)
    return ['%s: instructions above the A12, the iOS CPU floor (finding 267): %s (first at %s)'
            % (name, found, where)]


def main(argv):
    if len(argv) < 2:
        print('usage: cpu_floor.py MACH-O...  (exit 1 when one holds an instruction above the A12)')
        return 2
    bad = 0
    for path in argv[1:]:
        with open(path, 'rb') as f:
            data = f.read()
        scanned, counts, first = scan(data)
        print('%s: %d KiB of arm64 code scanned' % (path, scanned >> 10))
        for n, c in sorted(counts.items(), key=lambda x: -x[1]):
            print('  %6d  %s%s' % (c, n, '  (crypto chosen at run time: allowed)' if n in LIBRARY else ''))
        for a, w, n in first:
            print('  0x%x: %08x %s' % (a, w, n))
        if refused(counts):
            bad = 1
            print('  ABOVE THE FLOOR')
        elif not scanned:
            bad = 1
            print('  no arm64 code found')
        else:
            print('  ok: nothing above the A12')
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv))
