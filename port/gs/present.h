#ifndef PORT_GS_PRESENT_H
#define PORT_GS_PRESENT_H
#include "gs.h"
void gs_vsync(void);
void gs_window_apply(void);           /* DHD_WINDOW / _WIDTH / _HEIGHT changed: apply now (EE thread) */
void gs_present_options_apply(void);  /* DHD_PRESENT_FILTER / DHD_ASPECT changed */
extern volatile uint32_t gs_presented_frames;
/* Online races (port/hle/online.c): the window's title shows the lobby (UTF-8; the main thread
   sets it at its next pass), and a wait for the other players keeps the window responsive. */
void gs_window_title(const char *utf8);
void gs_pump_messages(void);
void gs_window_close(void);  /* as the window's close button: the game ends at the next field */
void gs_window_event(unsigned type);  /* the SDL thread's window events (sdl_host.c) */
/* The mouse (finding 092): the SDL thread's mouse events (an SDL_Event *); each pass of its loop
   the window's title and cursor (shown while the mouse moves, hidden when idle or riding). */
void gs_mouse_event(const void *event);
void gs_window_tick(void);
/* Where the renderer last showed the picture in its w x h window: [left, right) x [top, bottom). */
void gs_picture_rect(int left, int top, int right, int bottom, int w, int h);
/* That rect again with the window's pixel size, for a host that maps its own input into the
   picture (touch.c, finding 124). 0 when the renderer hasn't reported one yet. */
int gs_picture_rect_get(int *left, int *top, int *right, int *bottom, int *w, int *h);
/* The native renderer shows player gs_native_view's quarter of a 3-4 player split screen,
   scaled to the window (1-4; 0: the whole picture). online.c sets it during races. */
extern volatile int gs_native_view;
/* Decodes the selected display circuit into RGBA8; returns zero while disabled. */
int gs_display(Gs *g, uint32_t *pixels, unsigned *width, unsigned *height);
#endif
