# 173: STATE snapshots that stop for seconds on the SFU, then arrive at once

Status: in progress

Branch `fix/sfu-stalls` from `feat/online-latency` b8eb93c (finding 169), started 2026-11-06. This
file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

In online races another player's STATE snapshots sometimes stop for 0.5-6 s and then arrive all at
once (finding 169, Surprises; 0.7.12 has it too: in a PC↔Mac measurement 21 spike-seconds in 700,
18 of them host to guest). UDP datagrams from Cloudflare keep reaching our socket meanwhile, but
no STATE message comes out of str0m until the burst. STATE travels on a negotiated DataChannel the
client opens unordered with `MaxRetransmits{0}`, and the room asks Cloudflare for the same policy
(`datachannels/new`, `ordered: false, maxRetransmits: 0`) on both the publishing and the
subscribing session.

The lane finds the cause with evidence (the SCTP chunks on the wire: U bit, TSN gaps,
retransmissions, FORWARD-TSN, in both directions) and fixes it in the one transport path. Hard
rules: no player ever receives another's address, SDP, TURN credentials, address hash or install
id; no peer-to-peer or TURN mesh; no lockstep (finding 044); no second transport kept beside the
first.

Acceptance:
1. The cause, with the evidence, here.
2. The fix (or, if the cause is Cloudflare's and nothing we control helps, a measured mitigation
   with its trade-off, or the evidence and a stop).
3. Before/after numbers from local two-game races (`port/tools/online_race.py --lag`): gaps over
   100 ms per minute, snapshot age p99, spike-seconds.
4. `python tools/check.py port` and `python tools/check.py server` pass; boot and race state traces
   equal the base build's (feat/online-latency's tip).

Out of scope: TURN/TLS changes, the latency work of finding 169, phones and the browser.

## Progress

- [x] 2026-11-06: lane plan (this file).

## Decisions

## Surprises

## Result
