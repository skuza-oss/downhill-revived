# sctp-proto 0.10.8: unordered streams deliver out of order whatever the peer marks

Upstream: https://github.com/algesten/sctp-proto (crate sctp-proto 0.10.8, git da1ac299d949),
MIT OR Apache-2.0; upstream source and licences retained. str0m 0.25.0 depends on it.

Only `src/association/stream.rs` and `src/queue/reassembly_queue.rs` differ:

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

## Reapplying on an update

1. Replace this directory with the new published crate (Cargo.toml, licences, README, src,
   tests); retain both licences.
2. If upstream has no receive-side switch for this by then, reapply the three changes above
   (`git log -p -- port/render/vendor/sctp-proto` shows them).
3. Update the lockfile through rustgs.py, run `rustgs.py test --target <host> -- -p dhd_net`
   (the test `relay_sent_ordered_state_is_delivered_unordered` fails without the patch), then
   `check.py port`.
