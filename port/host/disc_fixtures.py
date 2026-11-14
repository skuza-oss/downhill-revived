"""Shared synthetic boot-disc fixtures. No game data. Output stays in the caller's scratch."""
from pathlib import Path
import struct
import sys

SECTOR = 2048

def image(cnf, volume=64, blocks=64, pvd=True, root=True):
    b = bytearray(blocks * SECTOR)
    def u32(at, value):
        struct.pack_into('<I', b, at, value)
    if pvd:
        at = 16 * SECTOR
        b[at:at+6] = b'\x01CD001'
        u32(at + 80, volume)
        struct.pack_into('<H', b, at + 128, SECTOR)
        u32(at + 158, 20)
        u32(at + 166, SECTOR)
    if root:
        at = 20 * SECTOR
        b[at] = 45
        u32(at + 2, 21)
        u32(at + 10, len(cnf))
        b[at + 32] = 12
        b[at + 33:at + 45] = b'SYSTEM.CNF;1'
    b[21 * SECTOR:21 * SECTOR + len(cnf)] = cnf
    return b

def write(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    usa = b'BOOT2 = cdrom0:\\SCUS_971.77;1\r\nVER = 1.00\r\n'
    cases = {'good': image(usa), 'no-volume': image(usa, pvd=False),
             'no-cnf': image(b'', root=False), 'other': image(b'BOOT2 = cdrom0:\\SLUS_203.12;1\r\n'),
             'europe': image(b'BOOT2 = cdrom0:\\SLES_522.02;1\r\n'),
             'japan': image(b'BOOT2 = cdrom0:\\SCPS_150.62;1\r\n'),
             'cut': image(usa)[:49152], 'tiny': b'abc',
             'empty-boot': image(b'BOOT2 ='), 'no-boot': image(b'nothing here'),
             'zero-volume': image(usa, volume=0),
             'empty-path': image(b'BOOT2 = cdrom0:\\'),
             'long-boot': image(b'BOOT2 = cdrom0:\\' + b'A' * 300),
             'binary-cnf': image(b'BOOT2\x01\x02 = \x7f')}
    for name, data in cases.items():
        (out / (name + '.iso')).write_bytes(data)

if __name__ == '__main__':
    write(sys.argv[1])
