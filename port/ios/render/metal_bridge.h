/* metal_bridge.h - iOS Metal Surface & ProMotion 120Hz Synchronization
   GPL-3.0-or-later. */
#ifndef DHD_IOS_METAL_BRIDGE_H
#define DHD_IOS_METAL_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize iOS Metal layer configuration */
void ios_metal_configure_layer(void *layer_ptr);

/* Configure ProMotion target frame rate (60 Hz or 120 Hz), from dhd.ini's fps at the start */
void ios_metal_set_target_fps(int fps);
/* Final presentation cap (short side, pixels; 0: the screen's own), separate from the game's scale. */
void ios_metal_set_output_limit(int pixels);
void ios_metal_set_game_active(bool active);
/* The picture's shape: 0 the screen's (widescreen on), 4/3 (widescreen off), -1 stretched. */
void ios_metal_set_phone_aspect(double aspect);
double ios_metal_game_aspect(void);
double ios_metal_display_hz(void);
int ios_metal_effective_fps(void);

/* Query whether ProMotion 120Hz is supported on this hardware */
bool ios_metal_has_promotion_support(void);

/* Shared CAMetalLayer and pixel size for port/gs/present.c */
void *ios_metal_get_layer(void);
void ios_metal_get_drawable_size(int *out_w, int *out_h);
int ios_metal_consume_resized(void);
int ios_metal_consume_layer_changed(void);

/* For the shared touch controls (port/host/touch_host.c, finding 224): the view in mm, the strips
   to keep clear and the cutout's band (mm); the Taptic Engine; the system keyboard for a game field. */
int ios_touch_screen(float *width_mm, float *height_mm, float strips[4], float cut[4]);
void ios_touch_buzz(int kind);
void ios_text_input(int on);
/* Tilt steering (finding 226): CoreMotion's gravity as the roll Android's touchTilt gives
   (degrees, turned right positive, 0 held level in landscape); on: 1 with a sensor, 0 without. */
int ios_touch_tilt_on(int on);
int ios_touch_tilt(float *degrees);

#ifdef __OBJC__
@class UIViewController;
UIViewController *ios_metal_create_view_controller(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DHD_IOS_METAL_BRIDGE_H */
