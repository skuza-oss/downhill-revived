/* metal_bridge.m - iOS Metal Surface & ProMotion Implementation
   GPL-3.0-or-later. */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <stdatomic.h>
#include <math.h>
#include "metal_bridge.h"
#include "touch_input.h"
#include "performance_stats.h"
#include "display_math.h"

@class MetalGameView;

static int s_target_fps = 60;
static int s_output_limit = 720;
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
static _Atomic(bool) s_external = false;
static _Atomic(int) s_effective_fps = 60;
static _Atomic(unsigned) s_render_preferences = 1;
static _Atomic(double) s_phone_aspect = 0;
static __weak UIViewController *s_phone_host;
static __weak UIViewController *s_external_host;
static void attach_to_host(UIViewController *host);

/* One layer moves between windows. Its lifetime and renderer surface stay intact. */
void ios_metal_set_phone_aspect(double aspect) { atomic_store(&s_phone_aspect, aspect); }
double ios_metal_game_aspect(void) {
    if (atomic_load(&s_external)) return 16.0 / 9.0;
    double phone = atomic_load(&s_phone_aspect);
    if (phone != 0) return phone;
    uint64_t size = atomic_load(&s_drawable_size);
    unsigned w = (unsigned)(size >> 32), h = (unsigned)size;
    return w && h ? (double)w / h : 16.0 / 9;
}

void ios_metal_set_render_preferences(int scale, bool point_filter) {
    atomic_store(&s_render_preferences, (unsigned)MAX(1, MIN(scale, 6)) | (point_filter ? 16u : 0u));
}
int ios_metal_render_scale(void) {
    unsigned prefs = atomic_load(&s_render_preferences);
    return (int)(prefs & 15u);
}
bool ios_metal_point_filter(void) { return (atomic_load(&s_render_preferences) & 16u) != 0; }
int ios_metal_effective_fps(void) { return atomic_load(&s_effective_fps); }

static CGSize limited_size(CGSize size) {
    if (!isfinite(size.width) || !isfinite(size.height) || size.width <= 0 || size.height <= 0)
        return CGSizeZero;
    double w, h;
    ios_display_limit(size.width, size.height, s_output_limit,
                      s_external_host != nil, &w, &h);
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

    CGSize screen_size = CGSizeMake(1280, 720);
    int w = (int)(screen_size.width > screen_size.height ? screen_size.width : screen_size.height);
    int h = (int)(screen_size.width > screen_size.height ? screen_size.height : screen_size.width);
    if (w <= 0) w = 1920;
    if (h <= 0) h = 1080;
    CGSize output = limited_size(CGSizeMake(w, h));
    w = (int)output.width; h = (int)output.height;
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
        if (!active) ios_perf_reset();
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

void ios_metal_get_timing(double *out_avg_fps, double *out_p99_ms) {
    ios_perf_snapshot(out_avg_fps, out_p99_ms);
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
    UIScreen *screen = s_external_host.view.window.screen ?: UIScreen.mainScreen;
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
    int fps = ios_display_target_fps(s_target_fps, maximum, s_external_host != nil,
                                     process.lowPowerModeEnabled || hot);
    atomic_store(&s_effective_fps, fps);
    int cadence = MIN(fps, MAX(1, maximum));
    s_refresh_driver.link.preferredFrameRateRange = CAFrameRateRangeMake(MIN(30, cadence), cadence, cadence);
    s_refresh_driver.link.paused = !s_game_active;
}

/* Native Metal View for SwiftUI Hosting - Owned by Rust GS Renderer (port/gs/present.c) */
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
        self.userInteractionEnabled = NO;
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

- (void)layoutSubviews {
    [super layoutSubviews];
    if (!self.window) return;
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

- (void)dealloc {
    uintptr_t self_ptr = (uintptr_t)(__bridge void *)self.metalLayer;
    atomic_compare_exchange_strong(&s_active_layer, &self_ptr, 0);
}

@end

/* Container VC that hosts the persistent singleton MetalGameView across Portrait/Landscape switches */
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
    if (!s_external_host) attach_to_host(self);
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (s_shared_metal_view.superview == self.view) {
        s_shared_metal_view.frame = self.view.bounds;
        [(UIView *)s_shared_metal_view setNeedsLayout];
        [s_shared_metal_view layoutIfNeeded];
    }
}
@end

UIViewController *ios_metal_create_view_controller(void) {
    MetalHostViewController *vc = [MetalHostViewController new];
    vc.view.backgroundColor = UIColor.blackColor;
    vc.view.opaque = YES;
    vc.view.userInteractionEnabled = NO;
    s_phone_host = vc;
    if (!s_external_host) attach_to_host(vc);
    return vc;
}

void ios_metal_attach_external_display(UIViewController *host) {
    s_external_host = host;
    attach_to_host(host);
    atomic_store(&s_external, true);
    update_display_preferences();
}

void ios_metal_detach_external_display(UIViewController *host) {
    if (s_external_host != host) return;
    s_external_host = nil;
    [s_shared_metal_view removeFromSuperview];
    attach_to_host(s_phone_host);
    atomic_store(&s_external, false);
    update_display_preferences();
}

/* Register the documented iOS 27 scene accessory from a persistent phone controller.
   Public Objective-C selectors keep older Xcode SDKs usable as well. */
@interface ExternalDisplayRegistrationController : UIViewController
@property (nonatomic, strong) id displayRegistration;
@end
@implementation ExternalDisplayRegistrationController
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (self.displayRegistration) return;
    Class accessoryClass = NSClassFromString(@"UISceneAccessory");
    SEL factory = NSSelectorFromString(@"externalNonInteractiveSceneAccessoryWithConfiguration:");
    SEL registration = NSSelectorFromString(@"registerSceneAccessory:");
    if (![accessoryClass respondsToSelector:factory] || ![self respondsToSelector:registration]) return;
    Class delegateClass = NSClassFromString(@"ExternalDisplaySceneDelegate");
    if (!delegateClass) return;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"TV"
                                                          sessionRole:UIWindowSceneSessionRoleExternalDisplayNonInteractive];
    config.delegateClass = delegateClass;
    id (*makeAccessory)(id, SEL, UISceneConfiguration *) = (void *)[accessoryClass methodForSelector:factory];
    id (*registerAccessory)(id, SEL, id) = (void *)[self methodForSelector:registration];
    id accessory = makeAccessory(accessoryClass, factory, config);
    if (accessory) self.displayRegistration = registerAccessory(self, registration, accessory);
}
@end

UIViewController *ios_metal_create_display_registration_controller(void) {
    ExternalDisplayRegistrationController *host = [ExternalDisplayRegistrationController new];
    host.view.backgroundColor = UIColor.clearColor;
    host.view.userInteractionEnabled = NO;
    return host;
}
