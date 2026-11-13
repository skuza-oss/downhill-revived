"""Build Downhill Revived for native iOS (iPhone 17 Pro Max / ARM64).

    python port/ios/build_ios.py [--wasm build/wasm/dhd_core.wasm | --wasm core_*.c] [--release] [--install]
    python port/ios/build_ios.py --simulator --wasm core_*.c    # the iOS Simulator (Apple silicon)

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
import plistlib
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

    bundled_simde = ROOT / 'port' / 'third_party' / 'simde-src'
    if (bundled_simde / 'simde' / 'x86' / 'sse2.h').is_file():
        os.environ.setdefault('SIMDE_DIR', str(bundled_simde))

    wasm2c = shutil.which('wasm2c')
    cache_wabt = devcaches / 'wabt/wabt-1.0.42'
    candidates = [cache_wabt]
    if wasm2c:
        candidates.append(Path(wasm2c).resolve().parents[1])
    for wabt_dir in candidates:
        if (wabt_dir / 'share/wabt/wasm2c').is_dir() and (wabt_dir / 'bin/wasm2c').is_file():
            os.environ.setdefault('WABT_DIR', str(wabt_dir))
            break

    if sys.platform == 'darwin':
        os.environ.setdefault('DHD_X86_CLANG', '/usr/bin/clang')


def version():
    text = (ROOT / 'port' / 'include' / 'version.h').read_text()
    name = re.search(r'#define DHD_VERSION "([0-9.]+)"', text).group(1)
    major, minor, patch = (int(x) for x in name.split('.'))
    return name, major * 10000 + minor * 100 + patch


def core_target(simulator=False):
    """build_host64.py's target and its static library (finding 224: the Simulator has its own)."""
    key = 'ios-sim-arm64' if simulator else 'ios-arm64'
    return key, ROOT / 'build' / ('host64-ios-sim' if simulator else 'host64-ios') / 'libdhd_ios.a'


def prepare_native_core(wasm=None, jobs=4, simulator=False):
    """Rebuild the module and host before copying the archive into the Xcode project. wasm: a .wasm,
    or wasm2c's parts (core.h, core-impl.h and core_*.c: give the core_*.c), as the macOS release
    takes them from the Windows build."""
    setup_env()
    key, core_lib = core_target(simulator)
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
        modules = [default_wasm]
    else:
        modules = [Path(w).resolve() for w in (wasm if isinstance(wasm, (list, tuple)) else [wasm])]
        for module in modules:
            if not module.is_file():
                raise FileNotFoundError('The explicitly supplied core does not exist: %s' % module)
        print('ios: using the explicitly supplied core; source-clock changes must already be in it', flush=True)
    split = ['--num-outputs', '8'] if len(modules) == 1 and modules[0].suffix == '.wasm' else []
    subprocess.run([sys.executable, str(ROOT / 'port/tools/build_host64.py'), *map(str, modules),
                    '--target', key, '--jobs', str(jobs), *split], cwd=ROOT, check=True)
    if not core_lib.is_file():
        raise RuntimeError('The native build did not produce %s' % core_lib)


def prepare_project_bundle(team=None, simulator=False):
    team = resolve_team(team)
    print('ios: preparing project bundle at', PROJECT)
    PROJECT.mkdir(parents=True, exist_ok=True)

    # 1. The app (until the shared mobile launcher), the host and the Metal bridge
    for name, src in (('App', HERE / 'app'), ('Host', HERE / 'host'), ('Render', HERE / 'render')):
        dst = PROJECT / name
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(src, dst)
    for stale in ('Launcher', 'Input', 'Assets', 'DownhillRevived-Bridging-Header.h'):   # the fork's layout
        path = PROJECT / stale
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()

    # 2. The app icon
    appicon_dst = PROJECT / 'AppIcon.xcassets'
    if appicon_dst.exists():
        shutil.rmtree(appicon_dst)
    shutil.copytree(HERE / 'project' / 'AppIcon.xcassets', appicon_dst)

    # 3. Info.plist & LaunchScreen.storyboard
    name, code = version()
    plist = plistlib.loads((HERE / 'project' / 'Info.plist').read_bytes())
    plist['CFBundleShortVersionString'] = name
    plist['CFBundleVersion'] = str(code * 100 + 1)
    (PROJECT / 'Info.plist').write_bytes(plistlib.dumps(plist))
    shutil.copyfile(HERE / 'project' / 'LaunchScreen.storyboard', PROJECT / 'LaunchScreen.storyboard')

    # 4. The game's static library (libdhd_ios.a)
    core_lib = core_target(simulator)[1]
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

    # 5. xcodegen project specification
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
        '      - path: App\n'
        '      - path: Host\n'
        '      - path: Render\n'
        '      - path: AppIcon.xcassets\n'
        '      - path: LaunchScreen.storyboard\n'
        '    settings:\n'
        '      base:\n'
        '        PRODUCT_BUNDLE_IDENTIFIER: app.skuza.DownhillRevived\n'
        '        PRODUCT_NAME: "Downhill Revived"\n'
        '        INFOPLIST_FILE: Info.plist\n'
        '        GCC_OPTIMIZATION_LEVEL: "3"\n'
        '        CLANG_ENABLE_OBJC_ARC: YES\n'
        '        MTL_ENABLE_DEBUG_INFO: "NO"\n'
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


def package_ipa(app_path: Path, output: Path | None = None) -> Path:
    """Package once, preserving executable modes and framework symlinks; publish atomically."""
    ipa_path = output if output is not None else OUT / 'DownhillRevived.ipa'
    ipa_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ipa-', dir=ipa_path.parent) as temp:
        staged = Path(temp) / ipa_path.name
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


def build_application(config, team, *, unsigned, derived_data, device_id=None, update_profiles=False,
                      simulator=False):
    print('ios: building with xcodebuild (%s)...' % ('simulator' if simulator else 'unsigned' if unsigned else 'signed'),
          flush=True)
    destination = ('id=' + device_id) if device_id else 'generic/platform=iOS Simulator' if simulator else 'generic/platform=iOS'
    cmd = [
        'xcodebuild', '-project', str(PROJECT / 'DownhillRevived.xcodeproj'),
        '-scheme', 'DownhillRevived', '-configuration', config,
        '-derivedDataPath', str(derived_data),
        '-destination', destination,
        *(['ARCHS=arm64'] if simulator else []),   # libdhd_ios.a is arm64 only (Apple silicon Macs)
        'DEVELOPMENT_TEAM=' + ('' if unsigned else team),
        'CODE_SIGN_STYLE=Automatic',
        'CODE_SIGNING_ALLOWED=' + ('NO' if unsigned else 'YES'),
        *(['-allowProvisioningUpdates'] if update_profiles else []),
        *(['-allowProvisioningDeviceRegistration'] if update_profiles and device_id else []),
        'build'
    ]
    subprocess.run(cmd, check=True)
    products = '%s-%s' % (config, 'iphonesimulator' if simulator else 'iphoneos')
    app_path = derived_data / 'Build' / 'Products' / products / 'Downhill Revived.app'
    if not app_path.is_dir():
        raise RuntimeError('The current build did not produce Downhill Revived.app')
    print('ios: built application at', app_path)
    if not unsigned:
        # Preserve the bundle as signed by Xcode, including nested code and the device profile.
        if not (app_path / 'embedded.mobileprovision').is_file():
            raise RuntimeError('The signed app has no provisioning profile. Configure its Team in Xcode, '
                               'or pass --allow-provisioning-updates to let Xcode update it.')
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app_path)], check=True)
    return app_path


def build_both(config, team, version_name, *, device_id=None, update_profiles=False):
    import package_unsigned

    OUT.mkdir(parents=True, exist_ok=True)
    public_name = f'DownhillRevived-{version_name}-GitHub-unsigned.ipa'
    private_name = f'DownhillRevived-{version_name}-Pessoal-signed.ipa'
    with tempfile.TemporaryDirectory(prefix='ipa-pair-', dir=OUT) as tmp:
        staging = Path(tmp)
        public_app = build_application(config, '', unsigned=True,
                                       derived_data=OUT / 'DerivedData-Unsigned')
        package_unsigned.package_unsigned_ipa(public_app, staging / public_name, update_canonical=False)
        private_app = build_application(config, team, unsigned=False, derived_data=OUT / 'DerivedData',
                                        device_id=device_id, update_profiles=update_profiles)
        package_ipa(private_app, staging / private_name)
        for name in (public_name, private_name):
            os.replace(staging / name, OUT / name)
            print('ios: IPA pronto em', OUT / name)
    return private_app


def main(argv=None, *, prompt_team=False):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--rebuild-core', action='store_true', help='force recompilation of libdhd_ios.a even if present')
    ap.add_argument('--wasm', type=Path, nargs='+', help='explicit prebuilt core: a .wasm, or the core_*.c of wasm2c '
                    '(with core.h and core-impl.h beside them); otherwise rebuild from current source')
    ap.add_argument('--release', action='store_true', default=True, help='build in Release configuration (default)')
    ap.add_argument('--debug', action='store_true', help='build in Debug configuration')
    ap.add_argument('--bundle-only', action='store_true', help='prepare project bundle without compiling')
    ap.add_argument('--core-only', action='store_true', help='rebuild the core and renderer without Xcode packaging')
    ap.add_argument('--jobs', type=int, default=4, help='parallel compiles (default 4)')
    ap.add_argument('--team', help='Apple development team (or DHD_APPLE_TEAM)')
    signing = ap.add_mutually_exclusive_group()
    signing.add_argument('--unsigned', action='store_true', help='compile without signing; cannot install this output')
    signing.add_argument('--both', action='store_true', help='generate separate unsigned and development-signed IPAs')
    signing.add_argument('--simulator', action='store_true', help='build the app for the iOS Simulator (Apple silicon), unsigned')
    ap.add_argument('--allow-provisioning-updates', action='store_true', help='let Xcode update signing assets in your Apple account')
    ap.add_argument('--device', help='paired device identifier for --install or signing with --both')
    ap.add_argument('--install', action='store_true', help='install to connected iPhone via devicectl')
    args = ap.parse_args(argv)
    if args.simulator and (args.install or args.device):
        ap.error('--simulator installs with xcrun simctl, not --install')
    if args.unsigned and args.install:
        ap.error('--unsigned cannot be combined with --install')
    if args.device and not (args.install or args.both):
        ap.error('--device requires --install or --both')
    if args.both and (args.bundle_only or args.core_only):
        ap.error('--both requires a complete app build')
    if args.install and (args.bundle_only or args.core_only):
        ap.error('--install requires a complete app build')
    if args.jobs < 1:
        ap.error('--jobs must be at least 1')
    if args.bundle_only and args.core_only:
        ap.error('--bundle-only and --core-only cannot be combined')
    if sys.platform != 'darwin' and not args.bundle_only:
        ap.error('iOS compilation requires macOS and the iPhoneOS SDK; use --bundle-only to prepare sources')

    preflight(args.bundle_only, args.core_only)
    team = resolve_team(args.team)
    if args.both and not team and prompt_team and sys.stdin.isatty():
        team = resolve_team(input('Team ID da sua conta Apple no Xcode (10 caracteres): ').strip())
    if args.both and not team:
        ap.error('informe o Team ID da Apple com --team ou DHD_APPLE_TEAM')

    device_id, device_info = args.device, args.device
    if args.install:
        if not device_id:
            device_id, device_info = find_connected_device()
        if not device_id:
            ap.error('No paired iOS device found; connect it or pass --device IDENTIFIER.')

    ver_name, ver_code = version()
    print('ios: Downhill Revived v%s (code %d) iOS Port' % (ver_name, ver_code))

    if not args.bundle_only:
        core_lib = core_target(args.simulator)[1]
        if args.rebuild_core or not core_lib.is_file():
            prepare_native_core(args.wasm, args.jobs, simulator=args.simulator)
        else:
            print('ios: using existing game core (libdhd_ios.a, %.1f MB)' % (core_lib.stat().st_size / (1024 * 1024)))
    if args.core_only:
        return 0
    prepare_project_bundle(team, simulator=args.simulator)

    if sys.platform != 'darwin' or args.bundle_only:
        print('\n[NOTE] Project bundle prepared at: build/ios/project/')
        return 0

    config = 'Debug' if args.debug else 'Release'
    if args.simulator:
        app_path = build_application(config, '', unsigned=True, derived_data=OUT / 'DerivedData-Simulator',
                                     simulator=True)
        print('ios: install it with: xcrun simctl install <device> "%s"' % app_path)
    elif args.both:
        app_path = build_both(config, team, ver_name, device_id=device_id,
                             update_profiles=args.allow_provisioning_updates)
    else:
        app_path = build_application(config, team, unsigned=args.unsigned, derived_data=OUT / 'DerivedData',
                                     device_id=device_id, update_profiles=args.allow_provisioning_updates)
        if args.unsigned:
            import package_unsigned
            package_unsigned.package_unsigned_ipa(app_path)
        else:
            package_ipa(app_path)

    if args.install:
        print('ios: installing onto device:', device_info)
        cmd_install = ['xcrun', 'devicectl', 'device', 'install', 'app', '--device', device_id, str(app_path)]
        return subprocess.run(cmd_install).returncode

    return 0


def cli(argv=None, *, prompt_team=False):
    try:
        return main(argv, prompt_team=prompt_team)
    except (RuntimeError, ValueError, OSError, EOFError, subprocess.CalledProcessError) as error:
        detail = getattr(error, 'stderr', None) or str(error)
        print('ios: build failed: %s' % detail, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(cli())
