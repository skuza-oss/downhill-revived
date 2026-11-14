# 246: Phone SFU fallback and network return

Status: in progress

Branch `fix/phone-online-links` from local main 07e38743, started 2026-11-14.

## Goal and acceptance

Explain the production TURN 400 from its logs and code, prove relay-only TLS on
two hidden PC games where credentials permit, and fix evidenced loss/return
defects. Quick, port, docs, Android native; server gate only if server changes;
base-equal boot/race traces if core net code changes. No phone, Mac, push, post,
visible game window or timing benchmark.

## Progress

- [x] Reserve lane and read area rules.
- [ ] Inspect production evidence, SFU/TURN setup and recovery state machines.
- [ ] Focused fixes and regression/fault tests; relay-only race proof.
- [ ] Required gates; exact lead phone checks and deploy requirement.

## Decisions

Keep SFU signaling and credentials isolated per member; log no other member's
address, SDP, session ID, credentials, address hash or install ID.

## Surprises

Pending investigation.

## Result

Pending implementation and checks.
