"""The release IPA's audit (port/ios/ipa_audit.py, finding 227) through package_release.py's ios_release
itself, with a made-up IPA (no game data, no real app): a clean one is published; a signed executable
whose signature folder was removed, a device UDID in a plist and a DHD_IPA_PRIVATE literal in a
resource are each refused. Only the Rust crates' notices (cargo metadata) are stood in for."""
from argparse import Namespace
from pathlib import Path
import io
import os
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'port' / 'tools'))
sys.path.insert(0, str(ROOT / 'port' / 'ios'))
import ipa_audit  # noqa: E402
import package_release  # noqa: E402

LC_SYMTAB, LC_CODE_SIGNATURE = 0x2, 0x1D


def macho(version, signed):
    """A small arm64 Mach-O: one LC_SYMTAB, and LC_CODE_SIGNATURE when signed; the version string the
    release check looks for in its data."""
    commands = LC_SYMTAB.to_bytes(4, 'little') + (24).to_bytes(4, 'little') + bytes(16)
    if signed:
        commands += LC_CODE_SIGNATURE.to_bytes(4, 'little') + (16).to_bytes(4, 'little') + bytes(8)
    header = (b'\xcf\xfa\xed\xfe' + (0x0100000C).to_bytes(4, 'little') + bytes(4) + (2).to_bytes(4, 'little')
              + (2 if signed else 1).to_bytes(4, 'little') + len(commands).to_bytes(4, 'little') + bytes(8))
    return header + commands + b'\0dhd-port %s test\0' % version.encode() + bytes(256)


def ipa(members):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w') as z:
        for name, data in members.items():
            z.writestr('Payload/Downhill Revived.app/' + name, data)
    return out.getvalue()


class ReleaseAudit(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        import build_ios
        self.version = build_ios.version()[0]
        (self.dir / 'elf').write_bytes(b'\x7fELF' + bytes(64))

    def tearDown(self):
        self.tmp.cleanup()

    def release(self, members, env=None):
        path = self.dir / 'in.ipa'
        path.write_bytes(ipa(members))
        a = Namespace(version=self.version, ipa=path, elf=self.dir / 'elf', settings=None, base_url=None,
                      core=None, wasm=None, jobs=1)
        out = self.dir / 'out' / ('DownhillRevived-%s-ios-experimental.ipa' % self.version)
        with patch.object(package_release, 'rust_notices', return_value='(the crates)'), \
                patch.dict(os.environ, env or {}, clear=False):
            return package_release.ios_release(a, package_release.PLATFORMS['ios'], out, self.dir / 'out' / 'latest.json',
                                               'https://github.com/skuza-oss/downhill-revived/releases/tag/v' + self.version)

    def refused(self, members, words, env=None):
        with self.assertRaises(SystemExit) as stop:
            self.release(members, env)
        self.assertIn(words, str(stop.exception))
        self.assertFalse((self.dir / 'out' / ('DownhillRevived-%s-ios-experimental.ipa' % self.version)).exists())

    def clean(self):
        return {'Downhill Revived': macho(self.version, False), 'Info.plist': b'<plist><dict></dict></plist>'}

    def test_clean_ipa_is_published(self):
        self.assertEqual(self.release(self.clean()), 0)
        with zipfile.ZipFile(self.dir / 'out' / ('DownhillRevived-%s-ios-experimental.ipa' % self.version)) as z:
            names = z.namelist()
        self.assertEqual(len(names), len(set(names)))   # the licences once, not twice
        self.assertIn('Payload/Downhill Revived.app/LICENSE.txt', names)

    def test_signed_executable_without_its_signature_folder(self):
        members = self.clean()
        members['Downhill Revived'] = macho(self.version, True)
        self.refused(members, 'still carries a code signature')

    def test_signed_framework(self):
        members = self.clean()
        members['Frameworks/x.framework/x'] = macho(self.version, True)
        self.refused(members, 'Frameworks/x.framework/x: a Mach-O that still carries a code signature')

    def test_unreadable_macho(self):
        members = self.clean()
        members['Frameworks/y.dylib'] = macho(self.version, False)[:40]
        self.refused(members, "can't be verified")

    def test_udid_in_a_plist(self):
        members = self.clean()
        members['Info.plist'] = b'<plist><string>00008110-001A2B3C4D5E6F70</string></plist>'
        self.refused(members, 'a device UDID')

    def test_private_literal_in_a_resource(self):
        members = self.clean()
        members['launcher/notes.txt'] = b'built by Somebody Example'
        self.refused(members, 'a builder identifier', {'DHD_IPA_PRIVATE': 'somebody example,ABCDE12345'})

    def test_provisioning_profile(self):
        members = self.clean()
        members['embedded.mobileprovision'] = b'<key>ProvisionedDevices</key>'
        self.refused(members, 'the IPA must be unsigned')

    def test_macho_parser(self):
        self.assertIsNone(ipa_audit.macho_signed(b'plain text'))
        self.assertFalse(ipa_audit.macho_signed(macho('1.0.0', False)))
        self.assertTrue(ipa_audit.macho_signed(macho('1.0.0', True)))
        thin = macho('1.0.0', True)
        fat = (b'\xca\xfe\xba\xbe' + (1).to_bytes(4, 'big') + (0x0100000C).to_bytes(4, 'big') + bytes(4)
               + (64).to_bytes(4, 'big') + len(thin).to_bytes(4, 'big') + bytes(4))
        fat += bytes(64 - len(fat)) + thin
        self.assertTrue(ipa_audit.macho_signed(fat))
        with self.assertRaises(ValueError):
            ipa_audit.macho_signed(thin[:30])


if __name__ == '__main__':
    unittest.main()
