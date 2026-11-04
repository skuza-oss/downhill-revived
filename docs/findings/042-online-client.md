# 042 — Online client: lobby and WebSocket transport (and the dropped lockstep core)

Status: partly superseded by 044 (the lockstep core; the WinHTTP transport, now Rust dhd_net in 099)
Checked: 2026-11-04

Do not bring back the lockstep core (`lockstep.c`: input delay, INPUT frames, per-field input
agreement, state hashes): it was dropped on 2026-10-16 and deleted; races are state sync (finding
044). Still true: the lobby client, JSON, XXH64, pings and clock offset, the lobby rejoin, and the
field-pacing rule below (cited by `port/hle/kernel.c` `kernel_pace_slip`). Changed: the transport
is the Rust crate `dhd_net` behind `port/net/ws.h` on every native platform (finding 099; no
WinHTTP), `NET_MAX_SLOTS` is 10 (finding 123), and a rejoin takes the member back with its rejoin
token (finding 137). The lockstep mechanics, its tests and the integration plan were cut (full
text: `git show 860ab6b:docs/findings/042-online-client.md`).

2026-10-16. Step 2 of `docs/ONLINE-PLAN.md`: the game-side network module, `port/net/`.

## What is there now

| File | What |
|---|---|
| `net.h` | The API the port uses (lobby, race frames, results). |
| `hash.c` | XXH64, written from the published specification (Cyan4973/xxHash `doc/xxhash_spec.md`, BSD 2-clause; no code copied); it matches the reference values for "", "abc" and a 39-byte string. |
| `json.c/h` | Reads lobby JSON by path (`members[2].name`) and escapes strings. |
| `client.c/h` | One player: lobby state machine, race frames, pings, clock offset, offline fallback. Thread-safe. |
| `ws.h` | The WebSocket/HTTPS client, the Rust crate `dhd_net` (`port/render/net`). |
| `net.c` | `net.h` over one client and one connection. |
| `nettool.c` | A command-line player (bots for tests). |
| `test_*.c`, `build_tests.py` | The tests (`python port/net/build_tests.py`). |

## How the lobby side works (still so)

- **Lobby rejoin.** A drop in the lobby (not a refusal, close codes 4000-4099) rejoins the room by
  its code with its rejoin token, then repeats the pick and ready. No rejoin during a race: the host's AI
  takes the rider over and this PC's race goes on offline (`client.c`).
- **Pings and clock.** Lobby: JSON `ping` every second; its `pong` gives the round trip and the
  server-clock offset from the lowest-RTT sample, which maps `startAt` to `net_seconds()`. Server
  pings are echoed; 10 s of silence drops the connection.

## Field pacing (the rule `kernel_pace_slip` keeps)

At first each test process paced its fields from a fixed `startAt`. A PC whose clock
estimate ran early stayed a field ahead for the whole race. It waited about 16 ms on almost
every field: 6-10 s of waiting in a 10 s race, while the other PCs waited almost nothing.

When a PC waits for the others (then `net_inputs_for_field`; now the start wait of a race), the
frame clock has to **slip** by the wait instead of catching up. With that, the early PC waits once and then runs in step. Two live races then
waited 0-236 ms in total per PC, all of it single jitter waits of at most 67 ms. The port's
virtual frame clock keeps the same rule during online races (`port/hle/kernel.c`).
