# 140: Rejoin after a drop: the lobby slot and the race slot

Branch `server-rejoin`, 2026-10-31. Server: `online/server/src/room.ts`, `src/protocol.ts`,
`test/room.test.ts`, `test/helpers.ts`, `vitest.config.ts`. Client: `port/net/client.c`,
`client.h`, `net.c`, `net.h`, `test_loopback.c`; the room screen `port/hle/online_menu.c` and the
boot barrier's title in `port/hle/online.c`. Protocol: `docs/online-protocol.md`, "Rejoining after
a drop".

## Before

- Server: a closed or errored socket went straight to `leave(memberId, "disconnect")`. The member
  was removed at once; in a race its slot went to `race.left` and everyone got `left`, so the
  host's AI took the rider (finding 047). A racer silent for 10 s left the same way (finding 135).
- Client: a lobby drop reconnected by the room's code up to 5 times, as a **new** member (new id;
  the client re-sent its pick and ready). A race drop set `offline` and `NET_FAILED`, and
  `check_left` (`online.c`) then ran every rider on this PC for the rest of the race. The
  protocol said "No reconnect in version 1".

## The design

**One secret per member, opt-in.** A hello whose `features` list `"reconnect"` gets a
`reconnectToken` in its own `welcome`: 32 random bytes (`crypto.getRandomValues`) as 64 hex digits,
stored on the member in the room's state, so it survives an eviction. Nobody else ever sees it
(it is in no broadcast). The same token serves the lobby and the race; the brief suggested a token
in `start`, but the welcome's one already reaches each racer alone and covers a lobby drop too.
On a successful rejoin it is rotated (the new `welcome` has a fresh one), so a token works once.
The check is constant-time (`tokensEqual`), and the room is per join code, so a token only fits
the room and the member that got it. The client sends only the token, never an id.

**A drop holds the slot for `RECONNECT_WINDOW_MS` (20 s).** `dropSocket` (close or error) and
`dropSilent` (finding 135's 10 s silence) call `detach` instead of `leave` for a member with a
token, in the lobby or in a race whose slot is still racing:

- lobby: the member stays in `room.members` with `"reconnecting":true` and keeps its id, name,
  rider, bike, ready flag and host role. No race starts while a slot is held (`start` answers
  `not_ready`, the countdown does not run): its client would not get `start`.
- race: the room broadcasts **`away`** `{"slot","memberId"}` at once and stops relaying to the slot
  (`raceSockets[slot] = null`). The slot is not in `race.left`, and the silence check skips it.

A hello on `/v1/rooms/<CODE>` with the right `reconnectToken` inside the window (and the host's
`version` and ISO, as any join) re-attaches the socket to the same member. In a race the server
also puts it back in `raceSockets`, `slotOf` and `heardAt` for its slot, adds `"slot":N` to the
`welcome`, and broadcasts **`back`** `{"slot","memberId"}` to everyone, the rejoiner too. Relaying
both ways resumes with the next frame. A welcome without `slot` to a client that was racing means
the race ended while it was away (the host sent `end`): it is in the room's lobby again.

A wrong, malformed, spent or expired token finds nothing, so the hello is an ordinary join: a new
member in the lobby, and `room_started` (4005) during a race. A token is never matched to a
member that is still connected. When the window runs out the alarm (`expireDetached`, armed for
the earliest expiry) removes the member through the usual `leave(..., "timeout")`: `left`
for a racer, host handover if it was the host. A `leave` message or a kick still removes the
member at once, token or not.

**Why `away` at once and `left` only later.** The other PCs need to know quickly that the rider is
not being driven, but `left` is final: `check_left` (`online.c`) hands the rider to the host's AI
and never gives it back. So the immediate signal is a new message type. Clients up to 0.7.3 ignore
unknown types (the `if` chain in `client_on_text`): they keep the puppet frozen at its last
snapshot, as they would for a silent racer, and resume it when snapshots come again (the puppet
ring only resets on a snapshot 60 ticks *older* than the newest, finding 044). Newer clients get
`racers_away` in `NetLobby` to show it. If the window ends, `left` comes as before.

**Older clients keep the old behaviour.** A member whose hello did not list `"reconnect"` has no
token, so its drop is `left` at once, exactly as before. Holding its slot would only delay the AI
takeover by 20 s with no way for it to come back. Old clients in a room with new ones see `away`
and `back` as unknown messages and `reconnecting` as an unknown member field, and ignore them.
`PROTOCOL` stays 1.

**What the others see meanwhile.** For up to 20 s the away rider stands still on the other PCs
(their puppets extrapolate 12 ticks and freeze). EVENT frames (hits) meant for its PC are lost;
the relay keeps nothing. If the host is the one away, its AI riders freeze too, and host-only
commands (`end`) wait until it comes back or `left` hands the host on. All of this is what
finding 135 already accepted for 10 s of silence.

## The client

- `client_connected`: `features` now `["countdown","reconnect"]`; while `NET_RECONNECTING` with
  a stored token, `reconnectToken` too.
- `welcome`: keeps the token (a welcome without one clears it: an older server). With
  `"slot"` equal to our race slot, the state goes back to `NET_RACING` and nothing is
  repeated (pick/ready are lobby messages); racing without `slot` is handled as `ended`.
- `client_on_closed` while racing: with a token, not after a refusal (close codes 4000-4099,
  e.g. 4005 when the token was refused), and within `REJOIN_MS` (22 s) of the drop, it returns 1
  (rejoin by the room's code), sets `NET_RECONNECTING` and `rejoining`, and leaves `offline` at
  0, so `check_left` does not take every rider over. Frames are not sent while disconnected
  (`client_send_frame` needs `connected`). Otherwise `offline` as before.
- `client_give_up` (new): `net.c`'s `on_close` calls it when ws.h's 5 attempts (backoff 1, 2, 4,
  8, 8 s) are spent: a race goes offline, a lobby reconnect fails (before, the state stayed
  `NET_RECONNECTING` with nothing more to come).
- `away`, `back`, `left`, `start`, `ended` keep `racers_away` up to date.

**`NetLobby` keeps its layout.** The game, the x64 host (`host_service_net_get_lobby` writes
`sizeof(NetLobby)` bytes into guest memory, `gen_host64_imports.py`) and the browser
(`port/web/www/net.js` copies `web_net_lobby_size()` bytes between two wasm32 modules) copy the
struct as it is. The new fields are three 4-byte words at the **end**, after `countdown_ms`:
`members_reconnecting` (bit i: `member[i]` is held), `racers_away` (bit s: racer s is away) and
`rejoining`. `NetMember` and `NetRacer` are unchanged (an earlier draft added a field to
`NetMember`, which would have moved every later offset). No pointers or 8-byte fields were added,
so wasm32 and x64 agree on the layout. Every module still has to be rebuilt together, as for any
`net.h` change.

**The game side.** The room screen shows a held member as `RECONNECTING...` (red stripe), the
Start cell says how many are reconnecting, and the status line says `RECONNECTING TO THE
SERVER...` while this PC is between connections. During a race nothing in the game uses
`rejoining` or `racers_away` yet: the race simply goes on, the frames stop and resume. Still to do
(the main session): a "reconnecting" line in the race HUD from `rejoining`, a marker on away riders
from `racers_away`, and deciding whether the host's AI riders should be handed on sooner when the
host is the one away.

## Tests

`cd online/server; npx vitest run`: 50 of 50 (3 runs in a row). New in
`describe("rejoin a race (finding 140)")`:

1. `holds a dropped racer's slot: away at once, back on rejoin, relayed both ways`: `away` to the
   two others; the others' frames still flow; the token brings back the same member id with
   `slot: 1` and a rotated token; `back` to all three; STATE from the rejoiner reaches both with
   slot 1, EVENT from slot 2 reaches it, its PING is answered with its slot.
2. `refuses a race rejoin with a wrong token, and the slot stays held`: a wrong token gets
   `room_started` and close 4005, the right token with another build `version_mismatch`; the
   right one still works.
3. `sends left when the grace period ends without a rejoin; the token is spent`.
4. `holds a silent racer that can rejoin instead of dropping it`: finding 135's silence gives
   `away` and a 1001 close, and the token still rejoins.
5. `tells a racer who comes back after the race ended that it is in the lobby`: no `slot`.
6. `leaves an older client's drop as it was: left at once, no away`.
7. `hands a racer who sends leave to AI at once, token or not`.

Plus the five lobby cases (`describe("reconnect in the lobby (finding 140)")`): the held slot
comes back with its state; wrong or absent tokens are new joins; a live member's token takes over
nothing; the window's end frees the slot; the host role survives and `start` is refused while a
slot is held. `vitest.config.ts` sets `RECONNECT_WINDOW_MS` to 300 ms. `npm run typecheck` passes.

`port/net/build_tests.py test_loopback test_json test_hash`: the fake relay now gives a token to
the player meant to rejoin and answers a hello with it by `welcome` with `slot` and a `back` to
all; its drop sends `away`. New run `a player drops halfway and rejoins its slot` (600 ms away):
the rejoiner is `NET_RACING` again, not offline, receives frames from everyone after it came back
(one gap per sender), and every frame it sent reached the others in order; the others saw
`racers_away` set, then cleared, and never `left`. The three old runs (including the drop with no
token, which still goes offline) pass unchanged. Output of the new run: the others got all 60
of the rejoiner's frames in order; the rejoiner got 46-47 of each other's 60 (the ~600 ms away);
`test_loopback: ok`, `test_json: ok`, `test_hash: ok`.

Negative control: with `canHold` refusing every race slot (a race drop is `left` at once, as
before), the five race-hold tests fail and the old-client and `leave` tests still pass.

`clang -fsyntax-only` on `online_menu.c`, `online.c`, `net.c` and `client.c` (SDL3 headers from
`build/port/sdl3`): clean.

## Not done

- Not deployed, and not tried with two real games (`port/tools/online_race.py` with one network
  link cut for a few seconds). The game itself was not rebuilt here (`build_wasm.py`,
  `build_host64.py`, `port/web/build.py` are the main session's).
- The race HUD's reconnecting notice and the away-rider marker (above).
- A held member still counts in the public list's player count.

## Sources

- Findings 044 (puppets and their ring), 047 (`check_left`, the AI takeover), 054 (`hello.features`),
  115/118 (server-owned room rules), 120 (the silence rule and the alarm).
- Cloudflare Durable Objects WebSocket hibernation and alarms, as already used in `room.ts`.
