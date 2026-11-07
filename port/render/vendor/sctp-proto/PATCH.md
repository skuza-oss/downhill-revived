# sctp-proto 0.10.8: unordered streams deliver out of order whatever the peer marks; peer stream resets taken in order

Upstream: https://github.com/algesten/sctp-proto (crate sctp-proto 0.10.8, git da1ac299d949),
MIT OR Apache-2.0; upstream source and licences retained. str0m 0.25.0 depends on it.

Two changes. The first is in `src/association/stream.rs` and `src/queue/reassembly_queue.rs`:

- `StreamState::handle_data`: a stream this side configured unordered (str0m's
  `ChannelConfig { ordered: false }`, applied through `set_reliability_params`) puts every
  received DATA chunk on the unordered reassembly path, whatever its U bit. DCEP messages stay
  ordered (RFC 8832 6). Each message is handed out as soon as it is complete.
- `Stream::set_reliability_params`: when a stream becomes unordered, messages queued before
  (data that arrived before the local channel was configured) move to the unordered path too.
- `ReassemblyQueue::release_ordered_as_unordered`: that move, with the byte count kept.

Why (finding 173): Cloudflare Realtime SFU forwards a subscribed DataChannel to the subscriber
ordered and reliable (U bit 0, retransmitted, no FORWARD-TSN) even when the publication and the
subscription both ask `ordered: false, maxRetransmits: 0` (also with `maxPacketLifeTime`, or
`ordered: false` alone). One lost datagram then held every later STATE snapshot until Cloudflare
retransmitted it, often only at its 1 s retransmission timer (backing off to 2-4 s): the stalls of
0.5-6 s players saw. With this patch the receiver needs nothing from the sender's ordering; SACKs,
the cumulative TSN, retransmission and the receive window are unchanged, so the association
behaves the same on the wire. Ordered streams (the reliable `events` channel) are untouched.

The second is in `src/association/mod.rs` (with its tests in `association_test.rs`):

- `Association::reconfig_ahead`, `process_reconfig_ahead`: a peer's Outgoing SSN Reset Request
  that arrives ahead of its turn (its sequence number past the next expected one, or the next one
  while an earlier request is deferred "In progress") is held, unanswered, and processed in
  sequence order once its predecessors are done (at most 64 ahead). Upstream answers "Error - Bad
  Sequence Number" or "Error - Request already in progress".

Why (finding 177): the game keeps its SFU association for a whole room stay, so the room closes
subscriptions (a member who leaves) and Cloudflare gives their stream ids to the next
subscriptions on that session at once. Cloudflare resets the closed streams with several RE-CONFIG
requests in flight at once (against RFC 6525 5.1.1, "at any given time, there MUST NOT be more than
one request in flight"), and they arrive out of order. Answered with an error, Cloudflare never
retried them: those streams were never reset on its side, the reciprocal resets never completed,
str0m kept their ids held, and the new channels on them never opened (`probe_parallel_subscriptions`
in `port/render/net/src/probe.rs`: from the second round on; with the patch 400 of 400 calls and
every channel worked over 20 rounds).

One more, in the tests only: `association_test.rs` uses addresses of the documentation range
192.0.2.0/24 (RFC 5737) where upstream's tests use private 192.168.1.x addresses (the
repository's checks refuse LAN addresses in added lines). The tests only parse them.

## Reapplying on an update

1. Replace this directory with the new published crate (Cargo.toml, licences, README, src,
   tests); retain both licences.
2. If upstream has no receive-side switch, or still refuses early RE-CONFIG requests, by then,
   reapply the changes above (`git log -p -- port/render/vendor/sctp-proto` shows them).
3. Update the lockfile through rustgs.py, run `rustgs.py test --target <host> -- -p dhd_net -p
   dhd_render --lib sfu::` (the test `relay_sent_ordered_state_is_delivered_unordered` fails
   without the first change), the crate's own tests in a copy outside the workspace (`cargo test
   --lib`; `test_future_peer_reconfig_sequence_is_held_until_its_turn` and
   `test_next_peer_reconfig_is_held_while_one_is_deferred` fail without the second), and with
   Realtime credentials `probe_parallel_subscriptions` (ignored by default); then `check.py port`.
