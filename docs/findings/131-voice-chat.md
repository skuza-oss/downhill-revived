# 131: voice chat in a race, with no server change

Players in an online race talk to each other: hold **V** (or an open microphone), and the riders
near you hear you from where you are on their screen. It works on any server, the official one
included, as it stands.

## What the architecture allowed

- One transport: the game's WebSocket to the room's Durable Object (official or self-hosted). No
  peer hosting, Steam or dedicated-server mode; the "host" is a slot like the others and forwards
  nothing.
- In a race the server relays binary STATE and EVENT frames to the race as they are and stamps the
  real sender's slot in byte 1 (`room.ts` `relay`). The official server has no chat message and
  drops JSON types it doesn't know by closing the socket; in the lobby it relays no binary frames.
- So voice (and race chat, finding 125) travel as new EVENT kinds: `EVENT_CHAT` (10) and
  `EVENT_VOICE` (11), `docs/online-protocol.md`. The sender is the stamped slot (it can't be faked);
  a frame never leaves its race. Older clients drop the kinds.
- The server has no positions, so it can't choose recipients by distance. Each receiver decides
  from its own copy of the riders' positions (the snapshots it already gets). A sender's own
  claims are never read: the packet carries no position or identity.
- No voice in the lobby without a server change (no binary relay there).

## The world's scale

1 metre is about **5.35 world units**: in a scripted race (`DHD_FPS_TRACE`) rider 0 moved 91 units
a second when the HUD read 38 MPH (17.0 m/s), 93 when it read 39. `voice_rules.h`
`VOICE_UNITS_PER_METRE`; the built-in 50 m range is about 268 units. (The taunts' 30/600 units are
about 5.6/112 m.)

## How

- **The codec**: libopus 1.5.2 (BSD-3-Clause), xiph.org's release source pinned by SHA-256 and built
  as a static library by `port/tools/opus.py` the way `sdl3.py` builds SDL3; linked into dhd.exe on
  win-x64 (`build_host64.py`) and into the browser's network module (below). The other native targets build `voice.c` without it (no voice yet). The notice
  is in `port/release/THIRD-PARTY-NOTICES.txt`.
- **Sending** (`port/host/voice.c`, its own thread): SDL3's recording device (the default, or the
  first whose name holds `voice_device`), 48 kHz mono, 20 ms frames, Opus VOIP at `voice_bitrate`
  (built-in 24 kbps), sent with `net_send_frame` while the push-to-talk key is held (`voice_key`,
  built-in V; not while a text field has the keyboard) or, with `voice_mode = open`, while the
  microphone passes a gate. The device opens when a race starts and closes when it ends; a device
  that fails or goes away is tried again every 4 s.
- **Hearing** (`voice.c`, the output's device thread): per slot a decoder and a queue of at most 16
  packets in sequence order (the oldest dropped); playback starts with two waiting (40 ms); a gap is
  concealed (Opus PLC) up to three times; late and repeated packets are dropped, and more than 60 a
  second from a slot. Each voice is mixed at the gain and pan the game gives it, moved there
  smoothly (no click), times `voice_volume`.
- **Who hears whom** (`port/hle/online.c` `voice_step`, each frame; the rules in
  `port/hle/voice_rules.h`):
  - racers hear racers within `voice_range` metres (built-in 50; squared distance, no root): full to
    a third of it, down to 15 % at it; one heard stays heard to 1.1 times the range (hysteresis);
  - finished players (watching, upstream's 0.7.5) hear each other at full level anywhere, and
    neither group hears the other;
  - a player who left is dropped at once (`on.gone`), and every voice when the race ends
    (`race_left`); this PC never hears itself;
  - the pan is where the speaker is across this player's view (behind: the middle).
- **On screen**: a rider talking shows `NAME - TALKING` on their name tag; this player sees `MIC`
  while sending, `NO MICROPHONE` when the key finds none.
- **Mute**: `/MUTE NAME` and `/UNMUTE NAME` in race chat (a name's start is enough), for this PC.
- **SETTINGS > Microphone and Mic Volume** (2026-11-01): Microphone lists Default and the host's
  recording devices by name (`host_service_voice_device`: SDL3's on the PC, the page's
  `enumerateDevices` in the browser, whose names come once a microphone is allowed), asked again
  each time the page opens; a new choice reopens the open microphone at once (voice.c on the PC,
  main.js in the browser). Mic Volume is `voice_mic`.
- **Settings** (dhd.ini, `table.rs`): `voice` (1), `voice_mode` (ptt/open), `voice_volume` (100),
  `voice_mic` (100, the microphone's level, 50-200 %), `voice_range` (25/50/100/200 m),
  `voice_bitrate` (8000-64000), `voice_key` (a letter), `voice_device`. PC SETTINGS has Voice Chat,
  Voice Mode, Voice Volume and Voice Range, live (the voice thread reads them every second).

## In the browser

The game core is the same in the browser, so `online.c` already decides who is heard and calls
`host_service_voice_*` there; they were no-ops in `worker.js`. Now:

- **The module.** `port/web/build_net.py` builds `voice.c` (`DHD_VOICE_WEB`) and libopus into the
  browser's network module, `dhd_net.wasm`, beside the client it already holds. libopus is the
  same pinned source, compiled for wasm32 by wasi-sdk's clang from its own source lists
  (`opus.py` `wasm_objects`; wasi-sdk has no archiver, so the objects go into the link). The
  module grows to about 600 KB. The client's `net_voice_hook` works there as natively: voice
  frames go from the socket to `voice.c`, not through the per-frame queue.
- **What voice.c asks of the host** comes from the worker (`port/web/net/voice_web.c`): the
  settings from the worker's live environment (`web_getenv`, so PC SETTINGS applies at once), the
  push-to-talk key as `host_service_key_held` reads it (`web_key_held`; none while a text field is
  open), and the log through the module's stderr. `port/web/net/SDL3/SDL.h` gives voice.c the few
  SDL calls it uses there: one thread, so plain atomics and no-op spinlocks.
- **The microphone** is the page's: only a page can call `getUserMedia`. `net.js` asks every
  10 ms whether the game wants it (`voice_web_wanted`: voice on, in a race) and tells the page
  (`mic`); `main.js` opens it with the browser's echo cancellation, noise suppression and gain
  control, through `audio.js`'s `dhd-mic` worklet on the page's 48 kHz AudioContext, which writes
  16-bit mono into a SharedArrayBuffer ring (1 s). `net.js` hands each 960 samples to
  `voice_web_capture`, which does what the native thread does with them (`capture_frame`: the
  level, the gate or the key, Opus, `net_send_frame`). The page's answer (`mic-state`: 1 open, -1
  refused or none) is the HUD's NO MICROPHONE. The microphone closes when the race ends.
- **Hearing**: `host_sound_write` mixes the voices into the game's sound (`voice_mix`) before it
  goes to the output ring, where the native output does it.
- **PC SETTINGS** in the browser shows Voice Chat, Voice Mode, Voice Volume and Voice Range,
  live (`settings.js`).
- `net.js` also gives the module the worker's environment (WASI `environ`), so `keep_ready`
  (finding 132) reads dhd.ini there too.

## Checked

- `port/tools/test_voice.py` (in `test_runtime.py`'s host tests on Windows): voice.c with the real
  libopus, 21 cases, all pass: a tone decoded at its pitch, gain 0 and mute silent, the pan, out of
  order and lost packets with no silent gap, eight talkers at once, malformed, wrong-kind,
  oversized and out-of-range frames dropped, a flood bounded, dropping a slot; and the rules: in
  range, out of range, the hysteresis band, the levels, the groups.
- `DHD_VOICE_TONE=Hz` sends a tone instead of the microphone, and receivers log each slot's
  packets taken and played every 5 s, for two-game tests (`online_race.py --a/--b`).
- Browser: `node port/web/check_net.mjs ws://...` (two clients through `net.js` and
  `dhd_net.wasm`): in a race the host's microphone ring carries a 440 Hz tone (open mic) and the
  guest hears it in its mix (RMS 2974) with the host's slot talking. It passes against upstream's
  0.7.6 server unchanged (`wrangler dev` on `skuza-oss/downhill-revived` main, which has no chat
  message: the room's chat line stays local with "Chat isn't on this server yet.") and against
  this branch's server (the room's chat reaches both).

## Open

- No voice in the lobby, and no server-side choice of recipients: both need a server change.
- Windows and the browser; macOS, Linux and Android build without libopus yet.
- Per-player volume (only mute), a talking marker on the room's member cards, and controller
  push-to-talk (every DualShock 2 button is the game's) are not done.
- A microphone's permission prompt is the system's (SDL3); a refusal shows as NO MICROPHONE.
