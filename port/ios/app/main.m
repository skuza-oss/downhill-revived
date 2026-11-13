/* The iOS app until the shared mobile launcher replaces this screen: the first start
   asks for the player's disc image (a document picker), copies it into the app with its progress,
   checks it (port/ios/host/ios_main.m) and starts the game, as the Android app does on its first
   start; later starts go straight to the game. The game's own menus, SETTINGS and touch controls do
   the rest (finding 224). The game ends the process itself when it ends (host_end, finding 210).
   GPL-3.0-or-later. */
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdlib.h>
#include <string.h>
#include "ios_main.h"
#include "metal_bridge.h"

@interface DiscViewController : UIViewController <UIDocumentPickerDelegate>
@property (nonatomic, strong) UILabel *message;
@property (nonatomic, strong) UIProgressView *progress;
@property (nonatomic, strong) UIButton *choose;
@end

static DiscViewController *s_disc;

/* dhd.ini's frame rate and picture shape for the Metal bridge, before the game reads the rest. */
static void display_settings(void) {
    char value[16] = "";
    int fps = ios_setting_read("fps", value, sizeof value) ? atoi(value) : 60;
    ios_metal_set_target_fps(fps >= 120 ? 120 : 60);
    value[0] = 0;
    if (ios_setting_read("widescreen", value, sizeof value) && !strcmp(value, "0"))
        ios_metal_set_phone_aspect(4.0 / 3.0);
}

/* The game on its own thread (8 MiB of stack, as a desktop's main thread): dhd_main runs SDL's
   event loop there and the game on SDL's threads; it returns only when the game cannot start. */
static void start_game(UIWindow *window) {
    display_settings();
    window.rootViewController = ios_metal_create_view_controller();
    NSThread *thread = [[NSThread alloc] initWithBlock:^{
        int code = ios_start_game(0, NULL);
        dispatch_async(dispatch_get_main_queue(), ^{
            s_disc = [DiscViewController new];
            window.rootViewController = s_disc;
            [s_disc loadViewIfNeeded];
            s_disc.message.text = [NSString stringWithFormat:@"The game could not start (code %d). Its log is "
                                   "dhd.log in the app's folder (Files > On My iPhone > Downhill Revived).", code];
        });
    }];
    thread.name = @"dhd";
    thread.stackSize = 8 << 20;
    thread.qualityOfService = NSQualityOfServiceUserInteractive;
    [thread start];
}

@implementation DiscViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.message = [UILabel new];
    self.message.numberOfLines = 0;
    self.message.textAlignment = NSTextAlignmentCenter;
    self.message.textColor = UIColor.whiteColor;
    self.message.text = @"Downhill Revived plays from your own disc image of Downhill Domination (USA, "
                        "SCUS-97177). Choose the .iso file: it is copied into the app once (about 2.4 GB).";
    self.progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.progress.hidden = YES;
    self.choose = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.choose setTitle:@"Choose the disc image" forState:UIControlStateNormal];
    self.choose.titleLabel.font = [UIFont boldSystemFontOfSize:20];
    [self.choose addTarget:self action:@selector(pick) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.message, self.progress, self.choose]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 24;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:40],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-40],
    ]];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (void)pick {
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}

static void import_progress(double fraction, uint64_t copied, uint64_t total) {
    (void)copied; (void)total;
    dispatch_async(dispatch_get_main_queue(), ^{ s_disc.progress.progress = (float)fraction; });
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *url = urls.firstObject;
    if (!url) return;
    BOOL scoped = [url startAccessingSecurityScopedResource];   /* a file of the app's own needs none */
    self.choose.enabled = NO;
    self.progress.hidden = NO;
    self.progress.progress = 0;
    self.message.text = @"Copying and checking the disc image...";
    NSString *path = url.path;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char error[512] = "";
        bool ok = ios_disc_import_from_url(path.fileSystemRepresentation, import_progress, error, sizeof error);
        NSString *problem = [NSString stringWithUTF8String:error] ?: @"";
        dispatch_async(dispatch_get_main_queue(), ^{
            if (scoped) [url stopAccessingSecurityScopedResource];
            self.choose.enabled = YES;
            self.progress.hidden = YES;
            if (ok) start_game(self.view.window);
            else self.message.text = [NSString stringWithFormat:@"%@\n\nChoose another file.", problem];
        });
    });
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    s_disc = [DiscViewController new];
    self.window.rootViewController = s_disc;
    [self.window makeKeyAndVisible];
    /* a disc already checked (dhd.ini's iso), or an image put in the app's folder through Files */
    if (ios_disc_is_ready()) start_game(self.window);
    return YES;
}

- (UIInterfaceOrientationMask)application:(UIApplication *)application supportedInterfaceOrientationsForWindow:(UIWindow *)window {
    (void)application; (void)window;
    return UIInterfaceOrientationMaskLandscape;
}

/* The game pauses while the app is not active (present.c's gs_pump_messages waits) and its
   surface is dropped; iOS may end a suspended app at any time, which loses nothing the game has
   written: dhd.ini is saved at each change and the memory card at each save (finding 224). */
- (void)applicationWillResignActive:(UIApplication *)application { (void)application; ios_lifecycle_will_resign_active(); }
- (void)applicationDidEnterBackground:(UIApplication *)application { (void)application; ios_lifecycle_did_enter_background(); }
- (void)applicationWillEnterForeground:(UIApplication *)application { (void)application; ios_lifecycle_will_enter_foreground(); }
- (void)applicationDidBecomeActive:(UIApplication *)application { (void)application; ios_lifecycle_did_become_active(); }
- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application { (void)application; ios_lifecycle_memory_warning(); }

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
