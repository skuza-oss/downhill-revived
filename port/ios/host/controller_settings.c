/* Native controller UI over dhd_settings' shared rules. GPL-3.0-or-later. */
#include "ios_main.h"
#include "touch_input.h"
#include "dhd_settings.h"
#include "controls_table.h"
#include <stdio.h>
#include <string.h>

static void saved_values(char values[3][512])
{
    static const char *keys[] = {"keyboard", "pad1", "pad2"};
    for (int device = 0; device < 3; device++) {
        values[device][0] = 0;
        ios_setting_read(keys[device], values[device], 512);
    }
}

int ios_controller_layout(int player, uint8_t *out, int size)
{
    if (player < 1 || player > 2 || !out || size < 34) return 0;
    char values[3][512];
    DhdControlsLayout layouts[3];
    saved_values(values);
    dhd_controls_layouts(values[0], values[1], values[2], layouts, NULL);
    for (int button = 0; button < 16; button++)
        for (int slot = 0; slot < 2; slot++) out[button * 2 + slot] = layouts[player].input[button][slot];
    out[32] = layouts[player].swap_sticks;
    out[33] = layouts[player].invert;
    return 1;
}

int ios_controller_edit(int player, int edit, int target, int slot, int input, char *error, int size)
{
    if (error && size > 0) error[0] = 0;
    if (player < 1 || player > 2) return -1;
    char values[3][512], value[512];
    saved_values(values);
    int result = dhd_controls_edit(values[0], values[1], values[2], player, edit, target, slot, input,
                                   1, value, sizeof value, error, size > 0 ? (size_t)size : 0);
    if (result != CONTROL_CHANGED) return result;
    if (!ios_setting_save(player == 1 ? "pad1" : "pad2", value)) {
        if (error && size > 0) snprintf(error, (size_t)size, "Could not save controls");
        return -1;
    }
    touch_input_queue_controller_layout(player, value);
    return result;
}

int ios_controller_input_count(void) { return CONTROL_PAD_INPUTS; }
int ios_controller_input_id(int index)
{
    return index >= 0 && index < CONTROL_PAD_INPUTS ? control_pad_inputs[index].id : 0;
}
const char *ios_controller_input_label(int index, bool playstation)
{
    return index >= 0 && index < CONTROL_PAD_INPUTS ? control_pad_inputs[index].label[playstation ? 1 : 0] : "";
}
const char *ios_controller_target_label(int index)
{
    return index >= 0 && index < CONTROL_BUTTONS ? control_targets[index].label : "";
}
