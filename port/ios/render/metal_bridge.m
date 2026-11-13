/* metal_bridge.m - iOS Metal Surface & ProMotion Implementation
   The CAMetalLayer the renderer draws into (port/gs/present.c), the display's rate, and what the
   shared touch controls need from UIKit (touches, the screen in mm, haptics, the keyboard).
   GPL-3.0-or-later. */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>
#include <stdatomic.h>
#include <math.h>
#include <os/lock.h>
#include "metal_bridge.h"
#include "display_math.h"

@class MetalGameView;

/* port/host/touch_host.c (finding 224): UIKit's touches as SDL's finger events */
extern void ios_touch_finger(int kind, long long id, float x, float y);
extern void ios_touch_cancel(void);
/* port/hle/text_input.c: one character for the focused field (any thread) */
extern void text_input_character(unsigned character);
enum { EV_DOWN, EV_MOVE, EV_UP, EV_CANCEL };   /* touch.h's TOUCH_EV_* */

static int s_target_fps = 60;
static int s_output_limit = 0;   /* the screen's own pixels; the game's scale sets the 3D's */
static bool s_game_active = false;
static _Atomic(double) s_display_hz = 60;
static void update_display_preferences(void);
static id<MTLDevice> s_device = nil;
static _Atomic(uintptr_t) s_active_layer = 0;
static _Atomic(uint64_t) s_drawable_size = 0; /* width and height form one snapshot */
static _Atomic(int) s_max_display_fps = 60;
static _Atomic(int) s_resized = 0;
static _Atomic(int) s_layer_changed = 0;
static MetalGameView *s_shared_metal_view = nil;
static _Atomic(int) s_effective_fps = 60;
static _Atomic(double) s_phone_aspect = 0;
static __weak UIViewController *s_phone_host;
static void attach_to_host(UIViewController *host);

/* The view in mm for touch_host.c, written on the main thread at each layout. */
static os_unfair_lock s_screen_lock = OS_UNFAIR_LOCK_INIT;
static float s_screen_mm[2], s_strips_mm[4], s_cut_mm[4];
static _Atomic(int) s_turn = 0;   /* the interface's UIInterfaceOrientation (screen_measure), for the tilt's axes */

void ios_metal_set_phone_aspect(double aspect) { atomic_store(&s_phone_aspect, aspect); }
double ios_metal_game_aspect(void) {
    double phone = atomic_load(&s_phone_aspect);
    if (phone != 0) return phone;
    uint64_t size = atomic_load(&s_drawable_size);
    unsigned w = (unsigned)(size >> 32), h = (unsigned)size;
    return w && h ? (double)w / h : 16.0 / 9;
}

int ios_metal_effective_fps(void) { return atomic_load(&s_effective_fps); }

static CGSize limited_size(CGSize size) {
    if (!isfinite(size.width) || !isfinite(size.height) || size.width <= 0 || size.height <= 0)
        return CGSizeZero;
    double w, h;
    ios_display_limit(size.width, size.height, s_output_limit, false, &w, &h);
    return CGSizeMake(w, h);
}

void ios_metal_configure_layer(void *layer_ptr) {
    if (!layer_ptr) return;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)layer_ptr;
    if (!s_device) {
        s_device = MTLCreateSystemDefaultDevice();
    }
    layer.device = s_device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.presentsWithTransaction = NO;
    layer.opaque = YES;
    layer.maximumDrawableCount = 3;

    /* a first size until the view is laid out */
    CGSize output = limited_size(CGSizeMake(1280, 720));
    int w = (int)output.width, h = (int)output.height;
    layer.drawableSize = output;
    atomic_store(&s_drawable_size, ((uint64_t)(unsigned)w << 32) | (unsigned)h);
    uintptr_t old_layer = atomic_exchange(&s_active_layer, (uintptr_t)layer_ptr);
    if (old_layer != 0 && old_layer != (uintptr_t)layer_ptr) {
        atomic_store(&s_layer_changed, 1);
    }
    atomic_store(&s_resized, 1);
}

void *ios_metal_get_layer(void) {
    return (void *)atomic_load(&s_active_layer);
}

void ios_metal_get_drawable_size(int *out_w, int *out_h) {
    uint64_t size = atomic_load(&s_drawable_size);
    if (out_w) *out_w = (int)(size >> 32);
    if (out_h) *out_h = (int)(size & UINT32_MAX);
}

int ios_metal_consume_resized(void) {
    return atomic_exchange(&s_resized, 0);
}

int ios_metal_consume_layer_changed(void) {
    return atomic_exchange(&s_layer_changed, 0);
}

void ios_metal_set_target_fps(int fps) {
    NSCAssert([NSThread isMainThread], @"Display settings belong to the main thread");
    s_target_fps = fps;
    update_display_preferences();
}

void ios_metal_set_game_active(bool active) {
    dispatch_async(dispatch_get_main_queue(), ^{
        s_game_active = active;
        if (!active) ios_touch_cancel();   /* every finger is gone (touch_host.c) */
        update_display_preferences();
    });
}

void ios_metal_set_output_limit(int pixels) {
    NSCAssert([NSThread isMainThread], @"Display settings belong to the main thread");
    s_output_limit = pixels > 0 ? MAX(360, MIN(pixels, 2160)) : 0;
    [(UIView *)s_shared_metal_view setNeedsLayout];
}

double ios_metal_display_hz(void) { return atomic_load(&s_display_hz); }

bool ios_metal_has_promotion_support(void) {
    /* Native SDL workers call this too; only UIKit's main thread queries UIScreen. */
    return atomic_load(&s_max_display_fps) >= 120;
}

/* Only requests the screen cadence. The existing renderer remains the sole draw/present loop. */
@interface MetalRefreshDriver : NSObject
@property (nonatomic, strong) CADisplayLink *link;
@property (nonatomic, weak) UIScreen *screen;
@end
@implementation MetalRefreshDriver
- (void)tick:(CADisplayLink *)link {
    double period = link.targetTimestamp - link.timestamp;
    if (period > 0) atomic_store(&s_display_hz, 1.0 / period);
}
@end
static MetalRefreshDriver *s_refresh_driver;

static void update_display_preferences(void) {
    if (!s_refresh_driver) {
        s_refresh_driver = [MetalRefreshDriver new];
    }
    UIScreen *screen = UIScreen.mainScreen;
    if (s_refresh_driver.screen != screen || !s_refresh_driver.link) {
        [s_refresh_driver.link invalidate];
        s_refresh_driver.screen = screen;
        s_refresh_driver.link = [screen displayLinkWithTarget:s_refresh_driver selector:@selector(tick:)];
        [s_refresh_driver.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        atomic_store(&s_display_hz, MIN(s_target_fps, screen.maximumFramesPerSecond));
    }
    NSProcessInfo *process = [NSProcessInfo processInfo];
    bool hot = process.thermalState >= NSProcessInfoThermalStateSerious;
    int maximum = (int)screen.maximumFramesPerSecond;
    atomic_store(&s_max_display_fps, maximum);
    int fps = ios_display_target_fps(s_target_fps, maximum, false, process.lowPowerModeEnabled || hot);
    atomic_store(&s_effective_fps, fps);
    int cadence = MIN(fps, MAX(1, maximum));
    s_refresh_driver.link.preferredFrameRateRange = CAFrameRateRangeMake(MIN(30, cadence), cadence, cadence);
    s_refresh_driver.link.paused = !s_game_active;
}

/* ---- the shared touch controls' side (finding 224) ---- */

/* UIKit gives no physical size: points per inch by device class (iPhones 153-163, iPads 132-163;
   the overlay sizes its buttons in mm, so a few per cent off is a few per cent larger or smaller). */
static float points_per_mm(void) {
    float per_inch = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? 132.0f : 158.0f;
    return per_inch / 25.4f;
}

int ios_touch_screen(float *width_mm, float *height_mm, float strips[4], float cut[4]) {
    os_unfair_lock_lock(&s_screen_lock);
    int known = s_screen_mm[0] > 0 && s_screen_mm[1] > 0;
    *width_mm = s_screen_mm[0];
    *height_mm = s_screen_mm[1];
    memcpy(strips, s_strips_mm, sizeof s_strips_mm);
    memcpy(cut, s_cut_mm, sizeof s_cut_mm);
    os_unfair_lock_unlock(&s_screen_lock);
    return known;
}

/* The view's size, the home indicator's strip, and the sensor housing (notch or Dynamic Island) as
   the band its safe-area inset covers on its side, as Android's cutout. */
static void screen_measure(UIView *view) {
    float mm = 1.0f / points_per_mm();
    CGSize size = view.bounds.size;
    UIEdgeInsets in = view.safeAreaInsets;
    float strips[4] = {0, 0, 0, (float)in.bottom * mm}, cut[4] = {0};
    UIInterfaceOrientation turn = view.window.windowScene.interfaceOrientation;
    BOOL turned = !CGAffineTransformIsIdentity(view.transform);   /* the game turned in a portrait window (below) */
    /* tilt's axes: the turned view stands as the interface would in landscape right (its top at the
       screen's right edge, so the player holds the home side to the right) */
    atomic_store(&s_turn, (int)(turned ? UIInterfaceOrientationLandscapeRight : turn));
    if (turned) {   /* the insets in the view's own axes */
        strips[0] = (float)in.left * mm, strips[1] = (float)in.top * mm;
        strips[2] = (float)in.right * mm, strips[3] = (float)in.bottom * mm;
    } else if (turn == UIInterfaceOrientationLandscapeRight && in.left > 0) {        /* the housing on the left */
        cut[2] = (float)in.left * mm;
        cut[3] = (float)size.height * mm;
    } else if (turn == UIInterfaceOrientationLandscapeLeft && in.right > 0) { /* on the right */
        cut[0] = (float)(size.width - in.right) * mm;
        cut[2] = (float)size.width * mm;
        cut[3] = (float)size.height * mm;
    } else if (in.top > 0) {                                                  /* portrait: on top */
        strips[1] = (float)in.top * mm;
    }
    os_unfair_lock_lock(&s_screen_lock);
    s_screen_mm[0] = (float)size.width * mm;
    s_screen_mm[1] = (float)size.height * mm;
    memcpy(s_strips_mm, strips, sizeof strips);
    memcpy(s_cut_mm, cut, sizeof cut);
    os_unfair_lock_unlock(&s_screen_lock);
}

/* touch.h's TOUCH_BUZZ_*: 1 a tick, 2 a double tick, 3 a strong one. */
void ios_touch_buzz(int kind) {
    dispatch_async(dispatch_get_main_queue(), ^{
        static UIImpactFeedbackGenerator *light, *heavy;
        if (!light) light = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        if (!heavy) heavy = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleHeavy];
        if (kind == 3) [heavy impactOccurred];
        else if (kind >= 1) {
            [light impactOccurred];
            if (kind == 2)
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                    [light impactOccurred];
                });
        }
    });
}

/* Tilt steering (finding 226): CMDeviceMotion's gravity (device axes: x right and y up in
   portrait, z out of the screen; pointing down) turned into the screen's axes as the interface is
   turned, then Android's roll (DhdActivity.touchTiltOn: atan2(-sx, sy) of the up vector in screen
   axes, x right, y up). Updates come on their own queue; the roll is kept as millidegrees. The
   screen is not locked to its side while tilt steers (Android locks it): iOS turns the interface
   only well past a steering tilt. */
static CMMotionManager *s_motion;
static _Atomic(int) s_tilt_milli = 0, s_tilt_known = 0;

int ios_touch_tilt_on(int on) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s_motion = [[CMMotionManager alloc] init]; });
    int sensor = s_motion.deviceMotionAvailable || s_motion.accelerometerAvailable;
    if (!on) atomic_store(&s_tilt_known, 0);
    /* started and stopped on the main thread, never waited for (the game thread asks) */
    dispatch_async(dispatch_get_main_queue(), ^{
        if (on && sensor && !s_motion.deviceMotionActive && !s_motion.accelerometerActive) {
            static NSOperationQueue *queue;
            if (!queue) { queue = [[NSOperationQueue alloc] init]; queue.maxConcurrentOperationCount = 1; }
            void (^take)(double, double) = ^(double gx, double gy) {
                double ux = -gx, uy = -gy, sx, sy;   /* up, device axes */
                int turn = atomic_load(&s_turn);
                if (turn == UIInterfaceOrientationLandscapeRight) { sx = -uy; sy = ux; }        /* Android's ROTATION_90 */
                else if (turn == UIInterfaceOrientationLandscapeLeft) { sx = uy; sy = -ux; }    /* ROTATION_270 */
                else if (turn == UIInterfaceOrientationPortraitUpsideDown) { sx = -ux; sy = -uy; }
                else { sx = ux; sy = uy; }
                atomic_store(&s_tilt_milli, (int)lround(atan2(-sx, sy) * 180.0 / M_PI * 1000.0));
                atomic_store(&s_tilt_known, 1);
            };
            if (s_motion.deviceMotionAvailable) {
                s_motion.deviceMotionUpdateInterval = 1.0 / 60.0;
                [s_motion startDeviceMotionUpdatesToQueue:queue withHandler:^(CMDeviceMotion *m, NSError *e) {
                    if (m) take(m.gravity.x, m.gravity.y);
                }];
            } else {   /* no gyroscope: the accelerometer (gravity plus the hands' motion), as Android's fallback */
                s_motion.accelerometerUpdateInterval = 1.0 / 60.0;
                [s_motion startAccelerometerUpdatesToQueue:queue withHandler:^(CMAccelerometerData *a, NSError *e) {
                    if (a) take(a.acceleration.x, a.acceleration.y);
                }];
            }
        } else if (!on) {
            if (s_motion.deviceMotionActive) [s_motion stopDeviceMotionUpdates];
            if (s_motion.accelerometerActive) [s_motion stopAccelerometerUpdates];
            atomic_store(&s_tilt_known, 0);
        }
    });
    return sensor;
}

int ios_touch_tilt(float *degrees) {
    if (!atomic_load(&s_tilt_known)) return 0;
    *degrees = atomic_load(&s_tilt_milli) / 1000.0f;
    return 1;
}

/* The system keyboard while a game field takes typing (present.c): its characters to the field. */
@interface KeyboardView : UIView <UIKeyInput>
@property (nonatomic) UITextAutocorrectionType autocorrectionType;
@property (nonatomic) UITextAutocapitalizationType autocapitalizationType;
@property (nonatomic) UIKeyboardType keyboardType;
@property (nonatomic) UIReturnKeyType returnKeyType;
@end
@implementation KeyboardView
- (BOOL)canBecomeFirstResponder { return YES; }
- (BOOL)hasText { return YES; }
- (void)insertText:(NSString *)text {
    [text enumerateSubstringsInRange:NSMakeRange(0, text.length) options:NSStringEnumerationByComposedCharacterSequences
                          usingBlock:^(NSString *piece, NSRange r, NSRange e, BOOL *stop) {
        (void)r; (void)e; (void)stop;
        UTF32Char c = 0;
        if ([piece getBytes:&c maxLength:sizeof c usedLength:NULL encoding:NSUTF32LittleEndianStringEncoding
                    options:0 range:NSMakeRange(0, piece.length) remainingRange:NULL])
            text_input_character(c == '\n' ? 13 : c);
    }];
}
- (void)deleteBackward { text_input_character(8); }
@end
static KeyboardView *s_keyboard;

void ios_text_input(int on) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!s_keyboard) {
            s_keyboard = [[KeyboardView alloc] initWithFrame:CGRectZero];
            s_keyboard.autocorrectionType = UITextAutocorrectionTypeNo;
            s_keyboard.autocapitalizationType = UITextAutocapitalizationTypeNone;
            s_keyboard.keyboardType = UIKeyboardTypeASCIICapable;
            s_keyboard.returnKeyType = UIReturnKeyDone;
        }
        if (on) {
            UIView *host = (UIView *)s_shared_metal_view;
            if (s_keyboard.superview != host) [host addSubview:s_keyboard];
            [s_keyboard becomeFirstResponder];
        } else {
            [s_keyboard resignFirstResponder];
        }
    });
}

/* Native Metal View - Owned by Rust GS Renderer (port/gs/present.c); its touches go to the shared
   touch controls (touch_host.c). */
@interface MetalGameView : UIView
@property (nonatomic, strong) CAMetalLayer *metalLayer;
@end

@implementation MetalGameView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.opaque = YES;
        self.userInteractionEnabled = YES;
        self.multipleTouchEnabled = YES;
        self.contentScaleFactor = [UIScreen mainScreen].nativeScale;
        self.metalLayer = (CAMetalLayer *)self.layer;
        ios_metal_configure_layer((__bridge void *)self.metalLayer);
    }
    return self;
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self setNeedsLayout];
    update_display_preferences();
}

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    screen_measure(self);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    if (!self.window) return;
    screen_measure(self);
    UIScreen *screen = self.window.screen;
    CGFloat scale = screen.scale;
    self.contentScaleFactor = scale;
    atomic_store(&s_max_display_fps, (int)screen.maximumFramesPerSecond);
    CGSize output = limited_size(CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale));
    int w = (int)output.width;
    int h = (int)output.height;
    if (w > 0 && h > 0) {
        uint64_t next_size = ((uint64_t)(unsigned)w << 32) | (unsigned)h;
        uint64_t old_size = atomic_exchange(&s_drawable_size, next_size);
        uintptr_t new_ptr = (uintptr_t)(__bridge void *)self.metalLayer;
        uintptr_t old_ptr = atomic_exchange(&s_active_layer, new_ptr);
        if (old_ptr != 0 && old_ptr != new_ptr) {
            atomic_store(&s_layer_changed, 1);
        }
        if (next_size != old_size) {
            /* Rust owns drawableSize after surface creation; request its next reconfigure. */
            atomic_store(&s_resized, 1);
        }
    }
}

- (void)fingers:(NSSet<UITouch *> *)touches kind:(int)kind {
    CGSize size = self.bounds.size;
    if (size.width <= 0 || size.height <= 0) return;
    for (UITouch *touch in touches) {
        CGPoint at = [touch locationInView:self];
        ios_touch_finger(kind, (long long)(intptr_t)(__bridge void *)touch,
                         (float)(at.x / size.width), (float)(at.y / size.height));
    }
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self fingers:touches kind:EV_DOWN]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self fingers:touches kind:EV_MOVE]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self fingers:touches kind:EV_UP]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self fingers:touches kind:EV_CANCEL]; }

- (void)dealloc {
    uintptr_t self_ptr = (uintptr_t)(__bridge void *)self.metalLayer;
    atomic_compare_exchange_strong(&s_active_layer, &self_ptr, 0);
}

@end

/* Container VC that hosts the persistent singleton MetalGameView across orientation changes */
@interface MetalHostViewController : UIViewController
@end

static void attach_to_host(UIViewController *host) {
    if (!host) return;
    if (!s_shared_metal_view) {
        s_shared_metal_view = [[MetalGameView alloc] initWithFrame:host.view.bounds];
        s_shared_metal_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    }
    if (s_shared_metal_view.superview != host.view) {
        [s_shared_metal_view removeFromSuperview];
        s_shared_metal_view.frame = host.view.bounds;
        [host.view addSubview:s_shared_metal_view];
    }
    [(UIView *)s_shared_metal_view setNeedsLayout];
    [s_shared_metal_view layoutIfNeeded];
}

@implementation MetalHostViewController
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    attach_to_host(self);
}
/* The game is landscape (finding 227). A window that stays portrait while it fills the screen (an
   iPad the system keeps upright: iPadOS 26 may not turn a resizable app's window for the request,
   main.m) shows the game a quarter turn round at the screen's whole size, so the player turns the
   iPad and sees it upright, with its controls at the screen's edges; the view's size, safe area
   and touches are then the turned view's own (UIKit converts them). Any other window (a Stage
   Manager window, Split View) the game takes as it is: the picture as large as it fits, the
   touch controls in the window. */
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (s_shared_metal_view.superview != self.view) return;
    CGSize size = self.view.bounds.size;
    UIWindow *window = self.view.window;
    BOOL whole = window && CGSizeEqualToSize(window.bounds.size, window.screen.bounds.size);
    BOOL turned = whole && size.height > size.width;
    s_shared_metal_view.autoresizingMask = UIViewAutoresizingNone;
    s_shared_metal_view.transform = CGAffineTransformIdentity;
    if (turned) {
        s_shared_metal_view.bounds = CGRectMake(0, 0, size.height, size.width);
        s_shared_metal_view.center = CGPointMake(size.width / 2, size.height / 2);
        s_shared_metal_view.transform = CGAffineTransformMakeRotation(M_PI_2);
    } else {
        s_shared_metal_view.frame = self.view.bounds;
    }
    static int logged = -1;
    if (logged != turned) {
        logged = turned;
        NSLog(@"Downhill Revived: the game's view %.0f x %.0f points%s", turned ? size.height : size.width,
              turned ? size.width : size.height, turned ? ", turned a quarter in a portrait window" : "");
    }
    [(UIView *)s_shared_metal_view setNeedsLayout];
    [s_shared_metal_view layoutIfNeeded];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
/* the first swipe at an edge goes to the game; a second one opens the system's (Android's gesture insets) */
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

UIViewController *ios_metal_create_view_controller(void) {
    MetalHostViewController *vc = [MetalHostViewController new];
    vc.view.backgroundColor = UIColor.blackColor;
    vc.view.opaque = YES;
    s_phone_host = vc;
    attach_to_host(vc);
    return vc;
}
