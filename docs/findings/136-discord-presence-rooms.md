# 136: Discord Rich Presence: rooms with their size, the rider, TRY AGAIN, the Social SDK

2026-10-30, branch `discord-presence` (from 0.7.3). Follows finding 106. A friend looking at the
player's profile now sees a room's track and how full it is, the rider in a race, and an elapsed
time that starts again with each run. A Discord that was restarted gets the presence back without
waiting for the game's state to change. Ask to Join is designed and its presence half written, but
switched off (below). On 2026-10-31 the presence also learnt to go through Discord's Social SDK
when the SDK's library sits next to the game (the user's choice); without it, the IPC as before.
Later that day Ask to Join was turned on through the SDK, and the game takes the join on the title
(not run in the game). The release ships the DLL only with `package_release.py --discord-sdk`,
which is off by default: a licence decision not made yet.

## What it showed before (finding 106)

| Game state | First line | Second line | Elapsed from |
|---|---|---|---|
| Title, menus (and anything not below) | In menus | | the session's start |
| Single race (and the other non-career modes) | Racing: Alpine MX | 2nd of 6 (race phases 2-4 only) | the race's first sight (load) |
| Career race | Career: Peru TDH | 1st of 10 | the same |
| Online room, public | Online room K7M2QX | 3/6 riders | the session's start |
| Online room, private | Online room | 3/6 riders | the same |
| Online race | Online race: BC TDX | 3rd of 4 | the race's first sight |
| Results | as the race, without a place | | the race's |
| Movie | Watching a movie | | the session's start |

Limits it stated: TRY AGAIN kept the elapsed time; macOS and Linux were never run against a Discord.
Two more limits it did not state: a Discord restarted while the state stayed the same (a long stay in
the menus) showed nothing until the next change, because the loss was noticed only at the next
write; and the room's size was plain text, not Discord's party field.

## What it shows now

| Game state | First line | Second line | Party size |
|---|---|---|---|
| Menus, movie | as before | | |
| Race, career, online race | Racing: Alpine MX (and so on) | Cosmo · 2nd of 6; before the place shows: Cosmo | |
| Online room, public | Online room: Alpine MX | Room K7M2QX (3 of 6) | [3, 6] |
| Online room, private | Online room: Alpine MX | Private room (3 of 6) | [3, 6] |

- **The party size.** The room's players and size go in Discord's `party.size` ([current, maximum]).
  Discord's client prints it after the second line as "(3 of 6)". The count is held between 1 and
  the size, so a size of 0 (not known yet) sends no party. Discord refuses a size without an id
  (tried 2026-10-31: `Field "party.id": String length is out of range. Must be between 2 and 128`).
  A private room's code must not be the id, so a room's party id is this session's own random token
  (`party_id`: `dhd-` and 16 hex digits from std's randomly keyed hasher), the same for every update
  and nothing of the room's. With Ask to Join it is the join secret.
- **The room's track** is `lobby.track` through `port_track_name`, the same table the race uses.
  A track missing from that table leaves "Online room".
- **The rider** is the setup block's +0x290, the first player's rider id, 0x20 × (index + 1), as the
  menus leave it. Finding 040 lists it as the human rider ids; online.c writes and reads it. The core
  sends index + 1 (0 when the word is not a rider id) in a new `HostPresence.rider` (the struct grows
  from 52 to 56 bytes). The crate names it from online.c's table ("Eric Carter", "T-bag").
- **TRY AGAIN.** The place shows only while the riders race (phases 2-4). The clock now keeps the
  race's start until the place has been seen and has then gone (the results, the next countdown).
  When the place comes back on the same kind and track, that run counts from that moment. The first
  run still counts from the load. A new track (NEW LEVEL) or kind starts again as before.
- **Refresh.** An unchanged presence goes again 60 s after the last update (`REFRESH`, `Pacer::due`).
  On a Discord that quit or restarted, the write fails. The worker then marks the presence lost and
  reconnects on its usual 30 s retry, so the presence returns within about 90 s. That is one update
  a minute, well inside Discord's limit (about 5 per 20 s).
- The log line shows the party: `discord: presence set: Online room: Alpine MX (Room K7M2QX, 3 of 6)`.

Already in place and kept: track names, the place, the per-race elapsed time. Discord not running at
start, or started later: one log line and a try every 30 s. Nothing blocks the game thread: the game
only posts to a channel, and the pipe's blocking reads and writes stay on the `discord` thread. The
one wait is the 0.5 s limit at exit.

## Ask to Join: the design (the state of 2026-10-31 is in "The game takes the join" below)

Discord's fields (sources below): `secrets.join` is "Secret for joining a party". Ask to Join needs
a non-empty `party.id`, a `party.size` with room left, and a non-empty join secret. When a friend
joins, Discord sends the game an `ACTIVITY_JOIN` event (`{"cmd":"DISPATCH","evt":"ACTIVITY_JOIN",
"data":{"secret":...}}`), but only after the game has sent `SUBSCRIBE` for it. `ACTIVITY_JOIN_REQUEST`
is answered with `SEND_ACTIVITY_JOIN_INVITE` or `CLOSE_ACTIVITY_REQUEST`. Discord's best practices say
"A Join secret should not be sent if the player can't invite anyone!"

Written (`lib.rs`): `join_secret(state)` gives `dhd-room-1:<code>` for a public room with a free
place, and nothing for a private or full room or outside a room. With `ASK_TO_JOIN`, `payload`
carries the secret and `activity` sends it as `secrets.join`, with the same string as `party.id`.
`ASK_TO_JOIN` is `false`, so neither field is sent today (a test checks this). A Join button that
does nothing would be worse than none, and these points are open:

Points 1 and 4 are answered by the Social SDK (below) when its library is present; 2, 3 and 5 stand.

1. **Reading Discord's events.** The worker reads the pipe only right after its own writes, through
   the crate's blocking `recv`. A join can arrive at any time, so the worker needs a way to wait on
   the pipe and the game's channel together. Options: `PeekNamedPipe` on Windows and a
   non-blocking socket on Unix, or the IPC framing written in the crate (the crate exposes
   `send(data, opcode)`, but its client has no non-blocking read). After the handshake, it sends
   `SUBSCRIBE` for `ACTIVITY_JOIN` (and `ACTIVITY_JOIN_REQUEST` to accept or refuse requests).
2. **Handing the code to the game.** The worker keeps the secret's code, and a new host service lets
   the core take it, only in the menus and never during a race. `online_menu.c` then opens ONLINE and
   joins as JOIN WITH A CODE does (`net_join_room`; codes are 6 of `code_letters`), with the same
   failure lines. The game takes only `dhd-room-1:` secrets.
3. **The server.** A code means something only on the server that made it. The secret names no
   server, because the presence must never carry an address other than the official one. So a player
   whose dhd.ini names another server would try the code on it and get "no such room". Either take
   the join only on the official server, or accept that.
4. **The game not running.** Discord starts it through a registered `discord-<application id>://`
   protocol (discord-rpc's `Discord_Register`: the HKCU `Software\Classes` keys on Windows, a
   desktop file on Linux, the bundle on macOS). That belongs to the launcher, and its exe path
   changes with each update.
5. **Approval and buttons.** Discord's RPC page says "we currently do not allow access to RPC for
   unapproved apps without being on the game's list of testers". It lists no scope for the
   ACTIVITY_* events, and discord-rpc subscribed to them without authorising, so it is unclear whether
   an unapproved application gets them. A community project reports that a join secret and custom
   buttons cannot be used together (the profile would lose "Get Downhill Revived" and "Join the
   Discord"). Neither was tried here.

Turning it on means finishing 2 and 3, shipping the SDK's library, flipping `ASK_TO_JOIN`, and trying
it with two Discord accounts.

## The Discord Social SDK (2026-10-31)

The user chose the SDK. Its documentation says ("a C++ library that Discord distributes and would be
integrated and shipped with the game itself"):
- `Client::SetActivityJoinCallback` hands the game the join secret. Discord holds a join for 180 s
  when it has to start the game first, so the callback is registered before `Client::Connect`.
- `Client::RegisterLaunchCommand` registers the command Discord runs to start the game, on each
  machine at startup (point 4).
- "You must set up your rich presence Activity with party information and a join secret to send
  game invites": the party size and join secret written here.
- Invites need no OAuth account link (provisional accounts work), and no approval step is mentioned
  for them. That makes point 5's approval doubt smaller.
- "Rich Presence can be set without authentication": without `Client::Connect` the SDK talks to the
  running Discord client. No sign-in, as with the IPC.

Version used: DiscordSocialSdk-1.10.19337 (the developer portal's download, 781 MB zip; Windows x64
`bin/release/discord_partner_sdk.dll` 9,882,040 bytes, `cdiscord.h`, `discordpp.h`,
`License-Notices.txt`).

### How it is used (`presence/src/sdk.rs`)

- **Loaded at run time** with libloading (0.8.9, already in the library through wgpu-hal), from the
  executable's own folder only: `discord_partner_sdk.dll`, `libdiscord_partner_sdk.so` or
  `libdiscord_partner_sdk.dylib`. Not the working folder or PATH. Nothing links against it, so the
  builds need no SDK. A game without the file behaves as before (the IPC). A file that doesn't load
  is logged once (`discord: the Social SDK didn't load (...); through the IPC`), and the IPC is used.
- **The C API** (cdiscord.h), not the C++ wrapper: every object is one opaque pointer (`_Init`,
  `_Drop`); setters copy the strings they get; a string or `ClientResult` handed to a callback
  belongs to the callback (`Discord_Free`, `_Drop`), as discordpp.h's wrappers do. The callbacks'
  user data carries the functions they need, so there is no global state. 43 functions are looked
  up; a missing one fails the load.
- **The worker** (`run_sdk`): the same state, lines, pacing (15 s, the 60 s refresh) and log lines as
  the IPC loop. `Discord_RunCallbacks` runs every 100 ms on the presence's thread, and every SDK call
  is made there. An update is answered in its callback; a refused one counts as a lost connection
  and goes again after 30 s, logged once with the SDK's message. At exit the presence is cleared
  within the 0.5 s limit. The SDK's own warnings and errors reach dhd.log, at most 20 a session
  (`discord: sdk: ...`). The thread's stack is 1 MB now (256 KB before), since the SDK runs on it.
- **Joins** (only with `ASK_TO_JOIN`): `SetActivityJoinCallback` before anything is sent, and
  `RegisterLaunchCommand` with the running executable's path, quoted. A secret is checked
  (`join_code`: `dhd-room-1:` and 6 code letters), and its code waits until the game takes it
  through `dhd_presence_take_join` (C, in dhd_presence.h). The Join button shows on desktop only
  (`SetSupportedPlatforms(Desktop)`).

### The game takes the join (points 2 and 3, written 2026-10-31, not run)

- **A core import**, `host_service_presence_join(uint32_t out)` (host_api.h): the native host hands
  it to `dhd_presence_take_join` with the core's 8-byte buffer (host.c). The browser's two no-op
  lists answer 0 (`www/worker.js`, `run_headless.mjs`), and `gen_host64_imports.py` checks the
  8-byte span. The core gains one import, so it must be rebuilt (`build_wasm.py`) with the host.
- **online_menu.c**: on the main menu `port_online_resume` takes a waiting join (`take_join`) and
  answers 1. That opens ONLINE, as after a closed room, and `port_online_menu` joins the code
  through `enter_room(0, code)`, the JOIN WITH A CODE path with its own `not_ready` refusals. In a
  race or another page the join waits for the title. Point 3: a code is taken only when `server()`
  is the official `wss://dhd.skuza.app`; otherwise one log line, and it is dropped.
- **`ASK_TO_JOIN` is on.** Only the Social SDK can receive a join, so the plain IPC drops the secret
  (`worker::for_ipc`). Its presence stays as before (the party and its id, both buttons, no secret).
  So a game without the SDK's DLL shows no Join button.
- **The party id is never the secret.** Discord refused an activity whose secret matched its party
  id ("secrets cannot match the party id", code 5005, tried live 2026-10-31). `party_id()` is always
  this session's token now.
- Tests: `the_join_reaches_only_the_sdk`, `the_presence_is_harmless_without_discord`, and the
  updated `the_join_secret`.
- **Not run in the game**: the title opening ONLINE by itself, the join, the refusal off the
  official server, and Discord starting the game for a join (`RegisterLaunchCommand`, the quoted
  dhd.exe path). This needs `build_wasm.py` and `build_host64.py`, the DLL beside dhd.exe, and two
  Discord accounts.

### Packaging: the DLL only with `--discord-sdk`, off by default

`port/tools/discord_sdk.py`, as `dxc.py` does for dxcompiler.dll: the SDK's archive is pinned by
version and SHA-256 (`DiscordSocialSdk-1.10.19337.zip`, `d7840975...62045`) and so is the DLL
(`f9953495...ab8ee`). It is taken from a cache outside the tree (%DHD_DISCORD_SDK_CACHE%, else
DHD_DEVCACHES/discord-social-sdk, else build/downloads/discord-social-sdk); `<cache>/1.10.19337/`
holds the zip. It cannot download: Discord's portal needs a sign-in, so a missing archive stops
it with where to get it.

`package_release.py --discord-sdk` (Windows only) puts the pinned `discord_partner_sdk.dll` beside
dhd.exe. Like dxcompiler.dll it is checked by its SHA-256 instead of the byte scans, and the SDK's
`License-Notices.txt` is appended to THIRD-PARTY-NOTICES.txt. **Without the option the package is as
before** (no DLL, no SDK notices), so a release presents the IPC presence with no Join button.

**Why off by default.** The user asked to ship the DLL. The SDK is Discord's closed binary, though,
and the game is GPL-3.0-or-later, so whether a release may carry it is a licence decision for the
project's copyright holders. That decision isn't made. By commit count most of the code is
`skuza-oss`'s (122 commits on this branch against 6), so a GPL section 7 additional permission for
the Discord SDK would be theirs to grant, not one contributor's. Nothing here changes LICENSE.

### What the user must decide or do

- **The licence**, above: grant (or get) a GPL section 7 additional permission for the Discord Social
  SDK from the copyright holders, then build releases with `--discord-sdk`; or leave the DLL to
  players who want it (put beside dhd.exe, it is used).
- **The developer portal**: the Social SDK enabled for the application (its Getting Started form).
  Rich Presence through the SDK worked here without it, as far as this one try shows. The `logo`
  art asset is also missing: Discord's answer to `SET_ACTIVITY` (IPC, 2026-10-31) echoed
  `large_text` but no `large_image`, which it gives back for an asset it knows. So the profile has
  shown no picture since finding 106; the user needs to upload an asset named `logo` under Rich
  Presence > Art Assets.

Sources: https://discord.com/developers/docs/social-sdk/index.html (overview),
https://docs.discord.com/developers/discord-social-sdk/development-guides/managing-game-invites
(invites, the join callback, the launch command, provisional accounts),
https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence
(presence without authentication, timestamps, buttons),
https://docs.discord.com/developers/discord-social-sdk/getting-started/using-c++ (the files, the C
API under the C++ wrapper, RunCallbacks), and the SDK's own `cdiscord.h` and `discordpp.h`.

Not done:
- **The room's own state** (waiting, the countdown to the start): `NetLobby.countdown_ms` has it, but
  the countdown lasts seconds while updates are 15 s apart.
- **The rider as an image**: needs art uploaded to the application.

## Sources

- Discord RPC: `SET_ACTIVITY` (activity types allowed), `SUBSCRIBE`, `ACTIVITY_JOIN` /
  `ACTIVITY_SPECTATE` / `ACTIVITY_JOIN_REQUEST`, `SEND_ACTIVITY_JOIN_INVITE`, `CLOSE_ACTIVITY_REQUEST`,
  and the approval note: https://docs.discord.com/developers/topics/rpc
- Activity object (party `id`, `size` [current, max]; secrets `join`/`spectate`/`match`; buttons at
  most 2, label 1-32 characters, url 1-512):
  https://docs.discord.com/developers/events/gateway-events
- Rich Presence best practices (keep the party size up to date, no join secret when nobody can
  join, details and state as short snippets): https://docs.discord.com/developers/rich-presence/best-practices
- Ask to Join's field requirements (party id, size with room, join secret): the Social SDK's
  `Activity` page, https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1Activity.html
  (through search results)
- discord-rpc (MIT, archived): `src/serialization.cpp` (the `party` and `secrets` JSON, `SUBSCRIBE`,
  the join reply) and `documentation/hard-mode.md` (subscribing, the `ACTIVITY_JOIN` frame, the
  `discord-<appid>://` protocol): https://github.com/discord/discord-rpc. Used for reference only, no
  code taken.
- The join-secret and buttons conflict (community report, not confirmed by Discord):
  https://github.com/christianavi/glass-cannon
- The discord-rich-presence crate 1.1.0 (already vendored): `Party::size`/`id`, `Secrets::join`,
  serialised only when set.

## Changes

- `port/host/host_api.h`: `HostPresence.rider`, 56 bytes. `gen_host64_imports.py` checks
  `sizeof(HostPresence)`, so it follows.
- `port/hle/presence.c`: the rider in races, the track in rooms.
- `port/hle/host.c`, `port/render/include/dhd_presence.h`, `presence/src/c.rs`: `rider` passed
  through `dhd_presence_set`.
- `presence/src/lib.rs`: `RIDERS`, `rider_name`, `join_secret`, `ASK_TO_JOIN`, `REFRESH`; the
  `Payload` party and join; the new room and race lines; the clock's runs; the pacer's refresh.
- `presence/src/worker.rs`: `party` (with its id) and `secrets` in the activity; the party in the log
  line; `run_sdk`, chosen at start when the SDK's library is beside the game.
- `presence/src/sdk.rs` (new): the Social SDK through its C API, loaded at run time.
- `presence/src/lib.rs`, `c.rs`, `dhd_presence.h`: `party_id`, `join_code`, `joined`, `take_join`,
  `dhd_presence_take_join`.
- `presence/Cargo.toml`, `port/render/Cargo.lock`: libloading =0.8.9 (one lock line).
- `.gitignore`: the SDK's files.
- `presence/src/tests.rs`: 12 tests (8 before), and 2 by hand against a running Discord.
- Later on 2026-10-31: `ASK_TO_JOIN` on, `party_id()` without the secret, `worker::for_ipc`;
  `host_service_presence_join` (host_api.h, host.c, `gen_host64_imports.py`, the browser's no-op
  lists); `online_menu.c`'s `take_join`, `port_online_resume`, `port_online_menu`;
  `port/tools/discord_sdk.py` (new); `package_release.py --discord-sdk`; tests.rs at 14.

## Checks (2026-10-30)

- `cargo test --locked --offline --target x86_64-pc-windows-msvc -p dhd_presence` (run directly in
  `port/render`, as finding 106 did; `rustgs.py test` always adds `--features native`, which this
  crate doesn't have): 11 passed, 0 failed. New tests: the rider beside the place, rooms with their
  party (also its JSON, and the private code absent), the join secret (and nothing of it sent while
  off), TRY AGAIN's clock, and the refresh in the pacer test.
- `cargo check` for `x86_64-unknown-linux-gnu` and `aarch64-apple-darwin`: clean.
- `clang -fsyntax-only -Wall -Wextra` on `port/hle/presence.c`: clean. The changed
  `host_service_presence` compiled against the real `host_api.h` and `dhd_presence.h`, with
  `_Static_assert(sizeof(HostPresence) == 56)`, in a scratch file (host.c itself needs SDL's headers).
- 2026-10-31, with the SDK: 12 passed, 0 failed, 2 ignored (the live ones). `cargo check` for
  `x86_64-unknown-linux-gnu`, `aarch64-apple-darwin` and `aarch64-linux-android`: clean. Against the
  Discord running on this PC (`\\.\pipe\discord-ipc-0`), each showing a public room for 5 s and then
  clearing it:
  - `the_sdk_against_discord` (SDK 1.10.19337's DLL, from outside the repository): the first try was refused for
    the missing party id (above); after `party_id`: `update: Some(Ok(()))`, passed;
  - `the_ipc_against_discord`: Discord's answer had `"party":{"id":"dhd-199393b983dbf5ad","size":[3,6]}`,
    `"state":"Room K7M2QX"`, `"details":"Online room: Alpine MX"`, both buttons, the start time, and
    no `large_image` (the missing asset); passed.
  The profile itself was not looked at.
- Later on 2026-10-31: `cargo test` (as above): 14 passed, 0 failed, 2 ignored, no warnings.
  `clang -fsyntax-only -Wall -Wextra` on `online_menu.c` and `presence.c`: clean, and
  `host_service_presence_join` against the headers in a scratch file: clean. `discord_sdk.py`:
  unpacks and checks the pinned DLL from the cache; with an empty cache it stops with where to get
  the archive. `package_release.layout()` called with dummy executables (no build): Windows without
  `--discord-sdk` has the same six files as before and no SDK notices; with it, the DLL and the
  notices; Linux with the flag, no DLL (`main` refuses the flag off Windows). `package_release.py`
  itself was not run.
- **Not run:** clippy (not installed for the 1.97.1 toolchain here); `build_wasm.py`,
  `build_host64.py`, `package_release.py` (kept off this machine while another build ran). The core
  must be rebuilt with the host because the struct grew. No game run and no Discord: so the rider
  word at +0x290 in single and career races, the room's track line, the "(3 of 6)" rendering, the
  TRY AGAIN restart and the refresh on a restarted Discord are checked only by unit tests and by
  reading.
