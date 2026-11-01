# 141: Rebindable controls, settings that apply at once, touch regions

Three pieces of work from the controls-remap and settings-live branches, merged into the
port-improvements line. Their code cited "finding 124", a number upstream has since used (the PERU
VU1 loop), so they are written up here.

## Rebindable controls

- `port/hle/controls.h`, `controls.c`: two tables, one per device. The keyboard (`keyboard.c`) has
  an entry per key it reads, with the pad button it makes in menus and while riding; the
  controllers (`gamepad.c`, SDL) an entry per pad input the port reads (a button, a trigger or a
  stick axis). The defaults are the layouts findings 059 and 091 shipped, so nothing changes until
  a player rebinds something.
- The bindings are text in dhd.ini: `bindings` (and `bindings_<name>` for more profiles,
  `controls` naming the active one), a comma list of `key.<control>=<key>` and
  `pad.<control>=<source>`. Unknown ids, keys and sources are skipped, so an old or hand-edited
  file never breaks the game. The settings crate (`table.rs`, `ini.rs`) knows the profile keys.
- OPTIONS > SETTINGS > Controls... (`controls_menu.c`) is a hub with a Keyboard page and a Gamepad
  page in the game's popup style; a change saves through the crate and asks the host to apply it
  (`host_service_controls_apply`), so the keyboard and the controllers follow at once. The row
  comes after the rows the host shows (a host showing fewer, like the browser, had Cross on the
  wrong row).

## Settings that apply at once

- Resolution (`DHD_SCALE`: the upscaled surfaces made again, `gs_native_scale_apply`) and V-Sync
  (the swap chain made again in the new present mode) apply while the game runs.
- Frame Rate: between rates above 60 at once (only the step changes); to or from 60 at the frame
  clock's next start, a new screen or race (patched `func_00238A68`), the footer saying so.
- Vibration: turning it on buzzes player 1's pad (`pad_preview.h`). The game sends motor values
  every field, zeros included, so the preview wins over them for a moment, then the game's apply
  again; turning it off stops at once.
- Widescreen and the voice rows apply at once too (STATE.md, 2026-11-01; finding 131). Every row
  that decides the widescreen shape (Widescreen, Display, Window Size and Picture, whose
  `DHD_ASPECT` ratio `wanted_aspect` reads) runs `port_widescreen_apply` and then presents again
  (`pc_settings.c` `widescreen_apply`). On the native PC builds the renderer takes the shape from
  what the module published at the last vblank (`host_widescreen_aspect` in `host.c`, published by
  `kernel.c`), so `widescreen_apply` publishes it again first (`host_frame_video`); without that the
  new field of view was shown in the old shape until something else re-presented. The browser
  reads `DHD_WIDESCREEN_ASPECT`, which `port_widescreen_apply` sets.

## Touch regions (Android)

`port/host/touch.c`, Android only: touches become player 1's pad through the same path as a
controller, a stick region at the bottom left and the riding buttons at the right, as fractions of
the game's picture. `DHD_TOUCH=0` turns it off; `DHD_TOUCH_LAYOUT` names a preset (`default`,
`left-handed`) or gives rectangles.

## Checked

- `port/tools/test_controls.py`: defaults, catalogs, the text round trip, rebinds, profile keys
  (ok, 2026-11-01).
- `port/tools/test_pad_preview.py`: the preview window, the game's values dropped meanwhile, off at
  once, no lasting override (pass, 2026-11-01).
- Built into the Windows and browser builds of 2026-11-01. Rebinding by hand in the CONTROLS page
  has not been tried in a game yet.

## Open

- The touch buttons are not drawn during play (the regions show only in the CONTROLS page's
  preview); that needs the renderer or a hooked HUD draw.
- Touch has not been tried on a phone.
