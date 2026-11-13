/* The iOS app (findings 224, 227): it opens into the launcher's page (launcher.m, the shared mobile
   launcher of finding 225), which imports the disc image and changes the settings; PLAY puts the
   game in its place. With the setting "launcher" off and the disc in place the game starts at once
   (not after a run that closed unexpectedly: the launcher's note shows instead). The game ends the
   process itself (host_end, finding 210): EXIT GAME leaves the app, and the next start opens the
   launcher again; a core is never started twice in one process.
   The launcher turns with the device (portrait or landscape); the game is landscape: on an iPad
   that does not turn the window (iPadOS 26 may not honour the request), the game's view lays itself
   out for the window it gets (metal_bridge.m).
   Test switch: DHD_IOS_DIRECT=1 starts the game at once, as the setting "launcher" off does.
   GPL-3.0-or-later. */
#import <UIKit/UIKit.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ios_main.h"
#include "launcher_calls.h"
#include "metal_bridge.h"

static BOOL s_game;   /* the game has the window (its orientations: landscape) */

/* dhd.ini's frame rate and picture shape for the Metal bridge, before the game reads the rest. */
static void display_settings(void) {
    char value[16] = "";
    int fps = ios_setting_read("fps", value, sizeof value) ? atoi(value) : 60;
    ios_metal_set_target_fps(fps >= 120 ? 120 : 60);
    value[0] = 0;
    if (ios_setting_read("widescreen", value, sizeof value) && !strcmp(value, "0"))
        ios_metal_set_phone_aspect(4.0 / 3.0);
}

/* A plain line when the game can't start at all (its log says why). */
static UIViewController *message_view(NSString *text) {
    UIViewController *vc = [UIViewController new];
    vc.view.backgroundColor = UIColor.blackColor;
    UILabel *label = [UILabel new];
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    label.textColor = UIColor.whiteColor;
    label.text = text;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [vc.view addSubview:label];
    UILayoutGuide *safe = vc.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [label.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [label.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:40],
        [label.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-40],
    ]];
    return vc;
}

static UIWindow *app_window(void);

/* The game's orientations: landscape. The window is asked to turn (an iPhone always does; an
   iPad's resizable window may not, and the game then lays out for the window it has). */
static void want_landscape(UIWindow *window) {
    [window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
    UIWindowScene *scene = window.windowScene;
    if (!scene) return;
    UIWindowSceneGeometryPreferencesIOS *landscape =
        [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
    [scene requestGeometryUpdateWithPreferences:landscape errorHandler:^(NSError *error) {
        NSLog(@"Downhill Revived: the window stays as it is (%@); the game lays out for it", error.localizedDescription);
    }];
}

void app_play(int edit_touch_layout, int checked) {
    UIWindow *window = app_window();
    if (s_game || !window) return;
    s_game = YES;
    display_settings();
    window.rootViewController = ios_metal_create_view_controller();
    want_landscape(window);
    /* The game on its own thread (8 MiB of stack, as a desktop's main thread): dhd_main runs SDL's
       event loop there and the game on SDL's threads; it returns only when the game cannot start. */
    NSThread *thread = [[NSThread alloc] initWithBlock:^{
        int code = ios_start_game(edit_touch_layout, checked);
        dispatch_async(dispatch_get_main_queue(), ^{
            s_game = NO;
            if (code == 3) {   /* no disc image the game can play: the launcher asks for it */
                window.rootViewController = launcher_view_controller();
            } else {
                window.rootViewController = message_view([NSString stringWithFormat:
                    @"The game could not start (code %d). Its log is dhd.log in the app's folder "
                    "(Files > On My iPhone > Downhill Revived).", code]);
            }
            [window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
        });
    }];
    thread.name = @"dhd";
    thread.stackSize = 8 << 20;
    thread.qualityOfService = NSQualityOfServiceUserInteractive;
    [thread start];
}

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

static UIWindow *app_window(void) { return ((AppDelegate *)UIApplication.sharedApplication.delegate).window; }

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    const char *why = ios_run_record_start();   /* how the run before ended: dhd-exit.txt, the note */
    launcher_set_last_exit(why);
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.backgroundColor = UIColor.blackColor;
    const char *direct = getenv("DHD_IOS_DIRECT");
    if (!why && ((direct && atoi(direct)) || launcher_should_start_game())) {
        [self.window makeKeyAndVisible];
        app_play(0, 0);
    } else {
        self.window.rootViewController = launcher_view_controller();
        [self.window makeKeyAndVisible];
    }
    return YES;
}

- (UIInterfaceOrientationMask)application:(UIApplication *)application supportedInterfaceOrientationsForWindow:(UIWindow *)window {
    (void)application; (void)window;
    if (s_game) return UIInterfaceOrientationMaskLandscape;
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? UIInterfaceOrientationMaskAll
                                                                                : UIInterfaceOrientationMaskAllButUpsideDown;
}

/* The game waits while the app is not active (present.c's app_away_wait, finding 227): fingers
   let go, a race paused offline at the return, the sound paused, no game time lost or caught up.
   iOS may end a suspended app at any time, which loses nothing the game has written (dhd.ini at
   each change, the memory card at each save); the next start says how it ended (run_record.h). */
- (void)applicationWillResignActive:(UIApplication *)application { (void)application; ios_lifecycle_will_resign_active(); }
- (void)applicationDidEnterBackground:(UIApplication *)application { (void)application; ios_lifecycle_did_enter_background(); }
- (void)applicationWillEnterForeground:(UIApplication *)application { (void)application; ios_lifecycle_will_enter_foreground(); }
- (void)applicationDidBecomeActive:(UIApplication *)application { (void)application; ios_lifecycle_did_become_active(); }

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
