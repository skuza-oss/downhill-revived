# 180: dynamic display refresh rate tracking (60/120 Hz)

Status: current
Checked: 2026-11-08

Branch `fix/shared-display-refresh` from `main` db3702d, started 2026-11-08.

## Goal and acceptance

When the game runs with display-following refresh rate (`DHD_FPS=monitor` or `DHD_FPS_AUTO=1`),
allow the clock to update its target rate when the host display's refresh rate changes (such as moving
a window between 60 Hz and 120/144 Hz monitors, or variable refresh rate / ProMotion mode changes).

Carries fractional fields (`phase`) and animation ticks (`anim_phase`) into the new rate proportionally,
avoiding hitches or jumps at transition boundaries.

Acceptance:
- `port_fps_update_display()` updates `requested` and returns 1 when the display refresh rate changes in monitor mode;
- returns 0 immediately when not in monitor mode or when rate is unchanged;
- leaves deterministic / fixed FPS runs and Windows state traces completely unchanged (`DHD_FRAME_CLOCK=1`);
- `python tools/check.py docs` passes with 0 errors.

## Decisions

- **Generic monitor mode trigger:** Rather than platform-specific conditionals, tracking is tied to
  `follow_display` (`DHD_FPS=monitor` or `DHD_FPS_AUTO`). All desktop and mobile platforms share this exact code path.
- **Phase rescaling:** When rate changes from `previous` to `rate`, both `phase` and `anim_phase` are multiplied
  by `(double)rate / previous` so physics and animation states remain continuous across the transition.

## Result

- `port/include/fps.h`: declared `port_fps_update_display()`.
- `port/hle/fps.c`: added `port_fps_update_display()` and `follow_display` flag.
- State trace and documentation checks pass with 0 errors.
