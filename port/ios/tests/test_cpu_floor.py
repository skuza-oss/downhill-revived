"""port/ios/cpu_floor.py's encodings against clang's own (finding 267): every instruction above the
A12 that it names is found by the scan, under the name it should have, and A12 instructions next to
them in the encoding space (the ARMv8.3 the A12 has, the same classes' older members) are not.
Needs clang (any host: it only assembles); skipped without it."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cpu_floor  # noqa: E402

CLANG = shutil.which('clang')
MARCH = '-march=armv8.6-a+sha3+sm4+fp16fml+fp16'   # ARMv8.6 holds FlagM(2), FRInt3264, RCPC-immo, SB, BF16, I8MM, DotProd

ABOVE = [   # (instruction, the name's start)
    ('eor3 v5.16b, v3.16b, v1.16b, v0.16b', 'eor3'), ('bcax v1.16b, v2.16b, v3.16b, v4.16b', 'bcax'),
    ('rax1 v1.2d, v2.2d, v3.2d', 'rax1'), ('xar v1.2d, v2.2d, v3.2d, #7', 'xar'),
    ('sha512h q1, q2, v3.2d', 'sha512h'), ('sha512h2 q1, q2, v3.2d', 'sha512h'),
    ('sha512su0 v1.2d, v2.2d', 'sha512su0'), ('sha512su1 v1.2d, v2.2d, v3.2d', 'sha512h'),
    ('sm3ss1 v1.4s, v2.4s, v3.4s, v4.4s', 'SM3'), ('sm4e v1.4s, v2.4s', 'SM3'),
    ('fmlal v1.4s, v2.4h, v3.4h', 'fmlal ('), ('fmlsl v1.2s, v2.2h, v3.2h', 'fmlsl ('),
    ('fmlal2 v1.4s, v2.4h, v3.4h', 'fmlal2 ('), ('fmlsl2 v1.4s, v2.4h, v3.4h', 'fmlsl2 ('),
    ('fmlal v1.4s, v2.4h, v3.h[7]', 'fmlal by'), ('fmlsl v1.4s, v2.4h, v3.h[1]', 'fmlsl by'),
    ('fmlal2 v1.4s, v2.4h, v3.h[2]', 'fmlal2 by'), ('fmlsl2 v1.2s, v2.2h, v3.h[0]', 'fmlsl2 by'),
    ('sdot v1.4s, v2.16b, v3.16b', 'sdot ('), ('udot v1.2s, v2.8b, v3.8b', 'udot ('),
    ('sdot v1.4s, v2.16b, v3.4b[3]', 'sdot by'), ('udot v1.2s, v2.8b, v3.4b[1]', 'udot by'),
    ('cfinv', 'cfinv'), ('rmif x3, #5, #9', 'rmif'), ('setf8 w4', 'setf8'), ('setf16 w5', 'setf16'),
    ('axflag', 'axflag'), ('xaflag', 'xaflag'),
    ('frint32z s1, s2', 'frint32z/'), ('frint32x d1, d2', 'frint32z/'), ('frint64z s1, s2', 'frint32z/'),
    ('frint64x d1, d2', 'frint32z/'), ('frint32z v1.4s, v2.4s', 'frint32z/frint32x/frint64z/frint64x vector'),
    ('frint64x v1.2d, v2.2d', 'frint32z/frint32x/frint64z/frint64x vector'),
    ('ldapur w1, [x2, #-4]', 'stlur/ldapur'), ('stlur x1, [x2, #8]', 'stlur/ldapur'),
    ('ldapursb w1, [x2]', 'stlur/ldapur'), ('stlurh w1, [x2, #2]', 'stlur/ldapur'),
    ('sb', 'sb'),
    ('bfdot v1.4s, v2.8h, v3.8h', 'bfdot ('), ('bfdot v1.2s, v2.4h, v3.2h[1]', 'bfdot by'),
    ('bfmmla v1.4s, v2.8h, v3.8h', 'bfmmla'), ('bfmlalb v1.4s, v2.8h, v3.8h', 'bfmlalb/bfmlalt ('),
    ('bfmlalt v1.4s, v2.8h, v3.h[5]', 'bfmlalb/bfmlalt by'), ('bfcvt h1, s2', 'bfcvt ('),
    ('bfcvtn v1.4h, v2.4s', 'bfcvtn'), ('bfcvtn2 v1.8h, v2.4s', 'bfcvtn'),
    ('smmla v1.4s, v2.16b, v3.16b', 'smmla'), ('ummla v1.4s, v2.16b, v3.16b', 'ummla'),
    ('usmmla v1.4s, v2.16b, v3.16b', 'usmmla'), ('usdot v1.4s, v2.16b, v3.16b', 'usdot ('),
    ('sudot v1.4s, v2.16b, v3.4b[1]', 'sudot by'), ('usdot v1.2s, v2.8b, v3.4b[3]', 'usdot by'),
]
A12 = [   # the A12 has these (ARMv8.3-A, crypto, FP16): never refused
    'eor v5.16b, v3.16b, v1.16b', 'bic v1.16b, v2.16b, v3.16b', 'sha256h q1, q2, v3.4s', 'sha1c q1, s2, v3.4s',
    'sha256su0 v1.4s, v2.4s', 'aese v1.16b, v2.16b', 'aesmc v1.16b, v2.16b', 'pmull v1.1q, v2.1d, v3.1d',
    'fmla v1.4s, v2.4s, v3.4s', 'fmla v1.4s, v2.4s, v3.s[1]', 'fmla v1.8h, v2.8h, v3.h[7]', 'fmul v1.4s, v2.4s, v3.s[3]',
    'fmulx v1.4s, v2.4s, v3.4s', 'fmulx v1.4s, v2.4s, v3.s[0]', 'frecps v1.4s, v2.4s, v3.4s', 'fmls v1.8h, v2.8h, v3.8h',
    'mul v1.4s, v2.4s, v3.s[1]', 'mla v1.4s, v2.4s, v3.s[1]', 'mls v1.4s, v2.4s, v3.s[1]', 'smull v1.4s, v2.4h, v3.h[1]',
    'sqrdmlah v1.4s, v2.4s, v3.s[1]', 'sqrdmlah v1.4s, v2.4s, v3.4s', 'sqrdmulh v1.4s, v2.4s, v3.s[1]',
    'fcmla v1.4s, v2.4s, v3.4s, #90', 'fcmla v1.4s, v2.4s, v3.s[1], #0', 'fcadd v1.4s, v2.4s, v3.4s, #90',
    'fjcvtzs w1, d2', 'ldapr w1, [x2]', 'ldaprb w1, [x2]', 'stlr x1, [x2]', 'ldar w1, [x2]', 'ldur w1, [x2, #-4]',
    'stur x1, [x2, #8]', 'ldurb w1, [x2]', 'casal w1, w2, [x3]', 'ldaddal x1, x2, [x3]', 'swpal w1, w2, [x3]',
    'crc32cx w1, w2, x3', 'frintx s1, s2', 'frinta d1, d2', 'frintz v1.4s, v2.4s', 'frintn v1.2d, v2.2d', 'frinti h1, h2',
    'fcvtzs w1, s2', 'fcvtn v1.4h, v2.4s', 'fcvt h1, s2', 'scvtf v1.4s, v2.4s', 'addv s1, v2.4s', 'uaddlv h1, v2.16b',
    'umull v1.8h, v2.8b, v3.8b', 'smlal v1.4s, v2.4h, v3.4h', 'add v1.4s, v2.4s, v3.4s', 'ext v1.16b, v2.16b, v3.16b, #3',
    'tbl v1.16b, {v2.16b}, v3.16b', 'msr nzcv, x1', 'mrs x1, nzcv', 'dmb ish', 'isb', 'nop', 'yield', 'csdb',
    'pacibsp', 'autibsp', 'retab', 'xpaclri', 'braa x1, x2', 'ldraa x1, [x2]', 'adds x1, x2, x3', 'ccmp x1, x2, #0, eq',
    'mrs x1, tpidrro_el0', 'fmov h1, w2', 'fadd h1, h2, h3', 'fabd v1.8h, v2.8h, v3.8h', 'fcmeq v1.8h, v2.8h, #0.0',
]


def assemble(lines, d):
    src, obj = Path(d) / 'a.s', Path(d) / 'a.o'
    src.write_text('.text\n' + '\n'.join(lines) + '\n')
    subprocess.run([CLANG, '--target=arm64-apple-ios17', MARCH,
                    '-c', str(src), '-o', str(obj)], check=True, capture_output=True)
    data = obj.read_bytes()
    sects = cpu_floor._code_sections(data, 0)
    _name, offset, size, _addr = next(s for s in sects if s[0] == '__TEXT,__text')
    return list(struct.unpack_from('<%dI' % (size // 4), data, offset)), data


@unittest.skipUnless(CLANG, 'requires clang')
class CpuFloor(unittest.TestCase):
    def test_above_found_by_name(self):
        with tempfile.TemporaryDirectory() as d:
            words, data = assemble([i for i, _ in ABOVE], d)
        self.assertEqual(len(words), len(ABOVE))
        for (insn, want), w in zip(ABOVE, words):
            self.assertIsNotNone(cpu_floor.name_of(w), '%s (%08x) not found' % (insn, w))
            self.assertTrue(cpu_floor.name_of(w).startswith(want), '%s (%08x): %s' % (insn, w, cpu_floor.name_of(w)))
        _scanned, counts, _first = cpu_floor.scan(data)
        self.assertEqual(sum(counts.values()), len(ABOVE))
        self.assertTrue(cpu_floor.problems('Downhill Revived', data))

    def test_a12_passes(self):
        with tempfile.TemporaryDirectory() as d:
            words, data = assemble(A12, d)
        self.assertEqual(len(words), len(A12))
        for insn, w in zip(A12, words):
            self.assertIsNone(cpu_floor.name_of(w), '%s (%08x) refused as %s' % (insn, w, cpu_floor.name_of(w)))
        self.assertEqual(cpu_floor.problems('Downhill Revived', data), [])

    def test_runtime_crypto_and_text(self):
        """SHA512 (only from crypto that asks the CPU first) is reported, not refused; the bytes of a
        credit string in the code section (ring's assembly: 'ldapurh' in 'CRYPTOGAMS') are text."""
        with tempfile.TemporaryDirectory() as d:
            _words, data = assemble(['sha512h q1, q2, v3.2d', 'sha512su0 v1.2d, v2.2d', 'nop',
                                     '.asciz " CRYPTOGAMS by <appro>, SHA512 for ARMv8"', '.p2align 2',
                                     'nop'], d)
        self.assertEqual(cpu_floor.name_of(0x59524320), 'stlur/ldapur (RCPC-immo, ARMv8.4)')   # ' CRY', word-aligned
        self.assertIn(b' CRY', data)
        _scanned, counts, _first = cpu_floor.scan(data)
        self.assertEqual(sum(counts.values()), 2, counts)
        self.assertEqual(cpu_floor.problems('Downhill Revived', data), [])

    def test_not_a_macho(self):
        self.assertEqual(cpu_floor.problems('launcher/app.js', b'\xca\xfe\xba\xbe\x00\x00\x00\x34class'), [])
        self.assertEqual(cpu_floor.problems('Info.plist', b'<?xml'), [])


if __name__ == '__main__':
    unittest.main()
