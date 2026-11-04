# 115: The race room, laid out (layout B)

Status: partly superseded by 132 (no track thumbnails)
Checked: 2026-11-04

A player asked for three things in the online room: READY in green, a sound when someone
joins or gets ready, and no way to spam Ready. The room was a column of menu rows. This work
replaces it with a screen laid out as the HTML mock-up the players chose (layout B), still in
the game's own panels, font, cursor bar and sounds.

## What the room shows

- **Left panel**: the track in large type (the host changes it with left/right, or Cross opens
  every track in a 4-column list), Computer Riders, Public/Private, and the join code
  (Circle or a click copies it through `host_service_clipboard_set`).
- **Right**: a card per place. Each card has the slot number, the name with HOST and YOU tags,
  the player's state, their rider, and their ping in ms with three bars. Places not yet filled
  show OPEN.
- **Action bar**: Choose/Change Rider, Ready, Start and Leave. When Start can't be used it
  says why ("2 NOT READY", "NEED 2 PLAYERS"). Guests see HOST STARTS.
- **Status line**: what happens now, e.g. "WAITING FOR BO", "BO IS CHOOSING A RIDER",
  "EVERYONE IS READY: UNREADY TO STOP".
- **Footer**: the buttons, as the game's glyphs (Cross select, Square ready, Circle copy code,
  Triangle leave).
- **Toasts** at the top right: who joined, left or got ready, each with a sound (S_SELECT,
  S_BACK, S_NEXT). Each player's ready sound plays at most once every 3 s.
- **Countdown**: once everyone is ready, the title counts down ("RACE STARTS IN 5") with a
  green bar under it. Unready stops it; the host's Start NOW starts at once.
- **Leave asks first** (Stay / Leave). The dialog says what happens: who becomes host, or that
  the room closes.

Colours mean one thing each:

| Colour | Meaning | Where |
| --- | --- | --- |
| green | ready | READY tag, card stripe, Ready cell, countdown bar |
| amber | not ready yet | NOT READY tag, Start's reason |
| red | a problem | ping of 150 ms or more, a player who left |

## How it is built

- `menu_page.c` gains a small drawing layer for a page that lays itself out (`menu_draw_*`):
  - the popups' translucent box (`func_00190E48` with `func_00190DB0` for the shade, one
    packet buffer per box, because `func_002103D8` queues it by reference);
  - flat and blended sprites (GIF PRIM 0x06/0x46);
  - the cursor bar (`func_0018F9C0`);
  - text in the menu's UiStyles, resized and recoloured (`func_0020FB80`, `func_0020F518`,
    measured with `func_0020F860`);
  - front-end sounds by name (`func_001C57A8`).

  The view is the popup's 640×224. A page with no rows gets no list panel and no cursor from
  the base update, so the room draws itself and reads the pads itself, with edge detection,
  a d-pad repeat after 24 fields then every 6, and mouse hover, click and wheel through
  `MenuPage.mouse`.
- Drawing from port code, learned the hard way:
  - **Packets must be in EE RAM.** The DMA masks addresses to EE RAM, and the port module's
    statics are outside it. The room's packet pools come from the game's heap
    (`func_001FD430`, its `operator new`) once.
  - **One spare qword per packet.** The DMA tag `func_00190E48` writes counts its own qword, so
    the DMA sends one qword past the packet. The game's buffers have a zero there. Each packet
    is zeroed first; otherwise the next packet's tag goes to the GIF and every later draw,
    the game's text included, is lost. A box is 14 qwords.
  - **Draw after the base update.** `MenuPage.draw` runs after `func_0018DC20`.
  - **Text is drawn over every box.** An overlay (track list, leave question) is drawn alone,
    not over the room.
  - The popups draw on every other field (30 fps), as the game's own do.
- **A pad word can read all ones for one field** as a press starts (`func_00220230` returns
  `~0`). Read as buttons, that is every button at once: Up in the room became Cross on Leave.
  The room and the rider screen's Back check skip such a field.
- Button glyphs in the game font: 0xAD Cross, 0xA7 Triangle, 0xA0 Square, 0xAB Circle,
  0x8D/0x8F left/right.
- The server (`online/server/src/room.ts`) owns the rules everyone has to agree on. See
  `docs/online-protocol.md` § Race room rules:
  - a changed track or option unreadies everyone;
  - two or more players may ride the same rider (issue #17);
  - `choosing` marks a player who is on the rider screen;
  - the countdown (`countdownEndsAt`, 5 s) runs only when every client sent the `countdown`
    feature, so 0.7.1 clients in the room keep the host-starts flow.
- The client sends choosing as `net_pick(-1, -1, 0)` (rider −1 means choosing), which needs no
  new import. Reopening the room re-sends the kept pick with `choosing:false`.
- nettool gains `--pick N` (0 = stays choosing) and `--ready-after S` (below 0 = never) for
  room tests.

## Not done yet

- Track thumbnails (in the mock-up) need the game's track preview art loaded at run time.
  The first build shows the track name large instead. (Done in finding 132: the picker shows the
  track's loading-page picture read from the player's disc.)
- The server change had not been deployed then. Deploying it together with a client release lets
  the countdown and rider ownership work for everyone.

## Tests

- `online/server`: `npm test` passes the race-room tests (unready on settings change, a
  shared rider with everyone ready, choosing, the countdown starting the race, unready stopping it,
  and no countdown without the feature).
- `port/net/build_tests.py` passes (json, hash, loopback).
- Screens: local `wrangler dev`, the x64 build driven by `DHD_AUTOPAD_FRAMES` (ONLINE, Create
  A Private Race, the rider screen, back to the room), nettool players with `--pick` and
  `--ready-after`, and `DHD_CAPTURE_FIELDS` pairs. Shown: the room with a ready, a not ready
  and a choosing player; the track focus, the track list and the leave question; a late
  join's toast; the countdown, which then started the race by itself.
