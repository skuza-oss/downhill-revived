# 022 — Audio: 989snd on libsd, native on the PC

Status: partly superseded by 099 (WASAPI output; the open lists of the rounds)
Checked: 2026-11-04

The protocol, data and mixer rules are current (`port/hle/sound.c`). Changed since: the host
output is SDL3's audio stream on every platform (`port/host/sound_out_sdl.c`, finding 099; WASAPI
and `link.py` are gone); the mixer gets wall time on the delta-time clock, so its sample count per
VBLANK follows real time rather than a fixed 4004/5 (finding 139, `port/hle/fps.c`); 989snd's END
grain, solo flags and instance limits now apply (finding 133). The "open" lists of each round
below were worked through in the later rounds of this finding.

Date: 2026-10-13. The PC build had no sound: `port/hle/iop.c` acknowledged the
sound driver's RPCs. This finding records what the game's sound system is and
the native replacement in `port/hle/sound.c`.

## What the sound system is

- **IOP side:** `cdrom0:\MOD\989SND.IRX;1` (169,632 bytes) next to Sony's
  `LIBSD.IRX` (28,557 bytes), both loaded by `func_0021F400`. Strings in the
  module say `989snd_Library`, `snd989`, `libsd` and
  `989snd (c)2000, 2001 Sony Computer Entertainment America (build with sce
  library rev %d.%d.%d)`. So the driver is **Sony's 989snd middleware** (the
  SCEA sound library) running on libsd/SPU2, not Incognito-custom IOP code.
  PCSX2's SPU2 (`pcsx2/SPU2`) is the chip behaviour reference; the driver
  protocol below is measured from this game's EE code, not assumed from libsd.
- **EE side:** the 989snd EE library in `src/unsorted/00201*.c`–`00202*.c`.
  `func_00201180` binds two SIF clients: `0x123456` (main RPC, queue) and
  `0x123457` (loading RPC), then sends function 0 with
  `{status packet D_004CD380, flags}`. Game code (`SKAT989.cpp` layer,
  `func_00208xxx`/`func_001C5040.c`) plays sounds **by 16-char name**
  (e.g. `S_NEXT`, `S_ENV_FAR_L`) inside a loaded bank, and gets a voice handle
  back through a per-command callback (`func_002013F8`, `func_00208C50`).

## The RPC protocol (measured)

- **Queued path (main server, function `0x4D`):** `func_00202048` appends
  `{short cmd, short len, data}` padded to 4 bytes after a count word in a
  double-buffered 4 KB queue; `func_00202388` sends it. The reply is one result
  word per command framed by `0xFFFFFFFF` words; `func_00201640` waits for the
  frame and `func_002013F8` invokes each command's callback with its result
  (a voice handle for plays). Direct calls (`func_00201E90`) use the function
  number as the command and read the result from reply word 1, with the same
  framing wait — a native reply must keep the frame or the EE spins.
- **Loading path (second server, function 3):** `func_00201718(lsn, 0)` sends
  `{lsn, 0}` and spins until the 4-byte reply differs from `0xFFFFFFFF`. There
  is no filename and no EE-side data: **the IOP driver reads the bank from the
  disc itself** (its strings show `sceCdRead` stream reads too). The reply is
  the bank handle the game stores (`D_004BD8A0`…) and passes back in plays.
- **Disc reads for the game loader** are queued command `0x38`
  `{lsn, sectors, EE address}` (`func_00202760`); completion clears
  `D_004CD380[0]` and puts the error in `D_004CD380[4]`. Already handled before
  this work and unchanged in behaviour.
- Commands seen live at the menu (payloads with `DHD_SND_TRACE=1`): queued
  `0x5C` play (48 bytes: bank handle, volume at +8 with `0x400` full scale,
  pitch at +12, the 16-char name at +16), `0x19` still-playing query (corrected below), `0x08` stop
  all, `0x09`/`0x0E`/`0x0F`/`0x64`/`0x4E`/`0x2C`/`0x21` settings and voice
  updates. Direct functions `0`, `8`, `42 (0x2A)` also occur. One port subtlety:
  the aggregate-ABI transform permutes the play name's 4-byte groups for some
  callers (e.g. `S_SELECT` arrives as groups at +8 and +20), so name lookup
  also matches by group set after the direct 16-byte compare fails.

## Data on the disc

- **`SND/*.BNK` — 989snd SBlk banks** (per-level `*_S.BNK`, `SHELL_S/SHELL_CH`,
  `SMUSIC01–04`, rider/pause banks). Header: six words (3, 2, 0x18,
  name-table end, waveform start, waveform length; the last two sum to the
  file size), `SBlk` at 0x18, then name records (16-byte NUL-padded name plus
  its index; measured `SHELL_S` has exactly the 7 menu names
  `S_CANCEL/S_NEXT/S_PREVIOUS/S_ENTER_GAME/S_BACK/S_HELI_SLIDER/S_SELECT`),
  then raw PS2 ADPCM waveforms, each ending at a block with flag bit 0 set.
  The native parser matches waveforms to sounds in index order. The full
  grain graph (a sound can have several grains/children, per the driver's
  `Didn't find child sound` string) is not parsed yet: multi-grain sounds
  currently play their first waveform.
- **`VAG/*.VAG` — streamed `VAGp` files** (48-byte header, big-endian data
  length and sample rate, name at +32, then ADPCM blocks): menu ambience
  `SHELL1/2/3` and the adaptive race-music stems `*LOW/*MED/*HIGH/*DRUM-L/R`.
- **`VAG/*.VPK` — packed streams** (` KPV` magic, sample rate 44100, 2
  channels at +16/+20): per-track music (`*MX/*TDH`) and 20 licensed songs
  (`01_LIC`–`20_LIC`). Mono/stereo interleave is now decoded; see the follow-up below.
- **`MOV/*.PSS`** movies carry their own audio; MPEG/IPU playback is still
  unimplemented (finding 021), so movie audio is out of scope here.

## The native implementation

`port/hle/sound.c` (declared in `sound.h`, wired as `rpc_sound`/`rpc_sound2`
from `iop.c`, ticked from `kernel.c` vsync, linked by `port/tools/link.py`
with `ole32.lib`):

- **ADPCM:** the SPU2 block decode (predictor table
  `{0,60,115/−52,98/−55,122/−60}`, `(nibble<<12)>>shift` filtered), integer
  only. Loop-start/loop-end flags are honoured for one-shot and looping
  voices; reverb is not modelled.
- **Voices/mixer:** 48 voices with fixed-point pitch resampling (SPU pitch,
  `0x1000` = recorded rate, banks assumed 22050 Hz), per-voice volume/pan and
  an SPU2-style ADSR envelope (libsd register decode plus a direct-step form).
  Mixing is integer arithmetic into 48000 Hz stereo; no floats anywhere, so
  every host renders bit-identical output.
- **Banks:** function 3 of the loading server reads the `.BNK` by LSN through
  `cdvd_read`, parses it and returns a fresh handle (1, 2, 3…), exactly where
  the EE expects it. Plays resolve bank handle + sound name to a waveform.
- **Streams:** `.VAG` images load whole and decode as game time advances them
  (the stream engine is covered by tests; which queued/direct command starts
  a disc stream is still to be identified — see below).
- **Host output (then):** WASAPI shared mode (plain C, COM) on a worker thread
  draining a half-second ring. `DHD_SND_OFF=1` disables it (headless/tests);
  with no audio device the thread exits and the game runs silently.

## Determinism

Audio never influences game state. `sound_tick()` runs once per vsync and
renders exactly `48000·1001/60000 = 4004/5` samples per frame from the vsync
count (integer Bresenham, no host clock). (Still so on the original clock that deterministic
runs keep; on the delta-time clock the mixer's time per VBLANK is wall time, finding 139.) Voice handles, bank handles, stream
positions and every reply word are functions of that count only. The WASAPI
thread only drains; if the host falls behind, rendered samples are dropped
from the ring *after* game-visible state advanced, and underruns play
silence. Disc reads for banks happen synchronously inside the RPC, as on the
real IOP, so load timing cannot leak host speed into the game either.

## What plays, and what is open

Verified live (`DHD_SND_TRACE=1`, scripted pad to the main menu): `SHELL_S`
and `SHELL_CH` load as handles, and menu plays (`S_SELECT`, `S_NEXT`,
`S_PREVIOUS`, `S_ENV_FAR_L`, …) resolve and start voices with no crash and
the game otherwise running as before. Audibility itself was not verified in
this headless session (no audio endpoint to listen on); the mixer output is
verified by the unit tests below.

Open, in order: (1) queued continuation streams and raw packed/movie audio;
(2) the full SBlk grain graph
(per-sound pitch/ADSR/volume, multi-grain and child sounds); (3) contracts of
the remaining direct functions and queued setters (currently they reply with
the same zeros the pre-audio build did, except the ones above); (4) ADSR
values from bank grains instead of the default envelope; (5) SPU2 reverb.

## Tests

`port/hle/test_sound.c`, run by `port/tools/test_runtime.py`: ADPCM vectors
computed from the SPU2 definition (silence, shift/filter cases, the
filter-1 prediction `(4096·60+32)>>6 = 3840`, flag pass-through), ADSR
attack/decay/release steps, synthetic SBlk parse + play, `VAGp` parse +
stream render, and mixer determinism (the same voice rendered in different
chunk sizes is byte-identical, and the sample position is exact).

## Disc-stream follow-up (2026-10-13)

The retail `989SND.IRX` dispatch table at IOP offset `0x1C5D4` resolves the
remaining stream entry points. This is evidence from the user's disc, not an
assumed libsd protocol; extracted IRX/disassembly and audio remain ignored.

- `0x2C` dispatches to wrapper `0xBB4`, then `0x11B00`: start/queue a stream
  at disc locations. The 32-byte request is `{lsn_left, lsn_right,
  (volume << 16) | byte_left, (pan << 16) | byte_right, group,
  previous_handle, queue_parameter, flags}`. The low halves are byte offsets
  within the sector. Flag bit 0 starts paused. Paired L/R VAGs have independent
  headers; a VPK uses the first location alone. Other flags and the queue
  parameter need further driver reconstruction.
- **Correction to the initial implementation:** `0x19` is
  `SoundIsStillPlaying`, returning the original handle while it exists, zero
  otherwise (wrapper `0x798`, implementation `0xDA94`). Treating it as stop
  made `func_001C54A0` restart menu music every update. Queries now preserve
  playback. `0x15` stops a sound; `0x2F` stops a stream.
- `0x13`/`0x14` pause/continue a sound; `0x2D`/`0x2E` pause/continue streams.
  `0x16`/`0x17` pause/continue streams selected by a group bit mask.
  `0x4F` reports stream readiness (1 when loaded, -1 for an invalid handle).
  Native loading is synchronous, so a successful start is ready immediately.
  `0x30`/`0x31`/`0x32` return position/time remaining/length in units of
  1/1024 second, using decoded source samples only.
- Direct RPCs now share the command dispatcher, as in retail (`0x177C`);
  formerly direct queries/plays always returned zero. Queue decoding rejects
  truncated command payloads instead of reading past the transmission.

[VGMStream's VPK parser](https://github.com/vgmstream/vgmstream/blob/master/src/meta/vpk.c)
identifies the header fields: per-channel bytes at +4, data offset +8,
interleave at +12 divided by two, sample rate +16, channel count +20 and
optional per-channel loop offset +0x7FC. This matches retail `01_LIC.VPK`:
44100 Hz, stereo, data at 0x800 and 0x5000-byte channel interleave. The port
validates header sizes and the last channel's physical extent before reading
or decoding. Mono/stereo VPK and mono/paired VAG streams are supported;
multichannel VPK is rejected explicitly.

Streams now resample at their recorded rate with an integer rational
accumulator. Repeating a source sample does not decode its ADPCM block again.
Pause, completion, status and loop positions advance solely through the game
mixer. VAG loop-start is flag bit 2 (4), with repeat bit 1 (2) on the end
block. Bank voices now use those same flags, including the first block, and
stop safely after emitting a one-shot's last block. Master-volume scaling
uses a 64-bit intermediate so 48 loud voices saturate correctly. Owned disc
stream buffers are reclaimed on stop/reset/slot reuse.

WASAPI now requests conversion of 48000 Hz stereo PCM to the endpoint mix
format, using the documented
[Windows shared-mode conversion flags](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants).
Endpoint/initialization failure and successful output start are logged under
`DHD_LOG`/`DHD_SND_TRACE`. No endpoint response affects game-visible state.

Verification: all 3,797 PC source units compile and the full link retains the
23 existing SDK stubs with no unresolved symbols. The runtime suite passes,
including sample-rate, stereo interleave, pause/resume, stop/status, truncated
RPC, first-block looping, non-looping start markers, saturation and five-vsync
(4004 samples) checks. Synthetic streams replay identically across render
chunk sizes. An ignored retail-data harness renders five seconds each of
`01_LIC.VPK` and `SHELL1-L.VAG`: respectively 479,511 and 479,788 nonzero stereo
output samples at 48000 Hz. This verifies actual disc payload decoding, not
speaker audibility or a race-music transition.

The native menu run (`build/port/audio-streams-final.log`) starts paired
`SHELL3IL/SHELL3IR` at LSN 754910/755387 and preloads paused `SHELL3BL/SHELL3BR`
at 756699/755864. Handles remain stable after fixing `0x19`; the run stops at
the harness deadline without a logged exception. A subsequent output-enabled
run (`audio-streams-output.log`) logs successful WASAPI initialization and
starts paired `SHELL1-L/SHELL1-R` at 760699/759602. No race is claimed.

Remaining: queued continuation into an existing handle (`previous_handle`,
currently rejected with a zero result rather than starting an untracked stream),
raw audio inside packed/movie resources, stream pan/group master volume and
other setters, full SBlk grain routing and bank ADSR, and reverb. Bank parsing
still assigns waveforms by index order rather than following the grain graph;
that is an approximation, not verified sound-by-sound fidelity. No matching
PS2 sources, `tools/`, renderer files or game data are changed/committed here.

## Audio review round 1: final-mix evidence and volume controls (2026-10-13)

`DHD_SND_WAV=<absolute path>` now writes the final 48000 Hz, stereo, 16-bit
integer mix before the WASAPI ring can drop samples. The mixer still advances
only by game vsyncs. Header lengths and PCM are flushed each tick, so the run
harness's forced termination does not require a normal-exit finalizer to make
the WAV playable. The Win32 signed `fseek` limit is checked before writing.
`DHD_SND_OFF=1` disables only the endpoint: WAV capture remains available.
Captures contain proprietary audio and must stay under ignored `build/`.

`DHD_SND_REPORT=<absolute path>` writes CSV rows with `command,calls,
unimplemented,unsupported`. `unimplemented` counts calls with no dispatcher;
`unsupported` counts partial operations within an existing dispatcher, including
queued stream continuations, unhandled angular pan/pitch flags, and bank group
volume (the bank parser still does not recover group metadata). IDs use the
full 16-bit command, avoiding the old first-seen log's low-byte alias/8-bit
wrap. These counters describe dispatch coverage, **not** sound-bank fidelity.
They do not count calls to the separate bank-loading server. The report and
WAV are updated on the game thread; no output-thread state affects replies.

The retail dispatch table and wrappers identify these contracts:

- `0x09` -> IOP `0x1A6C4`: group master volume, clamped to 0..1024. Group 15
  bypasses changes; group 16 controls the final master. Stream groups 0..14
  now use the quadratic gain identified in `0x1B144`. Bank group changes are
  explicitly counted as unsupported until proper SBlk routing is implemented.
- `0x1B` -> wrapper `0x7F8`, IOP `0xDBB8`: `{handle, volume, pan}`. The volume
  operation now affects stream and bank voices. `INT_MAX` preserves volume;
  negative stream values request an absolute 0..127 level (`0x18188`).
- `0x21` -> wrapper `0x90C`, IOP `0xDC94`: bit 0 selects volume, bit 1 pan,
  bit 2 auto-pan, bit 3 the short at +16, bit 4 the short at +20. Only volume
  and the existing centered pan behavior are handled here; other operations
  are reported. Setters retain playback position and do not restart envelopes.
- **Correction:** named-play `0x5C` has volume at +8 and **pan at +12**;
  pitch modulation/bend are shorts at +32/+34. The old mixer interpreted pan
  as a raw SPU pitch. Nonzero angular pan no longer slows the waveform. Pan
  and grain-based pitch are still open. The unordered name-word fallback is
  removed following the verified producer-ABI repair in finding 021.

Absolute voice gain and bank envelopes remain approximations. This round does
not implement the grain graph, group ducking, angular pan table, pitch setters,
queued streams, or reverb. In particular, an implemented volume dispatcher
does not establish that its resulting gain matches every retail sound.

Research used, as references only (no external implementation copied):

- [OpenGOAL 989snd player](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/player.cpp)
  and [block handlers](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/blocksound_handler.cpp):
  volume sentinel and grouped-handler contracts. The relevant player/loader
  code is ISC-licensed, Copyright 2021–2024 Ziemas.
- [OpenGOAL loader](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/loader.cpp):
  SBlk sound records point to grain programs; tones carry sample offsets,
  pitch/note and ADSR fields. This is the next routing reference, not yet used
  by the native parser.
  A read-only retail `SHELL_S` inspection agrees with the version-1 layout:
  seven sound records, thirteen grains, six VAGs; all seven sounds are in
  volume group 9. Thus assigning seven named sounds to consecutive waveform
  ends cannot be the correct mapping, even for this small menu bank.
- [PCSX2 ADSR](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/ADSR.cpp)
  and [reverb](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/Reverb.cpp)
  (GPL-3.0+): rate counters, exponential phases, and rotating-buffer reverb
  show why the current direct-step envelope and absent reverb cannot establish
  hardware fidelity. No GPL code is incorporated in this round.
- [ps2sdk libsd register definitions](https://github.com/ps2dev/ps2sdk/blob/master/common/include/libsd-common.h)
  and [vgmstream Sony bank parser](https://github.com/vgmstream/vgmstream/blob/master/src/meta/bnk_sony.c):
  cross-checks for voice/effect controls and sound/grain/wave table structure.
- [PCSX2 WAV dumper](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/Wavedump_wav.cpp):
  reference capture path. The installed PCSX2 has `[SPU2/Debug]` options
  `Global_Enable` and `Log_WAVE_Output`; its current configuration belongs to
  the concurrent comparison worker and was not changed.

Validation: standalone 32-bit `test_sound` passes, including existing decode,
stream/RPC and chunk-size determinism cases. New checks cover bank volume,
stream/group/master volume, sentinels, unchanged handles, the pan/pitch field
regression, readable WAV while still open, exact five-vsync PCM/headers, and
distinct unknown command IDs `0xAB`/`0x1AB`. Full `link.py` succeeds with 23
existing SDK stubs and no unresolved symbols; a private relink of the final
audio source also succeeds without `/force`. The combined runtime suite
stops before audio at the concurrent renderer's `_kernel_vsync_count` link
failure in the GS capture test. Its successful exit status does not mean the
entire suite passed; the independent audio suite was run separately.

The completed run uses the supplied `protect-round.ps1` pad sequence for 380
host seconds, with `DHD_SND_TRACE`, `DHD_SND_WAV` and `DHD_SND_REPORT` enabled.
It uses a private executable/map under `build/port/audio-round1-current/`;
the first attempt was discarded because a concurrent relink copied stale
audio code. The final private executable contains audio commit `58204199`.
`run-round.py` terminates at the deadline (Windows return code 1); no exception
is logged. Captures show SELECT LEVEL and then the Mt. Midoule waterfall/race
scenes. Menu `Shell1.wav` paired streams start, and paused `T1High.wav` stereo
race music is loaded alongside multiple 18/24 kHz announcement streams. Loading
this music is not proof that its transition or audible mix matches PCSX2.

Review artifacts (all ignored, containing retail data):

- `build/port/audio-round1-current/mix.wav`: 16,222,606 stereo frames,
  337.970958 seconds at 48000 Hz. Left/right nonzero sample counts are
  15,838,094/15,838,973. Left/right clipped counts are 7,663,930/7,661,458
  (approximately 47.2%). This is a **failed fidelity result**, not an audio
  parity sign-off. `menu-excerpt.wav` (game-audio seconds 60–80) has no clipped
  samples; `race-excerpt.wav` (270–290) is entirely +32767 in both channels.
  The race mix is severely wrong and must be corrected before judging music.
- `build/port/audio-round1-current/commands.csv`: final complete dispatcher
  counts. Missing commands: `0x00=1, 0x06=5, 0x0B=1, 0x0E=1, 0x0F=2253,
  0x2A=2, 0x4E=3, 0x60=4, 0x61=1, 0x64=1`. Partial-operation counts:
  `0x09=1115, 0x21=2400, 0x5C=25`. No queued `0x2C` continuation occurred
  in this run; its absence is not a test of the continuation implementation.
- `build/port/audio-round1-current-run.log`, `signal.json`, `missing-names.json`
  and `manifest.json` retain the evidence, sample statistics, unresolved names
  and executable/WAV hashes. `port/tools/audio_report.py` can inspect the WAV
  and command CSV without an audio backend.

The race bank at LSN 1225679 contains 127 sounds, 1,125 grains, 176 VAGs;
the native parser reports only 64 names. The log has 1,687 unresolved named
plays, including `P_DUM_USER09=900`, `E_HELI_FLYOVER=388` and
`B_AI_ROCK_RIDE=229`. These failed lookups are separate from missing-command
counts. Some names describe control/dummy grains, making a waveform-order
fallback particularly unsuitable. The source of the full-scale race DC has
not yet been isolated; grain/sample routing, bank gains and envelope fields
are the first areas to reconstruct. Suppressing this clipping with a blanket
master attenuation would hide the error rather than establish fidelity.

Next round: replace the bounded name scan and waveform-order pairing with
validated SBlk sound/grain/tone routing, including group and tone volume,
recorded pitch and ADSR; then trace the race DC to its contributing voices.
Implement bank unload and the observed settings/pitch/reverb operations,
then queued stream continuation. Capture comparable PCSX2 game audio with
aligned inputs; no such reference WAV was captured in this round. Speaker
audibility and complete menu-to-race audio transitions remain unverified.

## Audio review round 2: DC source, SPU levels and tone programs (2026-10-13)

The race DC was reproduced without the game or output endpoint. Several
incorrectly paired sounds (including `E_HELICOP_L`) selected a single 16-byte
ADPCM loop-ending filler block. It has repeat/end flags and constant positive
nibbles: the decoder correctly produced +28,672 and the voice correctly looped
it indefinitely. Two such voices add to 57,344, saturating the final mix to
+32,767. This was our sample-address selection bug; it was not a stopped
stream retaining its final sample, unsigned PCM, or evidence that missing
reverb inherently causes DC.

Commit `5b2a2a2d` replaces waveform-order pairing with validated version-1
SBlk tables. Names address sound records via the 32-bucket name table; each
sound points to its grain program; tone grains carry offsets into the sample
chunk. Bounds include metadata size, record/grain extent, wave alignment and
an in-bounds ADPCM end. Empty sounds' retail -40 grain-offset sentinel is
accepted only when their grain count is zero. The parser retains all 127
race sounds. A synthetic regression deliberately places a repeating filler
between two waveforms and reverses the tone order: addresses remain correct,
the one-shots end, and no filler/DC voice is started. A second regression
resolves a name at index 100. Retail offline inspection confirms the erroneous
single-block constant source disappears with the corrected addresses.

Commit `476ffb8a` adds sample-driven tone-program handlers (240 Hz, every 200
rendered samples). The driver handle belongs to a sound program, which can
have layered/delayed tones and child sounds; status no longer treats its first
waveform as the whole sound. Tone records supply bank/tone volume, group,
pan, center note/fine, bend range and ADSR. Waveform positions, envelopes,
grain delays, xorshift PRNG state and random/cycle selection history depend
only on command order and rendered samples. Newly created child handlers
cannot acquire an extra tick due to their position in the handler array.

Implemented grain operations include tones, delays, loops, random play/delay/
bend, register operations/tests, markers, wait-for-voices, cycles, key-off/kill
and same-bank children. Recursion and zero-delay programs are bounded, and
voice exhaustion/unsupported grains are reported in
`DHD_SND_REPORT`'s companion `.grains.csv`. LFO/noise and other unhandled grain
operations remain gaps. This is not a complete 989snd scripting implementation.

The gain audit follows the hardware and the retail 989snd normalization:

- ADPCM, envelope and volume arithmetic remains signed. SPU volume registers
  without bit 15 have a signed 15-bit value doubled to Q15; bit 15 selects
  a sweep with independent step/shift/direction/exponential/phase fields.
  Tests cover positive, negative and sweep modes. Tone gains use fixed
  registers; sweep support does not imply the EE exposes raw register writes.
- Multiplication uses signed Q15 shifts rather than division by 32,767. Dry
  sums use wide integers and saturate per sample before final master gain.
  The full master register is `0x3FFF`, Q15 32,766. Tests exercise 48 loud
  positive/negative voices and half-master output, ruling out wraparound and
  the old clamp-order error.
- Bank/tone gains and angular pans precede the quadratic group curve; that
  curve acts on the complete gain, not only the group multiplier. Mono disc
  streams use the same gain/pan normalization; stereo pairs retain distinct
  channels. There is no blanket attenuation or DC-removal filter.
- Tone pitch is derived from center note/fine, application modulation and
  bend, with the signed center-note PS1/PS2 rate distinction and SPU pitch
  cap. Lookup tables are generated from mathematical sin/exponential formulas,
  not extracted retail data. Gaussian interpolation is still unimplemented.
- ADSR decodes the actual separate phase fields and rate counters, including
  exponential attack/decay/release and sustain direction. Tone grains use
  their bank registers, not the direct-step envelope retained for core tests.

Commands: `0x09` now applies bank groups; `0x21`/`0x5C` use tone-program volume,
pan and pitch fields. Pause/continue/group-stop follow the program and its
voices/children. `0x06` unloads a bank only after invalidating all program/voice
references. `0x1F`/`0x20` handle bend/modulation; `0x0B` sets playback mode and
`0x61` stops selected groups.

**`0x0F` is SetReverbDepth**, not an effects-play command. Retail wrapper `0x58C`
calls `0xEF64`; selected cores receive left/right signed 16-bit depth writes
at `0xEFE0` (libsd selectors `0xB80`/`0xC80`). The register contract is now
handled and tested, including negative depth and independent core masks.
Wet DSP is still missing: nonzero requests explicitly increment the partial
operation counter. Reducing missing-dispatch counts to zero for this command
must not be interpreted as completed reverb rendering.

New saturation counters count actual dry-mix clipping before master scaling.
`audio_report.py` reports these alongside literal final-WAV endpoint counts;
the latter alone could undercount saturation when full master is 32,766.
Both measurements must be retained when comparing rounds.

The first race validation also revealed a **second end-state bug**. The retail
`EHigh.wav` streams contain a non-repeating ADPCM END block followed by a
repeating filler block inside the header's declared data length. Streaming
waited for the declared byte length, decoded past END, then looped the footer.
The preliminary race excerpt therefore still had a large positive offset
despite the bank-address fix. Commit `c19fcc7f` stops or loops a stream when
its decoded block ends, before considering the physical extent. Synthetic
one-shot and repeat/end regressions both append the same filler and prove it
is never emitted. An ignored retail harness renders 110 seconds of EHigh:
the last ten seconds have zero nonzero samples and zero mean, with the stream
inactive. No DC filter or footer-pattern muting is used; the fix follows the
ADPCM end/repeat flags. Full-capture measurements below use this final fix.

Two-core SPU routing and Gaussian interpolation are not reproduced fully by
the flattened native mix. That limitation, missing LFO/noise and missing wet
DSP remain explicit fidelity work; the tested arithmetic is not a complete
SPU2 sample-parity sign-off.

Research in this round:

- [PCSX2 Mixer](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/Mixer.cpp),
  [volume/ADSR definitions](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/defs.h)
  and [ADSR](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/ADSR.cpp):
  loop flags, signed Q15, saturation and register fields. GPL-3.0+ reference
  only; no PCSX2 implementation was incorporated.
- [OpenGOAL loader](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/loader.cpp),
  [grain semantics](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/sfxgrain.cpp),
  [voice management](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/vagvoice.cpp)
  and [pitch/pan reference](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/util.cpp):
  cross-checks for version-1 structures, 240 Hz programs, volume curves,
  parent/child operations and note units. ISC code was used as a semantic
  reference for a separate C implementation; tables were generated independently.
- The user's retail IRX dispatch/wrappers confirm group, playback, unload,
  pitch and reverb-depth contracts. Extracted bytes/disassembly remain ignored.

Standalone 32-bit audio tests pass: existing decode/stream tests, sample-address
and >63-name regressions, signed/swept levels, ADSR modes, delayed/waiting grain
boundaries, random-program replay across chunk sizes, signed reverb registers,
and unload while a program is delayed. Full shared link succeeds with its 23
existing stubs. Its executable then crashed at a boot callback before audio
playback, while the checkout's headers/runtime were changing; no shared code
was repaired by the audio worker. Validation was moved to a detached checkout
at round-1 `58204199`, with only the current audio files overlaid. All 3,797
units compile there and its full link also retains 23 stubs and no unresolved
symbols. Runtime/game-state determinism outside audio is not claimed.

Final review artifacts: `build/port/audio-round2-review/mix.wav`,
`menu-excerpt.wav`, `race-excerpt.wav`, `commands.csv`,
`commands.csv.grains.csv`, `signal.json`, `manifest.json` and
`build/port/audio-round2-review-run.log`. All are ignored. This executable
links the final audio object into the port worker's immutable
`build/port/fp-round2-snapshot` object set; that snapshot includes the EE float
and shared-header virtual-layout fixes (finding 021). Only a new private
response file, audio object, executable and map were written; the port
worker's snapshot was not modified. The manifest records source/executable/
WAV hashes. Earlier `audio-round2`, `audio-round2-verified` and
`audio-round2-final` captures are diagnostic attempts, **not** final results.

The earlier round-1-snapshot executable reached race gameplay but then hit
animation NaNs and an access violation at `0x402C0F27` reading `0x8893E212`.
Its private stack maps to `rc_func_002627A0`, 00204A98/00206AD8/00206440/
002065C0 and the 001B40A0/001B6708 race path, with no audio routines. No
shared game/runtime sources were repaired here. Once the port worker's
validated float snapshot became available, the same final audio implementation
was linked against that snapshot and the capture was repeated successfully.

Final measurements: 10,094,084 stereo frames, 210.293417 seconds at 48000 Hz;
literal PCM endpoint clipping **0%** and actual pre-master mixer saturation
**0%**, versus approximately 47.2% in round 1. Full-mix left/right peaks are
24,169/29,082. Race excerpt (game-audio seconds 190?210) left/right means are
-11.66/-16.35, with both positive and negative samples and no held-value/DC
plateau. Menu excerpt (60?80) means are 1.076/2.552. The retail 110-second
stream test separately verifies post-END silence. Licensed VPK and menu VAG
still decode nonzero stereo PCM in the five-second retail harness (479,460
and 479,734 nonzero samples respectively).

The supplied 380-second host-time pad script completes its deadline with no
logged crash. Captures show Mt. Midoule race gameplay and HUD, with bike,
landing/crash and announcement requests. WASAPI starts. Audio is still driven
only by delivered game frames; the slow renderer produces 210.29 seconds of
samples in 380 host seconds. Host-timed input, different game frame counts
and game-side random stream choices mean raw totals are coverage evidence,
not an equal-length native/PCSX2 PCM comparison or whole-game determinism proof.

Before -> final missing-dispatch counts:
`0x00 1->1, 0x06 5->0, 0x0B 1->0, 0x0E 1->1, 0x0F 2253->0,
0x2A 2->2, 0x4E 3->3, 0x60 4->4, 0x61 1->0, 0x64 1->1`.
Partial-operation counts: `0x09 1115->0, 0x21 2400->2270, 0x5C 25->0`.
`0x0F` still has **1,824 partial wet-DSP requests** among 1,825 depth writes.
Unresolved named bank plays: **1,687->1**. That single failure is a malformed
**direct** `0x5C` request: its volume/pan contain pointer-like values and its
16-byte name contains numeric/non-text values. Normal queued named plays
resolve, including all race names formerly outside the 64-entry scan. No
unordered/name-guessing fallback was restored for the corrupt request.

The final program engine executes 567 tone grains and 21 child starts, with
zero unsupported tone/child operations in this capture; its remaining
unsupported grain calls are **88 LFO grains (type 4)**. No queued `0x2C`
continuation was requested. Correct bank gains/ADSR, signed mixing and both
END/address fixes are covered by standalone tests; the final private link
has no unresolved symbols and the complete capture has no audio assertion.

The round is stopped for review. Next: trace the malformed direct request
with the shared ABI owners, address remaining auto-pan/stream parameters,
LFO programs and group ducking (`0x60`), then queued continuations and wet
reverb. Parent/child setter propagation, paused-stop lifecycle, instance
limits, imported/branch/stop-marker grains and physical core routing also
need reconstruction/coverage before claiming full 989snd compatibility.
Obtain an aligned PCSX2 audio reference; no such reference or speaker
audibility check is claimed in this round.


## Audio review round 3: retail reference, wet DSP and modulation

Retail PCSX2 audio is now captured independently of the native renderer. New
`port/tools/audio_pcsx2_capture.py` imports the comparison worker's
`pcsx2_retail.py` config/boot/runtime isolation and `compare_pcsx2.py` controller
serializer; it does not duplicate their launcher or edit their files. It uses
the existing version-2 adapter built from PCSX2 v2.8.2, software renderer 13.
The existing ROM0 BIOS is referenced in place, and the existing ISO is read via
`DHD_ISO`. Configs, emulator copies, decoded data and WAVs remain ignored under
`build/port/audio-*`. No BIOS, disc assets or upstream emulator implementation
is committed into the port.

An audio-only shadow translation unit is supplied to MSBuild through metadata,
without editing the comparison worker's downloaded sources. It writes
`spu2-mix.wav` before PCSX2's output DC filter, volume and time stretching;
`spu2-output.wav` includes the DC filter. `sample-clock.csv` maps every 4,800
samples to boot fields. `spu2-state.csv` records wet gates, effect depths and
buffer samples for both cores. Headers and data flush during recording, and the
normal adapter shutdown finalizes them. The final retail capture completes:
`build/port/audio-round3-reference-state/` (380 seconds, same frame-indexed pad
script as native). Both title/menu and race gameplay are independently booted.

Native evidence: `build/port/audio-round3-final/`, executable SHA-256
`e0ecf0f3a0585ed606b17baf65f6e7b186414f63ee1069e012a62a4a82e541ba`.
All non-audio objects are identical to the working round-three baseline;
only sound was replaced. A separate attempt that picked up concurrently
updated non-audio comparison hooks stalled on the loading screen; its output
in `audio-round3-native-after/` is not the race proof. The final run reaches
race gameplay without a logged crash, renders **380.38 seconds** of stereo
48000-Hz PCM, and stops its fixed-address launcher/child together. Native
`DHD_SND_OFF=1` disables the host device only; WAV rendering remains active.
Current source also links strictly in `build/port/audio-round3-build/`.
The capture predates only the diagnostic count for unexercised special stream
flags and the unexercised Echo/Delay default-parameter correction.

### Comparison measurements and their limits

`port/tools/audio_compare.py` saves RMS in 100-ms CSV bins, channel-averaged
Hann-window power spectra, band percentages and explicit WAV excerpts. No
implicit temporal or gain correction is applied. Findings are observational:

| Same boot-time window | Native RMS / peak | PCSX2 RMS / peak | Native RMS difference |
| --- | --- | --- | --- |
| Menu, 60-80 seconds | 1205 / 5882 | 844 / 5873 | +3.10 dB |
| Race, 350-370 seconds | 3207 / 28692 | 1470 / 10703 | +6.77 dB |

The menu window contains different transition phases. After boot second 65,
five consecutive 100-ms bins above RMS 32 begin at **65.0** natively and
**72.0** in PCSX2: sustained audio returns **7.0 seconds earlier** in native.
At approximately field 22,778, retail's race HUD reads **1:24**, while native
race-clock telemetry reads **12 seconds**. The 72-second race-state difference,
startup movie bypass in native only, and distinct music phases prevent these
windows from being interpreted as a clean master-volume calibration or an
aligned sound-event comparison. Menu envelope correlation is **-0.565**;
race correlation is **-0.074**. The first audible 100-ms bin is 45.0 on both.

Spectral differences are large: menu energy at 2-8 kHz is **18.77% native /
0.30% reference**, and below 100 Hz **48.47% / 77.00%**. Race energy below
100 Hz is **14.77% / 41.97%**, at 2-8 kHz **15.37% / 6.85%**, and above
8 kHz **2.42% / 0.48%**. These compare different audio content/phases; do not
attribute them solely to gain or interpolation. Establish matching menu/race
state and song position before adjusting gains or claiming PCSX2 parity.
The native race excerpt has means -2.74/-1.38 samples, real sign changes and
no constant/DC plateau. Full-mix peak is **32401**, RMS **1454.76**, endpoint
clipping **0%**, and pre-master saturation **0%**. Retained excerpts:
`menu-parity-native.wav`, `race-parity-native.wav`, and the corresponding
`*-reference.wav`, with JSON/CSV and `comparison.html` alongside.

### DSP and command coverage

The reverb is the register-driven SPU delay network: same/different IIR
reflections, four comb taps, two all-pass stages, signed-Q15 coefficients,
saturated delay RAM, alternating channels at 24 kHz and 39-tap down/up filters.
The game's `0x0E` core mask/preset and signed `0x0F` depths drive it. Voice
ranges (`0x4E`, plus allocator-mode `0x0D`) determine actual core routing;
tone flag 1 enables wet send, flag 16 selects wet-only playback. The final
capture produces **1,336,466 nonzero wet sample frames on core 1**. Menu wet
output can be zero while the game writes depths, because most menu voices
are on core 0. No invented send is added to every voice or stream.

A private harness invokes PCSX2's actual `Reverb.cpp` with its scalar FIR
reference, discarding only IRQ side effects. Against the native preset-3,
left-impulse run, **all 96,000 interleaved sample values match**, maximum delta
**0**. Oracle sources, executables and PCM are ignored in
`build/port/audio-reverb-oracle/`. This checks the network and scalar filters;
it does not claim bit-identical whole-game PCM or SIMD rounding parity.
Standalone tests additionally verify repeat/reset, independent cores, OFF,
wet-only tone routing and a room tail after the source voice ends.

LFO grains now support sine, square, triangle, saw and sample/hold random
waves; volume, pan, pitch-modifier, pitch-bend and modulation of another LFO's
rate/depth. The sine table is generated mathematically, not extracted.
Target-5/6 formulas were checked against retail IOP 0x2644/0x2704, since the
OpenGOAL reference leaves those targets empty. All phase, PRNG and driver
updates depend only on rendered samples. `0x21` smooth pan follows the shortest
arc at six degrees per driver tick (retail 0xDC94 -> 0x3708); immediate pan,
volume and pitch parameters work for bank handlers and streams. Stereo stream
pan routes the two channels through separate angular gains. Child setters
propagate, and stopping a paused handler allows its envelope release to finish.

`0x60` ducking follows allocated source groups, target masks, signed-Q16
attenuation, attack/release steps and minimum composition of four duckers;
retail setup 0x1B64C and the driver update at 0x1B3F8 establish the contract.
Native initialization, resident-stream buffer setup (`0x2A`) and stereo-output
SPDIF mode (`0x64`) are handled without creating IOP threads or hardware.

Queued `0x2C` continuations use a bounded FIFO of resident images, preserving
the parent handle and moving to the next cue on END without silence or footer
decode. A successful enqueue returns **1**, as retail 0x11E18 does, rather than
a new handle. Parameters inherit unless flag 64 requests per-cue volume/pan;
flag 16 requires a valid parent. Tests cover order, exact sample boundaries,
paused chains and stop/reset cleanup. No continuation is requested in the live
capture (23 stream starts, 23 matching start logs). Special flags 2/4 and raw
packed/movie-stream contracts remain explicitly partial when requested.

Before (accepted round 2) -> final missing/partial-operation counts:
`0x00 1->0, 0x0E 1->0, 0x0F wet 1824->0, 0x21 params 2270->0,
0x2A 2->0, 0x4E 3->0, 0x60 4->0, 0x64 1->0, LFO grains 88->0`.
The final run executes 5,086 depth writes, 1,948 parameter requests and 81
LFO grains, with **zero missing/partial counts for every exercised command
and grain**. Counts are coverage totals, not equal-duration native/retail
sample equivalence: round 2 used host-timed input and produced 210 seconds.

Unresolved named plays remain **1->1**. The direct `0x5C` packet still contains
pointer-like volume/pan and non-text name words. It is safely rejected, and the
new optional stack diagnostic is retained in the log; optimized x86 unwinding
stops in the RPC path and does not identify the producer. The trailing packet
word is not reliable proof of a caller return address. A shared ABI owner needs
to trace the 128-bit name producer; no matching sources were edited here.

Validation: standalone 32-bit sound tests pass, including new modulation,
ducking, FIFO, paused-release and wet routing regressions; private latest-source
link passes without unresolved symbols. The PCSX2 boot/capture completes,
Python capture/analysis tools compile, and the scalar reverb oracle matches.
The unrelated shared GS suite issue is not repaired or counted as audio work.
Remaining: align simulation/track state for gain/event comparison, Gaussian
sample interpolation, malformed producer, and unobserved noise/import/branch,
instance-limit and special-stream operations. Round stopped for review.

Sources and licensing:
- [PCSX2 v2.8.2 Reverb.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Reverb.cpp),
  [ReverbResample.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/ReverbResample.cpp),
  [Mixer.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Mixer.cpp),
  [spu2.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/spu2.cpp)
  and `spu2sys.cpp` document the reference network, mix gates and output path.
  GPL implementation is used only in the ignored emulator/oracle; native C DSP
  is independently written from the register equations.
- [no$psx SPU specifications](https://psx-spx.consoledev.net/soundprocessingunitspu/)
  describe the hardware equations and filter/register data.
- [ps2sdk effect.c](https://github.com/ps2dev/ps2sdk/blob/master/iop/sound/libsd/src/effect.c)
  supplied the preset register data at first. Since finding 053 the presets are
  no longer in the source: the mixer reads them at run time from the player's
  own disc (`MOD/LIBSD.IRX`, Sony's libsd, found by the table's shape).
- [OpenGOAL 989snd LFO](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/lfo.cpp),
  `lfo.h`, `sfxgrain.cpp` and `blocksound_handler.cpp` were semantic references;
  native modulation/handler code and mathematical sine data are independently
  written. Retail IOP disassembly supplied missing target and queue semantics;
  no disassembly or game bytes are committed.

## Audio review round 4: stream envelope gain (2026-10-13)

### Fixed gain and isolated PCSX2 evidence

The missing gain stage was the **stream ADSR envelope**, rather than a fixed
output shift. Retail 989snd writes ADSR1 **0x080F** and ADSR2 **0x1FC0** for
stream voices (IOP 0x172C8-0x172E4, with the stereo partner initialized the same
way). PCSX2's actual ADSR calculation attacks to 32767, then the decay tick
enters sustain around **16383**. Native VAG/VPK playback previously bypassed
the envelope entirely. Each stream now advances that envelope once per 48-kHz
game sample, freezes it while paused, and retains it across queued cues on the
same keyed voices. This fixes roughly **6 dB** of excess music gain without
changing bank-tone, master, pan or reverb depth gains.

The ignored private harness is `build/port/audio-round4-oracle/`, with its
build/run scripts in `build/port/audio-round4-*-oracle.py` and
`audio-round4-oracle.cmd`. It instruments the complete public PCSX2 v2.8.2
emulator through shadow translation units, reusing `audio_pcsx2_capture.py`
and the comparison worker's existing launcher/config/runtime adapter. The
original PCSX2 sources and other workers' launch code are not edited. BIOS is
referenced in place; game/BIOS bytes are never copied into the fixture.

On the first mixer call, the fixture suspends CPU/IOP execution and recursively
invokes **real `spu2Mix()`** for nine deterministic one-second cases, incrementing
its SPU sample cycle. It initializes both real cores, installs synthetic
filter-zero ADPCM in separate SPU RAM addresses (avoiding stale decoder cache
entries), keys a real voice, and exercises the actual decoder, Gaussian
interpolation, ADSR, fixed volume registers, core mix/clamps, core-0 external
routing, masters and `spu2Output()`. The raw pre-DC-filter PCM is retained.
It deliberately exits after 432,000 frames; the launch adapter's later missing
screenshot message is expected for this synthetic oracle, not a retail-game
capture result. The immutable reference runtime and WAVs are in
`build/port/audio-round4-tone-reference-stream-final/`.

Native counterparts render the same generated 107-Hz nibble-quantized sine
through an SBlk **0x5C tone grain** at known sound/tone/application volumes,
and through a VAG stream. No game asset is used. Both mixers render 48,000
frames per case; measurements exclude the first 4,000 frames. The known tone
registers are ADSR1 000F/ADSR2 1FC0, pitch 1000, full masters 3FFF, core-1
external volume 7FFF. Half application volume includes 989snd's 127-level
quantization; group-0 volume includes its quadratic law. Results are in
`tone-comparison.json`:

| Isolated case | Native peak | PCSX2 peak | RMS difference |
| --- | ---: | ---: | ---: |
| Full tone on core 0, hard left | 3585 | 3572 | +0.040 dB |
| Full tone on core 1, hard left | 3585 | 3572 | +0.037 dB |
| Half final master | 1793 | 1786 | +0.039 dB |
| Half application volume | 1779 | 1772 | +0.043 dB |
| Center pan, group 15 | 2535 | 2526 | +0.041 dB |
| Center pan, group 0 | 1793 | 1786 | +0.043 dB |
| Half group-0 volume, hard left | 897 | 893 | +0.048 dB |
| Stream, before envelope fix | 7168 | 3572 | **+6.063 dB** |
| Stream, after envelope fix | 3585 | 3572 | **+0.040 dB** |

The alternating-sample tone remains **+7.852 dB** louder: peak 3585 versus
1455. This exposes the remaining nearest-sample versus hardware Gaussian
filter difference. It is frequency-dependent, not a reason to halve all
bank-tone gains. Gaussian interpolation remains open for a later round.
Reviewer WAVs: `stream-before.wav`, `stream-native.wav`, `stream-reference.wav`
and `tone-0-native.wav`/`tone-0-reference.wav` in the oracle directory.

### Ducking, LFOs, transitions and diagnostics

`DHD_SND_GAIN_TRACE=<ignored CSV path>` now records sample-indexed 100-ms
bins of separate dry bank-effect, stream and depth-scaled reverb energies and
peaks, active-source counts, maximum voice gain, LFO-volume extrema, ducking
extrema and the final master. The companion `.events.csv` records stream and
volume/pause/stop commands with their game sample positions. It has no
wall-clock dependency and does not change the rendered PCM. Component RMS
values do not add, because the final mix contains cross terms.
`port/tools/audio_gain_report.py` summarizes these bins.

Volume LFOs only attenuate their base volume; final sound/tone volumes are
bounded at 127. Ducker composition takes the minimum of attenuations bounded
at 65536. A new constant-PCM regression checks the complete stream attack,
held level, pause/resume without re-keying, and a ducker returning to unity
with its target still allocated, without overshooting. Existing FIFO tests
also verify that queued boundaries do not restart the attack. The 32-bit
sound suite passes after replacing the old full-level stream expectations.

The accepted round-3 menu WAV has exactly **3.046958 seconds** of leading
zero PCM (window 60-80 seconds). PCSX2's corresponding window is already
playing/fading and has no leading zero interval. Native diagnostics show no
allocated active voices or streams at 60-63 seconds; the game requests its
next stereo stream at **63.0463125** seconds. The first nonzero output follows
about **0.646 ms** later. Thus the lead-in precedes the game's next audio
request, rather than an audio output/device delay. Retail and native menu
transitions remain at different game states; the silence does not match the
reference at the same boot-audio timestamp. Full event/song alignment is
explicitly deferred until race-reset and frame-sync fixes are available.

### Malformed producer identified

The frozen runtime's one rejected direct named play is now traced to the
**recompiled-to-native boundary in `func_001D1C58`**, at its call to
`func_00201B00` (EE call site 001D20FC). EE code loads a whole 128-bit name into
$a2; the generated caller nevertheless declares the third parameter uint32
and passes **`UW(6)`**. The actual transformed C builder expects **u128**,
so the name is truncated and the following native stack arguments move into
the wrong fields. Its Packet layout itself is correct. `recomp.py` deliberately
retains the old aggregate/quadword policy when resolving a `mode(TI)` typedef;
this specific boundary needs a real 128-bit argument contract, including any
other named-play builders reached from recompiled code. Shared ABI sources
are not edited here. No guessed-name fallback is restored; the malformed
packet remains safely rejected in this frozen run.

Sources: [PCSX2 v2.8.2 Mixer.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Mixer.cpp),
[ADSR.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/ADSR.cpp)
and `spu2.cpp` provide the actual private oracle paths.
[OpenGOAL 989snd vagvoice.cpp](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/vagvoice.cpp)
was used to check the volume/pan/group equations; retail IOP disassembly
establishes the stream envelope settings. Public GPL emulator code remains
in ignored build artifacts only. Native changes call the existing independently
written ADSR implementation; no new GPL DSP or game data is committed.

### Round-4 capture and validation limits

The complete attempt, `build/port/audio-round4-final/`, renders **380.81375
seconds**. Its native menu window at 60-80 seconds has left RMS **618.20**,
left peak **3966** (accepted round-3 same window: 1137.04 / 5200). Whole-run
peak is **9558**, endpoint clipping **0%**. It does **not** reproduce the
accepted race state: the game sets its audio groups to zero at roughly
256.3 seconds, the last active source bin is 257.0, and the 350-370-second
window is silent. These samples cannot establish a new race RMS, event or
spectrum comparison. No race-reset/rendering fix is attempted by the audio
worker. The run's gain trace has maximum voice gain **9726**, LFO-volume
range **-31..0**, ducker gains **65536** whenever updated, and no wet output
in the states reached. Nothing measured exceeds unity; live race-specific
modulation still needs the stable race run. The diagnostic-only pre-fix
prefix (`audio-round4-gain/`, 102.03525 seconds) is byte-identical to the
accepted round-3 PCM, verifying that the trace itself does not alter samples.

For listening without the live race state, the private harness also renders
one race-music paired VAG cue, referenced from the existing disc, at full
application/group volumes (1024), **without bank effects or game-state
alignment**. `race-stream-before.wav` -> `race-stream-native.wav` in the
oracle directory: left RMS **10293.37 -> 5165.71**, peak **32300 -> 16202**;
0.5-second RMS range **5602.95-11815.51 -> 2800.83-5918.82**, clipping
**0% -> 0%**. This checks the gain correction on real music; it is not the
post-fix counterpart of the previous live race excerpt. The remaining music
swings in this isolated cue belong to the source content, with identical
phase and volume requests before/after. Live effects/music/reverb balance
still requires the stable race state. All game WAVs remain ignored artifacts.

The forced-kill harness caught the full attempt's command CSV during an
in-place rewrite, leaving it empty. Command and grain reports now write a
temporary file and atomically replace the prior complete report after close.
A **90.07-second** validation run in `build/port/audio-round4-report-check/`
retains a complete command CSV after the same forced shutdown. Its calls
(missing dispatcher / partial operation) are:

| Command | Calls | Missing | Partial |
| --- | ---: | ---: | ---: |
| 00 | 1 | 0 | 0 |
| 06 | 1 | 0 | 0 |
| 08 | 3 | 0 | 0 |
| 09 | 37 | 0 | 0 |
| 0E | 1 | 0 | 0 |
| 0F | 773 | 0 | 0 |
| 19 | 771 | 0 | 0 |
| 2A | 1 | 0 | 0 |
| 2C | 1 | 0 | **1** |
| 4E | 3 | 0 | 0 |
| 5C | 3 | 0 | 0 |
| 64 | 1 | 0 | 0 |

Before -> after: missing dispatcher totals **0 -> 0**; exercised grain
partial totals **0 -> 0**; command 2C partial count **0 -> 1**, specifically
a **flag 0x04** stream request counted by the existing unsupported-flags mask.
The flag is retained as partial rather than guessed away. These are different
coverage windows, not equal-duration menu-to-race command counts. The full
attempt's grain CSV survives (118 tones, nine LFO grains, zero partial grains),
but its exact command totals are unavailable; no totals are reconstructed
from partial logs. The malformed race producer is identified above but is
not exercised in the post-fix scene path, so the previous **one** unresolved
play is not claimed fixed or reduced to zero.

Validation: 32-bit audio tests pass, including the new envelope/duck-release
and readable-dump checks; the private port links without unresolved symbols;
the nine-case PCSX2 actual-path fixture completes; the gain-report Python tool
compiles and summarizes the captures. Commits: `7a0f23c9` (stream envelope and
gain traces), `3378a065` (gain summarizer), `5305460c` (atomic command dumps).
Next: stable race/frame sync, Gaussian interpolation, stream flag 04 semantics,
and the shared 128-bit named-play boundary. Round stopped for review.


## Audio review round 5: Gaussian interpolation and quadword ABI (2026-10-13)

### Resampling and isolated PCSX2 evidence

Bank voices and mono/paired VAG/VPK streams now use the SPU2 four-tap
Gaussian interpolator. The phase selects one of 256 rows; each signed product
is shifted by 15 **before** the four terms are added in 32 bits. Streams now
advance a Q12 source position, using the quantized hardware pitch rather than
an exact rational sample rate. Retail header parsing at IOP 0x154C4-0x154D8
computes `(frequency << 12) / 48000`, storing the integer pitch before applying
modifiers. A 44.1-kHz stream therefore uses 3763, rather than an indefinitely
precise 0.91875 increment.

The next ADPCM block is decoded into a preview with a copy of its predictor
history. Previewing does not advance the live decoder, handle, queried position
or envelope. The four-sample window crosses block boundaries, repeat points
and queued cues. Ending one-shots supply zero lookahead instead of reading a
footer or retaining a held sample. Late enqueue invalidates the preview; a cue
handoff retains the fractional source position as well as its envelope.

The 1024 coefficients are adapted from OpenGOAL's ISC-licensed
[interpolation table](https://github.com/open-goal/jak-project/blob/master/game/sound/common/interp_table.inc),
with its [license](https://github.com/open-goal/jak-project/blob/master/LICENSE)
retained in `port/hle/audio_licenses/opengoal-ISC.txt`. All coefficients were
checked against PCSX2 v2.8.2's
[interpolate_table.h](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/interpolate_table.h).
The native integer kernel was independently written; PCSX2's GPL code remains
in the ignored oracle only. Hardware reference:
[PCSX2 Mixer.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Mixer.cpp),
`GetVoiceValues`, `DecodeSamples` and `MixVoice`.

Private artifacts: `build/port/audio-round5-oracle/`, built by
`build/port/audio-round5-oracle.cmd`. `stage.cpp` uses the actual unchanged
PCSX2 `GetVoiceValues` with its real table. Across **65,536 windows**, covering
all 256 phases, positive/negative extremes, deterministic random samples and
FIFO wrap positions, the native and PCSX2 int32 outputs have **zero unequal
values and zero maximum error** (`native-stage.pcm`, `pcsx2-stage.pcm`).

The nine-case actual-decoder/mixer/output fixture from round 4 was rerendered.
The core-1 looping tone is **byte-identical over all 48,000 stereo frames**,
including startup (`tone-1-native.wav`, `tone-1-pcsx2.wav`). In the remaining
core-0/master/pan/stream fixtures the largest sample difference is two units;
RMS differences range from **0 to +0.0123 dB**. The alternating tone's RMS
error improves **+7.852 -> +0.00597 dB**; native/reference full-clip peaks both
measure **2908** (steady-state peak 1455). See `comparison.json` and
`tone-7-{native,pcsx2}.wav`. The residual two-unit discrepancy comes from the
existing flattened core-0/master/output routing; no global normalization was
introduced to hide it.

This does **not** assert full SPU2 voice parity. Native one-shot lifetime still
ends at its declared ADPCM block boundary; PCSX2's physical decode FIFO can
observe END during prefetch and stop earlier. Those finite-END timings are
outside the continuous-tone proof. Physical voice allocation/core routing,
all fractional bank pitches and live race reverb balance also remain separate
coverage from the bit-exact interpolation stage.

The same real paired race VAG cue from round 4 was rendered directly from the
existing ISO, without changing gain or game state, into
`race-stream-native.wav`. Comparing the first 20 seconds against the round-4
post-envelope cue: stereo RMS **5094.51 -> 5026.40** (**-0.1169 dB**), peak
**16287 -> 16123**, clipping **0% -> 0%**. Relative spectral energy above
8 kHz changes **1.3088% -> 0.5549%**; 100-ms RMS correlation is **0.99917**.
`race-filter-comparison.json`/CSV retain every bin and all bands. This comparison
isolates the filtering/pitch-quantization change; its reference is our round-4
render, **not** an aligned PCSX2 race. WAVs remain ignored game data.

### General scalar quadword boundary

The malformed name came from generated `func_001D1C58`: native
`func_00201B00` expects a 128-bit scalar name in $a2, but the generated call
passed `UW(6)`. The native i386 argument stack then lost the name's upper words
and the subsequent volume/pan/pitch layout. This is a general scalar ABI bug,
not a sound-bank lookup exception.

`port/tools/recomp.py` now resolves mode(TI)/signed/unsigned scalar typedefs to
`i128`, preserves a whole EE GPR in both boundary directions, and handles
128-bit returns through $v0. Stack quadwords occupy aligned 16-byte slots; the
wrapper reserves their full extent and starts existing word-varargs after
that extent. The cache version advances to 4 so stale low-word contracts are
regenerated. `port/runtime/quad_abi.h` bridges the emulated union and native
mode(TI) scalar using 16-byte memcpy, avoiding alignment/alias casts. Untyped
variadic values and aggregates retain their existing conventions.

`python port/tools/test_quad_abi.py` compiles real, separate i386 translation
units. It checks 64 full names with high words set, a 48-byte play-shaped
packet, following negative pitch/scalar arguments, a 64-bit key, signed aliases,
whole-quadword returns, interspersed stack spills at offsets 0/16/32/48,
stack restoration, fixed stack quadwords followed by word-varargs, and stale
signature refresh. It passes. Existing scalar/float guards, 845 animation
callback cases, timer-thunk argument/return and data alias tests also pass.
The shared GS runtime-suite issue was not changed.

The ignored `audio-round5-producer.py` additionally links the **real existing**
`func_00201B00` packet builder with generated before/after call sites and the
native audio dispatcher, using a synthetic 16-byte bank name. Before: one
malformed/unresolved play. After: zero, correct full name/volume/pan/pitch
payload, and a successful bank handle. This is an isolated **1 -> 0** proof,
not a claim that the former live race trigger was replayed. The actual
`001D1C58` was regenerated and included in the private menu executable.
No `src/` file changed. The headers worker should sweep other typed scalar
quadword callers and regenerate them; this fix applies generally to arguments
and returns, not only 989snd. Own `recomp.py` hunks were staged with
`git apply --cached`; the other worker's preexisting blank-line edit is excluded.

### Command 2C flag 04 and live report

Retail RPC flag `0x04` maps to stream-control flag `0x400` at IOP 0x1212C;
queued cue promotion propagates it at 0x123F0. At EOF, 0x15D08 checks that flag.
Large files reset the current data cursor from the original one at 0x15D94
and the next LSN from the initial LSN at 0x15DA8. Small resident loops take
0x15EAC -> 0x175C0: first-block flags become 6 and the last block becomes 3.
This identifies **whole-cue looping**, independent of the source file's ADPCM
repeat bits. Import 0x1BB20 is sysclib ordinal 12, memcpy, confirmed by
[ps2sdk sysclib.h](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/sysclib/include/sysclib.h).
No driver bytes/disassembly are committed.

Native flag-04 loops rewind the cue without re-keying, preserve decoder/envelope
history, and hold queued successors until stopped; the retail loop branch
precedes EOF cue handoff. The unsupported flag mask now recognizes bit 4.
Tests cover a 44.1-kHz loop with a predictive second block marked END without
repeat, continuous decoder history at every seam, identical 480-sample output
in whole/17-sample chunks, a waiting queued successor, stop freeing the chain,
and the same cue ending normally without flag 4. Existing pause, VPK, stream
END/footer, queue, saturation, reverb, modulation and duck-release tests pass.

Live artifacts: `build/port/audio-round5-final/`, including `mix.wav`,
`menu-excerpt.wav` (60-80 seconds), command/grain CSVs, gain/event traces,
report, manifest and private strict executable/map. The comparison worker's
`pcsx2_retail.run_native` and standard frame-indexed pad script are reused.
The run renders **90.156729 seconds**; WAV and pre-master clipping are both
**0%** (whole-run left/right peaks **4089/4286**). The frozen non-audio objects
are the round-3 baseline, except regenerated `001D1C58`; executable SHA-256
`67aed14d5e87d5fdcd08300ed158ad596b3e20bebfccb4e97831205b6c841f66`.
A final rate-rounding ordering cleanup only affects nonzero stream modifiers;
this run requested none (the event trace records the sole 2C request and no
21 setter), so the captured stream pitch is unchanged. Current source is
also compiled/linked separately and exercised by the final isolated fixtures.

Counts compared with the round-4 90-second report (durations differ slightly):
00=1->1, 06=1->1, 08=3->3, 09=37->37, 0E=1->1, 0F=773->777,
19=771->775, 2A=1->1, 2C=1->1, 4E=3->3, 5C=3->3, 64=1->1.
**Every ID has zero missing-dispatch calls; 2C partial operations 1 -> 0;
all other partial operations 0 -> 0.** Exercised grain partial operations
remain zero; there are 35 tone and seven LFO-grain calls. No unresolved named
play appears in this menu log. These are menu counters, not a full race replay.

Commits: `c226c3a8` (general scalar quadword ABI and regressions),
`ae2a5c45` (Gaussian resampling, cue loops, tests and ISC table/license).
The live race reset and full event/song alignment remain with the port and
comparison workers; no renderer/reset change or new aligned-race claim is
made here. Next: the headers-worker quadword sweep, then stable frame-aligned
race music/effects/reverb comparison. Round stopped for review.

## Audio review round 6: menu requests, EOF and CPU cost (2026-10-14)

### Synchronization and capture scope

Exact race-state lockstep remains open in the comparison worker's adapter.
The port worker's local `race-proof/r3-round7-trap/manifest.json` reaches GO
at 291.95 wall seconds, then exits with access violation at 319.88 seconds;
other recent head proofs remain in phase 5. Neither establishes a stable long
race. The authorized fallback was therefore a 210-second menu comparison.
The headers worker retains the quadword sweep; no ABI tooling or src changed.

Native capture: `build/port/audio-round6-menu-native/`. Independent retail:
`audio-round6-menu-retail/`, manifest complete. Both reuse the comparison
worker's input/movie/boot/capture code, with triangle at 45 s, cross at 65 s,
and short alternating down/up pulses at 110/125/140/155/170/185 s. BIOS and ISO
are referenced in place. The retail WAV is the raw SPU2 mix, before the output
DC filter and host conversion; no gain normalization or time stretching.

New `audio_pcsx2_requests.py` observes complete SIF RPC packets and returned
handles through ignored shadow Sif1/Sif0 units, using the WAV's sample clock.
It does not modify DMA, RAM, driver requests or game execution. There are
19,525 requests and 19,488 observed result entries. The other 37 results remain
unknown; partial FIFO fragments are not guessed or counted as parity passes.
Protocol references: [ps2sdk RPC layout](https://github.com/ps2dev/ps2sdk/blob/master/common/include/sifrpc-common.h)
and [command layout](https://github.com/ps2dev/ps2sdk/blob/master/common/include/sifcmd-common.h).
The upstream [Sif1](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Sif1.cpp)
and [Sif0](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Sif0.cpp) implementations
remain GPL in the ignored emulator tree. No upstream DMA code enters the port.
Shadow units use private PDB/PCH settings and avoid PCH ordering dependencies.
Short capture schedules now omit screenshots beyond `--until` and leave neutral
movie input while asynchronous shutdown drains.

| Event, on each audio clock | Native game | Retail | Native minus retail |
| --- | ---: | ---: | ---: |
| First S_SELECT request | 45.0283125 s | 45.0262917 s | +2.021 ms |
| First S_SELECT PCM above one unit | 45.0322708 s | 45.0341042 s | -1.833 ms |
| First menu stream request | 63.0463125 s | 71.8216042 s | -8.775292 s |

These observe native dispatcher execution versus retail SIF arrival, not EE
call-entry lockstep. Native requests SHELL1-L/R with flag 4; retail requests
SHELL3IL/IR and paused, looping SHELL3BL/BR. The live songs therefore differ.
Aligning the first music requests and comparing 130 seconds gives 100-ms RMS
correlation -0.000578 and level difference +0.8128 dB. This measures a scene/
content discrepancy, not same-song fidelity. Native emits six navigation plays
for the down/up pulses; retail emits none. Both contain the initial UI sound.

The final audio source also strictly links in a private frozen-baseline port
executable and runs to 100 seconds (`audio-round6-menu-final/`). Its entire
100.183417-second PCM prefix is byte-identical to the long native capture.
Its new 5C event trace supplies the native UI request timestamp above.

### Retail-request replay and an actual stream EOF bug

`port/hle/replay_sound.c` replays the recorded requests at their sample indices,
loads assets from the existing ISO, remaps observed handles, and writes exact-
length PCM plus command/result reports. Unknown replies stay unknown. Disc-to-
EE writes are rejected. This compares audio under retail requests; it is not
a replay of native game/renderer state. All game payloads/WAVs stay ignored.

Both SHELL3 intro VAGs end with ADPCM flags 3. Their RPC lacks flag 4, and the
retail loader rewrites physical streaming-buffer flags (IOP 15E94 -> 175C0).
Native incorrectly inferred file repeat from the ADPCM marker, leaving the
intro playing on top of the bed. Disc-managed VAG one-shots now end; RPC bit 4
selects whole-cue repeat. Explicit VPK header loops and direct static ADPCM
test voices retain their behavior. Regressions cover END+REPEAT one-shots,
queue handoff, explicit file loops, pause and cleanup. Looping setter/ducker
fixtures now explicitly request looping instead of depending on raw markers.

| 130 seconds after the retail music request | Before EOF fix | After | Retail |
| --- | ---: | ---: | ---: |
| Stereo RMS | 852.22 | 715.02 | 668.83 |
| Level error | +2.1047 dB | +0.5801 dB | reference |
| 100-ms RMS correlation | 0.26710 | 0.17415 | reference |
| Clipping | 0% | 0% | 0% |

The low correlation is still a failed full-menu parity result. Overall gain
improves, but the envelope does not become aligned. A bounded +/-0.5-second
waveform search finds native intro content about 204.667 ms early, consistent
with immediate resident playback versus retail cold disc-buffer startup.
An explicitly recorded reference offset of 204.667 ms for the first 30 seconds
gives correlation 0.66837 / level error +0.6709 dB. It is supplemental, never
silently applied to the main score. Ambient grain/program/RNG timing remains
unisolated; native xorshift determinism is not proof of retail RNG parity.
No global attenuation or invented startup sleep was introduced.

The request-relative 0.8-second S_SELECT comparison is much closer: 100-ms RMS
correlation 0.999472, level error +0.02242 dB, peaks 1546/1544. This holds for
both the live native UI and the retail-request replay. Listening pairs are
`ui-live-request-aligned-{native,reference}.wav` and the long
`menu-final-{native,reference}.wav` under `audio-round6-replay/`.
JSON/CSV reports retain every 100-ms bin and spectrum band.

After the repeat fix, 12 still-playing polls return false earlier than retail,
confined to intro completion. The roughly 205-ms cold-start/driver completion
difference is separate from Gaussian filtering or physical END FIFO timing.
Deterministic buffering/readiness/KeyOn scheduling remains needed.

### Physical END-prefetch: remaining behavior and audibility

PCSX2 [Mixer.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Mixer.cpp)
prefetches in four-sample steps until more than 12 source samples are buffered.
A nonrepeat END stops the voice with FIFO samples still pending. Native static/
bank voices stop after consuming the declared block. The ignored
`audio-round6-end/prefetch.cpp` extracts unchanged PCSX2 header/address/prefetch
functions, substituting known filter-zero PCM. This isolates control timing;
it is not a full SPU2 output proof. Extra native output samples for 1/4-block
cases: pitch 4096 = 16 (0.333 ms), 3763 = 17 (0.354 ms), 2048 = 32 (0.667 ms),
1448 = 45-46 (0.938-0.958 ms). These timings are not bit-exact yet.

A nonsilent tail can change a small transient/reverb feed or grain completion.
The actual SHELL_S bank's five unique finite UI waveforms were decoded from
the existing ISO: four have zero peak in the final 16 source samples; the
NEXT/PREVIOUS waveform peaks at only three raw PCM units (-80.8 dBFS before
gains). HELI_SLIDER repeats and is unaffected by this stop rule. No substantial
audible tail error is indicated for those finite menu assets; speakers were
not used. Race crash/effect tails remain unverified. SHELL3 intro tails are
nonsilent (raw L/R peaks 3957/1835), but their driver-managed buffer endings
must not be equated with the static-voice FIFO model or full IOP KeyOff behavior.

### CPU cost and deterministic optimizations

`bench_sound.c` uses redistributable synthetic fixtures, 32-bit O2/SSE2/no
contraction, dumps/output disabled. GetThreadTimes measures CPU outside the
mixer over 30 audio seconds per trial, three trials; fixed sample counts alone
drive synthesis. Hardware: Ryzen 5 7600, six cores/12 threads. Accounting
quantum is 15.625 ms, approximately 0.52 ms per audio second for one trial.
Setup/ISO I/O and WASAPI cost are excluded. Full ranges: `audio-round6-perf/summary.json`.

| Synthetic load | Median CPU ms per audio second, before -> after |
| --- | ---: |
| Idle | 2.60 -> 1.56 |
| One stereo music stream | 5.21 -> 2.60 |
| Eight stereo streams | 25.52 -> 9.90 |
| Twelve bank voices + music | 17.19 -> 11.46 |
| Forty-eight bank voices + music | 54.17 -> 42.71 |
| Twelve voices + music + two wet cores | 35.94 -> 28.65 |
| Full native capacity + two wet cores | 94.27 -> 71.35 |

The last case is an oversubscribed ceiling of 48 bank voices plus eight stereo
streams, not an observed race or physical 48-voice allocation parity claim.
It falls from 9.43% to 7.14% of one core, about 1.19 CPU ms per audio tick on
this machine. It does not guarantee overall port frame rate on every modest PC.

Direct Gaussian windows avoid seam construction within blocks. Stream gain
caches invalidate on pan/application/group/ducker changes, and pitch refreshes
when modifiers change. Reverb reduces the long-running clock modulo the buffer
once per sample, then uses bounded 32-bit wraps rather than repeated 64-bit
remainders for each tap. Arithmetic is unchanged. The seven-case 4,032,000-byte
optimization PCM comparison is byte-identical. Nine isolated tone fixtures
remain byte-identical to round-5 native output; 96,000 reverb sample values still
match PCSX2 exactly. Audio tests/private strict link pass; no GS or ABI repair.

### Counts and next review

The long native capture is 210.14325 seconds, peak 10451, clipping 0% -> 0%.
Every exercised ID has zero missing/partial operations: 00=1, 06=1, 08=3,
09=37, 0E=1, 0F=4212, 19=4210, 2A=1, 2C=1, 4E=3, 5C=9, 64=1. The final
100.18-second run also has zero missing/partial counts (0F=1061, 19=1059,
5C=3; other totals unchanged). These differ in duration from round 5.

Full retail replay reveals movie-only missing commands: 3B=4, 3C=4, 3D=5,
3E=2, 40=535, total 550. All occur before menu music (last at 65.1975 s);
none occurs in the compared menu interval. Raw/movie audio remains outside
this round, rather than being counted as implemented. All other replay
commands/grains have zero missing/partial operations. Replay and independent
retail WAVs last 210.007479 seconds and clip 0%; replay peak is 9351.

Commits: 1c3e3b8e (CPU savings, disc EOF and regressions), b74a7f72 (retail
request/result observation and replay). Artifacts: audio-round6-menu-final/,
audio-round6-menu-native/, audio-round6-menu-retail/, audio-round6-replay/,
audio-round6-perf/, audio-round6-end/, audio-round6-oracle/, under ignored build/port.
Earlier short-retail attempts accidentally requested a 250-second screenshot
beyond a 210-second movie; those are diagnostic attempts, not the final capture.
Next: stream readiness/completion, isolate ambient grain/RNG parity, then
aligned race events when the owners provide a stable baseline. Static END
FIFO timing remains explicitly open. Round stopped for review.

## Audio review round 7 (2026-10-14)

### Requested O1 CPU measurement

The actual build distinguishes game `HOST_CFLAGS=-O1` from HLE `-O2` in
`port/tools/link.py`. This round explicitly benchmarks the requested O1,
i686/SSE2, SSE math, no contraction, `-fwrapv`/no strict aliasing configuration;
it does not silently label the earlier O2 measurements as O1. Synthetic bank
voices continuously decode/interpolate/advance ADSR and grain handlers, plus
one stereo stream and both active ROOM reverb cores. Setup, resident image
reads, WASAPI and diagnostics are excluded. GetThreadTimes measures CPU only;
fixed sample counts drive audio. Ryzen 5 7600, logical CPU 10 affinity,
AboveNormal priority, three trials of 60 audio seconds each. Accounting quantum
15.625 ms, about 0.2604 CPU ms per audio second per trial.

| Load | Before median (range), CPU ms/audio second | After median (range) | One core after |
| --- | ---: | ---: | ---: |
| 24 bank voices + stereo music + two wet cores | 42.71 (41.41-42.97) | 29.43 (29.17-30.21) | 2.94% |
| 48 bank voices + stereo music + two wet cores | 69.01 (67.45-69.27) | 50.78 (48.44-53.13) | 5.08% |

These are 26/50 logical voices; the upper case deliberately exceeds physical
SPU2 capacity by the music pair. It is a stress case, not a measured race
allocation. That case saves 26.4%, costs about 0.85 CPU ms per 60-Hz audio
tick, and remains above a 3% one-core target. This machine is not a modest-PC
performance guarantee. Migrating-thread 30-second runs were noisier; pinned
60-second measurements above are the report, retained with full trial CSVs in
`build/port/audio-round7-perf/summary.json`.

Gaussian's four independently rounded taps are unrolled for O1. Its maximum
absolute coefficient sum is 32643, bounding the voice/envelope product to
signed 32 bits; wide Q15 remains for wider reverb intermediates. Reverb caches
normalized delay offsets at preset changes and increments its 24-kHz cursor,
removing per-tap division. FIR zeros are skipped; the upsampler's exceptional
16384-to-32767 centre coefficient is retained exactly. Nine-case, three-second
optimization PCM is byte-identical (5,184,000 bytes). All ten reverb modes,
both cores, signed overrange inputs and odd/off/on switches match the earlier
implementation for 1,920,000 bytes. The unchanged PCSX2 ROOM oracle still
matches all 96,000 sample values. Audio tests pass at O1 and O2; private port
O1 audio compilation and strict link pass.

### Music request offset and cue selection

The live native request is sample 3026223 (63.0463125 s); retail's is 3447437
(71.8216042 s): 8.7752917 s later. This difference exists before the mixer.
`host_skip_movies()` and the `func_001EBFD8` patch bypass startup movie
execution under DHD_SKIP_MOVIES. Retail has 550 movie-path commands from
45.06094 to 65.19748 s: 3B=4, 3C=4, 3D=5, 3E=2, 40=535; native has none.
This confirms different boot/menu paths. The exact 8.775 s should not be
treated as the duration of one movie: the paths also have different loading
and timing histories. No sound output stage can explain the request offset.

Cue selection is game RNG, not an audio driver's choice. `func_001B9E88`
immediately seeds through `func_00222E10(func_00238F10(),0)` before constructing
the menu. `00238F10` adds the extended counter to EE Timer 2 COUNT at
0x10000800. The libc RNG at `002559D8` advances state with 0x41C64E6D/0x3039
and returns its low 31 bits. `001C4E68` sets x10 to rand()%2; `001C54A0`
advances initial 0 to 1 (SHELL3 intro + paused looping bed), or initial 1 to 3
(looping SHELL1). Both retail assembly and the source path were inspected.
Thus movie skipping changes the timer-derived seed and can legitimately pick
another cue. The constructor also uses another draw for the music-bank choice.
Only a zero timer seed takes the RTC fallback. No game RNG seed, movie policy,
caller or ABI behavior was changed. Parity uses captured retail requests and
their sample clock to align the exact cue, rather than forcing a live-game cue.

### Deterministic startup readiness

The read-only PCSX2 probe reuses the existing comparison launch/input adapter.
It derives the driver's relocated base from the main SIF server callback
(IOP 177C), observes stream headers and R250 state every 16 output samples,
and never invokes an RPC or writes IOP memory. Invalid initialization records
before active handles are ignored. Header ready bit 8, byte 22, is exactly the
bit queried by RPC 4F through IOP 13D90. In the fresh 125-second capture the
intro request at 3447437 becomes ready by 3457072 (200.7292 ms); the paused bed
at 3447489 becomes ready by 3461872 (299.6458 ms). Poll resolution is 0.333 ms.
Artifacts: `audio-round7-retail-final/driver-state.csv`, manifest and WAVs.

Native now schedules serialized startup service entirely on `grain_samples`:
100 ms cold seek/header plus a 100 ms read service, subsequent outstanding
jobs another 100 ms, then one 240-Hz dispatch interval before playback. RPC
4F returns pending 0, ready 1 or invalid -1. Loading progresses while paused;
pending playback holds ADSR/source position. Queue successors cannot leak
through Gaussian lookahead before their playback deadline. ISO acquisition
is synchronous and resident; its host cost cannot select a ready sample.
This is a documented timing approximation, calibrated on these paired VAGs,
not full CDVD seek/DMA/IOP emulation. Other seek distances and canceled-job
timing remain unverified. No worker thread or wall clock drives readiness.
Fixture-only `snd_test_stream_read_samples(0)` isolates existing DSP tests;
production defaults to 4800 samples per service. New deadline tests use the
production policy, vary synthetic host read cost and 17-sample render chunks,
and retain identical PCM, readiness/status and queued gaps.

### Ambient grains and actual driver RNG

Retail uses a 250-word XOR generator at IOP D3B0, with a 103-word lag and
16-bit output. Its seed procedure D29C uses the 0x015A4E35/+1 LCG, two passes
and a 16-bit diagonal. Independently generating seed 12345 reproduces all
500 initial bytes of the user's IRX table; no table bytes are included in
tracked code. Across the new capture, all 69 observed R250 states after 118
calls match the generated sequence. The earlier xorshift was deterministic
but not retail-compatible. OpenGOAL's host rand() is also not the retail oracle.

Routing differences were fixed with retail driver evidence: tone
volume/pan register/random values are captured at allocation (5500/5588), not
rerolled on each setter/LFO update; random volume is masked to seven bits;
RAND_DELAY at 5BC0 is inclusive of its maximum; RAND_PB at 5C0C maps
rand()%32767 through 65535/32767 before percentage scaling. The sample/hold
LFO's initialized half=1 means the negative half, so its first update retains
the initial hold rather than consuming two extra random words. Holding tone
parameters is also supported by [OpenGOAL's ISC tone routing](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/sfxgrain.cpp)
and [held voice gains](https://github.com/open-goal/jak-project/blob/master/game/sound/989snd/blocksound_handler.cpp).
The new RNG and fixes are independent C implementations of the observed
arithmetic; no proprietary instructions/table or GPL emulator code is copied.
Optional DHD_SND_RNG_TRACE writes sample/index/value diagnostics.

RNG event timing matches all 69 sampled states: median native minus observed
retail poll -0.3125 ms, absolute p95 3.0 ms, range -26.333 to +3.0 ms. The
outlier and physical END-prefetch behavior remain open; this is sampled grain
state timing, not a claim that every race effect has been aligned.

### Comparison and round report

Same original retail 210-second RPC trace, identical assets/cues and exact
request sample times, no fitted waveform offset: the 130-second menu interval
has 100-ms RMS correlation **0.174154 -> 0.959137**, level error
**+0.580071 -> -0.027345 dB**, RMS 666.73/668.83 and peak 7706/7181. First
30 seconds correlate 0.982704 / -0.043192 dB. Spectrum band energy shares
(0-100,100-500,500-2000,2000-8000,8000-24000 Hz) differ by at most 0.52
percentage points. The 0.8-second S_SELECT remains 0.999472 / +0.022419 dB.
Completion-query disagreements are **12 -> 0**, not inferred from silence.
Full replay processes 19,525 requests and compares semantic replies including
GetMasterVolume (0A), which is now implemented. Initial R6's report omitted
one missing 0A call: total missing was actually 551, now **551 -> 550**.
The remaining 550 are precisely the movie commands above, all before music.
Menu missing/partial and unresolved plays are **0 -> 0**. No race parity claim.

WAV pair: `build/port/audio-round7-replay/menu-native.wav` and
`menu-reference.wav` (130 seconds); whole timeline `mix.wav` (210.007479 s).
New live native `audio-round7-menu-final/mix.wav` lasts 100.150042 s, peaks
2906/3076, exercises 0F=1060,19=1058,5C=3, and has zero missing/partial calls.
Both runs clip **0% -> 0%**. Interactive RMS/CPU report is `review.html` beside
the replay; all JSON/CSV/capture data stay ignored. Initialization silence
still precedes the music request; readiness now adds its explicit modeled delay.

Sources: retail IRX/ELF were read locally only; primary protocol reference is
[ps2sdk SifRpcServerData](https://github.com/ps2dev/ps2sdk/blob/master/common/include/sifrpc-common.h).
Hardware parity references remain PCSX2 [Mixer.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Mixer.cpp)
and [Reverb.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Reverb.cpp).
Private upstream emulator instrumentation retains its GPL notice and is not
linked into the port. Existing coefficient/preset license notices remain.

Commits bcff9f52 (O1 CPU savings) and 583b07c4 (sample deadlines, retail RNG,
grain routing, volume query and probes). Next: stable frame-synchronized race,
the remaining 26-ms grain timing outlier, static END FIFO lifetime, and more
seek/readiness cases. No shared-header sweep, renderer or race-reset edits.
Round stopped for review.


## Audio review round 8 (2026-10-14)

### Required CPU budget, measured before timing work

The normal port compiles **sound.c at O2**, even though recompiled game units
use O1 (`link.py` lines 169/178). This round uses those actual production
HLE flags: i686, SSE2/SSE math, no contraction, host code included, without
SND_NO_HOST. GetThreadTimes measures mixer thread CPU, not elapsed time.
Setup, ISO acquisition, diagnostics and asynchronous WASAPI output are excluded.
All DSP advances by fixed samples; measurement clocks never drive playback.

The fixture now has exactly **24 or 48 total active voices**, respectively
22/46 bank tones plus the two channels of one stereo music stream. Both cores
run ROOM reverb. It checks the active count after setup and each audio second;
tones have varied pitches/pans and nonzero, long-lived retail-style ADSR.
The bank image is static: the earlier benchmark retained a pointer to a local
setup buffer, which was an invalid lifetime and is fixed. These are synthetic
busy loads, not claimed measurements of the game's actual race allocation.

Ryzen 5 7600, logical CPU 0 affinity, AboveNormal thread priority; five trials
of 120 audio seconds per load. All trials are retained, including contention.
The timer accounting quantum is 15.625 ms / 120 s = 0.1302 CPU ms/audio second.
The interleaved before/after optimization run reports:

- 24 voices: **26.43 -> 19.01 CPU ms/audio second**, 2.64% -> 1.90% of one core.
  Before range 25.00-33.98, after 17.58-23.70 ms.
- 48 voices: **54.82 -> 29.82 CPU ms/audio second**, 5.48% -> 2.98% of one core.
  Before range 42.06-59.11, after 28.39-36.46 ms.

After the timing fixes, an additional five-trial final-code run measures
**17.84 ms (1.78%)** at 24 voices, range 17.06-18.23, and
**29.04 ms (2.90%)** at 48, range 28.78-32.29. Both medians meet the approximate
3% budget. This host is not a modest-PC guarantee. Initial minimal-ADSR and
busy CPU-10 exploratory trials are not substituted for these final fixtures.
Before/after evidence is `build/port/audio-round8-perf/normal-build.csv` and
`normal-build.json`; final code: `audio-round8-final-perf/trials.csv` and
`summary.json`. Reproduce using `port/tools/audio_benchmark.py --seconds 120
--trials 5 --cpu 0`; its compile flags match the production sound object.

Optimizations: Gaussian uses SSE2 signed products with each tap rounded before
addition; reverb mirrors its 64-entry histories and sums contiguous FIR taps
with SSE2; ADSR caches phase-local rate decoding and computes exponential
release only on counter updates; fixed voice gains are cached at handler
updates while hardware sweeps still tick. Reverb's absolute coefficient sum
47526 bounds the signed-32 FIR intermediates; its exceptional center coefficient
is preserved. No output scaling, scheduling or floating-point approximation
was introduced. Before semantic timing changes, the full 210-second replay
was byte-identical to round 7. Private all-mode reverb comparison is exact for
1,920,000 bytes, the two busy PCM loads for 1,152,000 bytes, and 512 ADSR
register pairs including release/restart for 17,350,656 bytes. O1/O2 audio
unit tests pass. Earlier PCSX2 ROOM and Gaussian oracles remain the references.

### The 26-ms grain outlier

The read-only retail probe now records bank handler PC/delay/voice masks in
`audio-round8-retail/driver-grains.csv`. Ambient sound 7 waits at grain 11,
WAIT_FOR_ALL_VOICES (38). Retail still has mask 0x200 at sample 5583648,
clears it by 5585248, and consumes R250 state 104 by 5585264. Native previously
consumed it at 5584000: -26.333 ms. This was driver allocation lifetime,
not a random-delay rounding error.

Local IRX inspection: IOP 10FB0 reads ENVX (libsd parameter 0x500), 11014
counts down per-voice zero observations at global 26ED8, and 11060 resets that
count to four when ENVX is nonzero. Initial allocation sets four at 19240/19288.
Only the fourth zero poll frees the voice through 19A18 and clears the bank
handler mask via 7310. WAIT at 61E4 tests handler masks at +24/+28.
Native now separates silent DSP state from the owned bank allocation, retaining
that allocation until four consecutive zero-envelope polls on its 240-Hz
game-sample clock. Immediate stop/stealing remains immediate; paused voices
retain the existing frozen polling policy. Tests verify the silent-but-owned
interval. Optional DHD_SND_GRAIN_TRACE records native grain execution without
changing the clock. No proprietary code/table was copied or committed.

All 69 sampled R250 states match the fresh retail trace. Native minus retail
poll time: median -0.3125 ms, absolute p95 3.0 ms, range **-1.333 to +3.0 ms**;
state 104 is now at 5585200 (-1.333 ms). The 26.333-ms outlier is removed.
Retail probes sample every 16 output frames (0.333 ms), so these are sampled
state transitions, not instruction-exact event timestamps.

### Static END prefetch and readiness checks

Static bank voices now track SPU2's four-source-sample fetch head: fetch when
the FIFO has no more than 12 pending samples; the last END word stops a
nonrepeating physical voice before its pending tail is played. This changes
DSP cutoff; the driver's separate four-zero-poll allocation lifetime still
applies. Repeat flags and pitch zero do not acquire an artificial endpoint.
This is an independent hardware-behavior implementation, not copied emulator
code. The private oracle uses unchanged PCSX2 DecodeSamples/ConsumeSamples
with filter-zero source data; **32,766 endpoints are identical** across every
nonzero Q12 pitch 1..0x3FFF, one and four blocks. CSVs and upstream GPL harness
stay ignored in `audio-round8-end/`. Tracked tests cover representative endpoints,
repeat and stalled pitch. At 0x1000, one/four blocks stop at output sample
12/96, rather than consuming all 28/112 source samples. A nonzero finite tail
can now audibly stop a fraction of a millisecond earlier as on SPU2.

Driver-managed disc streams retain their buffer-flag/end policy: the game
rewrites stream buffer flags rather than exposing static-bank END directly.
Their observed completion replies still have zero mismatches. Earlier menu
END-tail measurements were zero or at most three raw sample units, so no
remaining audible menu-tail difference is demonstrated. Complete SPU FIFO/IRQ
and ENDX register emulation is outside the RPC HLE and is not claimed here.

Production readiness regressions additionally cover failed acquisition,
canceling a pending handle, a different disc offset, idle-queue cold startup,
slow synchronous host reads and 17-sample render chunks. Ready/status and PCM
remain identical. A failed acquisition reserves no slot; cancellation frees
the handle but keeps its already scheduled FIFO service slot. This is the
explicit deterministic HLE policy, not a measured claim of retail cancellation
latency. Seek distance still does not alter the calibrated fixed service cost;
no wall clock, disk-speed observation or worker completion selects readiness.

### Comparison and review artifacts

Replay uses the same 19,525 retail RPC requests, cues, assets and exact sample
times, with zero semantic reply mismatches. The 130-second menu interval has
100-ms RMS correlation **0.959137 -> 0.991853**, level error
**-0.027345 -> -0.031704 dB**, RMS 666.39/668.83, peak 7347/7181. Spectrum
band shares differ by at most 0.4835 percentage points. No fitted waveform
offset is applied. Full mix is 210.007479 seconds and peaks 6796/7347;
clipping remains **0% -> 0%**. Matched ambient event timing is reported above.

Fresh normal-O2 native menu capture: `build/port/audio-round8-menu-final/mix.wav`,
100.083313 seconds, with frozen non-audio objects and accepted scalar-quad
caller. Its manifest records executable and audio input hashes. It exercises
0F=1060, 19=1058 and 5C=3 with zero missing/partial calls and zero clipping.

Reviewer WAVs: `build/port/audio-round8-cpu-replay/menu-native.wav` and
`menu-reference.wav`, full timeline `mix.wav`, plus `menu.json`, summary and
command CSVs. Fresh retail diagnostics and audio are in `audio-round8-retail/`.
Missing calls **550 -> 550**: 3B=4, 3C=4, 3D=5, 3E=2, 40=535. These are the
requested untouched FMV calls, all before menu audio. Unsupported menu/grain
operations, partial calls and unresolved menu plays remain **0 -> 0**.

Race-state comparison remains open: finding 024's field-sync adapter establishes
native capture scheduling and exact GS dump-local fields, but its race results
replay native-generated packets. Finding 021's long frozen race demonstrates
no FP crash and visible world geometry, with rider/camera problems still open.
Neither establishes independently synchronized retail race gameplay/audio.
No renderer, ABI sweep, matching-source or movie-command changes were made.

Sources: hardware behavior checked against PCSX2 v2.8.2
[Mixer.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Mixer.cpp),
[Reverb.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/SPU2/Reverb.cpp)
and its interpolation table. Existing Gaussian/FIR ISC notices are retained;
SSE2/cache transformations are original native code. Retail driver reads and
unchanged GPL oracle functions are local evidence only, never linked into the
port or added to tracked sources.

Commits: 5587507c (normal-build CPU budget), c4e5d3fe (four-zero-poll bank
allocation), be71f074 (static END and readiness regressions). Next: independent
stable race event/spectrum comparison when its input/frame state can be aligned;
retail seek/cancellation calibration beyond the paired menu streams if needed.
Round stopped for review.
