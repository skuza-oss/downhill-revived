# 140: Widescreen everywhere: no 4:3 screens left

Status: current
Supersedes: parts of 055
Checked: 2026-11-04

In widescreen, 3D, the HUD and the menus already filled the screen (findings 043 and 056). The
full-screen 2D pictures and the movies were still pillarboxed at 4:3 with black bars. Players on
16:9 monitors and 20:9 phones saw those bars at every boot and every race load. Now every screen
fills the window. Nothing is stretched, squashed or cut off.

Code:
- `port/render/src/native/present.rs`, `port/gs/native.wgsl` (`present_shrink`, `present_fill`)
  and `port/render/src/native/init.rs`: the fill;
- `port/gs/native_core.inc` (`native_present_rect`, `native_note_flat`) and
  `port/gs/gpu_rust_shared.inc`: when to fill, and a movie's own shape;
- `port/hle/movie.c`, `port/gs/present.c`: the rows a movie fills;
- `port/hle/widescreen.c`, `port/hle/kernel.c`, `port/hle/host.c`, `port/hle/widescreen_flat.h`,
  `port/patches_widescreen.py`: card mode, the loading card's text, and the goggles.

No `src/` change. With widescreen off nothing changes: the fill, card mode and a movie's own shape
all depend on it.

## Survey

The game was run hidden (`hidden_game.py`, `DHD_TEST_INPUT`, `DHD_LAUNCHER=1`, `DHD_MEMCARD` with
its own folder, movies on) in a 1280x720 window (16:9) and a 1200x540 window (20:9, the shape of a
2400x1080 phone). The captures are `DHD_CAPTURE_PRESENTED` pictures of the window surface, as
players see it, one every 0.5 or 1 s. The routes: boot with every movie, the title, the main menu,
OPTIONS, CONTROLS, PC SETTINGS, the online name page (local server), and a single race (DHD_NAV)
through the shop, loading, flyover, countdown, race and results (`DHD_RACE_FINISH_AT`).

| Screen | Before | After | How |
|---|---|---|---|
| Memory card warning ("memory card not found") | 4:3, bars; its text squashed by s | fills; text in its shape | blurred fill; card mode |
| Memory card check card (grey, at boot) | 4:3, bars | fills | blurred fill |
| SCEA, copyright and Incognito movies | 4:3 with the movie's own black bands, bars | the movie at its own shape (about 1.62:1), full height, sides filled | movie rows + blurred fill |
| Attract movie (at boot) | as above | as above | as above |
| Startup loading card (sunset rock and logo) | 4:3, bars | fills | blurred fill |
| Title, main menu, OPTIONS, CONTROLS, PC SETTINGS, online name page | fill (Hor+ 3D, centred 4:3 menus) | unchanged | - |
| Shop, rider select | fill | unchanged | - |
| Goggles before a level ("SELECT LEVEL", "GET READY TO RACE") | 4:3, bars; the Hor+ 3D and its text squashed by s | full width, in shape | not a card (widescreen_flat.h) |
| Race loading card ("Single Event / Mt. Middle") | 4:3, bars; its text squashed by s | fills; text in its shape | blurred fill; the card setup's 2D unmapped |
| Bike shop loading card ("Bike Shop"), not reached | as the race card (same setup code) | as the race card | as above |
| Flyover, countdown, race, results | fill | unchanged | - |

s is (4/3) / aspect: 0.75 at 16:9, 0.6 at 20:9.

Captures (game-derived, not in the repository) stay in the scratch folder (`DHD_SCRATCH`) of the
machine that ran this round, `before/` and `after/` by route and shape.

## The fill

Default method for a 4:3 picture in a wider window: the same picture, scaled to cover the window
(keeping its shape), blurred and darkened, behind the sharp picture in the middle. No stretch, no
crop of the picture itself, no bars.

It is part of the native renderer's present pass, so it works the same on every backend wgpu
drives (DX12, Vulkan, Metal, Android's Vulkan, the browser's WebGPU):
1. `present_shrink`: a 64x48 RGBA8 texture, each texel the mean of 4x4 filtered samples of its
   part of the picture (the part inside the cut: a movie's rows only, not its black bands).
2. `present_fill`, in the present pass before the picture: the small texture over the whole frame,
   the cut's rectangle scaled to cover it, under a 3x3 tent of filtered samples 1.5 texels apart,
   times 0.45.
3. `present_pixel` draws the sharp picture in the cut, as before.

The cost: 49,152 samples into the small texture and one full-screen pass of 9 samples from it, only
while a card or movie is up. `DhdrPresentRect`'s last word (`pad`) is now `fill`; the size and layout
are unchanged.

Decisions:
- **No PC SETTINGS row.** The fill only replaces black bars on screens that have nothing else on
  them, and the user asked for no 4:3 screens. A "Bars" choice would bring back what the round
  removes, so it isn't offered.
- **Movies at their own shape.** Every movie on the disc is 640x368, which the PS2 showed in the
  middle of 640x448 with 40 black lines above and below (finding 055). At 4:3 those rows are a
  1.62:1 picture. In widescreen it is now shown at that shape, as tall as the window (or as wide, in
  a window narrower than 1.62:1), and only the sides get the fill. At 16:9 the movie takes 91% of
  the width instead of 75%, with nothing cut off. With widescreen off a movie is shown as before:
  640x448 at 4:3 with its bands.
- **Cards keep the fill, not wider art.** The cards are single 4:3 paintings, tiled. Nothing
  outside 4:3 exists to lay out.
- `DHD_SCREENSHOT_WINDOW` (GS captures as a window would show them) draws the new geometry but
  leaves the fill black; `DHD_CAPTURE_PRESENTED` has the real picture.
- The browser build gets the fill. Card mode can't reach its core (below), so there a card's text
  stays as before.

## Text over a card: card mode

The survey found that text drawn over a 4:3 card was squashed: "Single Event" was 19.6% of the
card's width at 16:9 and 15.4% at 20:9 (ratio 0.79, the ratio of the two s). The game's 2D maps
its X for a wide picture (finding 056), and the card is then shown at 4:3 anyway. So two changes:

- **Card mode** (`widescreen.c`): while it is on, every 2D helper (`port_ws_point`, `_span`,
  `_size`, `_hud_point`, the text and glyph calls) returns its input, as with widescreen off. The
  renderer decides it: after `NATIVE_FLAT_FIELDS` (8) fields of 2D only in a row,
  `gs_native_card` says so. The core asks once a vsync through `host_frame_video`, which now
  returns it, and the answer covers the next field's drawing. Each field handed to the renderer
  records the mode its 2D was drawn in (`host_widescreen_card_drawn`), and the presenter shows a
  field at 4:3 only when it is 2D only *and* was drawn in card mode. Picture and text switch in the
  same field, both ways. This fixed the memory card warning, whose text is drawn every field.
- **The loading card's setup** (`func_00182D78`): it draws the card and its text ("Single Event"
  and the track through `func_0014DE90`, or "Bike Shop") for eight frames before the level loads.
  The loading thread (`func_00221608`) then sends the same packets again, field after field, until
  the level is in. So the text is drawn once, before card mode can know. `port_card_setup(1/0)`
  around that function (`patches_widescreen.py`) draws its 2D unmapped. The first fields of the
  card are still shown wide (below) and stretched, text and art alike.

## The goggles were squashed

Finding 072 raised the card limit to 4096 primitives in the shop exit (screen 18) and screen 9
phase 0, so the goggles before a level (3,648-7,500 VU1 primitives) counted as a card and were
shown 4:3. But the goggles are 3D through the Hor+ view: at 4:3 the lens was 0.885 of the picture's
width at 16:9 and 0.70 at 20:9 (0.79 again). Their text ("SELECT LEVEL", "GET READY TO RACE") is
menu text, mapped for a wide picture. So both were squashed by s, more on wider screens.
`widescreen_flat_limit` is now 1000 in those states too. The goggles are shown wide, and only the
tiled card (0 or 144-170 primitives) counts.

A field trace of the load at 16:9 (VU1 primitives per field; the 4:3 switches are logged):

| Fields | What | VU1 primitives |
|---|---|---|
| 1157-1228 | goggles zoom, SELECT LEVEL | 7,461 down to 3,794 |
| 1229-1349 | goggles hold, GET READY TO RACE; card tiles stream in at the end | 3,648 |
| 1350-1452 | the card (the loading thread) | 0 |
| 1453 | the card's last field | 144 |
| 1464- | flyover | 10,000+ |

## Flicker

Every switch is logged (`gs native: field N: a card, shown 4:3 and filled` / `the full width
again`, with the field's VU1 primitives and limit). In every run of this round each card switched
once in and once out:
- boot: on at field 10 (the memory card check) and on through every movie and the startup card,
  off at the title's 3D (field 2852 at 20:9);
- race: on at field 1361 (the race card, from field 1350), off at 1464 (the flyover).

The rule is still 8 fields. Card mode adds the round trip from the renderer to the game: a card is
shown 4:3 from about 11 fields after it starts (8 before this round). Until then it is shown wide,
stretched, as before. No field went back and forth. On the way out, the game learns 2-3 fields
late that 3D is back, so those fields show their 2D unmapped over the wide 3D (after a movie that
returns to a menu, for example).

## Checks

- Windows x64 build; Android arm64 build (`build_host64.py --target android-arm64`), not
  installed.
- The state trace (`state_trace_run.py race`, 5000 fields), this branch against main:
  - widescreen off: all 5000 rows match;
  - `DHD_WIDESCREEN=16:9`: rows differ from field 1352, the race card's setup. A word diff of EE
    dumps (`--dump 1352,4000`): at 1352 the 58 differing words are all in the font's text packet
    buffer `D_004F3AD0` (0x4F3B08 on), the card's glyph X coordinates now unmapped. At 4000 (racing)
    20 words differ, leftover glyphs in the same buffer (0x4F3D68-0x4F3E98). CPU, scratchpad,
    hardware registers, VU0, GS memory and sound are identical. The game's own state is unchanged.
- Widescreen off, the memory card warning at 1280x720: main and this branch are the same pixels,
  with black bars.
- `cargo test` (port/render): `present.rs`'s `fill_covers_the_frame`,
  `uniform_is_little_endian_f32`, `scissor_clamps_to_the_frame`; `tests/abi_layout.rs`.
  `test_dhdr_abi.py`: the header and `abi.rs` agree on x86_64 and wasm32, all 293 rows. It also
  reports `dhdr_surface_counter` with no `_fn` type in `abi.rs`, which predates this round.
- The surveys above, before and after, at 16:9 and 20:9.
- The core changed (`widescreen.c`, `kernel.c`, `widescreen_flat.h`, the patch): core id
  8b5309504879e976, so online rooms need both players on this build.

## Research

- Filling the bars with the picture itself, scaled and blurred, is what emulator frontends
  offer for the same problem: RetroArch's `borders/bigblur` shader
  ([libretro forum](https://forums.libretro.com/t/filling-pillarbox-space-with-scaled-blurred-game-output/11932))
  and MAME's bgfx `border_blur` chain
  ([MAMEWorld](https://www.mameworld.info/ubbthreads/showflat.php?Number=353114)); video players
  have the same idea (AmbientBorder for mpv and ReShade, github.com/AAbrains/AmbientBorder), and
  TV broadcasts do it for 4:3 or phone video. No code was taken from any of them: the two passes
  here are written for the native renderer's present pass.
- PCSX2's widescreen patches leave 2D and FMVs at 4:3 (finding 056's research).

## Left

- A card is shown stretched for its first 11-12 fields (0.2 s) before it switches, as before (8
  fields). Shortening the rule would risk switching on a lone field of 2D only.
- The browser build: its core's `host_frame_video` is a JS stub, so card mode never turns on there,
  and cards switch to 4:3 after 8 fields with their text mapped, as before this round
  (`NATIVE_NO_CARD_MODE`). The fill and the movie shape work there too.
- The exact (reference) renderer, `DHD_RENDER=exact`, still pillarboxes; players use the native one.
- 2-4 player split screens were not checked (no cards in them; `DHD_VIEW` quarters never fill).
- Online room pages beyond the name page, the career and other modes' loading cards (the same
  setup function) and VIDEO CLIPS were not reached in the survey; they use the same paths.
