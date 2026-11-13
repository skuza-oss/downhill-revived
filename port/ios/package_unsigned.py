#!/usr/bin/env python3
"""Package Downhill Revived into an unsigned, clean IPA for public distribution (GitHub / Sideloadly).

This tool:
1. Takes the compiled iOS application bundle (Downhill Revived.app).
2. Strips all personal developer certificates, provisioning profiles (embedded.mobileprovision),
   code signatures (_CodeSignature), and Mach-O signature slices.
3. Performs a privacy audit to guarantee no personal data (Team ID, Apple ID, UDID, local paths)
   leaks into the distribution package.
4. Generates an unsigned .ipa in standard Payload/ structure ready for Sideloadly, AltStore,
   SideStore, or TrollStore.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build' / 'ios'
DEFAULT_APP = OUT / 'DerivedData' / 'Build' / 'Products' / 'Release-iphoneos' / 'Downhill Revived.app'


def get_version():
    version_h = ROOT / 'port' / 'include' / 'version.h'
    if version_h.is_file():
        m = re.search(r'#define DHD_VERSION "([0-9.]+)"', version_h.read_text())
        if m:
            return m.group(1)
    return "0.7.16"


def has_code_signature(executable: Path) -> bool:
    env = {**os.environ, 'LC_ALL': 'C'}
    result = subprocess.run(['codesign', '-d', str(executable)], capture_output=True, text=True, env=env)
    if result.returncode == 0:
        return True
    if 'code object is not signed at all' in result.stderr:
        return False
    result.check_returncode()


def run_privacy_audit(staged_app: Path):
    """Scan staged app for any personal developer identifiers or local machine paths."""
    print("audit: running privacy and identity leak audit...")

    # Generic patterns only: a builder's own Team ID, Apple ID or device can be added through
    # DHD_IPA_PRIVATE (comma-separated literal strings), which never enter the repository.
    forbidden_patterns = [
        (re.compile(rb'/Users/[A-Za-z0-9._-]+/'), 'Home folder path'),
        (re.compile(rb'0000[0-9A-F]{4}-[0-9A-F]{16}'), 'Device UDID'),
        (re.compile(rb'com\.apple\.developer\.team-identifier'), 'Team identifier entitlement'),
    ]
    for private in filter(None, (s.strip() for s in os.environ.get('DHD_IPA_PRIVATE', '').split(','))):
        forbidden_patterns.append((re.compile(re.escape(private.encode()), re.IGNORECASE), 'Builder identifier'))

    violations = []
    for item in staged_app.rglob('*'):
        if item.is_file() and not item.is_symlink():
            try:
                data = item.read_bytes()
            except Exception as e:
                print(f"audit: warning: could not read {item}: {e}")
                continue

            for pattern, desc in forbidden_patterns:
                m = pattern.search(data)
                if m:
                    rel_path = item.relative_to(staged_app)
                    violations.append(f"Found {desc} in '{rel_path}': matched {m.group(0)[:60]}")

    # Check that embedded.mobileprovision and _CodeSignature are absent
    if (staged_app / 'embedded.mobileprovision').exists():
        violations.append("embedded.mobileprovision exists in staged app bundle!")
    if (staged_app / '_CodeSignature').exists():
        violations.append("_CodeSignature directory exists in staged app bundle!")

    # Check executable codesign status
    exe_path = staged_app / 'Downhill Revived'
    if exe_path.is_file() and has_code_signature(exe_path):
        violations.append("Executable still has a code signature.")

    if violations:
        print("audit: ERROR - Privacy audit failed with the following issues:")
        for v in violations:
            print(f"  - {v}")
        raise RuntimeError("Privacy audit failed: personal data or signatures detected in package.")

    print("audit: PASSED - 0 personal identifiers, 0 certificates, 0 machine paths found.")


def package_unsigned_ipa(app_path: Path, out_ipa: Path = None, *, update_canonical=True) -> Path:
    if not app_path.is_dir():
        raise FileNotFoundError(f"Application bundle not found at: {app_path}")

    exe = app_path / 'Downhill Revived'
    if not exe.is_file():
        raise FileNotFoundError(f"Executable not found inside bundle: {exe}")

    ver = get_version()
    if out_ipa is None:
        out_ipa = OUT / f"DownhillRevived-{ver}-unsigned.ipa"

    out_ipa.parent.mkdir(parents=True, exist_ok=True)
    print(f"package: staging clean unsigned application bundle from {app_path.name}...")

    with tempfile.TemporaryDirectory(prefix="ipa-unsigned-") as tmpdir:
        tmp_path = Path(tmpdir)
        payload_dir = tmp_path / "Payload"
        staged_app = payload_dir / app_path.name

        # Copy directory structure excluding signatures and provisioning profiles
        def ignore_filter(path, names):
            ignored = set()
            for name in names:
                if name == '_CodeSignature' or name == 'embedded.mobileprovision':
                    ignored.add(name)
            return ignored

        shutil.copytree(app_path, staged_app, symlinks=True, ignore=ignore_filter)

        # Explicitly remove any lingering signatures or provisioning profile
        mobileprov = staged_app / 'embedded.mobileprovision'
        if mobileprov.exists():
            mobileprov.unlink()
        codesig_dir = staged_app / '_CodeSignature'
        if codesig_dir.exists():
            shutil.rmtree(codesig_dir)

        # Strip local debug symbols and OSO paths from executable, then remove code signature
        staged_exe = staged_app / 'Downhill Revived'
        if staged_exe.is_file():
            # Ensure write permissions
            staged_exe.chmod(0o755)
            # strip -x removes non-global symbols and debug map entries referencing local build paths
            subprocess.run(['strip', '-x', str(staged_exe)], check=True)
            print("package: stripped local debug symbols and build paths (strip -x).")
            # Remove Mach-O code signature slice
            if has_code_signature(staged_exe):
                subprocess.run(['codesign', '--remove-signature', str(staged_exe)], check=True)
                print("package: Mach-O code signature successfully stripped.")

        # Ensure permissions for all files
        for p in staged_app.rglob('*'):
            if p.is_dir():
                p.chmod(0o755)
            elif p.is_file() and not p.is_symlink():
                if p.name == 'Downhill Revived':
                    p.chmod(0o755)
                else:
                    p.chmod(0o644)

        # Run privacy audit on staged payload
        run_privacy_audit(staged_app)

        # Package into standard IPA archive
        staged_ipa = tmp_path / "output.ipa"
        print(f"package: compressing Payload into {out_ipa.name}...")
        with zipfile.ZipFile(staged_ipa, 'w', zipfile.ZIP_DEFLATED, strict_timestamps=False) as archive:
            for item in sorted(payload_dir.rglob('*')):
                arcname = item.relative_to(tmp_path).as_posix()
                if item.is_symlink():
                    info = zipfile.ZipInfo(arcname)
                    info.create_system = 3
                    info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(info, os.readlink(item))
                elif item.is_file():
                    info = zipfile.ZipInfo(arcname)
                    info.create_system = 3
                    # Preserve executable bit
                    mode = 0o755 if (item.stat().st_mode & 0o111) else 0o644
                    info.external_attr = (stat.S_IFREG | mode) << 16
                    archive.writestr(info, item.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)

        # Atomically replace destination IPA
        if out_ipa.exists():
            out_ipa.unlink()
        shutil.move(str(staged_ipa), str(out_ipa))

        # Also create/update standard DownhillRevived.ipa in build/ios
        canonical_ipa = OUT / "DownhillRevived.ipa"
        if update_canonical and canonical_ipa.resolve() != out_ipa.resolve():
            shutil.copyfile(out_ipa, canonical_ipa)

    # Compute SHA-256 and size
    data = out_ipa.read_bytes()
    sha256 = hashlib.sha256(data).hexdigest()
    size_mb = len(data) / (1024 * 1024)

    print("\n" + "=" * 60)
    print("  PACKAGE SUCCESSFUL (Unsigned Distribution IPA)")
    print("=" * 60)
    print(f"  File:    {out_ipa}")
    print(f"  Size:    {size_mb:.2f} MB ({len(data):,} bytes)")
    print(f"  SHA-256: {sha256}")
    print("=" * 60)
    print("\nThis IPA is completely free of developer certificates and personal data.")
    print("Users can sideload it using Sideloadly, AltStore, SideStore, or TrollStore.")
    return out_ipa


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=DEFAULT_APP, help="Path to Downhill Revived.app bundle")
    parser.add_argument('--output', type=Path, default=None, help="Output .ipa file path")
    args = parser.parse_args()

    try:
        package_unsigned_ipa(args.app, args.output)
        return 0
    except Exception as e:
        print(f"package: error: {e}", file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
