/* Keyboard and controller bindings' tables (finding 157): generated from
   port/render/settings/src/controls.rs (dhd_settings), whose test keeps this file equal to it
   (DHD_SETTINGS_BLESS=1 cargo test -p dhd_settings writes it). Don't edit it here.
   A layout's inputs: the keyboard's are the keys' vk (the Windows virtual-key code of the US key
   at the place), a controller's the inputs' id; 0 nothing. */
#ifndef DHD_CONTROLS_TABLE_H
#define DHD_CONTROLS_TABLE_H

#include <stdint.h>

enum { CONTROL_TARGETS = 24, CONTROL_BUTTONS = 16, CONTROL_SLOTS = 2, CONTROL_DEVICES = 3, CONTROL_KEYS = 91, CONTROL_PAD_INPUTS = 22 };
enum { CONTROL_KEYBOARD, CONTROL_PAD1, CONTROL_PAD2 };
enum { CONTROL_INVERT_LX = 1, CONTROL_INVERT_LY = 2, CONTROL_INVERT_RX = 4, CONTROL_INVERT_RY = 8 };
/* dhd_controls_edit's edits and answers (dhd_settings.h) */
enum { CONTROL_ASSIGN, CONTROL_CLEAR, CONTROL_RESET, CONTROL_SWAP_STICKS, CONTROL_INVERT };
enum { CONTROL_REFUSED = -1, CONTROL_UNCHANGED = 0, CONTROL_CHANGED = 1 };

/* A layout: each target's main and alternate input, a controller's stick options (52 bytes). */
typedef struct ControlsLayout { uint8_t input[CONTROL_TARGETS][CONTROL_SLOTS]; uint8_t swap_sticks, invert, unused[2]; } ControlsLayout;

/* The PS2 pad's buttons, then the keyboard's stick directions: dhd.ini name, the pages' word, what it
   does in a race, libpad's bit (0: a stick direction: axis 1 LX 2 LY 3 RX 4 RY, dir -1 / 1), and whether
   the menus need it. */
typedef struct { const char *name, *label, *race; uint16_t bit; uint8_t axis; int8_t dir; uint8_t essential; } ControlTarget;
static const ControlTarget control_targets[CONTROL_TARGETS] = {
    {"cross", "Cross", "Pedal (twice: sprint), confirm", 0x4000, 0, 0, 1},
    {"circle", "Circle", "Kick", 0x2000, 0, 0, 0},
    {"square", "Square", "Punch", 0x8000, 0, 0, 0},
    {"triangle", "Triangle", "Brake (in the air: tweak), back", 0x1000, 0, 0, 1},
    {"l1", "L1", "Tricks (with R1: Superman)", 0x0400, 0, 0, 0},
    {"r1", "R1", "Tricks (with L1: Superman), settings tabs", 0x0800, 0, 0, 1},
    {"l2", "L2", "Tricks", 0x0100, 0, 0, 0},
    {"r2", "R2", "Tricks, bunny hop", 0x0200, 0, 0, 0},
    {"l3", "L3", "Powerslide", 0x0002, 0, 0, 0},
    {"r3", "R3", "Sprint", 0x0004, 0, 0, 0},
    {"start", "START", "Pause, start the race", 0x0008, 0, 0, 1},
    {"select", "SELECT", "With up or down: camera, twice: bail out", 0x0001, 0, 0, 0},
    {"up", "Up", "Lean, menus", 0x0010, 0, 0, 1},
    {"down", "Down", "Lean, menus", 0x0040, 0, 0, 1},
    {"left", "Left", "Steer, menus", 0x0080, 0, 0, 1},
    {"right", "Right", "Steer, menus", 0x0020, 0, 0, 1},
    {"lstick_up", "Left Stick Up", "Lean", 0x0000, 2, -1, 0},
    {"lstick_down", "Left Stick Down", "Lean", 0x0000, 2, 1, 0},
    {"lstick_left", "Left Stick Left", "Steer", 0x0000, 1, -1, 0},
    {"lstick_right", "Left Stick Right", "Steer", 0x0000, 1, 1, 0},
    {"rstick_up", "Right Stick Up", "", 0x0000, 4, -1, 0},
    {"rstick_down", "Right Stick Down", "", 0x0000, 4, 1, 0},
    {"rstick_left", "Right Stick Left", "", 0x0000, 3, -1, 0},
    {"rstick_right", "Right Stick Right", "", 0x0000, 3, 1, 0},
};

/* The keys that can be bound: dhd.ini name, the pages' word, vk, SDL's scan code (the place). */
typedef struct { const char *name, *label; uint8_t vk; uint16_t scancode; } ControlKey;
static const ControlKey control_keys[CONTROL_KEYS] = {
    {"a", "A", 0x41, 4},
    {"b", "B", 0x42, 5},
    {"c", "C", 0x43, 6},
    {"d", "D", 0x44, 7},
    {"e", "E", 0x45, 8},
    {"f", "F", 0x46, 9},
    {"g", "G", 0x47, 10},
    {"h", "H", 0x48, 11},
    {"i", "I", 0x49, 12},
    {"j", "J", 0x4A, 13},
    {"k", "K", 0x4B, 14},
    {"l", "L", 0x4C, 15},
    {"m", "M", 0x4D, 16},
    {"n", "N", 0x4E, 17},
    {"o", "O", 0x4F, 18},
    {"p", "P", 0x50, 19},
    {"q", "Q", 0x51, 20},
    {"r", "R", 0x52, 21},
    {"s", "S", 0x53, 22},
    {"t", "T", 0x54, 23},
    {"u", "U", 0x55, 24},
    {"v", "V", 0x56, 25},
    {"w", "W", 0x57, 26},
    {"x", "X", 0x58, 27},
    {"y", "Y", 0x59, 28},
    {"z", "Z", 0x5A, 29},
    {"1", "1", 0x31, 30},
    {"2", "2", 0x32, 31},
    {"3", "3", 0x33, 32},
    {"4", "4", 0x34, 33},
    {"5", "5", 0x35, 34},
    {"6", "6", 0x36, 35},
    {"7", "7", 0x37, 36},
    {"8", "8", 0x38, 37},
    {"9", "9", 0x39, 38},
    {"0", "0", 0x30, 39},
    {"enter", "Enter", 0x0D, 40},
    {"backspace", "Backspace", 0x08, 42},
    {"tab", "Tab", 0x09, 43},
    {"space", "Space", 0x20, 44},
    {"minus", "Minus", 0xBD, 45},
    {"equals", "Equals", 0xBB, 46},
    {"lbracket", "Left Bracket", 0xDB, 47},
    {"rbracket", "Right Bracket", 0xDD, 48},
    {"backslash", "Backslash", 0xDC, 49},
    {"semicolon", "Semicolon", 0xBA, 51},
    {"quote", "Quote", 0xDE, 52},
    {"backquote", "Backquote", 0xC0, 53},
    {"comma", "Comma", 0xBC, 54},
    {"period", "Period", 0xBE, 55},
    {"slash", "Slash", 0xBF, 56},
    {"intlbackslash", "Key Beside Shift", 0xE2, 100},
    {"f1", "F1", 0x70, 58},
    {"f2", "F2", 0x71, 59},
    {"f3", "F3", 0x72, 60},
    {"f4", "F4", 0x73, 61},
    {"f5", "F5", 0x74, 62},
    {"f6", "F6", 0x75, 63},
    {"f7", "F7", 0x76, 64},
    {"f8", "F8", 0x77, 65},
    {"f9", "F9", 0x78, 66},
    {"f10", "F10", 0x79, 67},
    {"f11", "F11", 0x7A, 68},
    {"f12", "F12", 0x7B, 69},
    {"insert", "Insert", 0x2D, 73},
    {"home", "Home", 0x24, 74},
    {"pageup", "Page Up", 0x21, 75},
    {"delete", "Delete", 0x2E, 76},
    {"end", "End", 0x23, 77},
    {"pagedown", "Page Down", 0x22, 78},
    {"right", "Right", 0x27, 79},
    {"left", "Left", 0x25, 80},
    {"down", "Down", 0x28, 81},
    {"up", "Up", 0x26, 82},
    {"kp_divide", "Num Divide", 0x6F, 84},
    {"kp_multiply", "Num Multiply", 0x6A, 85},
    {"kp_minus", "Num Minus", 0x6D, 86},
    {"kp_plus", "Num Plus", 0x6B, 87},
    {"kp1", "Num 1", 0x61, 89},
    {"kp2", "Num 2", 0x62, 90},
    {"kp3", "Num 3", 0x63, 91},
    {"kp4", "Num 4", 0x64, 92},
    {"kp5", "Num 5", 0x65, 93},
    {"kp6", "Num 6", 0x66, 94},
    {"kp7", "Num 7", 0x67, 95},
    {"kp8", "Num 8", 0x68, 96},
    {"kp9", "Num 9", 0x69, 97},
    {"kp0", "Num 0", 0x60, 98},
    {"kp_period", "Num Period", 0x6E, 99},
    {"lshift", "Left Shift", 0xA0, 225},
    {"rshift", "Right Shift", 0xA1, 229},
};

/* The controller inputs that can be bound: id, dhd.ini name, the words of an Xbox, a PlayStation and a
   Nintendo pad, SDL_GamepadButton (-1: a trigger: 1 left, 2 right). */
typedef struct { uint8_t id; const char *name, *label[3]; int8_t sdl; uint8_t trigger; } ControlPadInput;
static const ControlPadInput control_pad_inputs[CONTROL_PAD_INPUTS] = {
    {1, "south", {"A", "Cross", "B"}, 0, 0},
    {2, "east", {"B", "Circle", "A"}, 1, 0},
    {3, "west", {"X", "Square", "Y"}, 2, 0},
    {4, "north", {"Y", "Triangle", "X"}, 3, 0},
    {5, "back", {"View", "Share", "Minus"}, 4, 0},
    {6, "start", {"Menu", "Options", "Plus"}, 6, 0},
    {7, "ls", {"Left Stick", "L3", "Left Stick"}, 7, 0},
    {8, "rs", {"Right Stick", "R3", "Right Stick"}, 8, 0},
    {9, "lb", {"LB", "L1", "L"}, 9, 0},
    {10, "rb", {"RB", "R1", "R"}, 10, 0},
    {11, "dpad_up", {"D-pad Up", "D-pad Up", "D-pad Up"}, 11, 0},
    {12, "dpad_down", {"D-pad Down", "D-pad Down", "D-pad Down"}, 12, 0},
    {13, "dpad_left", {"D-pad Left", "D-pad Left", "D-pad Left"}, 13, 0},
    {14, "dpad_right", {"D-pad Right", "D-pad Right", "D-pad Right"}, 14, 0},
    {15, "misc", {"Share", "Mic", "Capture"}, 15, 0},
    {16, "paddle_r1", {"P1", "Right Paddle", "Right Paddle"}, 16, 0},
    {17, "paddle_l1", {"P3", "Left Paddle", "Left Paddle"}, 17, 0},
    {18, "paddle_r2", {"P2", "Right Paddle 2", "Right Paddle 2"}, 18, 0},
    {19, "paddle_l2", {"P4", "Left Paddle 2", "Left Paddle 2"}, 19, 0},
    {20, "touchpad", {"Touchpad", "Touchpad", "Touchpad"}, 20, 0},
    {21, "lt", {"LT", "L2", "ZL"}, -1, 1},
    {22, "rt", {"RT", "R2", "ZR"}, -1, 2},
};

/* The defaults: the keyboard, Controller 1, Controller 2 (the game's layouts before rebinding). */
static const ControlsLayout control_defaults[CONTROL_DEVICES] = {
    {{{32, 88}, {67, 0}, {90, 0}, {83, 8}, {65, 0}, {68, 0}, {81, 0}, {69, 0}, {70, 0}, {87, 0}, {13, 0}, {9, 0}, {38, 0}, {40, 0}, {37, 0}, {39, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}, 0, 0, {0, 0}},
    {{{1, 0}, {2, 0}, {3, 0}, {4, 0}, {9, 0}, {10, 0}, {21, 0}, {22, 0}, {7, 0}, {8, 0}, {6, 0}, {5, 0}, {11, 0}, {12, 0}, {13, 0}, {14, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}, 0, 0, {0, 0}},
    {{{1, 0}, {2, 0}, {3, 0}, {4, 0}, {9, 0}, {10, 0}, {21, 0}, {22, 0}, {7, 0}, {8, 0}, {6, 0}, {5, 0}, {11, 0}, {12, 0}, {13, 0}, {14, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}, 0, 0, {0, 0}},
};

/* The settings: dhd.ini key and variable of the keyboard, Controller 1 and Controller 2. */
static const struct { const char *key, *env, *label; } control_devices[CONTROL_DEVICES] = {
    {"keyboard", "DHD_KEYBOARD", "Keyboard"},
    {"pad1", "DHD_PAD1", "Controller 1"},
    {"pad2", "DHD_PAD2", "Controller 2"},
};

#endif
