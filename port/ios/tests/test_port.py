"""Portable checks: python3 -m unittest discover -s port/ios/tests -v.

The Linux native clock check supplements, rather than replaces, the upstream
wasm32 test_fps.py suite. UIKit/Metal and an IPA still require Xcode/device tests.
"""
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
import stat
from unittest.mock import patch, Mock
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('ios_build', ROOT / 'port/ios/build_ios.py')
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)
CC = shutil.which('cc')


class BuildTests(unittest.TestCase):
    def test_missing_tools_stop_before_any_build(self):
        with patch.object(build.shutil, 'which', return_value=None), patch.object(build.subprocess, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'Missing build tools'):
                build.preflight()
            run.assert_not_called()

    def test_team_is_validated(self):
        self.assertEqual(build.resolve_team('TESTTEAM01'), 'TESTTEAM01')
        for invalid in ('', 'TEAM', 'testteam01', 'TESTTEAM01\nOTHER=VALUE'):
            if invalid:
                with self.assertRaises(ValueError): build.resolve_team(invalid)

    def test_multiple_devices_require_explicit_selection(self):
        devices = {'result': {'devices': [
            {'hardwareProperties': {'platform': 'iOS', 'udid': 'A'}, 'connectionProperties': {}},
            {'hardwareProperties': {'platform': 'iOS', 'udid': 'B'}, 'connectionProperties': {}}
        ]}}
        def write_listing(command, **kwargs):
            import json
            Path(command[-1]).write_text(json.dumps(devices))
        with patch.object(build.subprocess, 'run', side_effect=write_listing):
            with self.assertRaisesRegex(RuntimeError, '--device'):
                build.find_connected_device()

    def test_archive_replaces_old_entries_and_preserves_links_and_modes(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            app = out / 'Game.app'
            app.mkdir()
            executable = app / 'Game'
            executable.write_bytes(b'binary')
            executable.chmod(0o755)
            (app / 'Current').symlink_to('Game')
            with zipfile.ZipFile(out / 'DownhillRevived.ipa', 'w') as z:
                z.writestr('stale', b'old')
            with patch.object(build, 'OUT', out): build.package_ipa(app)
            with zipfile.ZipFile(out / 'DownhillRevived.ipa') as z:
                self.assertNotIn('stale', z.namelist())
                self.assertEqual(z.read('Payload/Game.app/Game'), b'binary')
                self.assertEqual(z.getinfo('Payload/Game.app/Game').external_attr >> 16 & 0o777, 0o755)
                self.assertTrue(stat.S_ISLNK(z.getinfo('Payload/Game.app/Current').external_attr >> 16))
                self.assertEqual(z.read('Payload/Game.app/Current'), b'Game')

    def test_failed_archive_keeps_previous_ipa(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            app = out / 'Game.app'
            app.mkdir()
            (app / 'Game').write_bytes(b'new')
            ipa = out / 'DownhillRevived.ipa'
            ipa.write_bytes(b'previous complete archive')
            with patch.object(build, 'OUT', out), patch.object(zipfile.ZipFile, 'write', side_effect=OSError('disk full')):
                with self.assertRaises(OSError): build.package_ipa(app)
            self.assertEqual(ipa.read_bytes(), b'previous complete archive')

    def run_mock_app_build(self, args, *, profile=True, verification_fails=False):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            app = out / 'DerivedData/Build/Products/Release-iphoneos/Downhill Revived.app'
            app.mkdir(parents=True)
            if profile: (app / 'embedded.mobileprovision').write_bytes(b'test-only')
            calls = []
            unsigned_package = Mock()
            def run(cmd, **kwargs):
                calls.append(cmd)
                if verification_fails and cmd[0] == 'codesign':
                    raise subprocess.CalledProcessError(1, cmd)
                return subprocess.CompletedProcess(cmd, 0)
            with patch.object(build.sys, 'platform', 'darwin'), \
                 patch.object(build.sys, 'argv', ['build_ios.py', '--team', 'TESTTEAM01', *args]), \
                 patch.object(build, 'OUT', out), patch.object(build, 'preflight'), \
                 patch.object(build, 'prepare_native_core'), patch.object(build, 'prepare_project_bundle'), \
                 patch.object(build, 'package_ipa') as package, patch.object(build.subprocess, 'run', side_effect=run), \
                 patch.dict(sys.modules, {'package_unsigned': SimpleNamespace(package_unsigned_ipa=unsigned_package)}):
                try:
                    result = build.main()
                except (RuntimeError, subprocess.CalledProcessError) as error:
                    return error, calls, package.call_count + unsigned_package.call_count
                return result, calls, package.call_count + unsigned_package.call_count

    def test_xcode_signs_selected_team_before_packaging_and_installing(self):
        result, calls, packages = self.run_mock_app_build(['--install', '--device', 'TEST-DEVICE'])
        self.assertEqual(result, 0)
        self.assertIn('DEVELOPMENT_TEAM=TESTTEAM01', calls[0])
        self.assertIn('CODE_SIGNING_ALLOWED=YES', calls[0])
        self.assertIn('id=TEST-DEVICE', calls[0])
        self.assertEqual(calls[1][:3], ['codesign', '--verify', '--deep'])
        self.assertEqual(calls[2][:5], ['xcrun', 'devicectl', 'device', 'install', 'app'])
        self.assertEqual(packages, 1)

    def test_missing_profile_prevents_packaging_and_installation(self):
        error, calls, packages = self.run_mock_app_build(['--install', '--device', 'TEST-DEVICE'], profile=False)
        self.assertIsInstance(error, RuntimeError)
        self.assertEqual(len(calls), 1)
        self.assertEqual(packages, 0)

    def test_invalid_signature_prevents_packaging_and_installation(self):
        error, calls, packages = self.run_mock_app_build(['--install', '--device', 'TEST-DEVICE'], verification_fails=True)
        self.assertIsInstance(error, subprocess.CalledProcessError)
        self.assertEqual(len(calls), 2)
        self.assertEqual(packages, 0)

    def test_unsigned_output_is_explicit(self):
        result, calls, packages = self.run_mock_app_build(['--unsigned'], profile=False)
        self.assertEqual(result, 0)
        self.assertIn('CODE_SIGNING_ALLOWED=NO', calls[0])
        self.assertEqual(len(calls), 1)
        self.assertEqual(packages, 1)

    def test_both_builds_once_and_keeps_signed_and_unsigned_packages_separate(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            calls = []

            def run(cmd, **kwargs):
                calls.append(cmd)
                if cmd[0] == 'xcodebuild':
                    derived = Path(cmd[cmd.index('-derivedDataPath') + 1])
                    app = derived / 'Build/Products/Release-iphoneos/Downhill Revived.app'
                    app.mkdir(parents=True)
                    (app / 'Downhill Revived').write_bytes(b'test executable')
                    if 'CODE_SIGNING_ALLOWED=YES' in cmd:
                        (app / 'embedded.mobileprovision').write_bytes(b'test profile')
                        (app / '_CodeSignature').mkdir()
                        (app / '_CodeSignature/CodeResources').write_bytes(b'test signature')
                return subprocess.CompletedProcess(cmd, 0)

            def public_package(app, output, *, update_canonical):
                self.assertFalse(update_canonical)
                self.assertFalse((app / 'embedded.mobileprovision').exists())
                return build.package_ipa(app, output)

            pack = SimpleNamespace(package_unsigned_ipa=Mock(side_effect=public_package))
            args = ['--both', '--rebuild-core', '--team', 'TESTTEAM01', '--device', 'TEST-DEVICE',
                    '--allow-provisioning-updates']
            with patch.object(build.sys, 'platform', 'darwin'), patch.object(build, 'OUT', out), \
                 patch.object(build, 'preflight'), patch.object(build, 'prepare_native_core') as core, \
                 patch.object(build, 'prepare_project_bundle'), patch.object(build.subprocess, 'run', side_effect=run), \
                 patch.dict(sys.modules, {'package_unsigned': pack}):
                self.assertEqual(build.main(args), 0)
                core.assert_called_once_with(None, 4)

            self.assertEqual(len(calls), 3)
            self.assertIn('CODE_SIGNING_ALLOWED=NO', calls[0])
            self.assertIn('DEVELOPMENT_TEAM=', calls[0])
            self.assertNotIn('-allowProvisioningUpdates', calls[0])
            self.assertIn('CODE_SIGNING_ALLOWED=YES', calls[1])
            self.assertIn('DEVELOPMENT_TEAM=TESTTEAM01', calls[1])
            self.assertIn('id=TEST-DEVICE', calls[1])
            self.assertIn('-allowProvisioningDeviceRegistration', calls[1])
            self.assertEqual(calls[2][:4], ['codesign', '--verify', '--deep', '--strict'])
            version, _ = build.version()
            names = {p.name: p for p in out.glob('*.ipa')}
            self.assertEqual(set(names), {f'DownhillRevived-{version}-GitHub-unsigned.ipa',
                                          f'DownhillRevived-{version}-Pessoal-signed.ipa'})
            with zipfile.ZipFile(names[f'DownhillRevived-{version}-GitHub-unsigned.ipa']) as public:
                self.assertFalse(any('mobileprovision' in name or '_CodeSignature' in name
                                     for name in public.namelist()))
            with zipfile.ZipFile(names[f'DownhillRevived-{version}-Pessoal-signed.ipa']) as personal:
                self.assertEqual(personal.read('Payload/Downhill Revived.app/embedded.mobileprovision'),
                                 b'test profile')
                self.assertIn('Payload/Downhill Revived.app/_CodeSignature/CodeResources', personal.namelist())

    def test_public_packager_accepts_already_unsigned_app_without_canonical_copy(self):
        spec = importlib.util.spec_from_file_location('ios_public_package', ROOT / 'port/ios/package_unsigned.py')
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            app = root / 'Downhill Revived.app'
            app.mkdir()
            (app / 'Downhill Revived').write_bytes(b'test executable')
            (app / 'embedded.mobileprovision').write_bytes(b'test profile')
            (app / '_CodeSignature').mkdir()
            (app / '_CodeSignature/CodeResources').write_bytes(b'test signature')
            output = root / 'GitHub-unsigned.ipa'

            def run(cmd, **kwargs):
                if cmd[:2] == ['codesign', '-d']:
                    return subprocess.CompletedProcess(cmd, 1, '', 'code object is not signed at all')
                self.assertEqual(cmd[:2], ['strip', '-x'])
                return subprocess.CompletedProcess(cmd, 0)

            with patch.object(package, 'OUT', root), patch.object(package.subprocess, 'run', side_effect=run):
                self.assertEqual(package.package_unsigned_ipa(app, output, update_canonical=False), output)
            with zipfile.ZipFile(output) as ipa:
                self.assertEqual(ipa.namelist(), ['Payload/Downhill Revived.app/Downhill Revived'])
            self.assertFalse((root / 'DownhillRevived.ipa').exists())
            self.assertTrue((app / 'embedded.mobileprovision').is_file())

    def test_failed_signing_keeps_previous_ipa_pair(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            version, _ = build.version()
            previous = {}
            for suffix in ('GitHub-unsigned', 'Pessoal-signed'):
                path = out / f'DownhillRevived-{version}-{suffix}.ipa'
                previous[path] = suffix.encode()
                path.write_bytes(previous[path])

            pack = SimpleNamespace(package_unsigned_ipa=Mock(
                side_effect=lambda app, path, **kwargs: path.write_bytes(b'new unsigned')))
            with patch.object(build, 'OUT', out), \
                 patch.object(build, 'build_application', side_effect=[out / 'Test.app', RuntimeError('signing failed')]), \
                 patch.dict(sys.modules, {'package_unsigned': pack}):
                with self.assertRaisesRegex(RuntimeError, 'signing failed'):
                    build.build_both('Release', 'TESTTEAM01', version)
            self.assertEqual({p: p.read_bytes() for p in previous}, previous)
            self.assertEqual(set(out.iterdir()), set(previous))

    def test_unsigned_install_is_rejected(self):
        with patch.object(build.sys, 'argv', ['build_ios.py', '--unsigned', '--install']):
            with self.assertRaises(SystemExit) as raised: build.main()
            self.assertEqual(raised.exception.code, 2)

    def test_source_module_precedes_native_build(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / 'assets').mkdir()
            (root / 'assets/vutext.bin').write_bytes(b'test-only')
            lib = root / 'build/host64-ios/libdhd_ios.a'
            lib.parent.mkdir(parents=True)
            lib.write_bytes(b'test-only')
            with patch.object(build, 'ROOT', root), patch.object(build.subprocess, 'run') as run:
                build.prepare_native_core(jobs=2)
            cmds = [c.args[0] for c in run.call_args_list]
            self.assertEqual(len(cmds), 2)
            self.assertTrue(cmds[0][1].endswith('build_wasm.py'))
            self.assertTrue(cmds[1][1].endswith('build_host64.py'))
            self.assertIn('ios-arm64', cmds[1])
            self.assertEqual(cmds[1][-2:], ['--num-outputs', '8'])

    def test_failure_stops_before_packaging(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / 'assets').mkdir()
            (root / 'assets/vutext.bin').write_bytes(b'test-only')
            with patch.object(build, 'ROOT', root), patch.object(build.subprocess, 'run',
                    side_effect=subprocess.CalledProcessError(1, 'build_wasm')) as run:
                with self.assertRaises(subprocess.CalledProcessError):
                    build.prepare_native_core()
                self.assertEqual(run.call_count, 1)

    def test_explicit_module_is_used(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            wasm = root / 'custom.wasm'
            wasm.write_bytes(b'test-only')
            lib = root / 'build/host64-ios/libdhd_ios.a'
            lib.parent.mkdir(parents=True)
            lib.write_bytes(b'test-only')
            with patch.object(build, 'ROOT', root), patch.object(build.subprocess, 'run') as run:
                build.prepare_native_core(wasm)
                self.assertEqual(run.call_count, 1)
                self.assertIn(str(wasm), run.call_args.args[0])
                with self.assertRaises(FileNotFoundError):
                    build.prepare_native_core(root / 'missing.wasm')

    def test_project_contains_core_stats_and_release_settings(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            lib = root / 'build/host64-ios/libdhd_ios.a'
            lib.parent.mkdir(parents=True)
            lib.write_bytes(b'test-only')
            stick = root / 'port/hle/stick.h'
            stick.parent.mkdir(parents=True)
            stick.write_bytes((ROOT / 'port/hle/stick.h').read_bytes())
            for name in ('port/render/include/dhd_settings.h', 'port/hle/controls_table.h', 'port/include/version.h'):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((ROOT / name).read_bytes())
            project = root / 'project'
            with patch.object(build, 'ROOT', root), patch.object(build, 'PROJECT', project), \
                    patch.object(build.subprocess, 'run'):
                build.prepare_project_bundle(team='TESTTEAM01')
            text = (project / 'project.yml').read_text()
            self.assertIn('"-ldhd_ios"', text)
            version, code = build.version()
            plist = plistlib.loads((project / 'Info.plist').read_bytes())
            self.assertEqual(plist['CFBundleShortVersionString'], version)
            self.assertEqual(plist['CFBundleVersion'], str(code * 100 + 1))
            self.assertIn('GCC_OPTIMIZATION_LEVEL: "3"', text)
            self.assertIn('DEVELOPMENT_TEAM: "TESTTEAM01"', text)
            self.assertTrue((project / 'Render/performance_stats.c').is_file())
            self.assertEqual((project / 'Input/stick.h').read_bytes(), stick.read_bytes())
            self.assertEqual((project / 'Host/dhd_settings.h').read_bytes(), (ROOT / 'port/render/include/dhd_settings.h').read_bytes())
            self.assertEqual((project / 'Host/controls_table.h').read_bytes(), (ROOT / 'port/hle/controls_table.h').read_bytes())
            self.assertTrue((project / 'Launcher/ControlsCustomizationView.swift').is_file())
            self.assertEqual((project / 'Lib/libdhd_ios.a').read_bytes(), b'test-only')

    def test_launcher_localizations_are_complete_and_copied_into_project(self):
        languages = {'en', 'pt-BR', 'es', 'fr', 'de', 'it', 'ja', 'zh-Hans'}
        with tempfile.TemporaryDirectory() as td:
            project = Path(td) / 'project'
            stick = Path(td) / 'port/hle/stick.h'
            stick.parent.mkdir(parents=True)
            stick.write_bytes((ROOT / 'port/hle/stick.h').read_bytes())
            for name in ('port/render/include/dhd_settings.h', 'port/hle/controls_table.h', 'port/include/version.h'):
                destination = Path(td) / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((ROOT / name).read_bytes())
            with patch.object(build, 'ROOT', Path(td)), patch.object(build, 'PROJECT', project), \
                    patch.object(build.subprocess, 'run'):
                build.prepare_project_bundle(team='TESTTEAM01')

            launcher = project / 'Launcher'
            tables = {}
            for resource in launcher.glob('*.lproj/Localizable.strings'):
                table = {}
                for line in resource.read_text(encoding='utf-8').splitlines():
                    if not line or line.startswith('//'):
                        continue
                    key, value = line.removesuffix(';').split(' = ', 1)
                    key, value = json.loads(key), json.loads(value)
                    self.assertTrue(line.endswith(';'))
                    self.assertNotIn(key, table)
                    self.assertTrue(value)
                    table[key] = value
                tables[resource.parent.stem] = table

            self.assertEqual(set(tables), languages)
            info = plistlib.loads((project / 'Info.plist').read_bytes())
            self.assertEqual(info['CFBundleDevelopmentRegion'], 'en')
            spec = (project / 'project.yml').read_text()
            self.assertIn('developmentLanguage: en', spec)
            self.assertIn('- path: Launcher', spec)
            self.assertEqual(tables['pt-BR']['Graphics Settings'], 'Configurações gráficas')

            english = tables['en']
            for source in launcher.glob('*.swift'):
                for key in re.findall(r'L10n\.(?:text|format)\("([^"]+)"', source.read_text(encoding='utf-8')):
                    self.assertIn(key, english, source.name)
            placeholders = re.compile(r'%(?:%|@|lld|llu|\.\d+f)')
            for language, table in tables.items():
                self.assertEqual(table.keys(), english.keys(), language)
                for key, value in table.items():
                    self.assertEqual(placeholders.findall(key), placeholders.findall(value), (language, key))

    def test_non_mac_compile_fails_clearly(self):
        with patch.object(build.sys, 'platform', 'linux'), \
                patch.object(build.sys, 'argv', ['build_ios.py', '--release']), \
                patch.object(build, 'prepare_native_core') as native:
            with self.assertRaises(SystemExit) as raised:
                build.main()
            self.assertEqual(raised.exception.code, 2)
            native.assert_not_called()


@unittest.skipUnless(CC, 'requires a C compiler')
class NativeChecks(unittest.TestCase):
    def test_stick_handles_neutral_invalid_and_extreme_inputs(self):
        code = r'''
#include "stick_math.h"
#include <assert.h>
#include <float.h>
int main(void){
    float x,y;
    touch_normalize_stick(0,0,0,1,&x,&y);assert(x==0 && y==0);
    touch_normalize_stick(0,0,1,1,&x,&y);assert(x==0 && y==0);
    touch_normalize_stick(NAN,1,0,1,&x,&y);assert(x==0 && y==0);
    touch_normalize_stick(1,INFINITY,0,1,&x,&y);assert(x==0 && y==0);
    touch_normalize_stick(1,0,0,1,&x,&y);assert(fabsf(x-1)<.00001f && y==0);
    touch_normalize_stick(1,1,.15f,1,&x,&y);assert(fabsf(hypotf(x,y)-1)<.00001f);
    touch_normalize_stick(FLT_MAX,FLT_MAX,1,NAN,&x,&y);assert(isfinite(x) && isfinite(y));
    for(int a=-100;a<=100;a++)for(int b=-100;b<=100;b++){
        touch_normalize_stick(a/30.f,b/30.f,(a%4)*.5f,(b%5)*1.f,&x,&y);
        assert(isfinite(x) && isfinite(y) && hypotf(x,y)<=1.00001f);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            src, exe = Path(td) / 'stick.c', Path(td) / 'stick'
            src.write_text(code)
            subprocess.run([CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT / 'port/ios/input'), '-I'+str(ROOT / 'port/hle'), str(src), '-lm', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)

    def test_frame_stats_measure_rate_percentile_and_reset(self):
        code = r'''
#include "performance_stats.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
static void *writer(void *ignored) {
    (void)ignored;
    for (int i=0;i<10000;i++) ios_perf_record_present(1000+i/60.0);
    return 0;
}
int main(void) {
    double fps=99,p99=99;
    ios_perf_reset();ios_perf_snapshot(&fps,&p99);assert(fps==0 && p99==0);
    int rates[]={30,60,120};
    for (int r=0;r<3;r++) {
        ios_perf_reset();
        for(int i=0;i<500;i++) ios_perf_record_present(1+(double)i/rates[r]);
        ios_perf_snapshot(&fps,&p99);
        assert(fabs(fps-rates[r])<.001);
        assert(fabs(p99-1000.0/rates[r])<.001);
    }
    ios_perf_reset();double now=1;ios_perf_record_present(now);
    for(int i=0;i<100;i++){now+=i<2 ? .1 : 1.0/60;ios_perf_record_present(now);}
    ios_perf_snapshot(&fps,&p99);assert(fps<60 && fabs(p99-100)<.001);
    ios_perf_record_present(NAN);ios_perf_record_present(INFINITY);
    ios_perf_record_present(now+2);ios_perf_snapshot(&fps,&p99);assert(fps<30); /* keep real stalls */
    ios_perf_reset();ios_perf_snapshot(&fps,&p99);assert(fps==0 && p99==0);
    pthread_t thread;assert(!pthread_create(&thread,0,writer,0));
    for(int i=0;i<10000;i++){ios_perf_snapshot(&fps,&p99);assert(isfinite(fps) && isfinite(p99));}
    pthread_join(thread,0);return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            src, exe = Path(td) / 'stats.c', Path(td) / 'stats'
            src.write_text(code)
            render = ROOT / 'port/ios/render'
            subprocess.run([CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-pthread',
                            '-I', str(render), str(render / 'performance_stats.c'), str(src), '-lm', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'supplemental Linux native clock fixture')
    def test_upstream_clock_suite_at_eleven_rates(self):
        stub = r'''
#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
uintptr_t guest_base=0;
const char *host_getenv(const char *n){return getenv(n);}
void host_putenv_s(const char *n,const char *v){setenv(n,v,1);}
void host_exit(int code){exit(code);}
__attribute__((constructor)) static void map_guest(void){
    void *p=mmap((void*)0x08000000,0x02000000,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    if(p==MAP_FAILED){perror("test RAM");exit(2);}
    p=mmap((void*)0x10000000,0x10000,PROT_READ|PROT_WRITE,
           MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    if(p==MAP_FAILED){perror("test registers");exit(2);}
}
'''
        with tempfile.TemporaryDirectory() as td:
            src, exe = Path(td) / 'host.c', Path(td) / 'fps'
            src.write_text(stub)
            subprocess.run([CC, '-O2', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off',
                '-I'+str(ROOT / 'port/runtime'), '-I'+str(ROOT / 'port/include'),
                str(ROOT / 'port/hle/fps.c'), str(ROOT / 'port/hle/test_fps.c'), str(src), '-lm', '-o', str(exe)], check=True)
            subprocess.run([str(exe),'60','display'], check=True, capture_output=True, timeout=20)
            for rate in (30,45,60,61,75,90,100,120,144,165,240):
                for mode in ([], ['wall']):
                    with self.subTest(rate=rate, mode=mode):
                        subprocess.run([str(exe),str(rate),*mode], check=True, capture_output=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
