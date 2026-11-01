"""The site's download links for a release: one release step after the GitHub release is up.

    python port/tools/site_downloads.py <release folder> <site folder>

The release folder holds that release's files (DownhillRevived-<version>-win-setup.exe, -win.zip,
-mac-experimental.dmg and .zip, -linux-experimental.AppImage and .zip). The site gets:
- _redirects: /download/windows, /download/windows-zip, /download/mac, /download/mac-zip,
  /download/linux, /download/linux-zip, each a 302 to that file in the GitHub release v<version>.
  These links stay the same from release to release (posts and the launcher's pages can use them).
- index.html: every <span data-version> says the version, every <span data-size="KEY"> the file's
  size in MB.
"""
import re
import sys
from pathlib import Path

REPO = 'https://github.com/skuza-oss/downhill-revived'
# the link's name: the file's ending after DownhillRevived-<version>
FILES = {'windows': '-win-setup.exe', 'windows-zip': '-win.zip',
         'mac': '-mac-experimental.dmg', 'mac-zip': '-mac-experimental.zip',
         'linux': '-linux-experimental.AppImage', 'linux-zip': '-linux-experimental.zip'}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    release, site = Path(sys.argv[1]), Path(sys.argv[2])
    setups = sorted(release.glob('DownhillRevived-*-win-setup.exe'))
    if len(setups) != 1:
        sys.exit('%s: want one DownhillRevived-<version>-win-setup.exe, found %d' % (release, len(setups)))
    version = setups[0].name[len('DownhillRevived-'):-len(FILES['windows'])]
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        sys.exit('odd version %r' % version)
    lines, sizes = [], {}
    for key, ending in FILES.items():
        f = release / ('DownhillRevived-%s%s' % (version, ending))
        if not f.is_file():
            sys.exit('missing %s' % f)
        sizes[key] = '%d MB' % max(1, round(f.stat().st_size / 1e6))
        lines.append('/download/%-12s %s/releases/download/v%s/%s 302' % (key, REPO, version, f.name))
    (site / '_redirects').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')

    page = site / 'index.html'
    html = page.read_text(encoding='utf-8')
    html, n = re.subn(r'(<span data-version>)[^<]*(</span>)', r'\g<1>%s\g<2>' % version, html)
    if not n:
        sys.exit('%s has no <span data-version>' % page)
    for key, size in sizes.items():
        html = re.sub(r'(<span data-size="%s">)[^<]*(</span>)' % re.escape(key), r'\g<1>%s\g<2>' % size, html)
    page.write_text(html, encoding='utf-8', newline='\n')
    print('site: version %s; %s' % (version, ', '.join('%s %s' % kv for kv in sizes.items())))


if __name__ == '__main__':
    main()
