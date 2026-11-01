# 133: The server's `start` needs two members; a test that lost its frame

## A race of one

The protocol (`docs/online-protocol.md`, "Retail preview flow") says the host starts "when all
present members are ready, with at least two members". Every other place keeps that rule:

- the countdown, `countdownReady` in `online/server/src/room.ts` (`st.members.length >= 2`);
- the race room's Start, `can_start` in `port/hle/online_menu.c` (`l->nmembers >= 2`, shown as
  "NEED 2 PLAYERS");
- the older online flow in `port/hle/online.c` (`all_ready`: `lobby.nmembers >= 2`).

The server's `start` handler (`onStart`) did not: it only checked that every member was ready, so
a host alone in the room could start a race with one slot. Our clients never send that, but a
modified client or `nettool --players 1` could. The race then ran with nobody to relay to, and the
room left the public list and refused joins (`room_started`) until the host left.

Fix: `onStart` answers `not_ready` ("Need 2 players to start.") when the room has fewer than two
members, before it checks who is ready. That is an existing non-fatal error code, so no client
change and no protocol revision are needed. The protocol's `start` line and its list of
non-fatal errors say so now. New test: `does not start with the host alone`
(`online/server/test/room.test.ts`). Without the fix it fails: the server answers with `start`,
not the error.

## A test that passed by luck

`keeps settings host-only and lets players pick their own rider` failed 3 times out of 3 on the
unchanged tree here (2026-10-30, vitest 4.1, `@cloudflare/vitest-plugin` from the lockfile), and
then intermittently. The test sent good settings, then `track: 999`, then waited for the `error`.
`Client.json()` discards every frame before the one it waits for (`Client.next`,
`test/helpers.ts`), so it threw away the `room` snapshot that the good settings produced, which
the server sends before the error. The test's next wait then needed another snapshot. The only
one left was the alarm's re-broadcast every `PING_INTERVAL_MS` (2 s), and that wait's own timeout
is also 2 s. The test now reads the snapshot before it sends the bad value. 8 runs out of 8 pass
with both changes.

## Checks

- `npm test` in `online/server`: 33 of 33, 8 runs in a row. `npm run typecheck` passes.
- With `onStart`'s guard reverted, only the new test fails.
- Not deployed. Like finding 115's server change, this takes effect when the server is deployed
  (`online/server/README.md`). Older clients are unaffected: they never start alone.
