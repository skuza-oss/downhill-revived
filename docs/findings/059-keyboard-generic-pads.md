# 059: keyboard play and generic controllers (SDL3)

Status: partly superseded by 099 (XInput/raw-HID fallback, the i686 SDL build)
Checked: 2026-11-04

The keyboard layout and the SDL3 pad rules (buttons by position, default mapping,
`gamecontrollerdb.txt`, vibration) are current (`port/hle/keyboard.c`, `port/hle/gamepad.c`).
Changed: SDL is the only controller path (the XInput and raw-HID fallback is gone; `DHD_SDL=0`
means no pads, the keyboard still plays), SDL runs on the process's main thread with the window
and audio (`port/host/sdl_host.c`, finding 099), and SDL is built for every 64-bit target by
`port/tools/sdl3.py` (the i686 build details below are retired). Dead zone and button icons:
finding 134.

Friends without a controller can play a whole race on the keyboard, and pads
that aren't XInput (cheap USB pads, Switch Pro, 8BitDo in D-input mode, other DirectInput/HID
pads) work through SDL3. Code: `port/hle/keyboard.c` (layout, menus versus riding, the ramp),
`port/hle/gamepad.c` (SDL3), `port/hle/host.c` (focus, merging, fallback, vibration),
`port/tools/sdl3.py` (SDL's pinned build). No change to `src/`.

Sources: the game's code (below); the US manual's default controls as PSXDataCenter lists them
(https://psxdatacenter.com/psx2/games2/SCUS-97177.html); SDL 3.4.18's headers and CMake files
(https://github.com/libsdl-org/SDL, zlib). SDL is linked unchanged, nothing of it is copied into
our code.

## What a race reads

Default controls (manual): steer D-pad or left stick left/right; lean D-pad or left stick
up/down; pedal Cross or right stick up (Cross twice or R3: sprint); brake Triangle or right
stick down; bunny hop R2; powerslide L3 or D-pad left/right twice; punch/kick Square/Circle
(or right stick left/right); tricks L1, L2, R1, R2 in the air; bail out SELECT twice; camera
SELECT + up/down; look back SELECT + L2; pause START.

In the code (finding 040's per-field pad reads):
- `func_00220550(flag)` sets `D_0029E290`, the input mode: 1 while riding (`func_001E3A68`,
  `func_00161198`; `func_00167B88` when the pause menu closes), 0 in menus (`func_00167AE0` opens
  the pause menu, `func_001E39A0`, `func_001B9B70` at race end). In mode 0 a button counts once
  per press (`func_00220640`, `D_00660D80`) and a stick only beyond |119| (`func_00220AE0`); in
  mode 1 buttons are held and carry their pressure.
- The rider's controller (`func_0017DD70`) runs one function per action pair. Each first reads
  the action's buttons (`func_0017EB00` -> `func_0017EB88`, table at +4), and only if none is
  down reads a stick axis through `func_00220AE0` (axis 0 right X, 1 right Y, 2 left X, 3 left
  Y, `func_002208E8`): `func_0017DFB0` actions 3/4 right Y (pedal/brake), `func_0017E1C0` 7/8
  left X (steer, input +0x1B0/+0x1B4, rider +0x1190/+0x1194), `func_0017E300` 9/10 left Y (lean).
  A stick value is squared (`v * v / 127`); a digital press is set to 1.
- `func_0012DEB0` turns the steering into a float: a value above 1 is scaled by 1/128, the
  digital 1 stays 1.0. So the d-pad steers at full lock and only the stick in between.
- `func_00220AE0`'s curve: raw 92-162 is the dead zone, 163-225 gives 1-64, 226-255 64-127 (and
  the mirror below 92).
- Menus (the popup manager, `func_001909A8`) read buttons only: the stick bytes are read but the
  per-direction flags are always cleared (`func_00190C88(..., 0, ...)`). Entries
  (`func_00190900`): 0-3 up/down/left/right, 4 Cross (accept in every SharedMenu,
  `func_0018E1C0`), 5 Triangle (back), 8 START (the race setup screen `func_0013BA60`, replays,
  pause).

So a keyboard needs the d-pad in menus and, to steer in proportion, the left stick while riding.

## Keyboard

`keyboard.c`; host.c reads the keys with `GetAsyncKeyState` only while the window in front
belongs to this process (the game's window; a console it was started from no longer counts) and
none of Alt, Ctrl and the Windows keys is held, so Alt+Tab, Alt+Enter, Ctrl+Esc stay the
system's. Shift and the function keys are not used (no Sticky Keys prompt). Keys merge with the
controller of player 1: buttons and pressures add, an arrow overrides the stick axis it moves.

| Key | Pad | Menus | Riding |
|---|---|---|---|
| arrows | d-pad / left stick | move | steer, lean (ramped stick) |
| Space, X | Cross | confirm | pedal (twice: sprint) |
| S, Backspace | Triangle | back | brake |
| Esc | Triangle / START | back | pause |
| Enter | START | start the race (setup screen) | pause |
| Tab | SELECT | | camera (+ up/down), look back (+ Q), bail out (twice) |
| Z, C | Square, Circle | | punch, kick |
| A, D | L1, R1 | | tricks |
| Q, E | L2, R2 | | tricks; E bunny hop |
| W, F | R3, L3 | | sprint, powerslide |

- Mode: `D_0029E290` (EE 0x0029E290). While riding the arrows tilt the left stick and press no
  d-pad (a d-pad press would win and give full lock at once); in menus they are the d-pad.
- Ramp: from the press, the wanted share of full steering grows linearly, reaching 1 after
  100 ms (one field ahead, so the first field already steers: 0.34 of full lock after 17 ms, 0.66
  after 50 ms, 0.99 at 100 ms). Each step writes the raw byte, nearest the centre, whose steering
  after `func_00220AE0` and the squaring reaches that share. Release: the centre at once; both
  directions held: the centre; turning restarts the ramp. Lean uses the same.
- A key keeps the meaning it had when it went down until it is released: Esc pressed while
  riding stays START when the pause menu turns the mode to 0, so the menu doesn't receive a
  Triangle that closes it again.
- Letters are positions: host.c turns each US letter's scan code into the virtual key of the
  window's current layout (`MapVirtualKeyExA`), so the cluster sits in the same place on AZERTY
  and QWERTZ. Checked on this PC's US layout (26 of 26 letters map to themselves); other
  layouts not installed here, so not tried.
- Enter is START, not Cross: the race setup screen starts on START and uses Cross on its rows, so
  one key for both would do two things there. Space and X confirm.

## Controllers: SDL3

SDL 3.4.18, built static by `port/tools/sdl3.py` (pinned URL and SHA-256 of the release tarball;
CMake + Ninja + clang `i686-pc-windows-msvc`, `/MT`), only joystick, gamepad, HIDAPI and events;
audio, video, render, GPU, camera, haptic, sensor off. Two CMake details:
- SDL's CMake checks for `dinput.h` only when audio or video is on, so with both off the
  DirectInput driver (the generic pads) was missing. `HAVE_DINPUT_H=1` restores it (the SDK has
  the header; SDL loads dinput8.dll itself).
- `HAVE_GAMEINPUT_H=0` leaves out the GameInput driver (C++, off by default on desktop).
- `LIBC_HAS_ATAN*=0`: SDL uses its own fdlibm atan, not the C runtime's. SDL's dynamic API
  table references every SDL function, so `SDL_atan` is linked even unused, and the runtime's
  atan brings `__atan_table`, whose doubles contain the bytes "Y:/(A6>", which
  `package_release.py` (rightly strict) refuses as an absolute path. Dynamic API itself can only
  be turned off by editing SDL's source, which we don't.
- The same table links all of SDL's code (GPU, video, ...), and in machine code an address can
  spell a path too: `push 0x407c2f3a` (a string in `SDL_gpu.c`) is the bytes "h:/|@". So
  `package_release.py` now looks for drive paths in the exe's data sections only (strings are
  never in code); the user-folder, repository-path and server checks still read the whole file.
  A path planted in `.rdata` is still refused (checked).
Drivers in the build: XInput, RawInput, Windows.Gaming.Input, DirectInput, HIDAPI (DualSense,
DualShock 4, Switch Pro, 8BitDo, ...). dhd.exe stays one file; it grows by about 1.6 MB.

`gamepad.c`: SDL ran on a thread of its own ("controllers"; now SDL's one thread, `sdl_host.c`): `SDL_Init(SDL_INIT_GAMEPAD)`
there, then a loop that waits for events (4 ms at most), opens each new device into the first
free player slot (1-4, kept until it disconnects), copies each player's state for the game
thread and sends the vibration it asked for. The game thread only copies (`gamepad_read`).
`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is on (SDL has no window, so pads read as before
whether or not the game is in front).
- Buttons by position: bottom Cross, right Circle, left Square, top Triangle (Switch Pro: B is
  Cross); Back/Share/Create SELECT, Start/Options/+ START, triggers L2/R2 with pressure.
- A joystick SDL has no gamepad mapping for gets a default layout (hat d-pad, axes 0/1 and 2/3,
  buttons 0-11: Cross Circle Square Triangle L1 R1 L2 R2 SELECT START L3 R3).
  `gamecontrollerdb.txt` beside dhd.exe adds mappings (`SDL_AddGamepadMappingsFromFile`), as does
  SDL's own `SDL_GAMECONTROLLERCONFIG`.
- Vibration: the small motor on/off as high frequency, the large one's speed as low frequency
  (as the XInput path did), `SDL_RumbleGamepad` for 1 s, sent again every 0.5 s while it lasts.
  Every pad SDL can rumble (XInput, DualSense, DualShock 4, Switch Pro, ...); DHD_VIBRATION=0 still
  turns it off.

Coexistence (then; the fallback is deleted): SDL replaces the direct XInput and raw-HID Sony code, which stays as the fallback:
`DHD_SDL=0`, or SDL not starting within 3 s. Each read uses one of the two, never both, so no
pad is read twice. A virtual pad from DS4Windows/DSX beside the real one shows as two players
(hide the real one, e.g. with HidHide, as those tools advise).

## Tests

- `test_keyboard` (port/tools/test_runtime.py): layout in both modes, the ramp against the
  game's own curve (within 0.03 of linear at every 4 ms, monotone, full at 100 ms), release,
  both directions, turning, the Esc latch, merging with a pad, DHD_KEYS.
- `test_gamepad`: SDL starts and enumerates. On this PC (a DualSense over USB): "DualSense
  Wireless Controller (ps5, vibration) is player 1", centred sticks, no buttons.
- In the game: `DHD_KEYS` (host.c) feeds fake keys from GO on the race clock (as DHD_RACE_PAD)
  through the same mapping and logs each change with the rider's steering input.

Not tested without hardware or hands: pressing real keys in the window, a generic DirectInput
pad, a Switch Pro or 8BitDo, actual vibration on any pad (the DualSense reports the capability;
nothing was made to rumble while the PC was in use), hot-plugging.
