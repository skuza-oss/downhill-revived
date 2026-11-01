/* The SDL thread (W11) is the process's main thread, on every platform: macOS's Cocoa wants its
   windows and its event loop there, and one structure serves Windows, Linux and Android the same
   way (Android's main thread is SDL_main's, port/host/android_main.c). sdl_host_main runs the game
   on a thread of its own (64 MiB of stack, normal priority, as main had) and this loop here: it
   owns the video subsystem and the window (SDL's window messages go to the thread that created
   the window, and SDL's documentation puts the video calls and the event queue on one thread),
   it pumps the events and sends them where they belong (the window, the keyboard, the text field,
   the controllers), and it runs the controllers' pass (gamepad.c) every 4 ms. The game ends the
   process itself (exit, host_exit) or returns from its thread; main then returns its code.

   The game thread never waits on SDL's queue. It reads copies: the keyboard's keys
   (keyboard.c), the controllers' states (gamepad.c), and the window's size and close (present.c).
   A modal drag or resize of the window stalls this thread only: the game keeps running.

   Keys: SDL's key events by place (scan code, for letters: the US place) and by the character the
   key types (keycode). Control characters that Windows sent as WM_CHAR (Backspace, Tab, Enter,
   Escape, Ctrl+letter) are sent from the key events here, repeats included, so the text field
   behaves as before. Text: SDL's UTF-8 text input, decoded to code points.

   Used through hle.h: sdl_host_main (launcher.c's main), sdl_host_start (the controllers' start,
   once), sdl_host_run (a function run on this thread, waiting for it: the window's calls). */
#include "host_api.h"
#include "../hle/hle.h"
#include "../hle/text_input.h"
#include "../gs/present.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_AtomicInt up_ok;        /* 1 once SDL's events are up on the main thread */
static SDL_AtomicInt game_running; /* the game's thread has not returned */
static SDL_InitState pads;         /* sdl_host_start's one start */
static Uint32 wake_event;          /* pushed by sdl_host_run: the loop runs the request at once */
static SDL_ThreadID main_thread;   /* sdl_host_main's */
static SDL_Mutex *slot_lock;       /* one request at a time */
static void (*request_fn)(void *); /* the pending request (under slot_lock) */
static void *request_arg;
static SDL_Semaphore *request_done;

/* Keys that are not letters, by the Windows virtual-key code the game's tables use (keyboard.c). */
static unsigned special_key(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    default: return 0;
    }
}

/* The control character Windows sent as WM_CHAR for the key (0: none). */
static unsigned control_char(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_BACKSPACE: return 8;
    case SDL_SCANCODE_TAB: return 9;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 13;
    case SDL_SCANCODE_ESCAPE: return 27;
    default: return 0;
    }
}

static void key_event(const SDL_KeyboardEvent *k)
{
    SDL_Scancode sc = k->scancode;
    SDL_Keycode key = k->key;
    int down = k->down;
    unsigned mods = k->mod;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        /* a letter: its place on a US keyboard, and the letter it types on this layout */
        unsigned place = 'A' + (sc - SDL_SCANCODE_A), typed = place;
        int letter = key >= 'a' && key <= 'z';
        if (letter) typed = (unsigned)(key - 'a' + 'A');
        keyboard_key(place, typed, down);
        /* Ctrl+letter is a control character to the field (Ctrl+V = 22); AltGr (Ctrl+Alt) types text */
        if (down && letter && (mods & SDL_KMOD_CTRL) && !(mods & SDL_KMOD_ALT))
            text_input_character((unsigned)(key - 'a' + 1));
        return;
    }
    unsigned vk = special_key(sc);
    if (vk) keyboard_key(vk, vk, down);
    if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL)
        keyboard_key(VKEY_CONTROL, VKEY_CONTROL, (mods & SDL_KMOD_CTRL) != 0);
    if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT)
        keyboard_key(VKEY_MENU, VKEY_MENU, (mods & SDL_KMOD_ALT) != 0);
    if (sc == SDL_SCANCODE_LGUI) keyboard_key(VKEY_LWIN, VKEY_LWIN, down);
    if (sc == SDL_SCANCODE_RGUI) keyboard_key(VKEY_RWIN, VKEY_RWIN, down);
    if (down && control_char(sc)) text_input_character(control_char(sc));
}

/* One code point of UTF-8 at *s; advances *s. */
static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= 0x3F >> more;
    for (int i = 0; i < more && (*p & 0xC0) == 0x80; i++) c = c << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

static void text_event(const SDL_TextInputEvent *t)
{
    const char *s = t->text;
    while (s && *s) {
        unsigned c = utf8_next(&s);
        if (c) text_input_character(c);
    }
}

static void dispatch(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        key_event(&e->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        text_event(&e->text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        keyboard_focus(1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:  /* WM_KILLFOCUS: the field's text is cleared, the keys let go */
        keyboard_focus(0);
        text_input_clear();
        break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        gs_mouse_event(e);  /* present.c: the pointer over the picture, for the menus */
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
        gs_window_event(e->type);
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        gamepad_device(PADDEV_ADDED, (unsigned)e->jdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        gamepad_device(PADDEV_REMOVED, (unsigned)e->jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        gamepad_device(PADDEV_REMAPPED, (unsigned)e->gdevice.which);
        break;
#ifdef __ANDROID__
    /* The on-screen controls (touch.c, finding 141): SDL's finger events, kept apart from the
       mouse path below, which only ever sees one finger. */
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        touch_event(e);
        break;
#endif
    default:
        break;
    }
}

/* A request from sdl_host_run, if one is waiting: run it, then let its caller go. */
static void run_request(void)
{
    SDL_LockMutex(slot_lock);
    void (*fn)(void *) = request_fn;
    void *arg = request_arg;
    SDL_Semaphore *done = request_done;
    request_fn = NULL;
    SDL_UnlockMutex(slot_lock);
    if (fn) {
        fn(arg);
        SDL_SignalSemaphore(done);
    }
}

/* On this thread, at the game's first sdl_host_start: the controllers (their own messages, logs
   and failures; gamepad.c reads dhd.ini's settings, which the game has loaded by then). */
static void start_input(void *unused)
{
    (void)unused;
    /* DHD_TEST_INPUT=1 (test runs on a desktop that is never shown, whose window never gets the
       focus): the keys sent to the window count, as if it had the keyboard. */
    if (getenv("DHD_TEST_INPUT")) keyboard_focus(1);
    gamepad_thread_start();
}

typedef struct { int (*fn)(void *); void *arg; } GameStart;

static int SDLCALL game_thread(void *start)
{
    GameStart *g = start;
    int code = g->fn(g->arg);
    SDL_SetAtomicInt(&game_running, 0);
    return code;
}

int sdl_host_main(int (*game)(void *), void *arg)
{
    enum { GAME_STACK = 64 << 20 };  /* the EE's fibers and wasm2c's frames run on it */
    host_thread_name("sdl");
    main_thread = SDL_GetCurrentThreadID();
    slot_lock = SDL_CreateMutex();
    int events = SDL_Init(SDL_INIT_EVENTS);
    if (events) wake_event = SDL_RegisterEvents(1);
    else fprintf(stderr, "sdl: no event queue (%s)\n", SDL_GetError());
    SDL_SetAtomicInt(&up_ok, events);
    GameStart start = {game, arg};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)game_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "game");
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &start);
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, GAME_STACK);
    SDL_SetAtomicInt(&game_running, 1);
    SDL_Thread *thread = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!thread) {
        fprintf(stderr, "sdl: no game thread (%s)\n", SDL_GetError());
        return 1;
    }
    while (SDL_GetAtomicInt(&game_running)) {
        run_request();
        if (!events) { SDL_Delay(4); continue; }
        SDL_Event e;
#ifdef __ANDROID__
        /* SDL's Android wait wakes itself with each event it queues (a core spinning, measured on
           a Mi 11i): the events so far, then a 4 ms sleep, the same cadence */
        while (SDL_PollEvent(&e)) dispatch(&e);
        SDL_Delay(4);
#else
        /* wakes on any event (sdl_host_run's too); 4 ms at most, so the controllers' states and
           vibration are never late */
        for (int got = SDL_WaitEventTimeout(&e, 4); got; got = SDL_PollEvent(&e)) dispatch(&e);
#endif
        gamepad_thread_update();
        gs_window_tick();
    }
    int code = 0;
    SDL_WaitThread(thread, &code);
    return code;
}

/* The controllers' start on the main thread, once (the game thread's first call waits for it);
   returns 1 when SDL's events are up. */
int sdl_host_start(void)
{
    if (SDL_ShouldInit(&pads)) {
        sdl_host_run(start_input, NULL);
        SDL_SetInitialized(&pads, true);
    }
    return SDL_GetAtomicInt(&up_ok);
}

/* Runs fn(arg) on the main thread and returns when it has. 0 if SDL's events aren't up there. */
int sdl_host_run(void (*fn)(void *), void *arg)
{
    if (!SDL_GetAtomicInt(&up_ok)) return 0;
    if (SDL_GetCurrentThreadID() == main_thread) { fn(arg); return 1; }  /* asked from the loop itself */
    SDL_Semaphore *done = SDL_CreateSemaphore(0);
    for (;;) {  /* one request at a time */
        SDL_LockMutex(slot_lock);
        if (!request_fn) break;
        SDL_UnlockMutex(slot_lock);
        SDL_Delay(1);
    }
    request_fn = fn;
    request_arg = arg;
    request_done = done;
    SDL_UnlockMutex(slot_lock);
    SDL_Event wake;
    SDL_zero(wake);
    wake.type = wake_event;
    SDL_PushEvent(&wake);  /* the loop's wait returns now */
    SDL_WaitSemaphore(done);
    SDL_DestroySemaphore(done);
    return 1;
}
