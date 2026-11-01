# 132: staying ready when the host changes the track

A ready player had to press ready again every time the host picked another track or lap count:
the server clears every member's ready flag on such a change (`room.ts`, the `settings` message:
"A new track or new options make a different race: everyone readies again for it").

The client now does it for them (`port/net/client.c` `read_room`, no server change): when a
`room` message shows this player no longer ready, the room's `settings.track` or `settings.laps`
different from the last one, and the player neither racing nor on the rider screen, it sends
`{"type":"ready","ready":true}` again. A player who unreadied themselves, or whose ready went for
another reason (a race's end), stays unready. Since everyone ready starts the countdown, a host's
track change in a room where all were ready leads straight to the countdown.

`keep_ready` in dhd.ini (`DHD_KEEP_READY`, built-in 1; `table.rs`): 0 turns it off.

Checked: `port/net/test_keep_ready.c` (in `build_tests.py`): a track change and a lap change
re-ready; an own unready, a player who wasn't ready, the rider screen and `keep_ready = 0` don't.
