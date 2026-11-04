/* touch_input.h - Native iOS Touchscreen Virtual Controls for Downhill Revived
   Provides multi-touch virtual analog stick and PlayStation button layout
   adapted for the iPhone 17 Pro Max display (2868 x 1320, landscape, safe-area aware).
   GPL-3.0-or-later. */
#ifndef DHD_IOS_TOUCH_INPUT_H
#define DHD_IOS_TOUCH_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration / definition of HostPad */
#ifndef HLE_H
typedef struct HostPad {
    uint16_t buttons;
    uint8_t right_x, right_y, left_x, left_y;
    uint8_t pressure[12];
    uint8_t connected;
} HostPad;
#define HLE_H_HOSTPAD_DEFINED 1
#endif

/* Virtual Control IDs */
typedef enum {
    TOUCH_CTRL_STICK = 0,
    TOUCH_CTRL_CROSS,       /* Pedal / Confirm (Double-tap: Sprint) */
    TOUCH_CTRL_CIRCLE,      /* Kick */
    TOUCH_CTRL_SQUARE,      /* Punch */
    TOUCH_CTRL_TRIANGLE,    /* Brake / Back */
    TOUCH_CTRL_L1,          /* Tricks */
    TOUCH_CTRL_R1,          /* Tricks */
    TOUCH_CTRL_L2,          /* Extra Tricks */
    TOUCH_CTRL_R2,          /* Extra Tricks */
    TOUCH_CTRL_START,       /* Start / Pause */
    TOUCH_CTRL_SELECT,      /* Select */
    TOUCH_CTRL_L3,
    TOUCH_CTRL_R3,
    TOUCH_CTRL_COUNT
} TouchControlID;

/* Normalized Control Geometry (0.0 to 1.0 relative to safe area / screen) */
typedef struct {
    float x;           /* Center X (0.0 = left, 1.0 = right) */
    float y;           /* Center Y (0.0 = top, 1.0 = bottom) */
    float radius;      /* Hit radius in points */
    float scale;       /* Visual scale factor (1.0 = default) */
    bool visible;
} TouchControlLayout;

/* Touch Layout Configuration */
typedef struct {
    float opacity;              /* Global controls opacity: 0.1 to 1.0 (default 0.5) */
    float stick_deadzone;       /* Analog deadzone: 0.0 to 0.4 (default 0.15) */
    float stick_sensitivity;    /* Stick sensitivity: 0.5 to 2.0 (default 1.0) */
    bool stick_floating;        /* True: stick centers where finger touches down */
    bool auto_hide_on_gamepad;  /* True: hide touch controls if physical pad connected */
    bool haptics_enabled;       /* True: trigger UIImpactFeedbackGenerator on press */
    TouchControlLayout controls[TOUCH_CTRL_COUNT];
} TouchConfig;

/* Initialize touch input subsystem with screen dimensions and safe-area insets */
void touch_input_init(float screen_w, float screen_h, float safe_left, float safe_right, float safe_top, float safe_bottom);

/* Update screen dimensions and safe area insets on rotation / resize */
void touch_input_update_viewport(float screen_w, float screen_h, float safe_left, float safe_right, float safe_top, float safe_bottom);

/* Reset all virtual controls to released state (used on app background / focus lost) */
void touch_input_reset(void);

/* Touch event handlers (called from UIKit / SwiftUI / SDL event pumps) */
void touch_input_begin(intptr_t touch_id, float x, float y);
void touch_input_move(intptr_t touch_id, float x, float y);
void touch_input_end(intptr_t touch_id, float x, float y);
void touch_input_cancel(intptr_t touch_id);

/* Merge touch controls state into HostPad (called at each field/frame in host_pad_device) */
void touch_pad_merge(HostPad *out);

/* Query whether a virtual button is currently pressed */
bool touch_input_is_pressed(TouchControlID ctrl_id);

/* Directly set or pulse a virtual control button state (e.g. from SwiftUI deck or pause menu) */
void touch_input_set_button(TouchControlID ctrl_id, bool pressed);
void touch_input_set_stick(float dx, float dy);
void touch_input_get_stick(float *out_dx, float *out_dy);

/* Configuration and layout persistence */
const TouchConfig *touch_input_get_config(void);
void touch_input_set_config(const TouchConfig *cfg);
void touch_input_restore_defaults(void);
bool touch_input_load_saved_layout(void);
bool touch_input_save_layout(void);

/* Physical gamepad presence callback */
void touch_input_set_gamepad_connected(bool connected);
bool touch_input_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* DHD_IOS_TOUCH_INPUT_H */

