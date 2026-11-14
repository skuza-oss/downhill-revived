/* Device admission before slot assignment (finding 232). SDL's automatic mappings do not
   establish that a partial HID device is a gamepad. No names or platform exceptions here. */
#ifndef DHD_GAMEPAD_DEVICE_H
#define DHD_GAMEPAD_DEVICE_H
#include <SDL3/SDL.h>
#include <string.h>

typedef struct {
    int type, real_type;
    const char *mapping;
    unsigned buttons, axes;  /* mapped SDL button/axis presence, not current input */
    int raw_buttons, raw_axes, hats;  /* used only without a gamepad mapping */
    int joystick_type;
} GamepadDevice;

/* SDL's guessed mappings use the wildcard name. A named database/user mapping is an
   explicit description, including unusual/accessibility pads with fewer controls.
   SDL exposes the mapping string, but not its internal mapping priority/source. */
static int gamepad_named_mapping(const char *mapping)
{
    const char *name = mapping ? strchr(mapping, ',') : NULL;
    if (!name || !*++name) return 0;
    const char *end = strchr(name, ',');
    return end && end > name && !(end == name + 1 && *name == '*');
}

static int gamepad_device_slot(const GamepadDevice *d, unsigned occupied)
{
    unsigned face = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                    (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH);
    unsigned dpad = (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) | (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
                    (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) | (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    unsigned left = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    int known = (d->type > SDL_GAMEPAD_TYPE_STANDARD && d->type < SDL_GAMEPAD_TYPE_COUNT) ||
                (d->real_type > SDL_GAMEPAD_TYPE_STANDARD && d->real_type < SDL_GAMEPAD_TYPE_COUNT);
    int controls = d->mapping ? (d->buttons & face) == face &&
                                  ((d->buttons & dpad) == dpad || (d->axes & left) == left) :
                               d->raw_buttons >= 4 && (d->hats > 0 || d->raw_axes >= 2);
    int mapped = gamepad_named_mapping(d->mapping);
    if (!known && !mapped && d->joystick_type != SDL_JOYSTICK_TYPE_UNKNOWN &&
        d->joystick_type != SDL_JOYSTICK_TYPE_GAMEPAD) return -1;
    if (!known && !mapped && !controls) return -1;
    for (int i = 0; i < 4; i++) if (!(occupied & (1u << i))) return i;
    return -1;
}
/* Rising button edges only; axes, triggers and sensors cannot claim a controller.
   Player 1 wins simultaneous presses. Once claimed, order is fixed for this run. */
static int gamepad_claim_slot(const unsigned pressed[4], int single_player, int *claimed)
{
    unsigned deliberate = 15u | (1u << SDL_GAMEPAD_BUTTON_START) |
        (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) | (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
        (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) | (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    if (*claimed) return -1;
    if (pressed[0] & deliberate) { *claimed = 1; return -1; }
    if (!single_player) return -1;
    for (int i = 1; i < 4; i++)
        if (pressed[i] & deliberate) { *claimed = 1; return i; }
    return -1;
}
#endif
