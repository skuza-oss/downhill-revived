/* Join by link: a room is shared as a link that carries its join code, e.g.
   https://dhd.skuza.app/join/K7QX2M (docs/online-protocol.md, join codes). This is the one place
   the code is taken out of (or put into) a link; the game's ONLINE page builds the link it shows
   and copies, and a player may paste either a code or a whole link — both end at net_join_room.
   Pure C with no port dependencies, so it is compiled into both sides: the game core (online_menu.c)
   and the host net.c (net_join_room). The launcher (port/launcher/src-tauri/src/join.rs) follows
   the same rule. */
#ifndef DHD_NET_JOIN_H
#define DHD_NET_JOIN_H

#include <stddef.h>

/* The server's join alphabet: 6 characters from this set, no 0 O 1 I L. */
#define JOIN_ALPHABET "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
enum { JOIN_CODE_LENGTH = 6 };

/* 1 when code is exactly JOIN_CODE_LENGTH alphabet characters (the case does not matter). */
int join_code_valid(const char *code);

/* The join code the text names, upper case, into out (JOIN_CODE_LENGTH + 1 bytes or more): the
   text itself when it is a code, else the code inside a link (".../join/K7QX2M", "dhd:join/K7QX2M",
   "https://host/K7QX2M"), else a lone six-character token. 1 when a code was found. */
int join_code_from_text(char *out, size_t size, const char *text);

/* The shareable link for a room: "<scheme>://<host>/join/<CODE>", from the relay's base URL
   (wss:// -> https://, ws:// -> http://; anything without a scheme gets https://). 1 on success;
   0 when the code is not one or the base names no host. */
int join_link(char *out, size_t size, const char *base_url, const char *code);

#endif
