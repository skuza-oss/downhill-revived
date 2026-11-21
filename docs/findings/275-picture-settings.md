# 275: picture settings: Brightness, Contrast, Saturation, Gamma (issue #115)

Status: in progress

Branch `feat/picture-settings` from `main` bbe4acc5 (0.8.10), started 2026-11-21. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A player asked for brightness, gamma, contrast and saturation as settings (issue #115): a change to
the shown picture only, so it keeps the original game's feel. Four settings in dhd.ini through the
settings crate, applied once in the renderer's present pass for both front ends and every backend,
changed in the game's REVIVED SETTINGS and in the launcher's Settings > Graphics.

Done when: all four neutral (the default) give today's picture byte for byte, so no reference moves;
unit tests for the crate's rows, the maths and the launcher's controls; a hidden run with 2x captures
(neutral, each setting off neutral, the page) and a script that shows the neutral capture equals the
base build's; boot and race state traces equal to the base's; `check.py port`, `check.py docs` and
the debug APK (the shaders through the SPIR-V and Mali checks). Out of scope: phone and Mac runs (the
lead's, at the release); HDR or wide-gamut output.

## Progress

- [x] 2026-11-21: renderer (`present/picture.rs`, `picture.wgsl` in both present passes), the
  settings rows, the game's Brightness & Color screen, the launcher's rows, PLAYING.md

## Decisions

- **Where:** one function, `picture_adjust` (`port/render/src/present/picture.wgsl`), appended to
  present.wgsl (the exact present pass) and native.wgsl (the native one) by `shaders.rs`, so the
  browser's WebGPU build has it too. The native pass applies it in `present_pixel`, `present_direct`
  and `present_fill` (widescreen's blurred sides), not in `present_shrink`, whose small texture the
  fill reads (it would be applied twice). The settings reach the uniform each present
  (`picture::uniform()`, four atomics set by `dhdr_picture`); no extra pass and no extra target.
- **Inside / outside:** inside are the game's picture, its menus and its movies (all are the
  scanned-out picture). Outside: the touch controls (drawn after the picture in the native pass, by
  the overlay in the exact one) and the FPS & Ping counter (`dhdr_surface_present`'s overlay): a
  player who darkens the game still reads the buttons. That costs nothing: they were separate draws.
- **Neutral is exact:** neutral writes zeros, and the shader's first line returns the sampled colour
  untouched when `adjust.w` (1 / gamma, never 0 otherwise) is 0: no arithmetic touches it, so the
  bytes written are the ones written before. The fill's darkening keeps its old order
  (`sum / 16 * FILL_SHADE`).
- **Maths (display-referred):** the surfaces are UNORM, not sRGB, and the picture's values are the
  PS2's display values, so the maths works on them as they are. In order: saturation mixes the colour
  with its Rec. 709 luma (`luma + (c - luma) * s`); contrast scales around mid-grey
  (`(c - 0.5) * k + 0.5`); brightness scales (`c * b`); the result is held to 0..1; gamma last
  (`c ^ (1 / g)`: above 1 lifts the mid-tones, black and white stay). Examples (tests in
  `picture.rs`): brightness 150 % takes grey 0.5 to 0.75; contrast 50 % takes black to 0.25 and white
  to 0.75; saturation 50 % takes pure red to (0.606, 0.106, 0.106); gamma 1.4 takes 0.25 to 0.372.
- **Ranges and steps:** Brightness, Contrast and Saturation 50 % to 150 % in steps of 10 %; Gamma 0.6
  to 1.4 in steps of 0.1. 11 and 9 steps, neutral in the middle of each, so five (four) presses from
  neutral reach an end; no value makes the picture unreadable. Other numbers in dhd.ini are held to
  the range by the renderer; text that is no number is neutral.
- **C to Rust:** `dhdr_picture(float, float, float, float)` in `dhd_render_ext.h`: numbers, because the
  browser's renderer has a memory of its own (a pointer to text would not reach it). C reads the four
  variables with `strtod` (NaN when unset or no number) when it reads the FPS counter's: at the first
  picture and when SETTINGS changes one (`gs_present_options_apply`); the browser at its first picture
  and on `web_env_changed`.
- **The game's page:** Graphics already has ten rows, and an eleventh runs into the tab names, so the
  four rows have their own screen, BRIGHTNESS & COLOR, opened from a Brightness & Color row at the end
  of the Display tab (the picture's other settings, its shape, are there too). The rows are sliders:
  left/right step and stop at the ends (other rows wrap around); each change is saved and shown at
  once; Reset Picture sets all four to neutral. Triangle goes back to the Display tab; the page's own
  Triangle on a PC still undoes what was changed since it opened. A tap beside a row steps it as on
  every value row of our pages (findings 092, 268).
- **The launcher:** Settings > Graphics gets the four as − value + steppers (`steps.js`, the same
  steps as the game; a pad reaches both buttons, a tap or click presses them) and a Reset Picture
  row; the tab's own Reset sets them back too. One page for every platform.

## Surprises

- Gamma 0.6 to 1.6 (the brief's example) has 1.0 off the middle; 0.6 to 1.4 keeps neutral in the
  middle of a slider, so it was used.

## Result

Filled in at the merge.
