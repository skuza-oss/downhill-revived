/* The PS2 system on the PC: the EE kernel's services and the IOP's, written natively (finding
   020). The game's SDK calls them through their retail entry points (func_XXXXXXXX), which these
   files define under readable names with an asm label. */
#ifndef HLE_H
#define HLE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "memory.h"
#include "../include/guest.h"
#include "../host/host_api.h"  /* the OS services: files, time, fibers, memory */

/* Defines a C function under the retail name of the SDK function it replaces. */
#define RETAIL(name) __asm__(#name)

/* Logs to stderr; DHD_LOG set in the environment turns on the verbose messages. */
void hle_log(const char *fmt, ...);
extern int hle_verbose;
/* DHD_GRAPHICS_ORDERED (the browser's threaded build sets it, port/web/www/worker.js; the native
   hosts' deferred vsync implies it, kernel.c): the graphics workers run every queued call in the
   order it was made (port/web/web_queue.h, port/gs/present.c's picture), so the waits that
   only put earlier graphics work before a later queued call are skipped (kernel.c's vsync after the
   VBLANK handlers, sceGsSyncPath). Every read of graphics state still waits: the GS calls that
   return a value, and ps2_graphics_sync before the EE touches mapped VU1/VIF1/GIF/GS state. */
extern int graphics_ordered;

/* getenv for a variable read every field (pads, the vsync). getenv crosses into the host, which
   scans the whole environment: these read it once, and again only after the core changed its
   environment (hle_env_changed, after every host_putenv of the core: online.c's set_env,
   online_menu.c's save, pc_settings.c and widescreen.c).
   The host's own writers (present.c's window size, host.c's settings before the core starts)
   set none of the variables read through this. */
extern unsigned hle_env_epoch;
static inline void hle_env_changed(void) { hle_env_epoch++; }
typedef struct { const char *value; unsigned epoch; } HleEnv;  /* epoch: hle_env_epoch + 1 when read */
static inline const char *hle_env(HleEnv *cache, const char *name)
{
    if (cache->epoch != hle_env_epoch + 1) {
        cache->value = getenv(name);
        cache->epoch = hle_env_epoch + 1;
    }
    return cache->value;
}

/* ---- host.c ---- */
void host_init(void);         /* the crash report */
void host_thread_name(const char *name); /* names the calling OS thread for bench/profile reports */
unsigned host_thread_cpu(const char **names, double *seconds, unsigned max); /* CPU time of each */
void host_poll_dump(void);   /* optional watchdog request at a kernel boundary */
void host_backtrace(void);    /* the same report, here */
void host_settings_load(void); /* dhd.ini next to the exe into DHD_* variables (host.c) */
int host_disc_ready(void);     /* the disc image checked (the launcher chooses it); nonzero: none usable, end */
/* launcher_link.c, the game's side of the launcher (finding 093) */
int launcher_open(void);      /* first thing: started straight, the launcher opens instead; nonzero: end */
void update_request(void);    /* the online menu's Update Now: ends the game; the launcher updates */

/* ---- game_data.c: the game's data from the player's disc ---- */
/* NULL if the disc image at path holds the retail SCUS_971.77 (checked by SHA-1), then in *elf
   (if elf isn't NULL) a malloc'd copy of it; else why not, in words for the player. */
const char *game_data_read(const char *path, uint8_t **elf);
int game_data_load(void);      /* the data image into PS2 RAM from DHD_ISO's disc; 0: can't */
void host_console_init(void);  /* the log's destination without a console window (host.c) */
int host_setting_save(const char *key, const char *value);  /* one dhd.ini line, others kept */
int host_setting_locked(const char *name);  /* the environment sets this DHD_* variable */
void host_pad_vibrate(int port, int light, int heavy);  /* the DualShock 2's motors to the PC pad */
void host_widescreen_init(void);  /* DHD_WIDESCREEN: Hor+ field of view, any aspect (widescreen.c) */
void host_widescreen_vsync(void); /* its live-memory part, once a field */
double host_widescreen_aspect(void);  /* the widescreen picture's width / height; 0 when off */
void widescreen_card_mode(int on);   /* the next field's 2D is for a card: unmapped (widescreen.c) */
void host_display_aware(void);    /* the process sees real pixels (DPI aware), before any window */
double host_seconds(void);    /* the game clock in seconds: virtual under DHD_FRAME_CLOCK (kernel.c) */
/* Just-in-time frames (kernel.c): the picture handed on at this VBLANK, when to show it (0: at
   once) and when its pad was read; the presenter calls pacing_present just before it presents. */
void pacing_handed(double *due, double *began);
void pacing_present(double due, double began);
enum { PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008, PAD_UP = 0x0010,
       PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080, PAD_L2 = 0x0100, PAD_R2 = 0x0200,
       PAD_L1 = 0x0400, PAD_R1 = 0x0800, PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000,
       PAD_SQUARE = 0x8000 };
int autopad_read(const char *script, double seconds, uint16_t *buttons);
int autopad_nav(const char *script, unsigned long long field, uint16_t *buttons);  /* DHD_NAV */
void race_sync_vsync(unsigned long long field);  /* DHD_RACE_SYNC, see autopad.c */
int race_sync_pad(int player, uint16_t *buttons);
double race_sync_seconds(void);  /* race clock seconds after GO (DHD_RACE_SYNC), -1 before */
int race_sync_capture(const char *prefix, char *name, size_t size);

/* One controller's state, as a DualShock 2 reports it: buttons as libpad's bits (1 = pressed,
   0x0001 SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT, L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS,
   0x8000 SQUARE), sticks 0..255 (128 centred, down/right positive), pressures 0..255 in the
   order right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2. */
typedef struct {
    uint16_t buttons;
    uint8_t right_x, right_y, left_x, left_y;
    uint8_t pressure[12];
    uint8_t connected;  /* host_pad_device: a controller for that player (for player 1 also the keyboard) */
} HostPad;
void host_pad_read(int port, HostPad *out);  /* a script if one is set, else the controller */
int host_pad_script(int player, HostPad *out);  /* DHD_NAV / DHD_AUTOPAD / DHD_RACE_PAD; 0 if none */
void host_pad_device(int port, HostPad *out);   /* the controller, and for port 0 the keyboard too */

/* A keyboard or controller layout (port/hle/controls_table.h, finding 157): which key or
   controller input stands for each PS2 button. */
struct ControlsLayout;

/* keyboard.c: keys (Windows virtual-key codes, down[key] != 0) as a pad by the layout (NULL: the
   default one), merged into `out`; `riding`: the game's riding controls are on (D_0029E290), the
   d-pad's keys move the left stick. */
void keyboard_pad(const struct ControlsLayout *layout, const uint8_t down[256], int riding, double now, HostPad *out);
/* The keys it reads with the layout (NULL: the default one) into list (49 at most): their count. */
int keyboard_keys(const struct ControlsLayout *layout, uint8_t *list);
void keyboard_reset(void);
int keyboard_game_axis(int raw);           /* the game's reading of a stick byte (-127..127) */
int keyboard_script(const char *script, double seconds, uint8_t down[256]);  /* DHD_KEYS */
/* The keyboard's state, fed by the SDL thread (sdl_host.c): a key by its place (position: the
   Windows virtual-key code of the US key, letters by place) and by the character it types on the
   current layout (typed). Focus lost lets every key go up. */
void keyboard_key(unsigned position, unsigned typed, int down);
unsigned keyboard_typed_of(unsigned position);  /* what that key types on the current layout */
int keyboard_position_down(unsigned position);
int keyboard_is_down(unsigned typed);           /* a key that types `typed` is held */
void keyboard_focus(int focused);               /* the game's window has the keyboard */
int keyboard_focused(void);
enum { VKEY_SHIFT = 0x10, VKEY_CONTROL = 0x11, VKEY_MENU = 0x12, VKEY_LWIN = 0x5B, VKEY_RWIN = 0x5C };

/* gamepad.c: controllers through SDL3 (XInput, DirectInput, HID: DualSense, DualShock 4, Switch
   Pro and the generic pads in SDL's mapping database). gamepad_start: 1 if SDL is running. The
   pads are read on the SDL thread (sdl_host.c: the main thread), which calls gamepad_thread_*.
   A controller's state by its inputs' places (GamepadRaw: a bit per input id of
   controls_table.h, the triggers' bytes 0-255, SDL's stick axes: left X, Y, right X, Y), which
   gamepad_apply turns into the PS2 pad's with a layout and the sticks' dead zone (stick.h). */
typedef struct {
    uint32_t buttons;
    uint8_t trigger[2];
    int16_t axis[4];
} GamepadRaw;  /* 16 bytes */
int gamepad_start(void);
int gamepad_raw(int player, GamepadRaw *out);  /* 0: no controller for that player */
void gamepad_apply(const struct ControlsLayout *layout, const GamepadRaw *raw, int dead_zone, HostPad *out);
void gamepad_vibrate(int player, int light, int heavy);
/* What a controller's buttons are labelled: for the button icons (port/gs/button_icons.c). */
enum { PAD_KIND_NONE = 0, PAD_KIND_PLAYSTATION = 1, PAD_KIND_XBOX = 2, PAD_KIND_NINTENDO = 3 };
int gamepad_kind_of(int type, int south_label);  /* PAD_KIND_* of an SDL_GamepadType and its bottom button's label */
int gamepad_kind(int player);  /* that player's controller's PAD_KIND_* (NONE without one) */
enum { PADDEV_ADDED = 1, PADDEV_REMOVED, PADDEV_REMAPPED };
/* port/host/touch_host.c: the touch controls (finding 141). The SDL thread passes its events
   (1: the event is the touch controls', not passed on) and calls the tick every pass; host_pad_device
   merges player 1's touches. */
union SDL_Event;
int touch_host_event(const union SDL_Event *e);
void touch_host_tick(void);
void touch_host_merge(HostPad *out);
/* sdl_host.c's SDL thread: the pads' start (1 if up), a device change, and every pass. */
int gamepad_thread_start(void);
void gamepad_device(int change, unsigned id);
void gamepad_thread_update(void);
/* sdl_host.c: the SDL loop on the main thread, the game on a thread of its own (launcher.c's
   main; returns the game's code); the controllers' start (once; 1 when SDL's events are up); a
   function run on the main thread, waited for (the window's calls). See sdl_host.c. */
int sdl_host_main(int (*game)(void *), void *arg);
int sdl_host_start(void);
int sdl_host_run(void (*fn)(void *), void *arg);

/* ---- kernel.c ---- */
enum { INTC_GS = 0, INTC_VBLANK_START = 2, INTC_VBLANK_END = 3, INTC_TIMER0 = 9 };

void kernel_init(void);
void kernel_dump(void);
/* Creates the main thread (SetupThread) and runs the threads; returns when the game exits. */
void kernel_run(void (*root)(void), uint32_t gp, uint32_t stack, uint32_t stack_size);
/* Delivers the interrupts that are due (vsync, alarms) and switches thread if one of higher
   priority became ready. Called at every kernel entry. */
void kernel_poll(void);
/* A store to INTC_STAT (0x1000F000): clears the causes whose bits are set in value. */
void kernel_intc_write(uint32_t value);
/* Runs the handlers of a DMAC channel (a transfer the hardware layer finished). */
void kernel_dmac_interrupt(int channel);

/* ---- dma.c ---- */
void vif_feed(int unit, const uint32_t *words, uint32_t n);  /* vif.c */
void dma_mmio_write(uintptr_t address, uint64_t value, unsigned size);
void dma_poll(void);  /* ends the DMA transfers the game started (see dma.c) */
void graphics_feed(int unit, uint32_t words, uint32_t count);  /* words: guest address */
void graphics_wait(void);
/* Writes deferred store-image reads (graphics.c) before host code passes PS2 RAM to the OS. */
void graphics_settle(void);
void graphics_shutdown(void);
/* VIF1/GIF run on the graphics worker unless DHD_MTVU=0 (graphics_queue.c). */
int graphics_threaded(void);
/* The host's side of the deferred vsync (graphics_queue.c; the game's side: kernel.c). */
int graphics_deferred(void);
void graphics_vblank(void);
void graphics_present(const void *payload, unsigned bytes);
extern void (*graphics_present_marked)(const void *payload);

/* ---- sif.c: RPC servers (one per IOP module server id) ---- */
typedef void (*RpcServer)(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_unknown(const char *server, int fno, void *send, int ssize, void *recv, int rsize);
int sif_poll(void);
double sif_next_event(void);
void sif_command(uint32_t cid, void *packet);  /* an IOP message to the EE's handler */

/* cdvd.c */
void rpc_cd_init(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_cd_scmd(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_cd_ncmd(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_cd_search(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_cd_diskready(int fno, void *send, int ssize, void *recv, int rsize);
int cdvd_find(const char *path, uint32_t *lsn, uint32_t *size);  /* a disc file by path */
int cdvd_read(uint32_t lsn, uint32_t count, void *dst);           /* 2048-byte sectors */

/* pad.c */
void rpc_pad(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_pad_ext(int fno, void *send, int ssize, void *recv, int rsize);
void pad_vsync(void);  /* refreshes the controllers' frames in EE memory */
/* The mouse in the game's popup menus (menu_page.c, finding 092): read once a field (pad_vsync),
   and the presses it makes added to player 1's pad (pads_read, pad_refresh). */
void menu_mouse_field(void);
void menu_mouse_merge(int player, HostPad *out);
void pad_refresh(void);  /* the newest frames' input read again (just-in-time frames, kernel.c) */
void presence_field(void);  /* presence.c: what the player is doing, for Discord (pad_vsync) */

/* ---- online.c: where the pads' input comes from ---- */
int pads_connected(void);   /* pads the game can open: DHD_PADS (1-4), 1 online, else 4 (pads_live) */
int pads_live(void);        /* played by hand: no DHD_PADS, script or DHD_NET_*; each pad then follows its controller */
int pads_multitap(void);    /* 3-4 pads: a multitap on port 0, player i in slot i */
void pads_begin_field(void);                /* each vsync, before the pads are read */
void pads_read(int player, HostPad *out);   /* player i's controller for this field */
/* touch_field.c: the touch controls' game side once a field (finding 141); the TOUCH CONTROLS
   pages' commands (TOUCH_COMMAND_*, host_api.h); widescreen.c's HUD hook state. */
void touch_field(void);
void touch_command(int command);
extern int touch_hud_place, touch_hud_seen;
int pads_online(void);      /* an online race: started with DHD_NET_* or from the ONLINE menu */
void hle_online_from_menu(void);            /* the ONLINE menu's room started its race */
int hle_online_race_riders(void);           /* riders in the online race (10 offline) */
void port_online_relock(void);              /* online_menu.c: the player's own unlocks again */
extern int hle_mc_hold;                     /* mc.c: card writes held (online, all unlocked) */
void hle_online_version(char *out, size_t size);  /* the build's identity for the lobby */
void kernel_pace_slip(double seconds);      /* kernel.c: the frame clock waited this long */
void kernel_pace_shared(int on);            /* kernel.c: online, fields keep to the shared race clock */

/* iop.c */
void *iop_ptr(uint32_t addr);  /* host pointer of an IOP memory address */
void rpc_fileio(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_iop_heap(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_loadfile(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_mc(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_mtap(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_mtap_open(int fno, void *send, int ssize, void *recv, int rsize);        /* 0x80000901 */
void rpc_mtap_connection(int fno, void *send, int ssize, void *recv, int rsize);  /* 0x80000903 */
void rpc_mtap_version(int fno, void *send, int ssize, void *recv, int rsize);     /* 0x80000905 */
void rpc_sound(int fno, void *send, int ssize, void *recv, int rsize);
void rpc_sound2(int fno, void *send, int ssize, void *recv, int rsize);
void sound_tick(void); /* sound.c: one frame of game time (finding 022) */

#endif
