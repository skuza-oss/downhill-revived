# 117: The race room's CHEATS setting, and where the game reads cheat codes

Status: current
Checked: 2026-11-04

Players asked that cheat codes not spoil online races (suggested by Noireadas Slytherin on
Discord). Since 0.7.3 the race room has a host setting, **CHEATS: OFF / ON**, OFF by default. With
it OFF, a code typed in an online race does nothing, and the race starts with no cheat on. With
it ON, and offline, cheats work as in the original. The online "everything unlocked"
(finding 095, `port/patches_unlock.py`) is a separate thing and is not affected.

## How the game reads a code

- `func_001E6F58` is the human rider's pad reader, at rider +0x1420. It only reads a pad when
  rider +0x7A5C is 1 (a human on this PC). It groups the presses: a group ends after more than
  2 frames with no new press (`D_0029D7D0`). After 20 quiet frames (`D_0029D7D4`) it matches
  the groups against the code table `D_00272448` (27 rows of {id, 8 buttons}: `func_001E72F0`,
  and `func_001E73B0` for the last 8 presses). The code's number goes in rider +0x1E3C (the
  reader's +0xA1C) for that one frame; 0 means no code.
- `func_0012C8B0` calls `func_0012C9E0` for human riders in race phases 2-4 only. So codes
  typed in the countdown or the flight over the track do nothing. `func_0012C9E0` asks
  `func_0017F050(part, n)`, which goes through `func_001E7460`, once for each code. Code 1
  ("Codes": Up, Triangle, Down, Cross, Left, Circle, Right, Square) toggles code entry at rider
  +0x1E3A. Codes 2-26 count only while code entry is on; 21 (Freeze Trick) counts even when it
  is off, but `func_0012C9E0` has no case for 21 or 23 (You Win). A code that takes effect sets
  rider +0x7BB8 to its number. `func_0012CF68` then shows the code's name from
  `D_00272818[n]` (`func_001E6E80`): this is finding 065's "cheat-message text".
- The effects are the bytes at rider +0x7BA8..+0x7BB6:

| Byte | Code | Name | Read by |
|---|---|---|---|
| +7BA8 | 4 | Super Bunny Hop | `func_0012E028` (jump scale 5) |
| +7BA9 | 6 | Anti Gravity | `func_0012C9B8` |
| +7BAA | 5 | Speed Freak (also sets +4214 to 1.5) | `func_0012C990`, `func_00136790`, `func_001634E0` |
| +7BAB | 7 | Super Smack | `func_0012E940` |
| +7BAD | 2 | Infinite Bottles (+6390 4, +7ADC -1) | `func_00130A88` |
| +7BAE | 8 | Extra Smack Time (copied to `D_0043EF2C`) | |
| +7BAF | 9 | Super Bounce | `func_00131998` |
| +7BB0 | 16 | Mega Flip | `func_00174968` |
| +7BB2 | 19 | Always Stoked | `func_001E61E0`, `func_001E5680` (stoke meter full) |
| +7BB3 | 1 | Codes (the message's "Unlocked") | |
| +7BB4 | 24 | Unlock Everything's marker | |
| +7BB5 | 25 | Buy Everything's marker | |
| +7BB6 | 26 | Combat Free | `func_0012E940` |

  Codes 3 (Combat Upgrade), 22 (Upgrade To Bottle), 17 (Adrenaline Boost), 18 (Stoke Trick Meter) and 20
  (Energy Restore) act once. 10-15 unlock bios and videos or add cash in the profile. 24
  (Unlock Everything) turns most of the flags on and calls `func_001F0780`.
- `func_0012C450` resets a rider when a race loads. It clears these flags, except +7BB2
  (Always Stoked, as finding 063 found), +7BB4 and +7BB5, so an Always Stoked turned on offline
  is still on in the next race. A test run set all 15 bytes and `D_0043EF2C` to 1 before an
  online race loaded. At GO only +7BB2, +7BB4 and +7BB5 were still on, and `D_0043EF2C` was 0
  again, so something else in the load resets it.

The code table and names above were read from the retail ELF on this PC. GameFAQs and the
fandom wiki block fetches (HTTP 403/402), so they were not used here. They are not in the repo.

## What the port does (port/hle/online.c, port/patches.py)

- The setting is bit 2 of the room's options (`ROOM_CHEATS`, `online_menu.c`). These travel
  in the server's `laps` field next to bit 1, computer riders off. The server stores any byte
  there, sends it to every member, and unreadies everyone when it changes
  (`online/server/src/room.ts` `onSettings`). So **the server needed no change**. A new room
  starts with the bit clear. A guest who becomes host keeps the room as it is.
- `func_0012C9E0` is patched to call `hle_cheat_code(obj)` before it reads the code. In an
  online race (`on.released`) whose room has cheats off, it logs the code and zeroes rider
  +0x1E3C, so all of the function's checks fail. The function still runs on, so the name tags
  (`hle_online_names`) and any message still draw. `hle_cheat_code_done` after it logs each
  code that was not blocked (offline, or online with cheats on), with the code-entry state and
  the flags now on.
- At GO online (`on.started`, `cheats_start`), with cheats off, the local rider's flags
  +7BA8..+7BB6 and `D_0043EF2C` are cleared. Where Speed Freak or Infinite Bottles was on,
  their own "off" values are restored (+4214 = 1.0, +7ADC = 3). Code entry (+0x1E3A) is left as
  it is, because blocked codes do nothing either way.
- The log (dhd.log): `online: race starts: ..., cheats on|off`; `cheats: the room's cheats are
  off; cleared: ...` or `... are on` at GO; `cheats: code N (name) blocked: the room's cheats
  are off`; `cheats: code N (name)..., code entry on|off; on: +7BB2 ...`. In the room:
  `online menu: room CODE: cheats ON|OFF` on every PC when the room's setting is first seen
  and on each change. On the host: `online menu: the room's cheats on|off`.
- Tests: `DHD_NET_CHEATS=1` makes a `DHD_NET_CREATE` host's room start with cheats on.

## Checked (2026-10-30, Windows, two minimized games through the official server)

The race pad typed Codes, then Always Stoked (19), then Super Bunny Hop (4), from 4 s after GO.

- Offline: all three took effect (`on: +7BA8 +7BB2 +7BB3`).
- Online with cheats off: both PCs logged `cheats off` at race start, and every code reached
  was logged as blocked.
- Online with cheats on (set only on the host, `DHD_NET_CHEATS=1`): the guest's race start also
  said `cheats on`, so the setting reaches the guest. The codes took effect on both PCs.
- With all flags set before the load (a test-only build, not committed), GO cleared
  `+7BB2 +7BB4 +7BB5`.
- In each online run the guest missed one or two of the later codes; the host never did. A
  missed code was never applied, and with cheats off it was not logged either. The cause was not
  looked into. The pad reader counts its quiet time in frames, and online races run at the
  player's frame rate, so this may be timing.
- The room screen's CHEATS row was not checked by eye. The scripted races skip the room menu.
