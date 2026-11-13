"""The iOS launcher and its platform rules (finding 227), without a Mac:
- the bridge's page side with the page's own bridge.js (test_bridge_ios.js, when node is there);
- SETTINGS on iOS shows exactly Android's rows: both hosts take phone.c's one list;
- the launcher replaced lane 224's first-start disc screen (one code path);
- the launcher turns with the device, the game asks for landscape, an iPad declares every orientation.
The C parts (launcher_calls.c, run_record.c, disc_copy.c) run in port/tools/test_runtime.py."""
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
IOS = ROOT / 'port' / 'ios'


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')


class LauncherIOS(unittest.TestCase):
    @unittest.skipUnless(shutil.which('node'), 'requires node')
    def test_bridge_page_side(self):
        out = subprocess.run(['node', str(IOS / 'tests' / 'test_bridge_ios.js')], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)

    def test_settings_rows_are_androids(self):
        rows = re.search(r'PHONE_SETTINGS_ROWS\[\] =\s*((?:"[^"]*"\s*)+);', read('port/host/phone.c'))
        listed = ''.join(re.findall(r'"([^"]*)"', rows.group(1))).split(',')
        for desktop in ('DHD_WINDOW', 'DHD_WINDOW_WIDTH', 'DHD_WINDOW_HEIGHT', 'DHD_KEYBOARD', 'DHD_DISCORD', 'DHD_NET_SERVER'):
            self.assertNotIn(desktop, listed)
        self.assertIn('DHD_LAUNCHER_AT_START', listed)
        self.assertIn('phone_settings_rows();', read('port/host/android_main.c'))
        self.assertIn('phone_settings_rows();', read('port/ios/host/ios_main.m'))
        # nobody else names a phone's rows: the one list
        for path in list((ROOT / 'port' / 'host').glob('*.c')) + list(IOS.rglob('*.m')):
            if path.name != 'phone.c':
                self.assertNotIn('DHD_PC_SETTINGS_ROWS', path.read_text(encoding='utf-8'), path)

    def test_first_start_disc_screen_is_gone(self):
        main = read('port/ios/app/main.m')
        self.assertNotIn('UIDocumentPicker', main)
        self.assertIn('launcher_view_controller()', main)
        header = read('port/ios/host/ios_main.h')
        for gone in ('ios_disc_import_from_url', 'ios_disc_validate', 'ios_disc_auto_detect'):
            self.assertNotIn(gone, header)
        # the disc's check is host.c's, as on Android
        self.assertIn('host_disc_check', read('port/ios/app/launcher.m'))
        self.assertIn('TARGET_OS_IPHONE', read('port/hle/host.c').split('const char *host_disc_check')[0][-400:])

    def test_orientations(self):
        plist = plistlib.loads((IOS / 'project' / 'Info.plist').read_bytes())
        self.assertIn('UIInterfaceOrientationPortrait', plist['UISupportedInterfaceOrientations'])
        self.assertEqual(len(plist['UISupportedInterfaceOrientations~ipad']), 4)
        main = read('port/ios/app/main.m')
        self.assertIn('if (s_game) return UIInterfaceOrientationMaskLandscape;', main)
        self.assertIn('requestGeometryUpdateWithPreferences', main)
        self.assertIn('turned a quarter in a portrait window', read('port/ios/render/metal_bridge.m'))

    def test_edge_back_never_takes_a_race_finger(self):
        # the review of lane 227: without the delegate UIKit never asked gestureRecognizerShouldBegin
        # and a recognized edge swipe cancelled the steering finger (finding 227)
        bridge = read('port/ios/render/metal_bridge.m')
        self.assertIn('@interface MetalGameView : UIView <UIGestureRecognizerDelegate>', bridge)
        self.assertIn('back.delegate = self;', bridge)
        self.assertIn('back.delaysTouchesBegan = NO;', bridge)
        self.assertIn('back.delaysTouchesEnded = NO;', bridge)
        should = bridge.split('- (BOOL)gestureRecognizerShouldBegin:')[1].split('}')[0]
        self.assertIn('ios_touch_back_allowed()', should)
        self.assertIn('if (ios_touch_back_allowed()) push(EV_BACK', read('port/host/touch_host.c'))

    def test_bridge_serves_only_its_own_files(self):
        launcher = read('port/ios/app/launcher.m')
        android = read('port/android/app/src/main/java/app/downhillrevived/LauncherActivity.java')
        csp = re.search(r'CSP = "([^;]+;[^"]*)"\s*\+\s*"([^"]*)"', android)
        self.assertIn(csp.group(1), launcher)
        self.assertIn(csp.group(2), launcher)
        self.assertIn('WKNavigationActionPolicyCancel', launcher)
        self.assertIn('[WKContentWorld worldWithName:', launcher)
        self.assertIn('statusCode:data ? 200 : 403', launcher)


if __name__ == '__main__':
    unittest.main()
