# 121: Discord Rich Presence: rooms with their size, the rider, TRY AGAIN, a restarted Discord

2026-10-30, branch `discord-presence` (from 0.7.3). Follows finding 106. A friend looking at the
player's profile now sees a room's track and how full it is, the rider in a race, and an elapsed
time that starts again with each run. A Discord that was restarted gets the presence back without
waiting for the game's state to change. Ask to Join is designed and its presence half written, but
switched off (below).

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
  the size, so a size of 0 (not known yet) sends no party. No `party.id` is sent: Discord needs one
  only for joining, and a private room's code must not be used for it.
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

## Ask to Join: designed, presence half written, off

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

Turning it on means finishing 1 to 4, flipping `ASK_TO_JOIN`, and trying it with two Discord
accounts.

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
- `presence/src/worker.rs`: `party` and `secrets` in the activity; the party in the log line.
- `presence/src/tests.rs`: 11 tests (8 before).

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
- **Not run:** clippy (not installed for the 1.97.1 toolchain here); `build_wasm.py`,
  `build_host64.py`, `package_release.py` (kept off this machine while another build ran). The core
  must be rebuilt with the host because the struct grew. No game run and no Discord: so the rider
  word at +0x290 in single and career races, the room's track line, the "(3 of 6)" rendering, the
  TRY AGAIN restart and the refresh on a restarted Discord are checked only by unit tests and by
  reading.
