# 185: online code we don't maintain, removed

Status: in progress

Branch `chore/online-cleanup` from `main` 32d8a8cd, started 2026-11-09. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

The owner's decisions (2026-11-09): the game's minimum is 0.7.13 / protocol 4 (`MIN_VERSION` in
`online/server/wrangler.jsonc`); online code that exists only for older releases, for designs
that were replaced, or for one-off measurements goes:

1. the WebSocket race relay in the room (race frames travel over the SFU since 0.7.12, P2P rooms
   over peer connections), and any sender of race frames over the room WebSocket;
2. paths for protocol 2-3 clients and older releases (the "no clientVersion" 0.1.0 path, the
   lockstep frame types' leftovers, comments and branches about 0.7.11 and protocols 2-3); an
   outdated client still gets `client_outdated` (Update Now);
3. the puppet interpolation mode (`DHD_PUPPET_PREDICT=0`): only predict-to-now stays;
4. one-off benchmark code (`DHD_NET_BENCH`'s CSV probes in the client, `bench.h`, `test_bench.c`,
   `cross_bench.py`), keeping the bots and loss knobs the tests use.

Out of scope: the P2P design (`src/p2p.ts`, `p2p-entry.ts`, `wrangler.p2p.jsonc`,
`port/render/session`, `peer.rs`/`p2p.rs` beyond bench counters), the browser's online code beyond
what 1-4 make dead, and any wire format 0.7.13 clients or test-p2p-1 testers use.

Done: `python tools/check.py port` and `python tools/check.py server` OK; boot and race state
traces equal 0.7.13's (`docs/TESTING.md`); a local normal-room race between this build and the
0.7.13 release build passes; the native P2P room tests still pass.

## Progress

- [x] 2026-11-09: finding reserved

## Decisions

## Surprises

## Result
