# Round 40: texture replacement packs (upscale mods) in PCSX2's format

Status: current
Checked: 2026-11-04

The native renderer (`DHD_RENDER=native`) loads PCSX2 texture replacement
packs unchanged: a folder of PNGs named by PCSX2's texture hash (in PCSX2,
`textures\SCUS-97177\replacements`). Code: `port/gs/texpack.c` (names, index, loaders, budget),
`port/gs/gpu_native.inc` (`native_replace`, the sampler states), `port/gs/raster.hlsl`
(`native_replacement`, flag 2048); since the D3D11 renderer's retirement the Rust renderer draws
them (`native.wgsl`'s replacement flag, `dhdr_replace_create`), and since 2026-10-27 the PNGs are
decoded by the Rust library on every platform (section "One decoder"). Packs are their authors'
work: players supply them, nothing of a pack is in the repository or the release.

Sources: PCSX2 v2.8.2 (GPL-3.0-or-later), `pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp`,
`GSTextureReplacementLoaders.cpp`, `GSTextureCache.cpp` (`HashCacheKey::Create`,
`HashTextureLevel`, `SourceRegion::Create`, `LookupHashCache`), `GSClut.cpp` (`Read32`,
`Expand16`), `GSLocalMemory.cpp` (block sizes, `fmsk`), `GSState.cpp` (`GetTex0Layer`, MTBA), in
the local tree `build/port/compare-pcsx2-src` (finding 025). The scheme is reimplemented in C;
no PCSX2 code is copied. XXH3-64 is xxHash 0.8.3 itself (BSD-2-Clause, `port/gs/xxhash.h`, the
version PCSX2 ships). PRs read for the history: PCSX2 #5547 (the first texture dumping and
replacement, whose name had TCC in bit 14) and #5574 (replacement fixes, no hash change).

## The name

    <TEX0 hash>-<CLUT hash>-<bits>.png    paletted textures (PSMT8, T4, T8H, T4HL, T4HH)
    <TEX0 hash>-<bits>.png                the others

Hashes are lower-case hex without padding (`%llx`), bits are `%08x`.

- **TEX0 hash**: XXH3-64 (seed 0) of the texture's GS memory, as `HashTextureLevel` reads it.
  - Formats whose bits fill each 32-bit word (`fmsk` all ones: PSMCT32, PSMT8, PSMT4, PSMZ32),
    when the texture is at least one block (8x8, T8 16x16, T4 32x16): the raw 256-byte blocks it
    covers, row of blocks by row of blocks, left to right, each block from the format's block
    layout (`gs_pixel_address` at the block's corner, block number masked to 0x3FFF).
  - Otherwise (PSMCT24, CT16, the 8H/4HL/4HH formats, textures smaller than a block): the
    texels expanded, row by row: one index byte each for the paletted formats, a 32-bit colour
    (TEXA applied, as `gs_color`) for the direct ones.
  - A mipmapped draw (MXL > 0, MMIN a mipmapped filter, PCSX2's "full" mipmapping) continues the
    same hash over levels 1..MXL (at most 6), from MIPTBP1/2, each level's TW/TH reduced by its
    number as `GetTex0Layer` does.
- **CLUT hash**: XXH3-64 of the palette as `GSClut::Read32` leaves it, 16 or 256 32-bit
  entries: CT32/CT24 entries as stored (CSA applied; T8 clamps the group to 240), CT16 expanded
  with TEXA (`Expand16`: alpha TA0 or TA1 by bit 15, 0 for a zero colour when AEM).
- **bits**: `PSM | TW << 6 | TH << 10`, and for the 16- and 24-bit direct formats (and Z24/Z16)
  `TA0 << 15 | AEM << 23 | TA1 << 24`. Bit 14 was TCC in the first PCSX2 versions (PR #5547):
  every name in the user's pack has it set; PCSX2 now ignores it and so do we.

What we don't compute: PCSX2's region names (`-rWxH-`, region clamp narrower than the texture,
or its "recent upload" estimate) and the base-level shift of distant mipmapped draws (lod.x > 0
hashes from a smaller level). The user's pack has no region names. `-mipN` files are dumps of
mip levels; PCSX2 never loads them by name (they don't parse) and neither do we.

Lookup order: the name with mip levels (if the draw is mipmapped), the base level alone, then
both again without TEXA in the bits (packs dumped by versions that had no TEXA there).

## Results on the user's pack

The pack (someone else's mod, read in place, never copied): 6,181 base PNGs and 3,383 `-mipN`
dumps in one folder; no region names; T8 and T4 nearly all of it (plus 44 T4HH, 36 T4HL, 27 T8H,
1 CT24); every replacement 4x the GS size (512-2048 pixels a side); bit 14 set in every name
(dumped by an early PCSX2).

The scripted race (`race_compare.py`'s NAV, race pad and seed; menus, rider and track choice,
loading flythrough, race to field 7000) at 2x, `DHD_TEXTURE_PACK_LOG`:

| part | textures drawn | in the pack |
| --- | --- | --- |
| menus (fields < 1500) | 161 | 161 |
| track flythrough and race | 110 | 110 |
| all, by PSM | T8 173, T4 95, T4HL 2, CT24 1 | 271 (100%), 40 found with mip levels |

So the hash is right for block-hashed T8/T4, expanded T4HL (non-full `fmsk`), CT24 (expanded with
TEXA, the name's TEXA bits) and the mip-level variant. Draw states using a replacement: 749,363
over the run. What the lookup doesn't see: render targets used as textures (`from_surface`) and
the native renderer's unsupported textures (region repeat and the like) keep the game's own
pixels, as PCSX2 never replaces render targets either.

Loading and memory: 271 PNGs loaded, 24-62 ms each on a loader thread (WIC decode + mips + upload),
all during the menus and the track's flythrough. The race peaks at 738 MB of replacement textures
(R8G8B8A8 with mips) within the default budget, 1,536 MB on this PC (half the dedicated memory DXGI reports). Process
memory: peak private 356 MB with the pack against 346 MB off (the textures are in graphics memory).
With `texture_cache_mb = 200`: 228 replacements released and 339 loaded over the run, peak 243 MB
(textures drawn in the last 8 fields are never released), the race picture identical.

Pictures (build/port/r40tex, not tracked): `shots-off/`, `shots-aniso/` (pack on, final build),
`shots-on/` (trilinear without anisotropy, the first build), `cmp-*.png` side by side,
`crop-race.png`, `crop-menu.png`, `cmp3-f001600.png` (off / trilinear / anisotropic). Trilinear
alone softened the ground at low angles against the original (sampled without mips, sharp but
aliased); 16x anisotropic filtering brought most of the detail back.

Gate (on the first builds, based on 076de351; main later carries two other changed words, finding
055): `gate_native_renderer.py` passes with `DHD_TEXTURES=off`, and with the pack on (EE dumps
and 1,154 flare Z readbacks match: replacement alpha changes no Z the game reads back in this
race). With no pack or textures off the pictures are byte-identical to the base build 076de351 at
eight fields from the menus to the race.

Speed, measured on a busy PC (the user working, other workers' games and builds running; the
planned interleaved benchmarks were stopped part way, so these are a sanity check, not a
benchmark):
- Paced at 60 (`DHD_FRAME_CLOCK=0`), pack on, one run, fields 1000-4000 (rider and track menus,
  the flythrough where most replacements stream in, the race start): 59.56 fields/s, p50
  16.7 ms, p99 21.6 ms, worst 1% 30.4 ms, max 69.4 ms. 17 of 3,000 frames took over 25 ms. The
  big ones fall on menu screen changes where 41-55 new textures appear at once (fields 1120,
  1464, 1756-1767), with the GS worker busy (`DHD_TIMELINE`: the EE waits for it); two (1402,
  3170) have nothing loading. No comparable run with the pack off was made, so how much of
  those hitches is the pack (the game uploads its own textures at the same moments) is open.
- Unpaced race, fields 6000-7000, everything already loaded, three interleaved runs before the
  stop: pack on 110-119 fields/s, off 106-132 (median 116 vs 132); GS worker 7.0-7.7 ms per
  field on against 6.4-7.3 off. 60 holds with room.
- The partial paced race run with the pack off: 59.94 fields/s, max 21.2 ms.

## Loading

- At start the pack folder is indexed recursively, file names only.
- A texture's replacement is looked up once per decode (`NativeTexture.pack`), on the GS
  worker, when the native renderer decodes a new texture (the decode key changes with its
  pages or palette, so the PCSX2 hash is computed only then).
- The first draw that uses a found replacement queues it. Two loader threads (below normal
  priority) read the file (`host_file_*`), decode the PNG (the Rust library's `dhdr_png_decode`,
  the png crate; WIC until 2026-10-27, see "One decoder" below), build the mip chain (2x2 box),
  and hand every level to the renderer (`ReplaceDevice`, one texture). Until it is ready the
  original texture draws; outside a live race the first draw waits up to 200 ms a field for it
  (finding 086, F1).
- Uploaded replacements count against a budget (`texture_cache_mb`; default half the card's
  dedicated memory, between 512 and 2048 MB, chosen when the process was 32-bit). Over it, the least
  recently drawn replacements that no field drew in the last 8 fields are released (the same
  margin the native texture cache uses for queued fields), and load again when needed.
- Alpha: PNG alpha is GS alpha, as in PCSX2 (its PNG loader copies RGBA bytes and makes a PNG
  without alpha 0x80, opaque): 128 is opaque. The shader reads it exactly as a decoded texture.
- Sampling (`native_replacement`): the GS texture coordinate divided by the GS texture size,
  so the replacement covers the same normalised space whatever its size; `SampleGrad` with the
  coordinate's derivatives. 16x anisotropic when the GS filters (TEX1 MMAG), else point
  magnification with linear minification between mip levels. PCSX2 (default trilinear
  "Automatic") samples a replacement with mips only for draws the GS mipmaps, else clamps to the
  base level (`GSRendererHW::EmulateTextureSampler`, `sampler.lodclamp`); its anisotropy is off
  by default. We always use the mips, trading the shimmer of a 4x texture minified from its base
  level for some softness. Wrap/clamp from CLAMP; region clamp is applied
  in the shader, kept half a replacement texel inside the region; region repeat (an address mask)
  keeps the original texture. TFX/TCC, fog and alpha test are unchanged.

## Config

- `texture_pack =` the pack's folder (or a folder holding `replacements\` or
  `SCUS-97177\replacements\`, PCSX2's layout). Empty: `textures/SCUS-97177/replacements` in
  `DHD_DATA_DIR` (the launcher's folder for the player's files, where dhd.ini is) when it is set,
  else beside the game, if it exists. Paths are UTF-8, either slash.
- `textures = on|off` (default on; PC SETTINGS > Texture Pack, applied at the next start).
- `texture_cache_mb = N` the budget.
- Environment: `DHD_TEXTURE_PACK`, `DHD_TEXTURES`, `DHD_TEXTURE_CACHE_MB`; diagnostics
  `DHD_TEXTURE_PACK_LOG=file` (every distinct texture drawn: field, PSM, size, name, found).
- Without a pack nothing changes: `texpack_open` returns 0, no replacement is ever looked up,
  flag 2048 is never set.

## One decoder on every platform (2026-10-27, branch `desk/texpack`)

Until now `texpack.c` was Windows code (Win32 folder listing, WIC, `wchar_t` paths) and Linux and
Android built `texpack_linux.c`, which found the folder and loaded nothing; macOS would have had
neither. Now one `texpack.c` runs everywhere (finding 099's rule: one code path):

- Folders and files through the shared host (`port/host/host_sdl.c`): `host_dir_*` lists the
  pack (a folder is closed before its subfolders are read, so one listing is open at a time),
  `host_path_is_dir` finds the folder, `host_file_*` reads each PNG whole. Paths are UTF-8 with
  either slash. `host_sdl.c` now takes and gives back its file and folder slots under a spin lock:
  the loader threads open files while the game thread opens the disc and the card's files.
- PNGs are decoded by the Rust library (`port/render/src/png_decode.rs`, exports `dhdr_png_size`
  and `dhdr_png_decode` in `dhd_render.h`), the `png` crate 0.18.1 (MIT OR Apache-2.0; with
  fdeflate, flate2 1.1.9, miniz_oxide 0.8.9, crc32fast, simd-adler32, adler2, bitflags: all MIT,
  Apache-2.0 or Zlib, in the release notices through `package_release.py`'s crate licences).
  flate2 is held at 1.1.9 in the lock so it shares png's miniz_oxide (1.1.10 would add 0.9.1).
  Every colour type, bit depth and interlacing becomes RGBA8; 16-bit samples keep their high byte
  (libpng's `png_set_strip_16`); C passes the alpha for a PNG without any (0x80) and owns the
  buffers (the mip levels follow the picture in the same allocation, as before).
- What did not change: the names and hashes, the lookup order, the mip chain, the budget, the
  waiting and the lifetime (the code was moved, not rewritten).

The alpha rule. PCSX2 2.8.2's PNG loader (`GSTextureReplacementLoaders.cpp`, `PNGLoader`) reads
rows with libpng and handles only 8-bit RGB (alpha 0x80) and RGBA (as stored); other kinds come
out wrong there. Ours generalises it: a PNG with an alpha channel or a tRNS chunk gives its alpha,
one with neither gives 0x80.

Checks (`port/tools/test_texpack.py`, harness `port/gs/test_texpack.c`; synthetic PNGs only):

- Names: model names the harness computes from the texels it writes (CT32 and T8 below a block,
  T4 with a CT16 CLUT and TEXA, CT16 with TEXA in the name, mip levels) equal texpack's; the
  block-hashed cases (CT32, T8, T4, CT24, mipmapped CT32, T8H) equal the names the WIC build gives
  (`GOLDEN`). File names: bit 14 ignored, upper-case hex and `.PNG`, sub-folders, a duplicate,
  six other names skipped (`-mipN`, region names, other files), dot-folders not entered; found by
  the base name when the pack has no mip-level name, and without TEXA in the bits.
- The folder: `DHD_TEXTURE_PACK` as the replacements folder, a `SCUS-97177` folder or a textures
  folder (trailing slash, forward slashes), `DHD_DATA_DIR` (and nothing beside the game when it
  is set), beside the game, under a non-ASCII path (with dhd.exe's UTF-8 manifest).
- Pixels: 19 kinds (RGBA8, RGB8, RGB8 with tRNS, palettes of 8, 4 and 2 bits with and without
  tRNS, grey 8/4/1 bits, grey with tRNS, grey+alpha 8/16, grey 16, RGB16, RGBA16, interlaced RGBA8,
  palette and RGB16, odd sides) each equal, with every mip level, to what the script made. A
  damaged IDAT, a truncated file, an empty file, a text file and a 4097-pixel side fail and draw
  nothing (one "can't load" line each, the decoder's reason before it).
- Budget (`DHD_TEXTURE_CACHE_MB=1`, 256x256 replacements): the least recently drawn goes first and
  only down to the budget, a released one loads again, `texpack_field` releases stale ones on its
  own. Stress: 3,000 race fields (no waiting), 40 replacements over two loaders and a 1 MB budget:
  73 made, 61 released, 12 live; no view released within 8 fields of a draw that got it, none
  released twice, none returned after its release.

WIC against the Rust decoder (`test_texpack.py --wic-reference 89bee8823`, the base level as
each hands it to the renderer):

| PNG kind | result |
| --- | --- |
| RGBA8, RGB8, RGB8 + tRNS, palette 8/4 bits, grey 8/4/1 bits, grey+alpha 8/16, RGB16, RGBA16, interlaced (RGBA8, palette, RGB16) | identical |
| palette with tRNS (8 and 2 bits) | WIC drops the palette's alpha (all 128); Rust keeps it |
| grey 8 with tRNS | WIC: the keyed grey made black, alpha 128; Rust: the grey, alpha 0 |
| grey 16 | 40 of 128 texels 1 apart: WIC rounds 16 to 8 bits, Rust keeps the high byte |
| a damaged IDAT (its zlib header) | WIC loads something, Rust refuses it |

So the new decoder is the same for the kinds packs are made of (PCSX2 dumps 8-bit RGBA) and
differs only where WIC was wrong (tRNS) or by rounding (16-bit grey).

In the game (Windows, the packaged dhd.exe, minimised, `DHD_NAV` from the title towards a race,
60 s, closed with WM_CLOSE so the report is written): a first run with a placeholder pack logged
222 distinct textures (`DHD_TEXTURE_PACK_LOG`); 24 of those names (T8, T4, T4HL, T8H, CT24) were
given synthetic 128x128 checker PNGs in a scratch folder (nothing committed) and the second run
found all 24 by name, loaded 24 (0.4 ms each, none failed), and 40,046 draw states used a
replacement (2,097 in the first run: the sharp font's).

Release checks: `package_release.py` passes (the crates' licences in THIRD-PARTY-NOTICES.txt).
Its retail-bytes scan found one 67-byte run: DEFLATE's distance base table (RFC 1951), which the
game's own zlib has at 0x299538 and png's inflater has too; the linked-library exemption missed it
because the linker's zero padding around it differs. `scan_retail.py` now allows that one public
table (`PUBLIC`: the table and the zero fill beside it); `package_release.py` (locked) is unchanged.
dhd.exe grows from 26,373,120 to 26,485,248 bytes (+112 KB) against `desk/integrate` 89bee8823.

## Left

- A paced pack-off/pack-on comparison on an idle PC, to tell whether the screen-change hitches
  are the pack's. If they are: spread loads (fewer per field) or move the texture creation off
  the loader threads' burst.
- Texture dumping in PCSX2's layout (to make packs): not done.
- PCSX2's region names (`-rWxH-`) and the base-level shift of distant mipmapped draws
  (lod.x > 0): not computed; this pack needs neither.
- Render targets used as textures and the native renderer's unsupported textures are never
  replaced (as in PCSX2 for render targets).
