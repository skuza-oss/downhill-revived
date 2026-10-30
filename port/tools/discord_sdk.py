"""The Discord Social SDK's library for the Windows package (finding 121): SHA-256 check, unpack.

    python port/tools/discord_sdk.py        prints the x64 DLL (for the package beside dhd.exe)

The presence crate (port/render/presence, sdk.rs) goes through Discord's Social SDK when
discord_partner_sdk.dll is next to the executable, so the Windows package ships it. The SDK is
Discord's own, closed, under Discord's developer terms; it is downloaded by hand from the
developer portal (Discord Social SDK > Downloads, signed in), so this script can't fetch it: put
the archive, pinned here by version and SHA-256 (nothing else is accepted), in the cache folder,
which stays out of the tree: %DHD_DISCORD_SDK_CACHE% if set, else DHD_DEVCACHES/discord-social-sdk
if that exists, else build/downloads/discord-social-sdk. Only the x64 release DLL and the SDK's
License-Notices.txt (the open-source parts inside it, appended to THIRD-PARTY-NOTICES.txt) are
unpacked, to <cache>/<VERSION>/. discord_krisp.dll (voice) is not needed for Rich Presence and not
shipped."""
import hashlib
import os
import sys
import zipfile
from pathlib import Path
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches

VERSION = '1.10.19337'
ARCHIVE = 'DiscordSocialSdk-%s.zip' % VERSION
SHA256 = 'd784097504685953849cc2842561d8a8013326fe0b8df65a1de63ddefde62045'
ARCHES = {'x64': 'discord_social_sdk/bin/release/discord_partner_sdk.dll'}
# The DLL itself: the package ships only these bytes (package_release.py checks them).
DLL_SHA256 = {'x64': 'f9953495b0d6263cc0b926e70f00adc2e3dda478e216ac6c8b36ba23840ab8ee'}
NOTICES = 'discord_social_sdk/License-Notices.txt'
DLL_NAME = 'discord_partner_sdk.dll'

ROOT = Path(__file__).resolve().parents[2]


def cache():
    if os.environ.get('DHD_DISCORD_SDK_CACHE'):
        return Path(os.environ['DHD_DISCORD_SDK_CACHE'])
    if (DEVCACHES / 'discord-social-sdk').is_dir():
        return DEVCACHES / 'discord-social-sdk'
    return ROOT / 'build' / 'downloads' / 'discord-social-sdk'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch():
    """The unpacked directory (the DLL, the notices), from the archive put in the cache by hand."""
    base = cache() / VERSION
    out = base / 'unpacked'
    stamp = out / 'sha256.txt'
    if stamp.exists() and stamp.read_text().strip() == SHA256:
        return out
    archive = next((p for p in (base / ARCHIVE, cache() / ARCHIVE) if p.exists()), None)
    if archive is None:
        sys.exit('discord_sdk: %s is not in %s: download it from the Discord developer portal (Discord '
                 'Social SDK > Downloads, version %s) and put it there' % (ARCHIVE, base, VERSION))
    got = sha256(archive)
    if got != SHA256:
        sys.exit('discord_sdk: %s has SHA-256 %s, expected %s' % (archive, got, SHA256))
    with zipfile.ZipFile(archive) as z:
        for name in list(ARCHES.values()) + [NOTICES]:
            target = out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(z.read(name))
    for arch, name in ARCHES.items():
        if sha256(out / name) != DLL_SHA256[arch]:
            sys.exit('discord_sdk: %s in %s is not the pinned file' % (name, ARCHIVE))
    stamp.write_text(SHA256 + '\n')
    return out


def dll(arch):
    """discord_partner_sdk.dll for 'x64', checked against DLL_SHA256."""
    path = fetch() / ARCHES[arch]
    if sha256(path) != DLL_SHA256[arch]:
        sys.exit('discord_sdk: %s is not the pinned file (SHA-256 %s); delete %s' % (path, DLL_SHA256[arch], path.parent))
    return path


def notices():
    """The SDK's License-Notices.txt: the open-source software inside the DLL."""
    return (fetch() / NOTICES).read_bytes()


if __name__ == '__main__':
    for arch in ARCHES:
        print(arch, dll(arch))
