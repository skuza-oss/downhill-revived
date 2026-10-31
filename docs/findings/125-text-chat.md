# 125: Text chat in the race room and the race; name tags

Players asked to talk in a room without a second app. The server side came first (commit
d5b5622, `docs/online-protocol.md` § Chat): a member sends `{"type":"chat","text"}`, everyone in
the room gets `{"type":"chat","memberId","name","text","at"}`, the sender too. The server cleans
the text (control characters, runs of spaces), takes 1-120 characters and 5 lines in 10 s from a
member, else the non-fatal error `slow_down`. This is the game's side.

## The client (port/net)

- `client.c` reads `chat` into a ring of the last 8 lines (`NetChat` in `net.h`: seq, sender's
  member id and name, text, server time), oldest first; each line has a number that only grows,
  so a screen knows what it has already shown. A `slow_down` refusal also becomes a line of its
  own (member id -1), so the player sees why a line did not go. A new room starts with an empty
  ring (`connect_to` sets the client up afresh); nothing is kept, as on the server.
- `client_chat` does what the server would before sending: control characters as spaces,
  trimmed, at most 120 bytes, never cutting a UTF-8 character. An empty line is not sent.
- `net_send_chat`, `net_get_chat` and their services `host_service_net_send_chat(text)` and
  `host_service_net_get_chat(out)`. The import generator checks the text as a string and the
  NetChat as a span (`gen_host64_imports.py`). The browser exports them from `dhd_net.wasm`
  (`build_net.py`) and `net.js` copies the text in and the NetChat out, as it does the NetLobby
  (`web_net_chat_size`). worker.js takes every net service from net.js, so it needs nothing more.

## In the race room (online_menu.c)

The room is full (finding 115's layout), so the chat is drawn over it alone, as the track list is:

- **L1** or **Y** (as in Counter-Strike) opens it; a click on the **L1 / Y CHAT** tag at the top
  left too. The tag counts the lines that came while it was closed, in green.
- The last lines, newest at the bottom, broken between words to the panel's width. The player's
  own lines are in the highlight colour, the client's notes in red.
- **SAY:** the line being typed, through the text field the code and name entry use
  (`host_service_text_input_*`): Enter says it and stays open for more, Esc closes.
- The quick lines for a pad, no keyboard needed: **GG**, **READY?**, **NICE!**, **ONE MORE?**
  (left/right and Cross, or a click). Triangle or L1 closes.
- Closed, a line from someone else is a note at the top right (the toasts of finding 115) with
  the S_NEXT sound.

The room's countdown goes on while the chat is open; the chat closes when the race starts, when
the player goes to the rider screen, and when the room is lost.

## In the race (online.c)

- **Y** on the race screen opens a line under the chat (**SAY: ..._**); Enter says it, Esc
  closes. The controller keeps riding meanwhile: text_input.c keeps the typed keys (and Enter,
  Esc) from the pad while the field is open and until they are let go, as for the join code.
  Y is free in keyboard.c's layout.
- The pause menu shows **CHAT: L1 GG R1 READY? L2 NICE! R2 ONE MORE?**; those four buttons say
  the line and do nothing else there (held as the menu opens, a trick button is not a press).
- The last 5 lines show at the left of this player's view, mid-height, for 8 s and fade over
  the last second: white for others, the player's own in yellow, notes in red, with a black edge
  as the names over the riders have. They are drawn in hle_online_hud (func_00176ED8's hook) in
  the HUD prompt's font calls (func_0020F588 for the colour, func_0020FB80 at 80 % of the prompt's
  size, func_0020F9A8 left-aligned), only in the local rider's view (rider 0's +0x7988), and the
  pen's saved state is put back.
- Lines said in the room before the race are not shown again.

Y needs one new host service, `host_service_key_held(key)`: 1 while a key that types `key` is
held, the window has the keyboard and no field is open (text_input.c over keyboard.c's state;
worker.js reads its key array, which main.js fills by key place). The room and the race take
its edges.

## What a line may have

The game's font draws button glyphs for bytes 0x80 and up, so a line is shown in capitals with
`A-Z 0-9 space . , ! ? ' - : /` (characters the game's own texts use) and anything else as `?`
(`port_chat_clean`); a UTF-8 character is one `?`. Typing takes the same characters
(`port_chat_letters`).

## Checks

- `python port/net/build_tests.py test_hash test_json test_loopback`: `test_hash: ok`,
  `test_json: ok`, `test_loopback: ok`. The loopback relay now does the server's chat: a line
  from each of four players reaches all four in order, a blank line is not sent, a line is
  trimmed, a 121-byte line whose last character is two bytes is cut before it (119 bytes), the
  ring keeps the last 8, and the sixth line in a row comes back as `slow_down`, a note for its
  sender only (`chat: 9 lines, slow_down after five`).
- `python port/web/build_net.py`, then `node port/web/check_net.mjs ws://127.0.0.1:8788`
  against `wrangler dev` (online/server at this commit): the guest says `  READY?  `, both get
  `GuestB: READY?`; `PASS`.
- `clang -fsyntax-only` (wasm32-wasip1, the module's flags) on online.c, online_menu.c,
  menu_page.c; `clang-cl /Zs` on text_input.c: no new warnings.
- Not built or run: the game core (`build_wasm.py`), the native host (`build_host64.py`) and the
  browser page (`web/build.py`). Nothing here has been seen on screen: the chat panel's layout,
  the race lines' place and size, and whether the HUD is drawn while the pause menu is open
  (the quick lines' legend needs it) are to be checked in a real room. Not deployed: the server's
  chat goes live with the next deployment. **Deploy the server before releasing this client**:
  a server without chat takes the line as an unknown type, answers `bad_message` and closes the
  socket (4000), which the client reads as a refusal and leaves the room.

## Name tags (online.c hle_online_names)

The other players' names over their riders were all one size and white from 1 to 150 units
away, so riders far down the hill covered the track with names. Now:

- **Distance**: as before up to 45 units (`NAME_NEAR`); beyond, the tag shrinks to 70 % and
  fades to nothing at 150 (`NAME_FAR`, the old cut-off), its black edge with it. The size goes
  through func_0020FB80 before the width is measured, so the on-screen bounds check (the HUD rule
  that a tag is drawn whole or not at all) still holds.
- **Colour**: each racer's tag has a colour by race slot (8 pastels at the PS2's 0-128 scale),
  the same on every PC, so a player is known at a glance.
- **Away**: a racer whose connection dropped while the server holds its slot (finding 126,
  `NetLobby.racers_away`, read with the `left` check every 15 fields) reads `NAME - AWAY`, grey
  and at half strength, until it is back or has left.

No ping marker: a racer's ping is not known during a race (the room's `rttMs` stop with the
lobby pings). Checked with `clang -fsyntax-only` only; not seen on screen.
