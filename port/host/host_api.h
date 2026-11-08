/* The host interface of the port (finding 081, phase 2): the OS services the module side uses,
   the game's code and runtime and the PS2 system's own code (port/runtime, port/hle). The module
   is wasm32 later and the host is native, so nothing here names a host type. Guest addresses are
   uint32_t; handles are small integers, 0 is "none"; the one host object the module keeps is the
   fiber id. Standard C (stdio, string, math) is used directly by the module.
   Implemented per host: host_win32.c (Windows). Other hosts add their own file. */
#ifndef PORT_HOST_API_H
#define PORT_HOST_API_H

#include <stddef.h>
#include <stdint.h>

/* The longest path the module handles (the host's own limit, MAX_PATH on Windows). */
enum { HOST_PATH_MAX = 260 };

/* ---- environment, errors and the program ---- */
const char *host_getenv(const char *name);  /* the variable's value, or NULL when it is not set */
unsigned long host_last_error(void);        /* the OS's code of the last failed host call (diagnostics) */
void host_error_box(const char *title, const char *message);  /* an error box, shown until it is dismissed */
unsigned host_exe_path(char *out, unsigned size);  /* the program's full path into out; its length */
int host_display_hz(void);                 /* the display's refresh rate in Hz; 0 when unknown */
void host_exit(int code);  /* ends the process at once, without the C library's exit handlers */
/* The FPU/SSE modes the EE runs in (kernel.c): round toward zero, denormals flushed, exceptions masked. */
enum { HOST_FP_EE = 1 };
void host_fp_mode(int mode);

/* ---- files: the disc image and the memory card's files ---- */
typedef int HostFile;
enum { HOST_FILE_NONE = 0 };
enum {
    HOST_FILE_READ = 1,      /* an existing file, read only */
    HOST_FILE_WRITE = 2,     /* read and write; the file must exist unless HOST_FILE_TRUNCATE */
    HOST_FILE_TRUNCATE = 4,  /* with WRITE: created, or emptied if it exists */
    HOST_FILE_DISCARD = 8    /* with WRITE: the writes go nowhere, and the path is not used */
};
HostFile host_file_open(const char *path, unsigned mode);  /* HOST_FILE_NONE if it can't be opened */
void host_file_close(HostFile f);
int64_t host_file_read(HostFile f, void *dst, size_t size);    /* bytes read (fewer at the end); -1 on error */
size_t host_file_write(HostFile f, const void *src, size_t size);  /* bytes written */
int host_file_flush(HostFile f);             /* 0 on success */
int host_file_seek(HostFile f, uint64_t offset);  /* from the start; 0 on success */
uint64_t host_file_size(HostFile f);

/* ---- paths and folders: the memory card's folders ---- */
typedef struct {                       /* local time */
    uint16_t year, month, day, hour, minute, second;
} HostTime;
typedef struct {                       /* one entry of a folder */
    char name[HOST_PATH_MAX];          /* its name, not the folder's; "." and ".." are listed as well */
    int is_dir;
    uint32_t size;                     /* its size in bytes, the low 32 bits */
    HostTime created, modified;
} HostDirEntry;
typedef int HostDir;
enum { HOST_DIR_NONE = 0 };
HostDir host_dir_open(const char *path, HostDirEntry *first);  /* the folder's first entry; HOST_DIR_NONE if none */
int host_dir_next(HostDir d, HostDirEntry *e);  /* the next entry; 0 at the end */
void host_dir_close(HostDir d);
int host_path_is_dir(const char *path);
int host_path_exists(const char *path);     /* a file or a folder of that name */
int host_dir_create(const char *path);      /* 1 if the folder was made */

/* ---- time and waiting: the wall clock (the game's own clock is virtual, kernel.c) ---- */
double host_wall_seconds(void);      /* since the first call */
void host_sleep_ms(unsigned ms);
void host_wait_wall(double seconds); /* until host_wall_seconds() reaches it */

/* ---- fibers: the EE threads run as fibers of one OS thread (kernel.c) ---- */
typedef int HostFiber;
enum { HOST_FIBER_NONE = 0 };
HostFiber host_fiber_main(void);     /* makes the calling thread a fiber (the idle one) */
HostFiber host_fiber_create(void (*fn)(void *), void *arg);
void host_fiber_switch(HostFiber f);
void host_fiber_delete(HostFiber f);
HostFiber host_fiber_current(void);
/* The host stacks the EE threads run on, as [low, high) ranges recorded as each yields; online.c
   leaves words that point into them out of its state hash. Returns the count. */
int host_stack_ranges(uintptr_t (**ranges)[2]);

/* ---- memory: PS2 RAM and its address space (port/runtime/memory.c decides the layout) ----
   A guest address is the host's address in this build (memory.h), so `at` is the address the
   view appears at. */
int host_ram_create(uint32_t size);  /* the one block of guest RAM, zeroed; 1 on success */
/* 1 if the block's [offset, offset + size) appears at `at`; with over_reservation, the reservation
   that starts at `at` is released first. */
int host_ram_view(uint32_t at, uint32_t offset, uint32_t size, int over_reservation);
int host_mem_commit(uint32_t at, uint32_t size);     /* commits [at, at + size), free or in a reservation of ours; 1 on success, else logs who holds it */
int host_mem_reserved(uint32_t at, uint32_t size);   /* a reservation that starts at `at` covers the range */

/* ---- x86 control state the VU and EE modes use (vu0_interp.c, ps2asm.c, kernel.c) ----
   host_fp_csr/host_fp_set_csr are the FP control word (MXCSR); where the host has none they read
   0 and do nothing, so the module's float results there are round-to-nearest. */
unsigned host_fp_csr(void);
void host_fp_set_csr(unsigned csr);
uint64_t host_cycles(void);  /* the cycle counter, for the timing diagnostics; 0 where there is none */

/* ---- the environment the game's settings are passed in (pc_settings.c, widescreen.c, online*.c) ---- */
void host_putenv(const char *text);                      /* "NAME=VALUE" (or "NAME=" to remove) */
void host_putenv_s(const char *name, const char *value);

/* ---- display (widescreen.c) ---- */
void host_dpi_aware(void);          /* the process sees real pixels, once; before any window */
double host_monitor_aspect(void);   /* the primary monitor's width / height; 16 / 9 when unknown */

/* ---- guest memory protection and access faults (graphics.c, deferred store-image reads) ---- */
enum { HOST_MEM_NOACCESS = 1, HOST_MEM_READWRITE = 2 };
/* Sets the protection of [at, at + size) in every view of it. 1 on success. */
int host_mem_protect(uint32_t at, uint32_t size, int protection);
/* Routes access faults on the calling thread to on_fault, with the faulting guest address. It
   returns 1 when it has handled the fault (the access is retried), 0 to pass it on. 1 if installed. */
int host_fault_guard(int (*on_fault)(uint32_t address));
int host_fault_guard_here(void);  /* the calling thread is the one host_fault_guard was called on */

/* ---- callbacks posted by host threads ---- */
/* Called only by the module owner at kernel polls: the generated boundary
   (gen_host64_imports.py) runs the posted jobs on dedicated EE fibers. */
void host_callbacks_drain(void);
/* ---- threads: a fire-and-forget job (online_menu.c) ---- */
int host_thread_detach(void (*fn)(void *), void *arg);  /* 1 if the thread started */

/* ---- the host process's memory regions (state_trace.c, opt-in: DHD_STATE_NORM=host) ---- */
/* Calls visit for each region [base, end) of the address space from 0x10000 up, with host_memory set
   for memory the host itself holds (not free, and not the program's own image). */
void host_memory_regions(void (*visit)(uintptr_t base, uintptr_t end, int host_memory));
/* The host executable's code section (dhd.exe's .text), for the state trace's code pointers. */
void host_code_range(uintptr_t *lo, uintptr_t *hi);

/* ---- diagnostics: the race stall heartbeat (autopad.c) ---- */
void host_heartbeat_start(void (*beat)(void));  /* once: a host thread calls beat() every 30 s of wall time */
uintptr_t host_heartbeat_eip(void);  /* where the thread that started the heartbeat is now (a host code address); 0 if unknown */

/* ---- callbacks into the module, guest-safe (finding 082, W7g) ----
   The module calls these, never the five function-pointer services above; the generated
   boundary (gen_host64_imports.py) implements them. fn is a module function as a 32-bit value
   (HOST_CALLBACK: its table index in wasm) and ctx a guest value that the host passes back
   unchanged as fn's first argument. The host runs
   fn only on the module's own thread: on a new fiber, at a kernel poll (posted jobs and the
   heartbeat on 64-bit hosts), or inside the faulting access (the guard). */
#define HOST_CALLBACK(fn) ((uint32_t)(uintptr_t)(fn))
HostFiber host_callback_fiber(uint32_t fn, uint32_t ctx);          /* void fn(ctx) */
int host_callback_detach(uint32_t fn, uint32_t ctx);               /* void fn(ctx); 1 if started */
int host_callback_fault_guard(uint32_t fn, uint32_t ctx);          /* int fn(ctx, address) */
void host_callback_memory_regions(uint32_t fn, uint32_t ctx);      /* void fn(ctx, base, end, host_memory) */
void host_callback_heartbeat(uint32_t fn, uint32_t ctx);           /* void fn(ctx) */
/* host_stack_ranges as guest data: up to max [low, high) pairs of 32-bit addresses at out. */
int host_stack_ranges_copy(uint32_t out, int max);
/* gs/capture.c's gs_capture_request (DHD_RACE_GSDUMP), the name a guest string. */
void host_service_gs_capture_request(uint32_t name);

/* ---- sound output ---- */
/* The mixer's output format: 48000 Hz, stereo, 16-bit PCM, interleaved. */
#define HOST_SOUND_RATE 48000

/* Opens the output device once, from the game thread at the first sound tick. Nothing happens
   with DHD_SND_OFF set; the mixer runs either way. */
void host_sound_open(void);
/* Hands over `frames` rendered stereo frames, in game order. Frames that don't fit in the host's
   queue are dropped and counted (the mixer's state has already advanced). */
void host_sound_write(const int16_t *stereo, int frames);
/* Forgets everything queued (test fixtures only). */
void host_sound_reset(void);
/* Wall-clock diagnostics: one call per sound tick. */
void host_sound_tick(void);
/* Logs the calling stack (DHD_SND_TRACE, unresolved voice plays). */
void host_sound_log_stack(void);
/* DHD_BENCH window: start (0) and end (1); prints the output counters at the end. */
void snd_host_bench(int end);
/* ---- end sound output ---- */

/* Explicit device/process services. Addresses name guest linear memory. */
void host_service_host_init(void);
void host_service_host_console_init(void);
int host_service_host_disc_ready(void);
int host_service_host_poll_dump(void);
void host_service_host_settings_load(void);
int host_service_host_setting_save(uint32_t a0, uint32_t a1);
int host_service_host_setting_locked(uint32_t a0);
void host_service_host_pad_device(int a0, uint32_t a1);
void host_service_host_pad_vibrate(int a0, int a1, int a2);
void host_service_graphics_feed(int a0, uint32_t a1, uint32_t a2);
void host_service_graphics_wait(void);
int host_service_graphics_threaded(void);  /* 2: threaded, with deferred vsyncs */
/* Deferred vsync (kernel.c): queues this VBLANK once the last field's picture is handed on and
   makes the CSR page provisional; the next host_service_graphics_wait puts the exact CSR there. */
void host_service_graphics_vblank(void);
void host_service_graphics_shutdown(void);
int host_service_movie_playing(void);
void host_service_movie_audio(uint32_t a0, int a1);
int host_service_launcher_open(void);
void host_service_update_request(void);
void host_service_vif_feed(int a0, uint32_t a1, uint32_t a2);
void host_service_gs_vsync(void);
void host_service_gs_pump_messages(void);
void host_service_gs_window_title(uint32_t a0);
void host_service_gs_native_view(int view);  /* gs/present.c's gs_native_view (online.c, each field) */
void host_service_gs_window_apply(void);
void host_service_gs_present_options_apply(void);
void host_service_gs_window_close(void);
/* The resolution scale the pictures are drawn at (asked 0), or the one asked for (asked 1:
   DHD_SCALE's, or DHD_SCALE=window's match before the device's limit); 0 before the first
   picture and without the native renderer (gs/gpu_rust_native.inc; PC SETTINGS shows it). */
int host_service_gs_scale(int asked);
/* 1 when this build offers online play: the one switch, the host's, so the core is the same on
   every platform (finding 143). Desktop builds: 1. Android (port/net/net.c NET_OFFERED, at compile
   time: not until phones are fast enough and the touch controls are ready) and the browser
   (port/web/www/net.js: its CSP blocks the race server's socket): 0, and no setting turns it on.
   At 0 the main menu has no ONLINE row, DHD_NET_* starts no online race, and the connections
   below refuse. */
int host_service_net_offered(void);
void host_service_net_init(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
int host_service_net_create_room(void);
int host_service_net_join_room(uint32_t a0);
void host_service_net_leave(void);
void host_service_net_get_lobby(uint32_t a0);
void host_service_net_pick(int a0, int a1, int a2);
void host_service_net_finish(uint32_t ticks, int place);
void host_service_net_back(void);
void host_service_net_rules(int finish_window, int next_race, int then);
void host_service_net_rotation(uint32_t address, int count);
void host_service_net_end_race(void);
void host_service_net_set_ready(int a0);
void host_service_net_start_race(void);
int host_service_net_racing(void);
double host_service_net_seconds(void);
double host_service_net_server_ms(void);
int host_service_net_send_frame(int a0, uint32_t a1, uint32_t a2);
int host_service_net_receive_frame(uint32_t a0, uint32_t a1);
int host_service_net_list_lobbies(uint32_t a0, int a1);

/* ---- async host services: blocking work on a host thread (online_menu.c) ----
   Only results cross to the game thread, as guest-address buffers. The module
   starts a job, then polls it from its page update until it is done. One job
   of each kind runs at a time; starting while one runs returns 0. */
/* The disc image's online check (dhd_disc.h): the image's and iso-check.txt's paths (guest
   addresses). 1 if the check began. */
int host_service_disc_check_start(uint32_t iso, uint32_t cache);
/* 0 working; else dhd_disc_check's answer: 1 hashed now, 2 from iso-check.txt (the game files hash
   in digest, 32 bytes), -1 unreadable, -2 not the USA disc's files; note (size bytes) what the log
   says. -1 when no check ran. */
int host_service_disc_check_poll(uint32_t digest, uint32_t note, int size);
int host_service_net_list_lobbies_start(int max);      /* 1 if the fetch began */
int host_service_net_list_lobbies_poll(uint32_t out, int max);  /* the lobby count, -1 failed, -2 working */

unsigned host_service_gs_bits_per_pixel(unsigned a0);
int host_service_gs_reads_pending(void);
void host_service_gs_settle_reads(void);
void host_service_gs_reset(void);
void host_service_gs_vblank(void);
uint64_t host_service_gs_priv_read(unsigned a0);
void host_service_gs_priv_write(unsigned a0, uint64_t a1);
uint32_t host_service_gs_read_image(uint32_t a0, uint32_t a1);
uint32_t host_service_gs_defer_read_image(uint32_t a0, uint32_t a1);
/* sceGsExecStoreImage queued behind the drawing (graphics_queue.c, finding 148): its 28-word
   packet, the read to destination and the unmask. 1 if queued, 0 if nothing was and the caller
   must wait and read; settle writes the queued reads' bytes. */
uint32_t host_service_gs_store_image(uint32_t packet, uint32_t destination, uint32_t bytes);
void host_service_gs_store_settle(void);
uint64_t host_service_gs_register(unsigned reg);
void host_service_net_set_room(uint32_t address);
uint64_t host_service_net_hash_ranges(uint32_t address, int count);

/* Graphics worker -> module mailbox. Only consume after graphics_wait. Packet
   output is {u32 path,u32 word_count,u32 words[word_count]}; snapshot is the
   fixed-width Gs prefix before native callbacks, with the caller's exact size. */
void host_service_gs_trace_begin(void);
uint32_t host_service_gs_trace_size(void);
void host_service_gs_trace_pop(uint32_t address);
void host_service_gs_snapshot(uint32_t address, uint32_t bytes);
void host_console_write(uint32_t address, uint32_t bytes);

/* Wall-clock presentation state stays native. These contain no guest pointers. */
void host_pacing_config(double period, uint64_t field);
void host_pacing_wait(void);
void host_pacing_handoff(int fresh);
void host_pacing_input(void);

void host_frame_context(uint64_t field, double seconds);

/* Last safely published EE registers for asynchronous native crash reports. */
void host_frame_registers(uint32_t address);
/* The module computes race capture names on its owner stack before presentation. */
void host_capture_name(uint32_t address, int ready);

double host_pacing_period(void);

/* Fixed-width VU1 register snapshot (2096 bytes) and its 64-qword VU0 view.
   Consume only after the graphics fence. No native pointers are copied. */
int host_vu1_snapshot(uint32_t address);
void host_vu1_file(uint32_t address);

/* Once a vsync, from the core: widescreen's shape and card limit (host_widescreen_aspect,
   host_widescreen_flat_limit). Returns 1 when the next field's 2D is to be drawn for a card,
   unmapped (widescreen.c's card mode, finding 140). */
int host_frame_video(double aspect, int flat_limit);

void host_service_host_backtrace(void);
int host_service_host_game_options_read(uint32_t a0, uint32_t a1);
int host_service_host_skip_movies(void);
int host_service_texpack_available(void);

/* ---- the mouse, and touch later (menu_page.c, finding 092) ----
   Where the pointer is over the game's picture, in its own units: x and y are fractions of the
   picture as the player sees it (65536 = its whole width or height), whatever the window's size,
   the bars beside the picture or the canvas's scale; outside the picture they are below 0 or
   above 65536. The counters only grow (the module compares them with the last ones it saw), so a
   click between two fields is never lost: moves, presses of the left, right and middle buttons,
   wheel steps (away from the player positive). buttons: those held now (1 left, 2 right, 4
   middle). seen: 1 once a mouse or a touch was used. A host without a mouse leaves it all 0. */
typedef struct {
    int32_t x, y;
    uint32_t buttons, seen, moves;
    uint32_t presses[3];
    int32_t wheel;
} HostMouse;  /* 36 bytes */
void host_service_mouse_state(uint32_t out);

void host_service_text_input_begin(void);
void host_service_text_input_end(void);
int host_service_text_input_edit(uint32_t a0, int a1, uint32_t a2);
int host_service_clipboard_set(uint32_t text);  /* 1 when the text is on the clipboard */

/* ---- keyboard and controller bindings (port/hle/controls_settings.c, finding 157) ----
   device: 0 the keyboard, 1 Controller 1, 2 Controller 2 (controls_table.h's CONTROL_*). */
int host_service_controls_layout(int device, uint32_t out);  /* the layout played with into out (52 bytes); 0: none */
int host_service_controls_pad(int player);  /* player 1's or 2's controller: its PAD_KIND_* + 1, 0 none */
/* A change (CONTROL_ASSIGN ...) by dhd_settings' rules, saved and played with at once: CONTROL_REFUSED,
   _UNCHANGED or _CHANGED; what the page says into message (size bytes). */
int host_service_controls_change(int device, int edit, int target, int slot, int input, uint32_t message, int size);
/* The wait for a key or button: command 1 begins, 0 asks (the input pressed since, -2 Esc, -1
   nothing yet), 2: 1 while anything is still held. */
int host_service_controls_capture(int device, int command);
/* A controller's state (hle.h GamepadRaw, 16 bytes) through player's layout into a HostPad (tests). */
void host_service_controls_map(int player, uint32_t raw, uint32_t out);
void host_service_movie_start(uint32_t name, uint32_t lsn, uint32_t size, int audio, int found);
int host_service_movie_update(int keep);

void host_service_movie_prepare(void);

/* Discord Rich Presence (port/render/presence, finding 106): what the player is doing, sent by the
   core (port/hle/presence.c) when it changes. kind: PRESENCE_*; a race's track as the game names
   it, the player's place (1 first, 0 not known) and the riders; an online room's join code (empty
   for a private room: its code never leaves the core), its players and size. Strings
   NUL-terminated. The host shows it on Discord when the discord setting is on (the browser and
   Android: nothing). */
enum { PRESENCE_NONE, PRESENCE_MENUS, PRESENCE_RACE, PRESENCE_CAREER, PRESENCE_ROOM, PRESENCE_ONLINE_RACE,
       PRESENCE_MOVIE };
typedef struct {
    int32_t kind, place, riders, members, max_members;
    char track[24], room[8];
} HostPresence;  /* 52 bytes */
void host_service_presence(uint32_t state);

/* The touch controls (finding 141; port/host/touch_host.c, port/hle/touch.c): the core's side
   (pad.c) once a field. In: what the overlay reads from the game (the race HUD hook, the rider's
   air flag) and the clocks; a TOUCH_COMMAND_* from the TOUCH CONTROLS pages. Out: where the HUD
   goes (0 the game's place, 1 moved for the right hand, 2 for the left), the online panel's Leave
   race, and the trick-stick hint's hold (the core waits while it is set). A host without touch
   leaves the outputs as they are (0). */
enum { TOUCH_COMMAND_NONE, TOUCH_COMMAND_SETTINGS, TOUCH_COMMAND_EDIT, TOUCH_COMMAND_HINTS, TOUCH_COMMAND_TILT_CENTRE };
typedef struct {
    double race_ms;        /* race time, ms (1/60 s fields; stops while paused) */
    int32_t race_hud;      /* the race HUD was drawn since the last call, on the race screen */
    int32_t air;           /* player 1's rider is in the air */
    int32_t online;        /* an online race */
    int32_t clock_moving;  /* the race clock advanced since the last call */
    int32_t pause_row[3];  /* the offline pause menu's rows of RESUME, RE-START and QUIT (-1: none) */
    int32_t fps;           /* VBLANKs a second */
    uint32_t vsync;        /* VBLANKs so far */
    int32_t command;       /* TOUCH_COMMAND_* */
    int32_t hud_mode;      /* out */
    int32_t leave_race;    /* out */
    int32_t freeze;        /* out */
    int32_t reserved;
} HostTouch;  /* 64 bytes */
void host_service_touch_field(uint32_t state);

#endif
