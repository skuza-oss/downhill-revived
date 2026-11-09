"""Build Downhill Revived for native iOS (iPhone 17 Pro Max / ARM64).

    python port/ios/build_ios.py [--wasm build/wasm/dhd_core.wasm] [--release] [--install]

1. Rebuilds the wasm core, native ARM64 host and Rust renderer from source (macOS / iOS SDK).
2. Generates the Xcode project structure, copying native Swift launcher files, touch control assets,
   Metal bridge, and platform host files.
3. Generates DownhillRevived.xcodeproj with xcodegen.
4. Invokes xcodebuild to produce the native signed .app bundle.
5. Packages DownhillRevived.ipa and optionally installs to a connected iOS device.
"""
import argparse
import json
import os
import stat
import tempfile
import zipfile
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'port' / 'tools'))

OUT = ROOT / 'build' / 'ios'
PROJECT = OUT / 'project'


def setup_env():
    devcaches = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')
    for cand in [
        devcaches / 'wasi-sdk/wasi-sdk-34.0-x86_64-macos/share/wasi-sysroot',
        devcaches / 'wasi-sdk/wasi-sdk-34.0-arm64-macos/share/wasi-sysroot',
        devcaches / 'wasi-sdk/wasi-sdk-34.0-x86_64-windows/share/wasi-sysroot',
    ]:
        if cand.exists():
            os.environ.setdefault('WASI_SYSROOT', str(cand))
            break

    wabt_dir = devcaches / 'wabt/wabt-1.0.42'
    if wabt_dir.exists():
        os.environ.setdefault('WABT_DIR', str(wabt_dir))
        wabt_bin = wabt_dir / 'bin'
        if wabt_bin.is_dir() and str(wabt_bin) not in os.environ.get('PATH', ''):
            os.environ['PATH'] = str(wabt_bin) + os.pathsep + os.environ.get('PATH', '')

    if sys.platform == 'darwin':
        os.environ.setdefault('DHD_X86_CLANG', '/usr/bin/clang')


def version():
    text = (ROOT / 'port' / 'include' / 'version.h').read_text()
    name = re.search(r'#define DHD_VERSION "([0-9.]+)"', text).group(1)
    major, minor, patch = (int(x) for x in name.split('.'))
    return name, major * 10000 + minor * 100 + patch


def prepare_native_core(wasm=None, jobs=4):
    """Rebuild the module and host before copying the archive into the Xcode project."""
    setup_env()
    if wasm is None:
        default_wasm = ROOT / 'build' / 'wasm' / 'dhd_core.wasm'
        # Always rebuild: an uploaded project may contain an older cached module.
        vutext = Path(os.environ.get('DHD_VUTEXT') or ROOT / 'assets' / 'vutext.bin')
        if not vutext.is_file():
            if not (ROOT / 'iso' / 'SCUS_971.77').exists():
                raise RuntimeError('Missing assets/vutext.bin and iso/SCUS_971.77. See docs/BUILDING.md.')
            print('ios: regenerating assembly and VU assets from your local game executable', flush=True)
            subprocess.run([sys.executable, str(ROOT / 'tools' / 'split.py')], cwd=ROOT, check=True)
        subprocess.run([sys.executable, str(ROOT / 'port/tools/build_wasm.py'), '-j', str(jobs)], cwd=ROOT, check=True)
        wasm = default_wasm
    else:
        wasm = Path(wasm).resolve()
        if not wasm.is_file():
            raise FileNotFoundError('The explicitly supplied wasm module does not exist: %s' % wasm)
        print('ios: using explicitly supplied wasm; source-clock changes must already be in it', flush=True)
    subprocess.run([sys.executable, str(ROOT / 'port/tools/build_host64.py'), str(wasm),
                    '--target', 'ios-arm64', '--jobs', str(jobs), '--num-outputs', '8'], cwd=ROOT, check=True)
    if not (ROOT / 'build/host64-ios/libdhd_ios.a').is_file():
        raise RuntimeError('The native build did not produce libdhd_ios.a')


def prepare_project_bundle(team=None):
    team = resolve_team(team)
    print('ios: preparing project bundle at', PROJECT)
    PROJECT.mkdir(parents=True, exist_ok=True)

    # 1. Launcher and Swift sources
    launcher_dst = PROJECT / 'Launcher'
    if launcher_dst.exists():
        shutil.rmtree(launcher_dst)
    shutil.copytree(HERE / 'launcher', launcher_dst)

    # 2. Touch input and host bridge
    input_dst = PROJECT / 'Input'
    if input_dst.exists():
        shutil.rmtree(input_dst)
    shutil.copytree(HERE / 'input', input_dst)
    shutil.copyfile(ROOT / 'port/hle/stick.h', input_dst / 'stick.h')

    host_dst = PROJECT / 'Host'
    if host_dst.exists():
        shutil.rmtree(host_dst)
    shutil.copytree(HERE / 'host', host_dst)
    shutil.copyfile(ROOT / 'port/render/include/dhd_settings.h', host_dst / 'dhd_settings.h')
    shutil.copyfile(ROOT / 'port/hle/controls_table.h', host_dst / 'controls_table.h')

    render_dst = PROJECT / 'Render'
    if render_dst.exists():
        shutil.rmtree(render_dst)
    shutil.copytree(HERE / 'render', render_dst)

    # 3. Touch Assets & AppIcon
    assets_dst = PROJECT / 'Assets' / 'touch-controls'
    if assets_dst.exists():
        shutil.rmtree(assets_dst)
    shutil.copytree(HERE / 'assets' / 'touch-controls', assets_dst)

    appicon_src = HERE / 'assets' / 'AppIcon.xcassets'
    appicon_dst = PROJECT / 'Assets' / 'AppIcon.xcassets'
    if appicon_dst.exists():
        shutil.rmtree(appicon_dst)
    if appicon_src.exists():
        shutil.copytree(appicon_src, appicon_dst)

    # 4. Info.plist & LaunchScreen.storyboard
    shutil.copyfile(HERE / 'project' / 'Info.plist', PROJECT / 'Info.plist')
    if (HERE / 'project' / 'LaunchScreen.storyboard').exists():
        shutil.copyfile(HERE / 'project' / 'LaunchScreen.storyboard', PROJECT / 'LaunchScreen.storyboard')

    # 5. Bridging Header
    bridging_header = PROJECT / 'DownhillRevived-Bridging-Header.h'
    bridging_header.write_text(
        '// DownhillRevived-Bridging-Header.h\n'
        '#import "ios_main.h"\n'
        '#import "touch_input.h"\n'
        '#import "metal_bridge.h"\n'
    )

    # 6. Check for compiled real game core (libdhd_ios.a)
    core_lib = ROOT / 'build' / 'host64-ios' / 'libdhd_ios.a'
    has_core = core_lib.exists()
    lib_settings = ''
    lib_dst = PROJECT / 'Lib'
    if lib_dst.exists():
        shutil.rmtree(lib_dst)
    if has_core:
        lib_dst = PROJECT / 'Lib'
        lib_dst.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(core_lib, lib_dst / 'libdhd_ios.a')
        print('ios: linked real game core (libdhd_ios.a, %.1f MB)' % (core_lib.stat().st_size / (1024 * 1024)))
        lib_settings = (
            '        LIBRARY_SEARCH_PATHS:\n'
            '          - $(SRCROOT)/Lib\n'
            '        OTHER_LDFLAGS:\n'
            '          - "-ldhd_ios"\n'
            '          - "-framework CoreMedia"\n'
            '          - "-framework CoreVideo"\n'
            '          - "-framework CoreAudio"\n'
            '          - "-framework AudioToolbox"\n'
            '          - "-framework AVFoundation"\n'
            '          - "-framework CoreBluetooth"\n'
            '          - "-framework CoreGraphics"\n'
            '          - "-framework CoreMotion"\n'
            '          - "-framework GameController"\n'
            '          - "-framework Metal"\n'
            '          - "-framework QuartzCore"\n'
            '          - "-weak_framework CoreHaptics"\n'
            '          - "-liconv"\n'
            '        DEAD_CODE_STRIPPING: YES\n'
        )

    # 7. xcodegen project specification
    project_yml = PROJECT / 'project.yml'
    project_yml.write_text(
        'name: DownhillRevived\n'
        'options:\n'
        '  bundleIdPrefix: app.skuza\n'
        '  developmentLanguage: en\n'
        '  deploymentTarget:\n'
        '    iOS: "17.0"\n'
        'targets:\n'
        '  DownhillRevived:\n'
        '    type: application\n'
        '    platform: iOS\n'
        '    deploymentTarget: "17.0"\n'
        '    sources:\n'
        '      - path: Launcher\n'
        '      - path: Input\n'
        '      - path: Host\n'
        '      - path: Render\n'
        '      - path: Assets\n'
        '      - path: LaunchScreen.storyboard\n'
        '    settings:\n'
        '      base:\n'
        '        PRODUCT_BUNDLE_IDENTIFIER: app.skuza.DownhillRevived\n'
        '        PRODUCT_NAME: "Downhill Revived"\n'
        '        INFOPLIST_FILE: Info.plist\n'
        '        SWIFT_VERSION: "5.0"\n'
        '        SWIFT_OPTIMIZATION_LEVEL: "-O"\n'
        '        SWIFT_COMPILATION_MODE: "wholemodule"\n'
        '        GCC_OPTIMIZATION_LEVEL: "3"\n'
        '        MTL_ENABLE_DEBUG_INFO: "NO"\n'
        '        SWIFT_OBJC_BRIDGING_HEADER: DownhillRevived-Bridging-Header.h\n'
        '        TARGETED_DEVICE_FAMILY: "1,2"\n' +
        ('        DEVELOPMENT_TEAM: "%s"\n' % team) +
        '        CODE_SIGN_STYLE: Automatic\n'
        '        ENABLE_BITCODE: NO\n'
        '        ENABLE_PREVIEWS: NO\n'
        '        ENABLE_DEBUG_DYLIB: NO\n'
        '        ASSETCATALOG_COMPILER_APPICON_NAME: AppIcon\n'
        '        ASSETCATALOG_COMPILER_LAUNCHIMAGE_NAME: ""\n'
        '        HEADER_SEARCH_PATHS:\n'
        '          - $(SRCROOT)/Host\n'
        '          - $(SRCROOT)/Input\n'
        '          - $(SRCROOT)/Render\n'
        + lib_settings +
        '    dependencies: []\n'
    )

    print('ios: generating DownhillRevived.xcodeproj with xcodegen...')
    subprocess.run(['xcodegen', 'generate', '--spec', str(project_yml), '--project', str(PROJECT)], check=True)
    print('ios: project bundle prepared successfully.')


def resolve_team(team=None):
    value = team or os.environ.get('DHD_APPLE_TEAM', '')
    if value and not re.fullmatch(r'[A-Z0-9]{10}', value):
        raise ValueError('The Apple development team must contain 10 uppercase letters/digits.')
    return value


def preflight(bundle_only=False, core_only=False):
    names = ['xcrun'] if core_only else ['xcodegen'] if bundle_only else ['xcrun', 'xcodegen', 'xcodebuild', 'codesign']
    missing = [name for name in names if not shutil.which(name)]
    if missing:
        raise RuntimeError('Missing build tools: ' + ', '.join(missing))
    if not bundle_only:
        subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True, capture_output=True, text=True)


def find_connected_device():
    with tempfile.NamedTemporaryFile(suffix='.json', delete=False) as f:
        tmp = f.name
    try:
        subprocess.run(['xcrun', 'devicectl', 'list', 'devices', '-j', tmp], check=True, capture_output=True)
        data = json.loads(Path(tmp).read_text(encoding='utf-8'))
        devices = data.get('result', {}).get('devices', [])
        candidates = []
        for d in devices:
            hw = d.get('hardwareProperties', {})
            conn = d.get('connectionProperties', {})
            dev_props = d.get('deviceProperties', {})
            if hw.get('platform') == 'iOS' and conn.get('tunnelState') != 'unavailable':
                udid = hw.get('udid') or d.get('identifier')
                name = dev_props.get('name', 'iOS Device')
                candidates.append((udid, '%s (%s)' % (name, udid)))
        if len(candidates) > 1:
            raise RuntimeError('Several iOS devices are available. Select one with --device IDENTIFIER.')
        return candidates[0] if candidates else (None, None)
    finally:
        Path(tmp).unlink(missing_ok=True)


def package_ipa(app_path: Path) -> Path:
    """Package once, preserving executable modes and framework symlinks; publish atomically."""
    OUT.mkdir(parents=True, exist_ok=True)
    ipa_path = OUT / 'DownhillRevived.ipa'
    with tempfile.TemporaryDirectory(prefix='ipa-', dir=OUT) as temp:
        staged = Path(temp) / 'DownhillRevived.ipa'
        with zipfile.ZipFile(staged, 'w', zipfile.ZIP_DEFLATED, strict_timestamps=False) as archive:
            for item in sorted(app_path.rglob('*')):
                name = (Path('Payload') / app_path.name / item.relative_to(app_path)).as_posix()
                if item.is_symlink():
                    info = zipfile.ZipInfo(name)
                    info.create_system = 3
                    info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(info, os.readlink(item))
                elif item.is_file():
                    archive.write(item, name)
        os.replace(staged, ipa_path)
    print('ios: IPA generated at', ipa_path)
    return ipa_path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--wasm', type=Path, help='explicit prebuilt core; otherwise rebuild from current source')
    ap.add_argument('--release', action='store_true', default=True, help='build in Release configuration (default)')
    ap.add_argument('--debug', action='store_true', help='build in Debug configuration')
    ap.add_argument('--bundle-only', action='store_true', help='prepare project bundle without compiling')
    ap.add_argument('--core-only', action='store_true', help='rebuild the core and renderer without Xcode packaging')
    ap.add_argument('--jobs', type=int, default=4, help='parallel compiles (default 4)')
    ap.add_argument('--team', help='Apple development team (or DHD_APPLE_TEAM)')
    ap.add_argument('--unsigned', action='store_true', help='compile without signing; cannot install this output')
    ap.add_argument('--allow-provisioning-updates', action='store_true', help='let Xcode update signing assets in your Apple account')
    ap.add_argument('--device', help='explicit paired device identifier for --install')
    ap.add_argument('--install', action='store_true', help='install to connected iPhone via devicectl')
    args = ap.parse_args()
    if args.unsigned and args.install:
        ap.error('--unsigned cannot be combined with --install')
    if args.device and not args.install:
        ap.error('--device requires --install')
    if args.install and (args.bundle_only or args.core_only):
        ap.error('--install requires a complete app build')
    if args.jobs < 1:
        ap.error('--jobs must be at least 1')
    if args.bundle_only and args.core_only:
        ap.error('--bundle-only and --core-only cannot be combined')
    if sys.platform != 'darwin' and not args.bundle_only:
        ap.error('iOS compilation requires macOS and the iPhoneOS SDK; use --bundle-only to prepare sources')

    team = resolve_team(args.team)
    preflight(args.bundle_only, args.core_only)
    device_id, device_info = None, None
    if args.install:
        device_id, device_info = (args.device, args.device) if args.device else find_connected_device()
        if not device_id:
            ap.error('No paired iOS device found; connect it or pass --device IDENTIFIER.')

    ver_name, ver_code = version()
    print('ios: Downhill Revived v%s (code %d) iOS Port' % (ver_name, ver_code))

    if not args.bundle_only:
        prepare_native_core(args.wasm, args.jobs)
    if args.core_only:
        return 0
    prepare_project_bundle(team)

    if sys.platform != 'darwin' or args.bundle_only:
        print('\n[NOTE] Project bundle prepared at: build/ios/project/')
        return 0

    print('ios: building with xcodebuild...')
    config = 'Debug' if args.debug else 'Release'
    cmd = [
        'xcodebuild', '-project', str(PROJECT / 'DownhillRevived.xcodeproj'),
        '-scheme', 'DownhillRevived', '-configuration', config,
        '-derivedDataPath', str(OUT / 'DerivedData'),
        '-destination', ('id=' + device_id) if device_id else 'generic/platform=iOS',
        'DEVELOPMENT_TEAM=' + team,
        'CODE_SIGN_STYLE=Automatic',
        'CODE_SIGNING_ALLOWED=' + ('NO' if args.unsigned else 'YES'),
        *(['-allowProvisioningUpdates'] if args.allow_provisioning_updates else []),
        'build'
    ]
    res = subprocess.run(cmd)
    if res.returncode != 0:
        return res.returncode

    # Locate built app
    app_path = OUT / 'DerivedData' / 'Build' / 'Products' / ('%s-iphoneos' % config) / 'Downhill Revived.app'
    if not app_path.is_dir():
        print('ios: the current build did not produce Downhill Revived.app')
        return 1
    print('ios: built application at', app_path)

    # Xcode compiles assets and signs nested code in dependency order with the selected team.
    # Do not mutate the signed bundle or pick an unrelated certificate/profile afterwards.
    if not args.unsigned:
        if not (app_path / 'embedded.mobileprovision').is_file():
            raise RuntimeError('The signed app has no provisioning profile. Configure its Team in Xcode, '
                               'or pass --allow-provisioning-updates when you want Xcode to update it.')
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app_path)], check=True)
        package_ipa(app_path)
    else:
        import package_unsigned
        package_unsigned.package_unsigned_ipa(app_path)

    if args.install:
        print('ios: installing onto device:', device_info)
        cmd_install = ['xcrun', 'devicectl', 'device', 'install', 'app', '--device', device_id, str(app_path)]
        return subprocess.run(cmd_install).returncode

    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, 'stderr', None) or str(error)
        print('ios: build failed: %s' % detail, file=sys.stderr)
        sys.exit(1)
