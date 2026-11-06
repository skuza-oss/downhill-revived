# 173: STATE snapshots that stop for seconds on the SFU, then arrive at once

Status: current
Checked: 2026-11-06

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

- [x] 2026-11-06: lane plan (this file), cc9f9e9.
- [x] The cause, from the wire (Cause below): Cloudflare sends the subscribed STATE channel ordered
      and reliable, whatever policy is asked; one lost datagram holds every later snapshot in our
      SCTP receiver until Cloudflare's retransmission, often its 1 s timer.
- [x] The fix: sctp-proto vendored unchanged (f116354), then patched so a stream this side opened
      unordered delivers each complete message at once whatever the sender's U bit (72d4100,
      `port/render/vendor/sctp-proto/PATCH.md`); Rust test
      `relay_sent_ordered_state_is_delivered_unordered` (fails without the patch, passes with it).
- [x] Test switch `DHD_NET_SFU_DROP_IN=N` (e6ba527, TESTING.md): N percent of the SFU's records
      dropped on arrival, so a local race reproduces a lossy link.
- [x] Before/after measurements, publisher-leg check, state traces, `check.py port` and `server`
      (Result).
- [ ] PC↔Mac check of the fix: not run (For the lead).

## Cause

Evidence from a temporary trace (never committed): a copy of sctp-proto that wrote every SCTP
packet in and out, chunk by chunk (DATA with TSN, stream, SSN and U bit; SACK with its gap blocks;
FORWARD-TSN), every message handed to str0m, and in `sfu.rs` every UDP datagram with its DTLS
record sequence number. Two local games (`online_race.py --lag`, host with 8 AI riders, real SFU,
WARP off), runs t1 and t2 of 120 s each, base build b8eb93c plus the trace:

- **Every DATA chunk Cloudflare sends us has U = 0**, on the STATE stream too (t2 guest: 10,839
  ordered, 0 unordered), with SSNs counting up. Ours to Cloudflare carry U = 1 (host: 10,863
  unordered STATE chunks). Cloudflare also **retransmits** lost STATE chunks and never sends a
  FORWARD-TSN (0 in every run): the subscriber leg is reliable and ordered.
- The guest lost 311 of 12,671 DTLS records from Cloudflare (2.5%, single isolated records, spread
  evenly over the race) before they reached our socket; every record that did arrive reached
  SCTP (12,360 = 12,360), so neither our reader nor str0m drops anything. The host lost none in
  that run; loss on this path comes and goes (another run lost none either way).
- Of 348 STATE chunks that arrived after a later one: 116 within 20 ms (reordering on the way),
  197 at 20-100 ms (Cloudflare's fast retransmission), 10 at 100-500 ms and **25 at about 1,008 ms
  (its retransmission timer)**. Example: TSN …545 (SSN 246) missing at 44.04 s; we SACKed the gap
  about 70 times; Cloudflare resent it with another lost one at 45.05 s, and 93 messages came out
  within 5 ms. One run had a lost retransmission: 2 s of nothing (timer backoff), then a burst.
- Because the chunks are ordered, sctp-proto must hold every later message until the missing SSN
  arrives: the time a STATE message waited inside SCTP between arrival and delivery had p99
  909 ms, max 1,024 ms, 954 messages over 100 ms (t2 guest; the host, with no loss, p99 0.08 ms).

What we ask is what Cloudflare documents (`datachannels/new` with `ordered: false,
maxRetransmits: 0` on the publication and on each subscription; [DataChannels](https://developers.cloudflare.com/realtime/sfu/datachannels/),
[changelog 2026-08-13](https://developers.cloudflare.com/changelog/post/2026-08-13-datachannels-reliability-ordering/)).
Its answer does not echo the policy. Variants tried on the local server (temporary edits, 45 s
races, the trace on): the subscription with `ordered: false` and no retry field (V3), and both
the publication and the subscription with `ordered: false, maxPacketLifeTime: 50` (V4). Both
still arrived U = 0, retransmitted, no FORWARD-TSN; V4 stalled both ways (p99 hold-back 848 /
947 ms). No report of this was found (str0m and Cloudflare community searched); related: pion/sctp
#106 (a receiver held by unordered reassembly) and RFC 3758's note that a late FORWARD-TSN delays
delivery.

Ruled out with the same trace:
- **Our publisher leg**: str0m sends STATE with U = 1 and PR-SCTP works: with 3% of the host's
  outgoing records dropped (temporary switch), it sent 325 FORWARD-TSNs and no retransmission,
  Cloudflare re-numbers the subscriber leg (the guest saw no TSN hole) and forwards without
  waiting for them (guest: 0 stalled steps; inter-arrival gaps 100-200 ms 2, as without loss 1).
- **str0m's reassembly, receive window, our poll loop**: the window never fell below ~1 MB (we
  advertise 1 MiB); poll gaps at most 49 ms, input waits at most 12.2 ms in every run; messages came out within 0.3 ms of
  their chunk once in order.

## Decisions

- Fix it in our receiver, not by asking Cloudflare differently: no request variant changes what
  Cloudflare sends, and the receiver is ours. A stream we open unordered now delivers each complete
  message at once, whatever the U bit the sender sets (sctp-proto `StreamState::handle_data`;
  messages queued before the channel was configured move over too). The association behaves the
  same on the wire (SACKs, cumulative TSN, window, Cloudflare's retransmissions), only the
  hand-out order changes; DCEP and the ordered `events` channel are untouched. A late message is
  harmless: the game rejects a snapshot older than the newest it holds for that rider before it
  touches anything (`port/hle/puppet.c`, `receive_record`). One transport path, no fallback.
- sctp-proto is vendored (`port/render/vendor/sctp-proto`, MIT OR Apache-2.0, licences kept) as an
  unchanged commit first, then the patch, so the diff is reviewable; `PATCH.md` says what and why
  and how to reapply. str0m 0.25.0 has no switch for this.
- Not chosen: a channel per snapshot or several STATE channels (each still ordered on Cloudflare's
  leg, more allocations per member and race, and only a share of the stalls avoided); a second
  subscriber session as a duplicate path (twice the traffic, and the same Cloudflare sender
  behaviour on both).
- The loss switch drops DTLS application records only (byte 0 = 23), so ICE consent and the
  handshake are untouched, deterministically (record n is dropped when 37n mod 100 < N), like the
  existing `DHD_NET_DROP_STATE`.

## Surprises

- Cloudflare's documented per-channel delivery policy is not applied to what it sends a subscriber
  (Cause). Its own retransmissions run at its 1 s minimum timer often enough to matter: 25 of the
  guest's 311 lost records in t2 waited for it.
- Two stalls remain that are Cloudflare's sender, not our receiver, and the fix cannot touch: (1) a
  lost chunk whose timer retransmission is lost too: Cloudflare sends no new data until the next
  one (2 s in t1; its congestion window collapses after the timeout); (2) once, at a race start,
  Cloudflare acknowledged the guest's messages as usual but forwarded nothing to the host for
  1.0 s, then sent them at once, with no loss and nothing outstanding (f1). In traced runs: one of
  each in 8 traced minutes per direction. They are what is left in the "after" numbers (N-fix-1's
  0.7 s event in both directions at once, which no SCTP hold-back explains).
- A base run (N-base-2) had a wrong shared clock from its start (snapshot ages 0 ms, the first
  window "on arrival 2100 ms"): the clock skew finding 169's PC↔Mac run C also saw. Its
  snapshot-age and lag numbers are left out of the table; its gaps are kept.
- Worktree setup: `npm ci` in `online/server` replaced the `node_modules` junction that
  `tools/worktree_setup.py` makes with a real folder on C:, and after `--move` put it back as
  `online_server_node_modules\node_modules`; and wrangler fails through a junction whose target
  folder is not named `node_modules` (node resolves the real path: "Cannot find module
  'esbuild'"). Pointing the junction at `…\online_server\node_modules` worked. Not fixed here
  (For the lead).

## Result

Three commits on `fix/sfu-stalls` after the plan: f116354 (sctp-proto 0.10.8 vendored unchanged),
72d4100 (the patch, its test, the docs), e6ba527 (the loss switch).

### Numbers

Local two-game races, 120 s each (GO to about 70 s of racing and the finish wait), host with 8 AI
riders, through the real SFU from this PC, WARP off, the PC shared with other lanes' games.
Before = b8eb93c (t1, t2: the same with the trace; L3: e6ba527 with sctp-proto unpatched, so the
loss switch exists); after = e6ba527 (f1: with the trace). Each cell: host → guest (the guest
drawing the host) ; guest → host. From dhd.log: gaps over 100 ms per minute and the snapshot-age
p99 (median of the 30 s windows / worst window) of the other human's slot; spike-seconds = seconds
whose "shown behind" exceeds its median + 100 ms (the PC↔Mac measurement's rule); stalled steps
from `puppet_lag.py`.

| Run | Gaps > 100 ms per minute | Snapshot age p99, ms (median / worst window) | Spike-seconds (of ~118) | Stalled steps |
|---|---|---|---|---|
| Before, natural loss: t1 | 5.0 ; 0 | 949 / 1001 ; 47 / 47 | 9 ; 0 | 493 ; 0 |
| Before, natural loss: t2 | 8.5 ; 0 | 984 / 1001 ; 46 / 47 | 10 ; 0 | 426 ; 0 |
| Before, natural loss: N-base-1 | 4.0 ; 3.5 | 489 / 931 ; 177 / 456 | 2 ; 0 | 112 ; 0 |
| Before, natural loss: N-base-2 (clock wrong) | 6.5 ; 5.0 | - | - | - |
| After, natural loss: f1 | 0.5 ; 0.5 | 44 / 96 ; 48 / 881 | 0 ; 0 | 0 ; 0 |
| After, natural loss: N-fix-1 | 2.0 ; 1.5 | 82 / 732 ; 85 / 697 | 1 ; 0 | 42 ; 40 |
| After, natural loss: N-fix-2 | 0 ; 0 | 24 / 78 ; 28 / 74 | 0 ; 0 | 0 ; 0 |
| Before, 3% of the SFU's records dropped on both: L3-nofix-1 | 7.5 ; 7.5 | 974 / 1001 ; 989 / 1001 | 17 ; 15 | 797 ; 757 |
| Before, 3% dropped: L3-nofix-2 | 11.5 ; 3.5 | 963 / 1001 ; 924 / 1001 | 15 ; 6 | 835 ; 374 |
| After, 3% dropped: L3-fix-1 | 0 ; 0 | 50 / 54 ; 59 / 63 | 0 ; 0 | 0 ; 0 |
| After, 3% dropped: L3-fix-2 | 2.0 ; 1.5 | 103 / 225 ; 72 / 131 | 0 ; 0 | 0 ; 0 |

In short: host → guest, natural loss, gaps over 100 ms 4.0-8.5 → 0-2.0 a minute, age p99 (median
window) 489-984 → 24-82 ms, spike-seconds 21 → 1 in about 350 s; with 3% loss on both legs
3.5-11.5 → 0-2.0 a minute, 924-989 → 50-103 ms, 53 → 0 spike-seconds and 374-835 → 0 stalled
steps. Inside SCTP a message now waits at most 0.3 ms (f1; 11.6 ms with outgoing loss, P3-fix),
against p99 909 ms before. What is left (N-fix-1, L3-fix-2's 131-225 ms windows) are Cloudflare's
own sending pauses (Surprises).

### Checks

- `rustgs.py test -- -p dhd_net -p dhd_render --lib`: 15 + 9 passed (4 ignored: the live probes),
  among them `relay_sent_ordered_state_is_delivered_unordered` (without the patch it fails:
  message 11 never arrives while message 10 is lost).
- State traces (`state_trace_run.py`, base b8eb93c against e6ba527, same recipe): boot all 600 rows
  match, race all 5,000 rows match. Only the network library changed.
- `python tools/check.py port -j 6`: OK on 9c583ab (all eight steps). The first run failed only in
  test_runtime ("no ps2_asm() calls in build/wasm/src"): a worktree whose core came from the module
  cache has no `build/wasm/src`; `DHD_MODULE_CACHE=0 build_wasm.py` (279 s, every object from the
  object cache) made it, and the rerun passed. `python tools/check.py server`: OK (npm test,
  typecheck).

### PC↔Mac, merged `main` (cfe842a, with finding 169)

Run by the lead's measurement worker after the merge, the same way as finding 169's comparison
(temporary test Worker, both directions, 5 complete two-player races each, prediction on, lag
traces on both games). Against finding 169's build alone (C there):

| | 169 alone | 169 + this fix |
|---|---|---|
| Snapshot age p50 / p95 / p99 (ms) | 30-31 / 104-132 / 478-840 | 22-30 / 37-47 / 40-61 |
| 30 s windows with p99 over 300 ms | 11 of 27 | 0 of 27 |
| Gaps over 100 ms per minute | 0-3.3 | 0-1.2 |
| Spike-seconds, PC / Mac | 9 / 7 | 0 / 0 |
| Drawn behind real time, p50 / p95 / p99 (ms) | 0 / 3-7 / 5-55 | 0 / 3-4 / 6 |
| Distance from the real rider, p50 / p95 / p99 (units) | 0.11-0.18 / 0.6-1.2 / 1.2-7.1 | 0.13-0.14 / 0.68-0.83 / 1.23-1.52 |

Finish times matched on both machines in every race. The shared clock was symmetric (age on
arrival 16-40 ms on the PC, 17-38 on the Mac); the inflated round trip that skewed it in 169's run
did not recur, so finding 169's clock fix (56c8ce1) is still untested against it. No room
WebSocket dropped at a race start (0 of 12), but those drops were intermittent and nothing here
touches the room connection.

### For the lead
- Worth telling Cloudflare: subscriptions ignore `ordered` / `maxRetransmits` /
  `maxPacketLifeTime` (U bit 0, retransmitted, no FORWARD-TSN), with this file's evidence. If they
  fix it, their sender stops retransmitting STATE and the remaining timer-backoff pauses go too;
  our patch stays correct either way.
- `tools/worktree_setup.py`'s `node_modules` junction (Surprises).
