/* metal_bridge.h - iOS Metal Surface & ProMotion 120Hz Synchronization
   GPL-3.0-or-later. */
#ifndef DHD_IOS_METAL_BRIDGE_H
#define DHD_IOS_METAL_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize iOS Metal layer configuration for 2868x1320 OLED display */
void ios_metal_configure_layer(void *layer_ptr);

/* Configure ProMotion target frame rate (60 Hz or 120 Hz) */
void ios_metal_set_target_fps(int fps);
/* Final presentation cap (short side), separate from the game's internal scale. */
void ios_metal_set_output_limit(int pixels);
void ios_metal_set_game_active(bool active);
void ios_metal_set_phone_aspect(double aspect);
double ios_metal_game_aspect(void);
double ios_metal_display_hz(void);
int ios_metal_effective_fps(void);
void ios_metal_set_render_preferences(int scale, bool point_filter);
int ios_metal_render_scale(void);
bool ios_metal_point_filter(void);

/* Query whether ProMotion 120Hz is supported on this hardware */
bool ios_metal_has_promotion_support(void);

/* Presented-frame submission intervals, not GPU timestamps; zero before samples. */
void ios_metal_get_timing(double *out_avg_fps, double *out_p99_ms);

/* Shared CAMetalLayer and pixel size for port/gs/present.c */
void *ios_metal_get_layer(void);
void ios_metal_get_drawable_size(int *out_w, int *out_h);
int ios_metal_consume_resized(void);
int ios_metal_consume_layer_changed(void);

#ifdef __OBJC__
@class UIViewController;
UIViewController *ios_metal_create_view_controller(void);
UIViewController *ios_metal_create_display_registration_controller(void);
void ios_metal_attach_external_display(UIViewController *host);
void ios_metal_detach_external_display(UIViewController *host);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DHD_IOS_METAL_BRIDGE_H */

