/* touch_input.m - Implementation of Native iOS Touchscreen Controls
   for Downhill Revived. Supports multi-touch, floating/fixed stick,
   PlayStation face buttons, triggers, haptics, and layout persistence.
   GPL-3.0-or-later. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include "touch_input.h"
#include "stick_math.h"
#ifndef PAD_CROSS
enum {
    PAD_SELECT   = 0x0001,
    PAD_L3       = 0x0002,
    PAD_R3       = 0x0004,
    PAD_START    = 0x0008,
    PAD_UP       = 0x0010,
    PAD_RIGHT    = 0x0020,
    PAD_DOWN     = 0x0040,
    PAD_LEFT     = 0x0080,
    PAD_L2       = 0x0100,
    PAD_R2       = 0x0200,
    PAD_L1       = 0x0400,
    PAD_R1       = 0x0800,
    PAD_TRIANGLE = 0x1000,
    PAD_CIRCLE   = 0x2000,
    PAD_CROSS    = 0x4000,
    PAD_SQUARE   = 0x8000
};
#endif
#include <math.h>
#include <stdio.h>
#include <os/lock.h>

#define MAX_ACTIVE_TOUCHES 16
#define DEFAULT_STICK_RADIUS 65.0f
#define DEFAULT_BUTTON_RADIUS 38.0f

typedef struct {
    intptr_t id;
    TouchControlID control;
    float start_x;
    float start_y;
    float cur_x;
    float cur_y;
    bool active;
} ActiveTouch;

static os_unfair_lock s_lock = OS_UNFAIR_LOCK_INIT;
static TouchConfig s_config;
static ActiveTouch s_touches[MAX_ACTIVE_TOUCHES];

static float s_screen_w = 2868.0f;
static float s_screen_h = 1320.0f;
static float s_safe_l = 62.0f;
static float s_safe_r = 62.0f;
static float s_safe_t = 0.0f;
static float s_safe_b = 21.0f;

/* Stick runtime deflection */
static float s_stick_dx = 0.0f;
static float s_stick_dy = 0.0f;
static float s_stick_floating_center_x = 0.0f;
static float s_stick_floating_center_y = 0.0f;
static bool  s_stick_has_floating_center = false;
static uint32_t s_direct_buttons = 0;
/* Keep a tap shorter than a game field until the next pad poll. */
static uint32_t s_pending_buttons = 0;
static int s_gamepad_deadzone = 5;
static bool s_gamepad_deadzone_pending = true;
static bool s_editing;
static char s_controller_layout[2][512];
static bool s_controller_layout_pending[2];
/* The engine's environment writer, used on the game thread as in REVIVED SETTINGS. */
void host_putenv_s(const char *name, const char *value);

/* Physical gamepad presence */
static bool s_gamepad_connected = false;

/* Haptics */
static UIImpactFeedbackGenerator *s_haptic_light = nil;
static UIImpactFeedbackGenerator *s_haptic_medium = nil;

static const uint16_t s_pressure_order[12] = {
    PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
    PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2
};

static void trigger_haptic(bool heavy) {
    if (!s_config.haptics_enabled) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (heavy) {
            if (!s_haptic_medium) s_haptic_medium = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
            [s_haptic_medium impactOccurred];
        } else {
            if (!s_haptic_light) s_haptic_light = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
            [s_haptic_light impactOccurred];
        }
    });
}

void touch_input_restore_defaults(void) {
    os_unfair_lock_lock(&s_lock);
    memset(&s_config, 0, sizeof s_config);
    s_config.opacity = 0.50f;
    s_config.stick_deadzone = 0.02f;
    s_config.stick_sensitivity = 1.0f;
    s_config.stick_floating = false;
    s_config.auto_hide_on_gamepad = true;
    s_config.haptics_enabled = true;

    /* Left Side: Analog stick */
    s_config.controls[TOUCH_CTRL_STICK] = (TouchControlLayout){
        .x = 0.14f, .y = 0.72f, .radius = DEFAULT_STICK_RADIUS, .scale = 1.0f, .visible = true
    };

    /* Left Side: L1 and L2 triggers */
    s_config.controls[TOUCH_CTRL_L1] = (TouchControlLayout){
        .x = 0.12f, .y = 0.32f, .radius = DEFAULT_BUTTON_RADIUS, .scale = 1.0f, .visible = true
    };
    s_config.controls[TOUCH_CTRL_L2] = (TouchControlLayout){
        .x = 0.12f, .y = 0.16f, .radius = DEFAULT_BUTTON_RADIUS, .scale = 1.0f, .visible = true
    };

    /* Right Side: Action diamond */
    /* Cross (Pedal / Confirm / Sprint) at bottom */
    s_config.controls[TOUCH_CTRL_CROSS] = (TouchControlLayout){
        .x = 0.86f, .y = 0.78f, .radius = DEFAULT_BUTTON_RADIUS + 4.0f, .scale = 1.1f, .visible = true
    };
    /* Square (Punch) at left of diamond */
    s_config.controls[TOUCH_CTRL_SQUARE] = (TouchControlLayout){
        .x = 0.78f, .y = 0.64f, .radius = DEFAULT_BUTTON_RADIUS, .scale = 1.0f, .visible = true
    };
    /* Circle (Kick) at right of diamond */
    s_config.controls[TOUCH_CTRL_CIRCLE] = (TouchControlLayout){
        .x = 0.94f, .y = 0.64f, .radius = DEFAULT_BUTTON_RADIUS, .scale = 1.0f, .visible = true
    };
    /* Triangle (Brake / Back) at top of diamond */
    s_config.controls[TOUCH_CTRL_TRIANGLE] = (TouchControlLayout){
        .x = 0.86f, .y = 0.50f, .radius = DEFAULT_BUTTON_RADIUS, .scale = 1.0f, .visible = true
    };

    /* Right Side: R1 and R2 triggers */
    s_config.controls[TOUCH_CTRL_R1] = (TouchControlLayout){
        .x = 0.88f, .y = 0.32f, .radius = DEFAULT_BUTTON_RADIUS, .scale = 1.0f, .visible = true
    };
    s_config.controls[TOUCH_CTRL_R2] = (TouchControlLayout){
        .x = 0.88f, .y = 0.16f, .radius = DEFAULT_BUTTON_RADIUS, .scale = 1.0f, .visible = true
    };

    /* Top Center: Start and Select (clear of Dynamic Island center) */
    s_config.controls[TOUCH_CTRL_START] = (TouchControlLayout){
        .x = 0.60f, .y = 0.10f, .radius = 30.0f, .scale = 0.9f, .visible = true
    };
    s_config.controls[TOUCH_CTRL_SELECT] = (TouchControlLayout){
        .x = 0.40f, .y = 0.10f, .radius = 30.0f, .scale = 0.9f, .visible = true
    };

    s_config.controls[TOUCH_CTRL_L3] = (TouchControlLayout){ .visible = false };
    s_config.controls[TOUCH_CTRL_R3] = (TouchControlLayout){ .visible = false };

    os_unfair_lock_unlock(&s_lock);
}

void touch_input_init(float screen_w, float screen_h, float safe_left, float safe_right, float safe_top, float safe_bottom) {
    touch_input_restore_defaults();
    touch_input_update_viewport(screen_w, screen_h, safe_left, safe_right, safe_top, safe_bottom);
    touch_input_load_saved_layout();
}

void touch_input_update_viewport(float screen_w, float screen_h, float safe_left, float safe_right, float safe_top, float safe_bottom) {
    os_unfair_lock_lock(&s_lock);
    s_screen_w = screen_w > 0 ? screen_w : 2868.0f;
    s_screen_h = screen_h > 0 ? screen_h : 1320.0f;
    s_safe_l = safe_left;
    s_safe_r = safe_right;
    s_safe_t = safe_top;
    s_safe_b = safe_bottom;
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_reset(void) {
    os_unfair_lock_lock(&s_lock);
    for (int i = 0; i < MAX_ACTIVE_TOUCHES; i++) {
        s_touches[i].active = false;
    }
    s_stick_dx = 0.0f;
    s_stick_dy = 0.0f;
    s_stick_has_floating_center = false;
    s_direct_buttons = 0;
    s_pending_buttons = 0;
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_set_button(TouchControlID ctrl_id, bool pressed) {
    if ((int)ctrl_id < 0 || ctrl_id >= TOUCH_CTRL_COUNT) return;
    os_unfair_lock_lock(&s_lock);
    if (s_editing && pressed) {
        os_unfair_lock_unlock(&s_lock);
        return;
    }
    if (pressed) {
        if (!(s_direct_buttons & (1u << (unsigned)ctrl_id)))
            s_pending_buttons |= (1u << (unsigned)ctrl_id);
        s_direct_buttons |= (1u << (unsigned)ctrl_id);

    } else {
        s_direct_buttons &= ~(1u << (unsigned)ctrl_id);
    }
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_set_stick(float dx, float dy) {
    os_unfair_lock_lock(&s_lock);
    touch_normalize_stick(s_editing ? 0 : dx, s_editing ? 0 : dy, s_config.stick_deadzone, s_config.stick_sensitivity,
                          &s_stick_dx, &s_stick_dy);
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_set_editing(bool editing) {
    os_unfair_lock_lock(&s_lock);
    s_editing = editing;
    memset(s_touches, 0, sizeof s_touches);
    s_stick_dx = s_stick_dy = 0;
    s_stick_has_floating_center = false;
    s_direct_buttons = s_pending_buttons = 0;
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_queue_controller_layout(int player, const char *value) {
    if (player < 1 || player > 2 || !value || strlen(value) >= sizeof s_controller_layout[0]) return;
    os_unfair_lock_lock(&s_lock);
    snprintf(s_controller_layout[player - 1], sizeof s_controller_layout[0], "%s", value);
    s_controller_layout_pending[player - 1] = true;
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_set_gamepad_deadzone(float deadzone) {
    int percent = (int)lroundf(touch_clamp(deadzone, 0, .95f, .05f) * 100);
    os_unfair_lock_lock(&s_lock);
    if (percent != s_gamepad_deadzone) {
        s_gamepad_deadzone = percent;
        s_gamepad_deadzone_pending = true;
    }
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_get_stick(float *out_dx, float *out_dy) {
    os_unfair_lock_lock(&s_lock);
    if (out_dx) *out_dx = s_stick_dx;
    if (out_dy) *out_dy = s_stick_dy;
    os_unfair_lock_unlock(&s_lock);
}

static void get_control_pixel_pos(TouchControlID id, float *out_x, float *out_y, float *out_r) {
    const TouchControlLayout *c = &s_config.controls[id];
    float usable_w = s_screen_w - (s_safe_l + s_safe_r);
    float usable_h = s_screen_h - (s_safe_t + s_safe_b);
    *out_x = s_safe_l + (c->x * usable_w);
    *out_y = s_safe_t + (c->y * usable_h);
    *out_r = c->radius * c->scale;
}

static TouchControlID hit_test_control(float x, float y) {
    /* If stick is floating and touch is in left half of screen below upper 25% */
    if (s_config.stick_floating && x < (s_screen_w * 0.45f) && y > (s_screen_h * 0.25f)) {
        return TOUCH_CTRL_STICK;
    }

    /* Check fixed stick first */
    float cx, cy, cr;
    get_control_pixel_pos(TOUCH_CTRL_STICK, &cx, &cy, &cr);
    float dstick = hypotf(x - cx, y - cy);
    if (dstick <= cr * 1.5f) {
        return TOUCH_CTRL_STICK;
    }

    /* Check action buttons */
    for (int i = 1; i < TOUCH_CTRL_COUNT; i++) {
        if (!s_config.controls[i].visible) continue;
        get_control_pixel_pos((TouchControlID)i, &cx, &cy, &cr);
        float dist = hypotf(x - cx, y - cy);
        if (dist <= cr) {
            return (TouchControlID)i;
        }
    }
    return (TouchControlID)-1;
}

void touch_input_begin(intptr_t touch_id, float x, float y) {
    os_unfair_lock_lock(&s_lock);
    int slot = -1;
    for (int i = 0; i < MAX_ACTIVE_TOUCHES; i++) {
        if (!s_touches[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        os_unfair_lock_unlock(&s_lock);
        return;
    }

    TouchControlID ctrl = hit_test_control(x, y);
    if ((int)ctrl >= 0) {
        s_touches[slot].id = touch_id;
        s_touches[slot].control = ctrl;
        s_touches[slot].start_x = x;
        s_touches[slot].start_y = y;
        s_touches[slot].cur_x = x;
        s_touches[slot].cur_y = y;
        s_touches[slot].active = true;

        if (ctrl == TOUCH_CTRL_STICK) {
            if (s_config.stick_floating) {
                s_stick_floating_center_x = x;
                s_stick_floating_center_y = y;
                s_stick_has_floating_center = true;
                s_stick_dx = 0.0f;
                s_stick_dy = 0.0f;
            } else {
                float cx, cy, cr;
                get_control_pixel_pos(TOUCH_CTRL_STICK, &cx, &cy, &cr);
                float dx = (x - cx) / (cr > 0 ? cr : 1.0f);
                float dy = (y - cy) / (cr > 0 ? cr : 1.0f);
                touch_normalize_stick(dx, dy, s_config.stick_deadzone, s_config.stick_sensitivity,
                                      &s_stick_dx, &s_stick_dy);
            }
        } else {
            /* Button press haptic */
            trigger_haptic(ctrl == TOUCH_CTRL_CROSS || ctrl == TOUCH_CTRL_R1 || ctrl == TOUCH_CTRL_L1);


        }
    }
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_move(intptr_t touch_id, float x, float y) {
    os_unfair_lock_lock(&s_lock);
    for (int i = 0; i < MAX_ACTIVE_TOUCHES; i++) {
        if (s_touches[i].active && s_touches[i].id == touch_id) {
            s_touches[i].cur_x = x;
            s_touches[i].cur_y = y;

            if (s_touches[i].control == TOUCH_CTRL_STICK) {
                float cx, cy, cr;
                if (s_config.stick_floating && s_stick_has_floating_center) {
                    cx = s_stick_floating_center_x;
                    cy = s_stick_floating_center_y;
                    cr = s_config.controls[TOUCH_CTRL_STICK].radius * s_config.controls[TOUCH_CTRL_STICK].scale;
                } else {
                    get_control_pixel_pos(TOUCH_CTRL_STICK, &cx, &cy, &cr);
                }
                float dx = (x - cx) / (cr > 0 ? cr : 1.0f);
                float dy = (y - cy) / (cr > 0 ? cr : 1.0f);
                touch_normalize_stick(dx, dy, s_config.stick_deadzone, s_config.stick_sensitivity,
                                      &s_stick_dx, &s_stick_dy);
            }
            break;
        }
    }
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_end(intptr_t touch_id, float x, float y) {
    (void)x; (void)y;
    os_unfair_lock_lock(&s_lock);
    for (int i = 0; i < MAX_ACTIVE_TOUCHES; i++) {
        if (s_touches[i].active && s_touches[i].id == touch_id) {
            if (s_touches[i].control == TOUCH_CTRL_STICK) {
                s_stick_dx = 0.0f;
                s_stick_dy = 0.0f;
                s_stick_has_floating_center = false;
            }
            s_touches[i].active = false;
            break;
        }
    }
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_cancel(intptr_t touch_id) {
    touch_input_end(touch_id, 0, 0);
}

bool touch_input_is_pressed(TouchControlID ctrl_id) {
    os_unfair_lock_lock(&s_lock);
    bool pressed = (s_direct_buttons & (1u << (unsigned)ctrl_id)) != 0;
    for (int i = 0; !pressed && i < MAX_ACTIVE_TOUCHES; i++) {
        if (s_touches[i].active && s_touches[i].control == ctrl_id) {
            pressed = true;
            break;
        }
    }
    os_unfair_lock_unlock(&s_lock);
    return pressed;
}

void touch_pad_merge(HostPad *out) {
    if (!out) return;
    os_unfair_lock_lock(&s_lock);
    for (int player = 0; player < 2; player++) {
        if (!s_controller_layout_pending[player]) continue;
        host_putenv_s(player ? "DHD_PAD2" : "DHD_PAD1", s_controller_layout[player]);
        s_controller_layout_pending[player] = false;
    }
    if (s_editing) {
        memset(out, 0, sizeof *out);
        out->left_x = out->left_y = out->right_x = out->right_y = 128;
        out->connected = 1;
        os_unfair_lock_unlock(&s_lock);
        return;
    }

    /* Visibility is a UI policy; it must never discard a real touch sample. */
    uint32_t buttons = s_direct_buttons | s_pending_buttons;
    s_pending_buttons = 0;

    uint16_t b = 0;
    if (buttons & (1u << TOUCH_CTRL_CROSS))    b |= PAD_CROSS;
    if (buttons & (1u << TOUCH_CTRL_CIRCLE))   b |= PAD_CIRCLE;
    if (buttons & (1u << TOUCH_CTRL_SQUARE))   b |= PAD_SQUARE;
    if (buttons & (1u << TOUCH_CTRL_TRIANGLE)) b |= PAD_TRIANGLE;
    if (buttons & (1u << TOUCH_CTRL_L1))       b |= PAD_L1;
    if (buttons & (1u << TOUCH_CTRL_R1))       b |= PAD_R1;
    if (buttons & (1u << TOUCH_CTRL_L2))       b |= PAD_L2;
    if (buttons & (1u << TOUCH_CTRL_R2))       b |= PAD_R2;
    if (buttons & (1u << TOUCH_CTRL_START))    b |= PAD_START;
    if (buttons & (1u << TOUCH_CTRL_SELECT))   b |= PAD_SELECT;
    if (buttons & (1u << TOUCH_CTRL_L3))       b |= PAD_L3;
    if (buttons & (1u << TOUCH_CTRL_R3))       b |= PAD_R3;

    for (int i = 0; i < MAX_ACTIVE_TOUCHES; i++) {
        if (!s_touches[i].active) continue;
        switch (s_touches[i].control) {
            case TOUCH_CTRL_CROSS:    b |= PAD_CROSS;    break;
            case TOUCH_CTRL_CIRCLE:   b |= PAD_CIRCLE;   break;
            case TOUCH_CTRL_SQUARE:   b |= PAD_SQUARE;   break;
            case TOUCH_CTRL_TRIANGLE: b |= PAD_TRIANGLE; break;
            case TOUCH_CTRL_L1:       b |= PAD_L1;       break;
            case TOUCH_CTRL_R1:       b |= PAD_R1;       break;
            case TOUCH_CTRL_L2:       b |= PAD_L2;       break;
            case TOUCH_CTRL_R2:       b |= PAD_R2;       break;
            case TOUCH_CTRL_START:    b |= PAD_START;    break;
            case TOUCH_CTRL_SELECT:   b |= PAD_SELECT;   break;
            case TOUCH_CTRL_L3:       b |= PAD_L3;       break;
            case TOUCH_CTRL_R3:       b |= PAD_R3;       break;
            default: break;
        }
    }


    out->buttons |= b;

    /* Both orientations use these bytes. Keep the physical stick when touch is neutral. */
    if (s_stick_dx != 0 || s_stick_dy != 0) {
        out->left_x = touch_stick_byte(s_stick_dx);
        out->left_y = touch_stick_byte(s_stick_dy);
    }

    /* UI changes arrive under the input lock. Only the game thread changes the engine
       environment, so SDL's layout/invert/swap and the game's own deadzone row still apply.
       host_pad_device reads the new value on the next poll. */
    if (s_gamepad_deadzone_pending) {
        char value[4];
        snprintf(value, sizeof value, "%d", s_gamepad_deadzone);
        host_putenv_s("DHD_STICK_DEAD_ZONE", value);
        s_gamepad_deadzone_pending = false;
    }

    /* Update button pressures */
    for (int i = 0; i < 12; i++) {
        if (out->buttons & s_pressure_order[i]) {
            out->pressure[i] = 255;
        }
    }

    out->connected = 1;
    os_unfair_lock_unlock(&s_lock);
}

const TouchConfig *touch_input_get_config(void) {
    static _Thread_local TouchConfig snapshot;
    os_unfair_lock_lock(&s_lock);
    snapshot = s_config;
    os_unfair_lock_unlock(&s_lock);
    return &snapshot;
}

void touch_input_set_config(const TouchConfig *cfg) {
    if (!cfg) return;
    os_unfair_lock_lock(&s_lock);
    s_config = *cfg;
    s_config.opacity = touch_clamp(s_config.opacity, 0, 1, .5f);
    s_config.stick_deadzone = touch_clamp(s_config.stick_deadzone, 0, .95f, .05f);
    s_config.stick_sensitivity = touch_clamp(s_config.stick_sensitivity, 0, 3, 1);
    for (int i = 0; i < TOUCH_CTRL_COUNT; i++) {
        TouchControlLayout *c = &s_config.controls[i];
        c->x = touch_clamp(c->x, 0, 1, .5f);
        c->y = touch_clamp(c->y, 0, 1, .5f);
        c->radius = touch_clamp(c->radius, 1, 300, DEFAULT_BUTTON_RADIUS);
        c->scale = touch_clamp(c->scale, .1f, 5, 1);
    }
    os_unfair_lock_unlock(&s_lock);
}

void touch_input_set_gamepad_connected(bool connected) {
    os_unfair_lock_lock(&s_lock);
    s_gamepad_connected = connected;
    os_unfair_lock_unlock(&s_lock);
}

bool touch_input_is_visible(void) {
    os_unfair_lock_lock(&s_lock);
    bool visible = !(s_config.auto_hide_on_gamepad && s_gamepad_connected);
    os_unfair_lock_unlock(&s_lock);
    return visible;
}

/* Storage / persistence */
static NSString *get_layout_config_path(void) {
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *docDir = paths.firstObject;
    return [docDir stringByAppendingPathComponent:@"touch_controls_layout.json"];
}

bool touch_input_save_layout(void) {
    TouchConfig snapshot;
    os_unfair_lock_lock(&s_lock);
    snapshot = s_config;
    os_unfair_lock_unlock(&s_lock);
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    dict[@"opacity"] = @(snapshot.opacity);
    dict[@"deadzone"] = @(snapshot.stick_deadzone);
    dict[@"sensitivity"] = @(snapshot.stick_sensitivity);
    dict[@"floating"] = @(snapshot.stick_floating);
    dict[@"autoHide"] = @(snapshot.auto_hide_on_gamepad);
    dict[@"haptics"] = @(snapshot.haptics_enabled);

    NSMutableArray *ctrlArr = [NSMutableArray array];
    for (int i = 0; i < TOUCH_CTRL_COUNT; i++) {
        [ctrlArr addObject:@{
            @"id": @(i),
            @"x": @(snapshot.controls[i].x),
            @"y": @(snapshot.controls[i].y),
            @"radius": @(snapshot.controls[i].radius),
            @"scale": @(snapshot.controls[i].scale),
            @"visible": @(snapshot.controls[i].visible)
        }];
    }
    dict[@"controls"] = ctrlArr;

    NSError *err = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:dict options:NSJSONWritingPrettyPrinted error:&err];
    if (err || !data) return false;
    return [data writeToFile:get_layout_config_path() atomically:YES];
}

bool touch_input_load_saved_layout(void) {
    NSString *path = get_layout_config_path();
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return false;

    NSError *err = nil;
    NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
    if (err || ![dict isKindOfClass:[NSDictionary class]]) return false;

    TouchConfig cfg = *touch_input_get_config();
    if ([dict[@"opacity"] isKindOfClass:[NSNumber class]]) cfg.opacity = [dict[@"opacity"] floatValue];
    if ([dict[@"deadzone"] isKindOfClass:[NSNumber class]]) cfg.stick_deadzone = [dict[@"deadzone"] floatValue];
    if ([dict[@"sensitivity"] isKindOfClass:[NSNumber class]]) cfg.stick_sensitivity = [dict[@"sensitivity"] floatValue];
    if ([dict[@"floating"] isKindOfClass:[NSNumber class]]) cfg.stick_floating = [dict[@"floating"] boolValue];
    if ([dict[@"autoHide"] isKindOfClass:[NSNumber class]]) cfg.auto_hide_on_gamepad = [dict[@"autoHide"] boolValue];
    if ([dict[@"haptics"] isKindOfClass:[NSNumber class]]) cfg.haptics_enabled = [dict[@"haptics"] boolValue];

    NSArray *ctrlArr = dict[@"controls"];
    if ([ctrlArr isKindOfClass:[NSArray class]]) {
        for (NSDictionary *cDict in ctrlArr) {
            if (![cDict isKindOfClass:[NSDictionary class]] || ![cDict[@"id"] isKindOfClass:[NSNumber class]]) continue;
            int cid = [cDict[@"id"] intValue];
            if (cid >= 0 && cid < TOUCH_CTRL_COUNT) {
                if ([cDict[@"x"] isKindOfClass:[NSNumber class]]) cfg.controls[cid].x = [cDict[@"x"] floatValue];
                if ([cDict[@"y"] isKindOfClass:[NSNumber class]]) cfg.controls[cid].y = [cDict[@"y"] floatValue];
                if ([cDict[@"radius"] isKindOfClass:[NSNumber class]]) cfg.controls[cid].radius = [cDict[@"radius"] floatValue];
                if ([cDict[@"scale"] isKindOfClass:[NSNumber class]]) cfg.controls[cid].scale = [cDict[@"scale"] floatValue];
                if ([cDict[@"visible"] isKindOfClass:[NSNumber class]]) cfg.controls[cid].visible = [cDict[@"visible"] boolValue];
            }
        }
    }
    touch_input_set_config(&cfg);
    return true;
}

