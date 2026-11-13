/* The launcher on iOS (finding 227): the desktop launcher's own page (port/launcher/ui, packed into
   the app as launcher/ by build_ios.py) in a WKWebView, and the page's calls (port/launcher/BRIDGE.md)
   answered by launcher_calls.c, the same shared code Android's launcher calls (the settings crate,
   host.c's disc check, the online check, the update check). Here only what UIKit has to do: the web
   view and its one origin, the bridge's transport, the document picker and the disc's copy (its
   rules: disc_copy.c, host_disc_check), the share sheet, links, and PLAY.

   The page is the app's own and nothing else: the web view loads only dhd-launcher://app/..., served
   from the app's launcher/ folder with the content security policy Android's launcher sends; every
   other request gets 403 and no navigation leaves the page. The bridge: bridge_ios.js, injected at
   the document's end in a content world of its own, posts the page a MessagePort with "dhd-bridge",
   as Android's app does; the page's messages reach the "dhd" message handler of that world only,
   and the answers go back through it (dhdDeliver).

   Test switch (the Simulator's checks, finding 227): DHD_LAUNCHER_TEST names a file in the app's
   Documents with one line each: "wait SECONDS", "mark NAME" (logged, then 4 s still, for a
   screenshot), "turn portrait" or "turn landscape" (the launcher's window asked to turn), or a
   JavaScript expression run in the page (e.g. bridge.call('save_setting', {key: 'perf_counter',
   value: '1'})); each result goes to Documents/launcher-test.log. GPL-3.0-or-later. */
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app.h"
#include "ios_main.h"
#include "launcher_calls.h"
#include "disc_copy.h"
#include "include/version.h"   /* port/, the project's header search path */

extern const char *host_disc_check(const char *path);

static NSString *const SCHEME = @"dhd-launcher";
/* Android's LauncherActivity.CSP: the page's own files, no network, no inline script. */
static NSString *const CSP = @"default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
    "font-src 'self'; connect-src 'self'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'";

@class LauncherViewController;
static __weak LauncherViewController *s_launcher;
static LauncherHost s_host;

static void log_test(NSString *line) {
    NSString *path = [[NSString stringWithUTF8String:ios_documents_dir()] stringByAppendingPathComponent:@"launcher-test.log"];
    FILE *f = fopen(path.fileSystemRepresentation, "a");
    if (f) { fprintf(f, "%s\n", line.UTF8String); fclose(f); }
    NSLog(@"launcher: %@", line);
}

static NSString *gb(uint64_t bytes) { return [NSString stringWithFormat:@"%.1f GB", bytes / (double)(1ull << 30)]; }

/* ---- the page's files under one origin ---- */

@interface LauncherFiles : NSObject <WKURLSchemeHandler>
@end
@implementation LauncherFiles
- (void)webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    NSURL *url = task.request.URL;
    NSString *path = url.path, *type = nil;
    NSDictionary *types = @{@"html": @"text/html", @"js": @"text/javascript", @"css": @"text/css", @"json": @"application/json",
                            @"woff2": @"font/woff2", @"png": @"image/png", @"svg": @"image/svg+xml"};
    NSData *data = nil;
    if ([url.scheme isEqualToString:SCHEME] && [url.host isEqualToString:@"app"] && !url.port && path.length > 1
        && ![path containsString:@".."] && (type = types[path.pathExtension.lowercaseString])) {
        NSString *file = [[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"launcher"]
                          stringByAppendingPathComponent:[path substringFromIndex:1]];
        data = [NSData dataWithContentsOfFile:file];
    }
    NSMutableDictionary *headers = [@{@"Content-Security-Policy": CSP, @"X-Content-Type-Options": @"nosniff",
                                      @"Cache-Control": @"no-store"} mutableCopy];
    if (data) headers[@"Content-Type"] = [type hasPrefix:@"text/"] || [type hasSuffix:@"json"] ? [type stringByAppendingString:@"; charset=utf-8"] : type;
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:url statusCode:data ? 200 : 403
                                                             HTTPVersion:@"HTTP/1.1" headerFields:headers];
    [task didReceiveResponse:response];
    [task didReceiveData:data ?: [NSData data]];
    [task didFinish];
}
- (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task { (void)webView; (void)task; }
@end

/* ---- the disc image's import (DiscImport.java's job: check, room, copy, check the copy, put it in place) ---- */

@interface DiscJob : NSObject {
@public
    volatile int cancel;   /* a newer pick replaced it: it publishes nothing and sends nothing more */
}
@property (nonatomic) long long number;
@property (nonatomic) long long generation;   /* disc_publish's: it lands only while the newest */
@property (nonatomic, strong) NSURL *url;
@end
@implementation DiscJob
@end

static atomic_llong s_jobs;

static void job_event(DiscJob *job, NSString *stage, NSString *extra) {
    if (job->cancel) return;
    NSString *payload = [NSString stringWithFormat:@"{\"job\":%lld,\"stage\":\"%@\"%@}", job.number, stage, extra ?: @""];
    launcher_event(&s_host, "disc-import", payload.UTF8String);
}

static void job_problem(DiscJob *job, const char *coded_or_text, int coded) {
    if (coded) {
        job_event(job, @"problem", [NSString stringWithFormat:@",\"problem\":%s", coded_or_text]);
    } else {
        NSData *q = [NSJSONSerialization dataWithJSONObject:[NSString stringWithUTF8String:coded_or_text] ?: @""
                                                    options:NSJSONWritingFragmentsAllowed error:nil];
        job_event(job, @"problem", [NSString stringWithFormat:@",\"problem\":%@", [[NSString alloc] initWithData:q encoding:NSUTF8StringEncoding]]);
    }
}

static void job_progress(void *context, uint64_t done) {
    DiscJob *job = (__bridge DiscJob *)context;
    struct stat st;
    long long total = stat(job.url.path.fileSystemRepresentation, &st) == 0 ? (long long)st.st_size : 0;
    job_event(job, @"copying", [NSString stringWithFormat:@",\"done\":%llu,\"total\":%lld", done, total]);
}

static BOOL job_deletable(NSURL *url) {
    return [NSFileManager.defaultManager isDeletableFileAtPath:url.path];
}

/* The job's work on its own thread: everything that may block (a provider downloading the
   document first, the reads, the copy). */
static void job_run(DiscJob *job) {
    NSString *docs = [NSString stringWithUTF8String:ios_documents_dir()];
    NSString *disc = [docs stringByAppendingPathComponent:@"SCUS-97177.iso"];
    BOOL scoped = [job.url startAccessingSecurityScopedResource];
    job_event(job, @"checking", nil);
    __block NSError *coordination = nil;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:job.url options:NSFileCoordinatorReadingWithoutChanges error:&coordination
                                 byAccessor:^(NSURL *source) {
        char coded[600];
        const char *path = source.path.fileSystemRepresentation;
        const char *problem = host_disc_check(path);
        if (problem) { job_problem(job, problem, 0); return; }
        struct stat st;
        if (stat(path, &st) != 0) {
            launcher_coded(coded, sizeof coded, "disc.problem.openFailed", "detail", strerror(errno), (const char *)NULL);
            job_problem(job, coded, 1);
            return;
        }
        uint64_t size = (uint64_t)st.st_size;
        /* a file already in the app's folder (put there through the Files app): moved into place */
        NSString *resolved = source.URLByResolvingSymlinksInPath.path, *home = [docs stringByResolvingSymlinksInPath];
        if ([resolved hasPrefix:[home stringByAppendingString:@"/"]]) {
            if (![resolved isEqualToString:[disc stringByResolvingSymlinksInPath]]) {
                int moved = disc_publish(job.generation, path, disc.fileSystemRepresentation);
                if (moved == 1) return;   /* a newer pick replaced this one: nothing moved, nothing said */
                if (moved != 0) {
                    launcher_coded(coded, sizeof coded, "disc.problem.copyFailed", "detail", strerror(errno), (const char *)NULL);
                    job_problem(job, coded, 1);
                    return;
                }
            }
            remove([docs stringByAppendingPathComponent:@"iso-check.txt"].fileSystemRepresentation);
            job_event(job, @"done", [NSString stringWithFormat:@",\"deletable\":false,\"size\":%llu", size]);
            return;
        }
        uint64_t free_bytes = ios_available_disk_space();
        if (size + DISC_COPY_MARGIN > free_bytes) {
            launcher_coded(coded, sizeof coded, "disc.problem.noRoom", "need", gb(size + DISC_COPY_MARGIN).UTF8String,
                           "free", gb(free_bytes).UTF8String, (const char *)NULL);
            job_problem(job, coded, 1);
            return;
        }
        NSString *temp = [disc stringByAppendingFormat:@".%lx-%lld.part", (long)time(NULL), job.number];
        FILE *in = fopen(path, "rb"), *out = in ? fopen(temp.fileSystemRepresentation, "wb") : NULL;
        if (!in || !out) {
            launcher_coded(coded, sizeof coded, "disc.problem.openFailed", "detail", strerror(errno), (const char *)NULL);
            job_problem(job, coded, 1);
            if (in) fclose(in);
            return;
        }
        job_event(job, @"copying", [NSString stringWithFormat:@",\"done\":0,\"total\":%llu", size]);
        int64_t copied = disc_copy(in, out, &job->cancel, job_progress, (__bridge void *)job);
        fclose(in);
        int synced = fflush(out) == 0 && fsync(fileno(out)) == 0;
        synced &= fclose(out) == 0;
        if (job->cancel) { remove(temp.fileSystemRepresentation); return; }
        if (copied < 0 || !synced) {
            remove(temp.fileSystemRepresentation);
            if (ios_available_disk_space() < (64ull << 20)) launcher_coded(coded, sizeof coded, "disc.problem.outOfSpace", (const char *)NULL);
            else launcher_coded(coded, sizeof coded, "disc.problem.copyFailed", "detail", "a read or write failed", (const char *)NULL);
            job_problem(job, coded, 1);
            return;
        }
        /* the copy itself: the same disc, and complete */
        char why[400];
        const char *bad = host_disc_check(temp.fileSystemRepresentation);
        if (!bad) {
            FILE *f = fopen(temp.fileSystemRepresentation, "rb");
            uint64_t volume = f ? disc_volume_bytes(f) : 0;
            if (f) fclose(f);
            bad = disc_completeness((uint64_t)copied, size, volume, why, sizeof why);
        }
        if (bad) {
            NSLog(@"launcher: the disc's copy: %s (%lld bytes)", bad, copied);
            remove(temp.fileSystemRepresentation);
            job_problem(job, bad, 0);
            return;
        }
        /* in place only while this is the newest pick, checked and renamed under one lock
           (disc_publish): a superseded import never lands over a newer choice */
        int published = job->cancel ? 1 : disc_publish(job.generation, temp.fileSystemRepresentation,
                                                       disc.fileSystemRepresentation);
        if (published != 0) {
            remove(temp.fileSystemRepresentation);
            if (published < 0) job_problem(job, "The copy can't be stored.", 0);
            return;
        }
        remove([docs stringByAppendingPathComponent:@"iso-check.txt"].fileSystemRepresentation);  /* the old file's answer */
        NSLog(@"launcher: disc image copied: %lld bytes, checked", copied);
        job_event(job, @"done", [NSString stringWithFormat:@",\"deletable\":%@,\"size\":%lld",
                                 job_deletable(job.url) ? @"true" : @"false", copied]);
    }];
    if (coordination) {
        char coded[600];
        launcher_coded(coded, sizeof coded, "disc.problem.openFailed", "detail", coordination.localizedDescription.UTF8String,
                       (const char *)NULL);
        job_problem(job, coded, 1);
    }
    if (scoped) [job.url stopAccessingSecurityScopedResource];
}

/* ---- the page ---- */

@interface LauncherViewController : UIViewController <WKNavigationDelegate, WKScriptMessageHandler, UIDocumentPickerDelegate>
@property (nonatomic, strong) WKWebView *web;
@property (nonatomic, strong) WKContentWorld *world;
@property (nonatomic, strong) DiscJob *job;
@property (nonatomic) int picking;   /* the pick_disc call waiting for the picker, 0 none */
@property (nonatomic) BOOL loaded;
@end

@implementation LauncherViewController

- (void)loadView {
    WKWebViewConfiguration *config = [WKWebViewConfiguration new];
    config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
    [config setURLSchemeHandler:[LauncherFiles new] forURLScheme:SCHEME];
    config.allowsInlineMediaPlayback = NO;
    config.dataDetectorTypes = WKDataDetectorTypeNone;
    self.world = [WKContentWorld worldWithName:@"dhd-bridge"];
    NSString *js = [NSString stringWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"bridge_ios" ofType:@"js"]
                                             encoding:NSUTF8StringEncoding error:nil] ?: @"";
    WKUserScript *script = [[WKUserScript alloc] initWithSource:js injectionTime:WKUserScriptInjectionTimeAtDocumentEnd
                                               forMainFrameOnly:YES inContentWorld:self.world];
    [config.userContentController addUserScript:script];
    [config.userContentController addScriptMessageHandler:self contentWorld:self.world name:@"dhd"];
    self.web = [[WKWebView alloc] initWithFrame:CGRectZero configuration:config];
    self.web.navigationDelegate = self;
    self.web.opaque = NO;
    self.web.backgroundColor = [UIColor colorWithRed:0x1A / 255.0 green:0x14 / 255.0 blue:0x30 / 255.0 alpha:1];
    self.web.scrollView.backgroundColor = self.web.backgroundColor;
    self.web.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;  /* the page pads by env(safe-area-inset-*) */
    self.web.scrollView.bounces = NO;
    self.web.allowsLinkPreview = NO;
    self.view = self.web;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    s_launcher = self;
    [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"dhd-launcher://app/index.html"]]];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(resumed) name:UIApplicationDidBecomeActiveNotification object:nil];
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return NO; }

/* back from the share sheet, the picker or a browser: the page reads the settings again */
- (void)resumed {
    if (self.loaded && self.view.window) launcher_event(&s_host, "resumed", NULL);
}

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
    (void)webView;
    NSURL *url = action.request.URL;
    BOOL own = [url.scheme isEqualToString:SCHEME] && [url.host isEqualToString:@"app"] && !self.loaded
               && action.targetFrame.isMainFrame;
    decide(own ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);   /* links go through the bridge */
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    (void)webView; (void)navigation;
    self.loaded = YES;
    const char *test = getenv("DHD_LAUNCHER_TEST");
    if (test && *test) [self runTest:[NSString stringWithUTF8String:test]];
}

/* The web view's process ended (iOS takes it back under memory pressure): the page again. */
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    self.loaded = NO;
    [webView reload];
}

/* the page's message, on the main thread: answered on a worker (the update check, the online check wait) */
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller;
    if (![message.body isKindOfClass:NSString.class] || ![message.frameInfo.request.URL.scheme isEqualToString:SCHEME]) return;
    NSString *text = message.body;
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    if (launcher_settings_call(text.UTF8String)) {
        /* dhd.ini's calls in the order they came (launcher_calls.h): the ticket is taken here, on
           the main thread where the page's messages arrive one by one */
        unsigned long ticket = launcher_order_ticket();
        dispatch_async(queue, ^{ launcher_receive_ordered(&s_host, ticket, text.UTF8String); });
    } else {
        dispatch_async(queue, ^{ launcher_receive(&s_host, text.UTF8String); });
    }
}

- (void)deliver:(NSString *)text {
    if (!self.web) return;
    [self.web callAsyncJavaScript:@"if (typeof dhdDeliver === 'function') dhdDeliver(text);" arguments:@{@"text": text}
                          inFrame:nil inContentWorld:self.world completionHandler:nil];
}

/* ---- the picker ---- */

- (void)pick:(int)call {
    if (self.picking) launcher_answer(&s_host, self.picking, 1, NULL);   /* the one before: cancelled */
    self.picking = call;
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    if (self.picking) launcher_answer(&s_host, self.picking, 1, NULL);
    self.picking = 0;
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    int call = self.picking;
    self.picking = 0;
    NSURL *url = urls.firstObject;
    if (!url) { if (call) launcher_answer(&s_host, call, 1, NULL); return; }
    NSLog(@"launcher: disc image chosen: %@", url.lastPathComponent);
    long long number = [self startImport:url];
    if (call) launcher_answer(&s_host, call, 1, [NSString stringWithFormat:@"{\"job\":%lld}", number].UTF8String);
}

- (long long)startImport:(NSURL *)url {
    DiscJob *before = self.job;
    if (before) before->cancel = 1;
    DiscJob *job = [DiscJob new];
    job.number = atomic_fetch_add(&s_jobs, 1) + 1;
    job.generation = disc_publish_begin();   /* every older import is superseded from here */
    job.url = url;
    self.job = job;
    NSThread *thread = [[NSThread alloc] initWithBlock:^{ job_run(job); }];
    thread.name = @"disc copy";
    thread.qualityOfService = NSQualityOfServiceUserInitiated;
    [thread start];
    return job.number;
}

/* ---- Share log: the device, how the last run ended, the logs (Android's LogShare) ---- */

- (void)shareLog {
    NSString *docs = [NSString stringWithUTF8String:ios_documents_dir()];
    NSDateFormatter *day = [NSDateFormatter new];
    day.dateFormat = @"yyyy-MM-dd";
    NSString *name = [NSString stringWithFormat:@"downhill-revived-%s-log-%@.txt", DHD_VERSION, [day stringFromDate:NSDate.date]];
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
    NSMutableData *out = [NSMutableData data];
    UIDevice *device = UIDevice.currentDevice;
    [out appendData:[[NSString stringWithFormat:@"Downhill Revived %s on %@ %@ (%@)\n\n", DHD_VERSION, device.systemName,
                      device.systemVersion, device.model] dataUsingEncoding:NSUTF8StringEncoding]];
    for (NSString *file in @[@"dhd-exit.txt", @"dhd.log", @"dhd-previous.log"]) {
        NSData *data = [NSData dataWithContentsOfFile:[docs stringByAppendingPathComponent:file]];
        if (!data) continue;
        const NSUInteger most = 1792 << 10;   /* a crash is at the end */
        if (data.length > most) data = [data subdataWithRange:NSMakeRange(data.length - most, most)];
        [out appendData:[[NSString stringWithFormat:@"==== %@ ====\n", file] dataUsingEncoding:NSUTF8StringEncoding]];
        [out appendData:data];
        [out appendData:[@"\n\n" dataUsingEncoding:NSUTF8StringEncoding]];
    }
    [out writeToFile:path atomically:YES];
    UIActivityViewController *share = [[UIActivityViewController alloc]
        initWithActivityItems:@[[NSURL fileURLWithPath:path]] applicationActivities:nil];
    share.popoverPresentationController.sourceView = self.view;   /* an iPad shows it as a popover */
    share.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(self.view.bounds), CGRectGetMidY(self.view.bounds), 1, 1);
    share.popoverPresentationController.permittedArrowDirections = 0;
    [self presentViewController:share animated:YES completion:nil];
}

/* ---- the test switch ---- */

- (void)runTest:(NSString *)file {
    NSString *path = [[NSString stringWithUTF8String:ios_documents_dir()] stringByAppendingPathComponent:file];
    NSArray<NSString *> *lines = [[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil]
                                  componentsSeparatedByString:@"\n"];
    log_test([NSString stringWithFormat:@"test: %lu lines from %@", (unsigned long)lines.count, file]);
    [self runLines:lines at:0];
}

- (void)runLines:(NSArray<NSString *> *)lines at:(NSUInteger)i {
    if (i >= lines.count) { log_test(@"test: done"); return; }
    NSString *line = [lines[i] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    __weak LauncherViewController *weak = self;
    if (!line.length || [line hasPrefix:@"#"]) { [self runLines:lines at:i + 1]; return; }
    if ([line hasPrefix:@"wait "]) {
        double seconds = [line substringFromIndex:5].doubleValue;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [weak runLines:lines at:i + 1];
        });
        return;
    }
    if ([line hasPrefix:@"mark "]) {   /* a moment for the check's screenshot: logged, then 4 s still */
        log_test(line);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [weak runLines:lines at:i + 1];
        });
        return;
    }
    if ([line hasPrefix:@"turn "]) {   /* the launcher in portrait or landscape (no rotation in the Simulator's tools) */
        BOOL wide = [line hasSuffix:@"landscape"];
        UIWindowSceneGeometryPreferencesIOS *want = [[UIWindowSceneGeometryPreferencesIOS alloc]
            initWithInterfaceOrientations:wide ? UIInterfaceOrientationMaskLandscapeRight : UIInterfaceOrientationMaskPortrait];
        [self.view.window.windowScene requestGeometryUpdateWithPreferences:want errorHandler:^(NSError *error) {
            log_test([NSString stringWithFormat:@"turn: %@", error.localizedDescription]);
        }];
        log_test(line);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [weak runLines:lines at:i + 1];
        });
        return;
    }
    NSString *body = [NSString stringWithFormat:@"return JSON.stringify(await (%@));", line];
    [self.web callAsyncJavaScript:body arguments:@{} inFrame:nil inContentWorld:WKContentWorld.pageWorld
                completionHandler:^(id result, NSError *error) {
        log_test([NSString stringWithFormat:@"%@ -> %@", line, error ? [@"error: " stringByAppendingString:error.localizedDescription]
                                                                       : [NSString stringWithFormat:@"%@", result]]);
        [weak runLines:lines at:i + 1];
    }];
}

@end

/* ---- LauncherHost for launcher_calls.c ---- */

static void host_send(void *context, const char *json) {
    (void)context;
    NSString *text = [NSString stringWithUTF8String:json];
    dispatch_async(dispatch_get_main_queue(), ^{ [s_launcher deliver:text]; });
}
static void host_pick(void *context, int id) {
    (void)context;
    dispatch_async(dispatch_get_main_queue(), ^{ [s_launcher pick:id]; });
}
static void host_retry(void *context, int id) {
    (void)context;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSURL *url = s_launcher.job.url;
        long long number = url ? [s_launcher startImport:url] : 0;
        launcher_answer(&s_host, id, 1, number ? [NSString stringWithFormat:@"{\"job\":%lld}", number].UTF8String : NULL);
    });
}
static void host_delete_original(void *context, int id, long long job) {
    (void)context;
    dispatch_async(dispatch_get_main_queue(), ^{
        DiscJob *j = s_launcher.job;
        BOOL deleted = NO;
        if (j && j.number == job) {
            BOOL scoped = [j.url startAccessingSecurityScopedResource];
            NSError *error = nil;
            deleted = [NSFileManager.defaultManager removeItemAtURL:j.url error:&error];
            if (!deleted) NSLog(@"launcher: the original couldn't be deleted: %@", error.localizedDescription);
            if (scoped) [j.url stopAccessingSecurityScopedResource];
        }
        launcher_answer(&s_host, id, 1, deleted ? "true" : "false");
    });
}
static void host_play(void *context, int edit, int checked) {
    (void)context;
    dispatch_async(dispatch_get_main_queue(), ^{ app_play(edit, checked); });
}
static void host_open(void *context, const char *url) {
    (void)context;
    NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (u) [UIApplication.sharedApplication openURL:u options:@{} completionHandler:nil];
    });
}
static void host_share(void *context) {
    (void)context;
    dispatch_async(dispatch_get_main_queue(), ^{ [s_launcher shareLog]; });
}
static const char *host_licences(void *context) {
    (void)context;
    static NSString *text;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableString *all = [NSMutableString string];
        for (NSString *name in @[@"LICENSE", @"THIRD-PARTY-NOTICES"]) {
            NSString *t = [NSString stringWithContentsOfFile:[NSBundle.mainBundle pathForResource:name ofType:@"txt"]
                                                    encoding:NSUTF8StringEncoding error:nil];
            if (t.length) [all appendFormat:@"%@%@", all.length ? @"\n\n" : @"", t];
        }
        text = all.length ? all : nil;
    });
    return text.UTF8String;
}
static uint64_t host_free(void *context) { (void)context; return ios_available_disk_space(); }

static void host_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        s_host.dir = ios_documents_dir();
        s_host.version = DHD_VERSION;
        s_host.send = host_send;
        s_host.pick_disc = host_pick;
        s_host.retry_import = host_retry;
        s_host.delete_original = host_delete_original;
        s_host.play = host_play;
        s_host.open_url = host_open;
        s_host.share_log = host_share;
        s_host.licences = host_licences;
        s_host.free_space = host_free;
    });
}

UIViewController *launcher_view_controller(void) {
    host_init();
    return [LauncherViewController new];
}

int launcher_should_start_game(void) {
    host_init();
    return launcher_starts_directly(&s_host);
}
