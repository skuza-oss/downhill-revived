# 116: Track thumbnails for the race room (research, not built)

Status: partly superseded by 132 ("not built": 132 built it)
Checked: 2026-11-04

Built later from this research: finding 132 reads each track's loading-page picture from the
player's disc and shows it in the room's track picker.

The race room's mock-up had a small picture of the track (finding 115). The room shows the
track name in large type instead. This is what the game has that could serve as the picture,
and what it would cost to use it.

## The only per-track 2D art: the loading screens

- No front-end screen traced so far loads a per-track picture. The level selector
  (`func_00139BF8`, finding 050) is the likeliest place for one; the career, pause and results
  screens were not checked. The "track preview" in finding 043 is the 3D fly-over before
  the race.
- The loading screens are the only per-track pictures. `LOADBAR/` holds 78 of them, and each
  one is 4 files:
  - `.TEX`: the picture, a 512×512 linear T8 texture;
  - `.RTX`: its palette;
  - `.PTR` and `.NGP`: the scene that draws it.

  Some of the files are packed in the game's IE deflate container. Finding 021 shows ALPINE
  loading `LOADBAR/LALPSE`.
- `func_00182330(track, mode)` chooses the name. It is a format string filled with the track's
  short name from `D_0026EF60[track]`, plus a mode suffix (`D_0026F0D8[mode]`) in some modes.
  A few modes and special cases have their own fixed screens. So every track has at least one
  picture.
- It is not yet known whether the pictures are clean shots of the track or carry text or
  logos. Check this by eye before relying on them.

## How the game loads and draws them

1. `func_001B7198` takes the name from `func_00182330`. `func_00182838` initialises the
   loading screen.
2. `func_001829C0` reads the texture and palette lists through `func_0020D290`, then sets them
   up with `func_00218E88` (texture relocation) and `func_00218D98` (texture banks).
3. `func_00182A88` loads the scene (`func_0020B088`, `func_00158680`). `func_00182D78` makes a
   scene object (`func_00208FB8`, `func_00218278`).
4. The load-bar thread `func_00221608` draws it as a scene through `func_00218720`, not as a
   menu sprite.
5. `func_001831A0` stops the thread and frees the scene. Nothing of it is resident while the
   menus run, which hold `SHELL/UI` (finding 050).

## Why it isn't built

- **Global texture state.** `func_00218E88` and `func_00218D98` change it, so running the
  loading-screen setup from the room would replace the shell's live textures.
- **The room would need its own path.** That means:
  - read and unpack the `.TEX`/`.RTX` pair from the player's ISO;
  - upload a 256 KB picture and its palette into GS memory the shell doesn't use;
  - draw it with a textured sprite packet that follows the finding 115 rules (heap packet,
    zeroed, a spare qword);
  - free it when the track changes or the room closes.

  None of these steps is traced or tested yet. The free GS memory while the shell runs is the
  main unknown.
- Estimate: about a day of agent work, for a decorative change. The large name stays.

## If this is picked up

1. Look at a few pictures (e.g. `LALPSE`) to see whether they work as thumbnails.
2. Map the shell's GS memory use while the online room is open, to find space for one T8 page
   and its palette.
3. Trace the game's texture upload (`func_002197F0`) to reuse it for one picture, then draw it
   as a sprite.

Sources: findings 021, 043 and 050, and the decompiled functions above. No outside code was
used.
