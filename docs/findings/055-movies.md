# 055 — The movies play (round 40)

Status: partly superseded by 140 (movies shown at 4:3)
Checked: 2026-11-04

The startup movies (Sony and copyright screens, the Incognito logo, the attract movie) and
OPTIONS > VIDEO CLIPS now play on the PC with picture and sound. PC SETTINGS > Skip Movies
(dhd.ini `skip_movies`, DHD_SKIP_MOVIES) is the player's choice again, then 0 by default (today
the player default is 1, movies skipped: `skip_movies` in `port/render/settings/src/table.rs`,
since finding 101). The race,
gate and bench tools still set DHD_SKIP_MOVIES=1.

## 1. Why every movie crashed or hung

The game's player is Sony's "mpegstr" stream sample:
- `videodec` / `vibuf` (0x0023B050-0x0023E188): a thread runs libmpeg (`sceMpegGetPicture`,
  SDK region 0x0024C000-0x00253D38) on the IPU. The game itself writes the toIPU DMA chain
  (0x1000B4xx).
- A VBLANK handler (func_0020BED0 -> func_00224438) sends finished pictures to the GS over PATH3
  (DMA ch2, 0x1000A000 = 0x105). func_0020BDA0 sets DISPFB2/DISPLAY2, with DY 90 instead of 50:
  the 368-line picture is centred.
- `audiodec` (0x002422B0-0x00242998) streams the PCM into 989snd's external stream by SIF DMA
  (989snd commands 0x3B alloc, 0x3C free, 0x3D stop, 0x3E start, 0x40 position).

On the port, hardware registers are plain memory. There is no IPU and no IPU DMA, and the `_ri*`
IOP calls are stubs. So the PS2 path can't run at all, whatever its pointers do. Rounds 37/38
(finding 050) fixed the first crash (func_0023C1D0's five arguments), and libmpeg then had
nothing to decode with. An earlier guess, that the D_0029DB1C buffer addresses were raw PS2
addresses, is wrong: port/runtime/memory.c already relocates 0x29DB0C-0x29DB1C.

With the player replaced (section 2), two port bugs showed up:
- **OPTIONS > VIDEO CLIPS crashed** in `cdvd_find`, reading 0x00275500. The clip table
  `D_0028E960` (12-byte records {name, next record, sound}, read by func_001B53C0) has 27
  records. From the 7th record on, the names are raw words (`.word 0x00275500`): they point into
  one labelled block of "mov/*.pss" strings, 0x18 apart, so splat left them as numbers. The only
  reader of a name is the patched func_00223BB0, so host_movie_start maps an address below 32 MB
  to its host copy (PS2_RAM_BASE + address).
  - A first fix relocated the 21 words in port/tools/data.py (`RAW_STRING_POINTERS`, as for
    CONTROLS in round 37). It worked, but those 21 EE words then differed from the retail image,
    and the native renderer gate's reference with them. It was taken out: the table keeps the
    retail words.
- **A movie that ends in the frame it started hangs the game.** Both callers latch "a movie
  runs" from `D_0029DAFC` at the top of the next frame: func_001EBFD8 (startup) into D_0029D920,
  func_001B53C0 (VIDEO CLIPS) into D_0029D408. Only then do they wait for `D_0029DB00`. If
  func_00223C60 ended the movie at once (file missing, DHD_ISO unset), DAFC was 0 again before
  the latch, and the loading screen waited forever. host_movie_update now never ends a movie on
  its first call after the start.

## 2. The native player

The player's entry points are patched in port/patches.py (unit 00223BB0.c). No src/ change.
- `func_00223BB0(name, sound)`: `host_movie_start`, then the flags as the PS2 player leaves them
  while a movie runs: the sound mask `func_00201D50(0xFFFFFDFF)`, D_0029DB20 = sound,
  D_0029DC1C = 0, D_0029DB00 = 0, D_0029DAFC = 1.
- `func_00223C60(keep)`, once a game frame (keep 0 = the player pressed Cross/START to skip):
  `host_movie_update`. When it says the movie is over, the flags are set as func_00224308 leaves
  them: D_0029D9F8 = 0, D_0029DB00 = 1, D_0029DAFC = 0.
- The PS2 code below stays compiled and is never reached. The VIDEO CLIPS patch of round 37
  (a chosen clip ends at once) is gone.

port/hle/movie.c:
- A decoder thread reads the file from the disc image with its own handle (`cdvd_find` gives the
  LSN and size) and splits the MPEG program stream:
  - packs are skipped;
  - video PES 0xE0 goes to libmpeg2;
  - private stream 1 (0xBD) with sub-stream 0xA0 is the sound.
- Pictures go to 20 RGBA slots (0.67 s ahead). The conversion is BT.601 studio range in fixed
  point, with nearest chroma. Sound goes to a 2.7 s ring.
- At the end of the file the decoder feeds a sequence end code (00 00 01 B7), so libmpeg2 hands
  out the last picture.

The disc's movies, all MPEG-2 640x368 progressive frame pictures with 48 kHz PCM16 stereo:

| file | size | video | sound |
|---|---|---|---|
| DHSCEAPR | 2.2 MB | 4.0 s | played silent (sound 0) |
| DHCOPYRT | 4.9 MB | 6.0 s | played silent |
| DHINCOG1 | 4.7 MB | 5.0 s | 8.0 s (cut at the last picture, as on the PS2) |
| LOADING | 0.85 MB | | |
| ATTRACT | 110 MB | 122.5 s | yes |
| CRASHES | 52 MB | 66.4 s | yes |
| CREDITS | 92 MB | | yes |

- Frame rate code 4 (29.97) or 5 (30).
- Sound: an "SShd" header (type 1 = PCM16LE, 48000 Hz, 2 channels, interleave 0x200) and "SSbd",
  then 0x400-byte blocks, left 0x200 then right 0x200.
- The video leads the sound by up to 0.46 s, the sound the video by up to 0.54 s. The offset is
  taken from the first PTS of each.

**Timing.** The movie's clock is the number of samples the mixer has rendered since the start.
sound.c's `render` calls `movie_audio` for every chunk, 4004/5 samples a field, and adds the
movie's PCM after the voices. The picture due is `clock * 27 MHz / (48000 * frame period)`. The
presenter shows the last decoded picture whose time has come.
- Picture and sound can't drift apart: both follow one counter.
- A late picture keeps the previous one on screen; late sound is silence.
- A slow disc read never moves the clock.
- The ring overwrites its oldest sound rather than wait, so the decoder never stalls on it.
- A movie is over at its last picture: the PS2 player (func_0023C420, state 3) also cuts the
  sound there (INCOG has 3 s more sound than picture).

**Picture.** port/gs/present.c asks `movie_picture` each vsync and gets NULL when no movie runs.
- The exact path copies the 640x448 picture into its frame.
- The native renderer gets it through `gs_native_movie`. The render thread uploads it to the
  display texture (`native_movie_scanout`, the texture code shared with `native_scanout` as
  `native_display_size`) and presents it at 4:3 even in widescreen, as the PS2 showed it.
- The picture is centred in 640x448, 40 black lines above and below, as DISPLAY2's DY 90 does.

**Speed.** On this PC the decoder ran the whole 122 s attract movie in 4.4 s of wall time,
about 28x real time, on one thread. Nothing runs when no movie plays: one flag test per mixer
chunk and per vsync.

## 3. libmpeg2 and other options

Options weighed:
- (a) An IPU emulation, after PCSX2's IPU (GPL-3). It would keep the PS2 player, but needs the
  IPU FIFO/DMA timing, the IDEC/BDEC/CSC commands, the toIPU/fromIPU chains, PATH3 uploads of
  the CSC output and 989snd's external stream. That is weeks of work, and the result runs at the
  game's pace through the EE interpreter paths.
- (b) HLE at the player's entry points, with a small MPEG-2 decoder: chosen. The game sees the
  same flags as on the PS2.

The decoder is **libmpeg2 0.5.1** (https://libmpeg2.sourceforge.io/), GPL-2.0-or-later:
- its portable C core is in port/third_party/libmpeg2, with COPYING, AUTHORS and README.port;
- source: Debian's mpeg2dec_0.5.1.orig.tar.gz (sourceforge returned 404), sha256 checked
  against Debian's mpeg2dec_0.5.1-11.dsc;
- the port is GPL-3, which the "or any later version" allows;
- it is credited in port/release/THIRD-PARTY-NOTICES.txt;
- `config.h` is ours: no CPU-specific code; it builds with clang -O2 -msse2 in link.py.

One change: header.c's `default_intra_quantizer_matrix` is uint16_t, not uint8_t, with the same
values. The game executable holds the same MPEG-2 table (D_00281480, Sony's libmpeg), so as
uint8_t it was a 64-byte run of the ELF in dhd.exe and port/tools/scan_retail.py refused it.
The table is from the MPEG-2 standard, not game data, but the check stays as strict as it was.

PCSX2's IPU (build/port/compare-pcsx2-src/pcsx2/IPU) was read for the stream layout, not used.
Movie files come from the player's disc only. Nothing of them is in the repository or in
dhd.exe.

## 4. Tests

Standalone: port/hle/test_movie.c decodes a movie from the disc image on a simulated field
clock. It writes BMPs and a WAV.
- Build: clang `-DMOVIE_TEST` with movie.c and the libmpeg2 files.
- Results:
  - sceapr: 241 fields;
  - attract: 7341 fields, 3384 picture changes;
  - incog: 300 fields;
  - copyrt: 360 fields;
  - crashes: 3979 fields;
  - credits: 6876 fields, in 4.4 s of wall time.
- Every file decodes to the end; pictures and sound were checked by eye and ear.

In the game, on clean builds (`build_clean.py`) of a5c87e5c (exe 9e7213d70261a6f5) and 36d79fb1
(b04daf4e2aad74ef, without the data.py relocation). Menus were driven with DHD_NAV; screenshots and
WAVs are local, under build/port/r40movies/final:
- **Startup**, skip_movies 0 (`final/intro`): DHSCEAPR, DHCOPYRT, DHINCOG1 and ATTRACT play one
  after the other.
  - 300 fields take 5.0 s of host time: real time.
  - Screenshots every 300 fields: the copyright screen, the Incognito logo, the attract movie.
    They are centred with 40 black lines above and below.
  - `mix.wav`: silence over the two silent logos, then the INCOG sting and the attract music.
- **OPTIONS > VIDEO CLIPS**, skip_movies 1 (`final/clip`): the list opens and CREDITS plays with
  picture and sound.
  - Cross after 15 s skips it: LOADING, then the list again, as on the PS2.
  - The game runs in real time afterwards.
  - 36d79fb1 (`final/clip2`): INTRO (ATTRACT, a raw name word mapped by host_movie_start)
    plays, and Cross after 10 s goes back to the list. Earlier builds showed the same with
    skip_movies 0.
- Sound level: ATTRACT and CREDITS reach full scale in their own PCM (test_movie's WAVs). The
  game's mix of CREDITS averages a little below the source: nothing is mixed twice.
- `port/tools/scan_retail.py` on both exes: 14 runs, all from the decompiled source.
  `package_release.py` accepts a5c87e5c's (zip made under build/, not published).

**Gate.** `gate_native_renderer.py` on 36d79fb1. Its launcher, `race_compare.launch_port`, sets
DHD_SKIP_MOVIES=1, and the gate's log says "bypassing startup movie sequence". bench_race,
gate_gs_optimization, gate_hw_renderer, gate_vu1_native, compare_pcsx2, race_proof,
pcsx2_retail, lockstep_local and online_race set it too.
- Both arms match each other at every moment through GO+4500, with all 1,153 readbacks.
- Against the canonical reference r39-plain, two words differ at every moment:
  - 0x004F4324: 0x10 -> 0x24;
  - 0x004F4454: 0 -> 0x10.
- The UI worker's gate run of this round (build/port/r40ui/gate1) has exactly these two words,
  so they come from the widescreen / defaults / EXIT GAME commits, not from this work.
- So the gate fails on main until they are traced and a new reference is recorded. This round's
  own changes add no difference.
- With the data.py relocation of section 1, the 21 clip-table words differed too, which is why
  it was taken out.

Not done:
- The picture is shown at 4:3 in widescreen too, as the PS2 showed it. (Done later: movies fill
  a widescreen window with their own picture blurred at the sides, finding 140.)
- The movies' sound is mixed at the PCM's own level. The volume the PS2 player gives 989snd's
  stream was not checked against PCSX2, and no game volume setting applies to it.
