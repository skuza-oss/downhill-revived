# 237: phones get every online mode

Status: in progress

Branch `feat/mobile-all-online` from `main` 2e7f48c6, started 2026-11-14. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner, 2026-11-14: "Let's allow mobile to play all online modes normally." Android and iOS
offer the desktop's ONLINE menu: Quick Race, Public Races, Create A Public Race, Create A Private
Race, Room Region, Create A P2P Race, Join With A Code; the non-P2P rooms over the same SFU
transport (Cloudflare Realtime, TURN over TLS as the fallback, finding 177). This replaces finding
143's "no online on phones" and the P2P-only rule of findings 180, 186 and 224 for Android and iOS.
The browser is out of scope: it keeps P2P rooms only.

Done when: `check.py quick`, `port`, `docs` pass; boot and race state traces equal the base build;
on the Mi 11i (release-signed PGO APK of this branch, official server, private rooms): the menu
shows every row, an SFU private room made on the phone and on the PC each joined by the other, a
public room seen in the PC's list, Quick Race from the phone, a P2P room, Home for 10 s in an SFU
race, and the log's SFU link kind with no address of the other player.

## Progress

- [x] 2026-11-14: the one limit removed: `port/net/net.c`'s `NET_OFFERED` was `NET_OFFER_P2P` under
  `__ANDROID__` / `TARGET_OS_IPHONE`; every native build now offers SFU and P2P rooms, and the
  refusals that followed from it (`connect_to`, `net_join_room`, `net_list_lobbies`) are gone.
- [ ] docs and players' texts
- [ ] PC checks, state traces
- [ ] phone runs a-f

## Decisions

- The server needs no change: it never looks at a platform (nothing in `online/server/src` reads
  one); a phone is a member like a PC. No deploy.
- The browser keeps its own switch (`port/web/www/net.js`, `NET_OFFER_P2P`), as the owner said.

## Surprises

## Result
