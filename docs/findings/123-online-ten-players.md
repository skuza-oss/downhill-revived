# 123: Ten players in an online room

Each PC still runs one human. The other humans take computer-rider slots.
The split-screen controller and camera arrays stay at four. The online room,
race, loaded and GO arrays now hold ten. The puppet leaver arrays also hold ten.
Wire rider IDs were already global IDs in 0–9. Hits, finishes, results and
leaver hand-over already walk the race's rider count.

## Track limits and start places

The sources are `func_001B89C8` and `func_00182290`. With one human, type 2
has nine computer riders, type 1 seven, type 0 five. MOSH also has five.
Submode `x10C == 10` reduces type 2 to eight computer riders. Online uses
the ordinary single race. Computer riders OFF still keeps every human puppet.

- Ten: 0 ALPINE, 1 MOAB, 2 JUNGLE, 3 PERU, 5 GLACIER, 11 AUBER ST,
  12 FUJI, 19 BC, 20 CITY.
- Eight: 4 PERU TDH, 10 JUNGLE TD, 13 MOAB TDH, 16 AUBER STD,
  18 CITY TDH, 22 GLACIER T, 24 ALPINE T.
- Six: 6 ALPINE MX, 7 MOAB MX, 8 JUNGLE MX, 9 PERU MX, 14 GLACIER X,
  15 AUBER MX, 17 CITY MX, 23 MOSH, 25 BC TDX.

Before changing the grid, I read the start markers in all LVL NGP files on
the local USA disc. Large tracks have marker 8066 and ten places. Medium
tracks have 8064 and eight. Small tracks have 8065 and six. The single-player
and multiplayer files agree.
`func_001A0498` reads the selected place at path +0xB0, with a 16-byte stride.
The retail builder `func_0019FF58` confirms the counts: marker bits 0x7E080000
make ten, 0x7E000000 eight, and 0x7E040000 six. It spaces gates six feet apart
in two rows. The scanned positions are distinct on every racing level.
The existing global-ID start-point selection and no-random-swap patch already
give distinct, shared places. No extra places were added.

The scan used the repository's local NGP reader. No extracted data enters git.
Cloudflare's Workers best practices page was consulted for the server work:
https://developers.cloudflare.com/workers/best-practices/workers-best-practices/
The relay hot path does not change.

## Room and wire

Public and private rooms default to ten. The host sets 2–10 through ROOM RULES.
The picker calls the game's track classifier and shows `NAME - LIMIT`, using
ASCII glyphs. It keeps the same pickable tracks. The game sends limits with
the rotation; there is no server track table. NEXT and RANDOM skip tracks
smaller than room membership. At grid lock a small selection switches to the
next fitting track, keeping all ready players. A ten-rider entry is required.
The notice names the old track, its limit and the replacement.

Protocol 2 keeps `/v1/` URLs. Settings add parallel `trackLimits` beside
`rotation`. `lastEvent` can be `track_switch:old:limit:new`. The host import
packs the limit class above the low five track-ID bits of each byte. Both
native and browser bridges decode those bytes into the same JSON fields.
Snapshots keep plain rotation IDs. False choosing and zero bike fields are
omitted and retain their existing receiver defaults.

`NetLobby` has ten member, racer and result entries. The browser generator had
its own hard-coded eight; it and the browser's three array slices are fixed.
The game had a four-racer clamp and a four-entry GO-time array/loop; both are
fixed. `RaceSetup.rider[4]` is the offline controller setup and stays four.
The name label loop already walks all active riders. Its trailing four-entry
`func_0020F420` loop is GS text-context cleanup and must stay four.
Ten maximum 255-byte snapshots, each with a size byte, plus the frame header
take 2562 bytes. A compile-time assertion checks this against NET_FRAME_MAX.
There are no new per-frame relay operations.

The room has two columns of five cards. Each keeps name, host/you label,
ping bars, readiness, race state and live or last result. The existing rule
focus, pad, keyboard, mouse and wheel paths also handle the capacity row.

## Checks

Server: 56 tests pass, including the ten-member room and lobby count, default
and every capacity 2–10, NEXT/RANDOM skipping, grid-lock switch and notice,
and the UTF-8 snapshot budget with ten members, prior results and 64 entries.
An eleventh member is refused. Malformed limits and rotations without a
ten-rider fallback are refused. Browser message tests pass. Generated ABI
checks pass for four targets. Native network tests pass, including slot 9.
Nettool builds. The wasm core links strictly with no signature conflicts
after thunks; its offset scan is clean, core ID fd03b2d06f13d9d9.
Hidden-desktop room races, this PC hosting against nettool bots:

- 9 bots on ALPINE (10 riders): the room shows PLAYERS 10/10 in two columns, all ten start at
  distinct gates, the nine puppets ride with their names, and the room lists all ten results.
- 9 bots with the room on ALPINE MX (6): `ALPINE MX TAKES 6: RACING AUBER ST`; all ten race
  AUBER ST.
- 3 bots on PERU TDH (8) with computer riders on: 4 players and 4 computer riders. The next race
  went on to GLACIER.

Scripted hosts start their room on a chosen track with `DHD_NET_TEST_TRACK=<game track index>`
(with `DHD_NET_SCRIPTED`): posted keys don't reach the room while the scripted pad runs.
The room's host card reads HOST (it read H), and the rules line says AUTO without its 30 s so it fits
the narrower column.

## Merge with spectate

`online.c` changes the release-time nslots clamp, their_go storage, sender
indexing and GO loop,
and adds a STATE batch size assertion. It leaves state_sync's return to room,
race_banner and pause rows alone. `puppet.c` only changes NET_SLOTS from 8 to
10, outside the camera code. No spectate changes were copied into this lane.
