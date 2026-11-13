/* The launcher page's calls on iOS (port/launcher/BRIDGE.md; finding 227): the message handling,
   in plain C over the shared code Android's launcher calls too (the settings crate's dhd_settings,
   host.c's host_disc_check, dhd_disc_check, phone.c's phone_update_find). The page's message
   {id, call, args} goes in through launcher_receive; the answer {id, ok, value|error} and the
   events {event, payload} come out through the host's send. What only UIKit can do (the document
   picker, the share sheet, opening a link, starting the game) is the host's: port/ios/app/launcher.m
   fills LauncherHost. port/ios/tests/test_launcher_calls.c drives it with a fake page.
   GPL-3.0-or-later. */
#ifndef DHD_IOS_LAUNCHER_CALLS_H
#define DHD_IOS_LAUNCHER_CALLS_H

#include <stdint.h>

typedef struct LauncherHost {
    const char *dir;        /* the app's Documents folder, with a trailing slash: dhd.ini, the disc, the logs */
    const char *version;    /* this build's (version.h) */
    void *context;
    /* a message to the page (any thread; the host puts it on the web view's) */
    void (*send)(void *context, const char *json);
    /* the document picker for the disc image; the host answers the call itself (launcher_answer)
       with {job} once a document is picked, or null when cancelled */
    void (*pick_disc)(void *context, int id);
    /* the last pick's document again ({job} or null), and its deletion (true when deleted) */
    void (*retry_import)(void *context, int id);
    void (*delete_original)(void *context, int id, long long job);
    /* PLAY: the game in the launcher's place */
    void (*play)(void *context, int edit_touch_layout, int checked);
    void (*open_url)(void *context, const char *url);
    void (*share_log)(void *context);
    /* the licence texts the app carries (NULL: none; the host frees nothing, the text is kept) */
    const char *(*licences)(void *context);
    uint64_t (*free_space)(void *context);
} LauncherHost;

/* The caps an iOS launcher reports (BRIDGE.md, "Capabilities"). */
extern const char LAUNCHER_IOS_CAPS[];
/* The largest resolution scale a phone's renderer draws (gpu_rust_shared.inc NATIVE_PHONE_SCALE_MAX). */
enum { LAUNCHER_PHONE_SCALE_MAX = 6 };

/* The run before closed unexpectedly: why, said once by the next snapshot (NULL: nothing). */
void launcher_set_last_exit(const char *why);
/* dir's disc image copy (SCUS-97177.iso) into out; 1 when it is there. */
int launcher_disc_path(const LauncherHost *h, char *out, size_t size);
/* The setting "launcher" is off and the disc is ready: the app starts the game at once. */
int launcher_starts_directly(const LauncherHost *h);

/* The settings' order (finding 227). dhd.ini is read, changed and written whole by each save
   (dhd_settings_save), so two saves at once could lose one, and the game must not read it while a
   save is still writing. The calls that touch it (snapshot, which may correct the disc path,
   save_setting, and play, which hands the file to the game) take a ticket where the page's messages
   arrive one by one (the web view's handler, the main thread) and run one at a time in that order,
   on whatever thread; the game's own correction at its start takes one too (launcher_settings_run).
   After PLAY a settings call is refused: the file is the game's. The long calls (the update check,
   the online check's hashing, the copy) take none. */
int launcher_settings_call(const char *message);   /* 1 for a call that takes a ticket */
unsigned long launcher_order_ticket(void);
void launcher_receive_ordered(const LauncherHost *h, unsigned long ticket, const char *message);
/* The game refused to start before a core took the settings (ios_start_game's last disc check, 3):
   the launcher shows again, and its settings calls and PLAY work again (in the settings' order). */
void launcher_start_refused(void);
/* fn(context) in the settings' order (blocks until the calls before it are done; not from inside one) */
void launcher_settings_run(void (*fn)(void *context), void *context);

/* One message from the page; may block (the update check, the online check's hashing): call it
   on a worker thread. */
void launcher_receive(const LauncherHost *h, const char *message);
/* An answer (value_json: a JSON value, NULL for null; ok 0: value_json is the error) and an event. */
void launcher_answer(const LauncherHost *h, int id, int ok, const char *value_json);
void launcher_event(const LauncherHost *h, const char *event, const char *payload_json);
/* A coded error the page words itself: {"code": code, "params": {name: value, ...}} (pairs, NULL-ended). */
void launcher_coded(char *out, size_t size, const char *code, ...);

#endif
