use super::*;

const MARKER: u64 = 1_734_000_003_000;

/// A star for member `me` of a room whose hub is `hub`, with `links` (id, "star" | "sfu" | "") and
/// a race of `slots` (member ids by slot) unless empty.
fn star(me: u64, hub: u64, links: &[(u64, &str)], slots: &[u64]) -> Star {
    let mut s = Star::new(true);
    s.room(&json!({"type": "welcome", "memberId": me}));
    s.room(&routes(hub, links, slots, &[]));
    s
}

fn routes(hub: u64, links: &[(u64, &str)], slots: &[u64], left: &[u8]) -> Value {
    let members: Vec<Value> = links.iter().map(|(id, l)| if l.is_empty() { json!({"id": id}) } else { json!({"id": id, "link": l}) }).collect();
    let race = if slots.is_empty() { Value::Null } else { json!({"startAt": MARKER, "slots": slots, "left": left}) };
    json!({"type": "p2p", "hub": hub, "members": members, "race": race})
}

/// A state bundle as the game sends it (slot 0 until stamped).
fn state(slot: u8) -> Vec<u8> {
    let mut f = vec![0u8; 24];
    f[0] = STATE;
    f[1] = slot;
    f[2] = 6;
    f
}

fn event(slot: u8) -> Vec<u8> {
    vec![EVENT, slot, 2, 7, 7]
}

fn envelope(frame: &[u8], marker: u64) -> Vec<u8> {
    let mut b = vec![KIND_FRAME];
    b.extend(marker.to_le_bytes());
    b.extend(frame);
    b
}

/// The game frames and SFU frames among `actions`, and the peers each frame went to.
fn games(actions: &[Action]) -> Vec<Vec<u8>> {
    actions.iter().filter_map(|a| if let Action::Game(f) = a { Some(f.clone()) } else { None }).collect()
}
fn sfus(actions: &[Action]) -> Vec<Vec<u8>> {
    actions.iter().filter_map(|a| if let Action::Sfu(f) = a { Some(f.clone()) } else { None }).collect()
}
fn peers(actions: &[Action]) -> Vec<(u64, bool, Vec<u8>)> {
    actions.iter().filter_map(|a| if let Action::Peer(id, r, b) = a { Some((*id, *r, b.clone())) } else { None }).collect()
}

/// Hub 1 races in slot 0, member 2 on the star in slot 1, member 3 on the SFU in slot 2, member 4
/// on the star but not racing.
const LINKS: &[(u64, &str)] = &[(1, ""), (2, "star"), (3, "sfu"), (4, "star")];
const SLOTS_3: &[u64] = &[1, 2, 3];

fn linked(mut s: Star, ids: &[u64]) -> Star {
    s.take();
    for id in ids {
        s.link(*id, true, 0);
    }
    s.take();
    s
}

#[test]
fn the_hub_links_to_every_member_not_on_the_sfu_and_a_member_to_the_hub() {
    let mut hub = star(1, 1, LINKS, &[]);
    let connects: Vec<_> = hub.take().into_iter().filter(|a| matches!(a, Action::Connect { .. })).collect();
    assert_eq!(connects, vec![Action::Connect { member: 2, offerer: true }, Action::Connect { member: 4, offerer: true }]
        .into_iter().filter(|a| connects.contains(a)).collect::<Vec<_>>());
    assert_eq!(connects.len(), 2);
    let mut member = star(2, 1, LINKS, &[]);
    assert_eq!(member.take(), vec![Action::Connect { member: 1, offerer: false }]);
    // A member on the SFU holds no peer; one the room moves there drops its own.
    let mut f = star(3, 1, LINKS, &[]);
    assert!(f.take().is_empty());
    member.room(&routes(1, &[(1, ""), (2, "sfu")], &[], &[]));
    assert_eq!(member.take(), vec![Action::Close(1)]);
}

#[test]
fn up_and_down_links_are_reported_to_the_room() {
    let mut member = star(2, 1, LINKS, &[]);
    member.take();
    member.link(1, true, 10);
    assert_eq!(member.take(), vec![Action::Room(json!({"type": "p2p_link", "hub": 1, "member": 2, "up": true}))]);
    let mut hub = star(1, 1, LINKS, &[]);
    hub.take();
    hub.link(4, false, 10);
    assert_eq!(hub.take(), vec![Action::Room(json!({"type": "p2p_link", "hub": 1, "member": 4, "up": false}))]);
    assert!(!hub.up(4));
}

/// The mixed room's paths: star to star through the hub, star to the SFU through the hub's
/// publication, the SFU's racers into the star through the hub.
#[test]
fn a_mixed_room_delivers_every_racer_to_every_racer_once() {
    let mut hub = linked(star(1, 1, LINKS, SLOTS_3), &[2, 4]);
    let mut s = linked(star(2, 1, LINKS, SLOTS_3), &[1]);
    let mut f = star(3, 1, LINKS, SLOTS_3);
    f.take();

    // The star member's state: to the hub, which hands it to its game, publishes it for the SFU's
    // racer and forwards it to no one else (member 4 doesn't race).
    s.game_frame(&state(0));
    let to_hub = peers(&s.take());
    assert_eq!(to_hub.len(), 1);
    let (id, reliable, bytes) = &to_hub[0];
    assert_eq!((*id, *reliable), (1, false));
    assert_eq!(bytes[..9], envelope(&[], MARKER)[..]);
    hub.peer_message(2, false, bytes, 20);
    let out = hub.take();
    assert_eq!(games(&out).iter().map(|f| f[1]).collect::<Vec<_>>(), vec![1]);
    assert_eq!(sfus(&out).iter().map(|f| f[1]).collect::<Vec<_>>(), vec![1]);
    assert!(peers(&out).is_empty());
    // The fallback racer receives it from the hub's publication with the star racer's slot.
    f.sfu_frame(1, sfus(&out)[0].clone());
    assert_eq!(games(&f.take()).iter().map(|f| f[1]).collect::<Vec<_>>(), vec![1]);

    // The fallback racer's event, from its own publication: the hub's game and the star.
    f.game_frame(&event(0));
    let mine = sfus(&f.take());
    assert_eq!(mine.len(), 1);
    assert_eq!(mine[0][1], 2);
    hub.sfu_frame(3, mine[0].clone());
    let out = hub.take();
    assert_eq!(games(&out)[0][1], 2);
    let fwd = peers(&out);
    assert_eq!(fwd.iter().map(|(id, r, _)| (*id, *r)).collect::<Vec<_>>(), vec![(2, true)]);
    s.peer_message(1, true, &fwd[0].2, 30);
    assert_eq!(games(&s.take())[0][..2], [EVENT, 2]);

    // The hub's own frame: the star racer and the SFU.
    hub.game_frame(&state(0));
    let out = hub.take();
    assert_eq!(peers(&out).iter().map(|(id, _, b)| (*id, b[10])).collect::<Vec<_>>(), vec![(2, 0)]);
    assert_eq!(sfus(&out)[0][1], 0);
    f.sfu_frame(1, sfus(&out)[0].clone());
    assert_eq!(games(&f.take())[0][1], 0);
}

#[test]
fn no_game_speaks_for_another_rider() {
    let mut hub = linked(star(1, 1, LINKS, SLOTS_3), &[2, 4]);
    // A star member's frame claiming slot 2: stamped with its own slot by the hub.
    hub.peer_message(2, false, &envelope(&state(2), MARKER), 10);
    assert_eq!(games(&hub.take())[0][1], 1);
    // A member that doesn't race sends nothing that counts.
    hub.peer_message(4, false, &envelope(&state(0), MARKER), 10);
    assert!(hub.take().is_empty());
    // On the SFU a non-hub's frame must carry its own slot.
    let mut f = star(3, 1, &[(1, ""), (2, "sfu"), (3, "sfu")], SLOTS_3);
    f.take();
    f.sfu_frame(2, state(0));
    assert!(f.take().is_empty());
    f.sfu_frame(2, state(1));
    assert_eq!(games(&f.take()).len(), 1);
    // Its own slot back from the hub's publication is not a rider of someone else's.
    f.sfu_frame(1, state(2));
    assert!(f.take().is_empty());
    // A star member takes only the hub's frames, of racers still in, never its own slot.
    let mut s = linked(star(2, 1, LINKS, SLOTS_3), &[1]);
    s.peer_message(1, false, &envelope(&state(1), MARKER), 10);
    assert!(s.take().is_empty());
    s.room(&routes(1, LINKS, SLOTS_3, &[2]));
    s.peer_message(1, false, &envelope(&state(2), MARKER), 10);
    assert!(s.take().is_empty());
}

#[test]
fn another_race_and_the_wrong_channel_are_dropped() {
    let mut hub = linked(star(1, 1, LINKS, SLOTS_3), &[2]);
    hub.peer_message(2, false, &envelope(&state(0), MARKER - 1), 10);
    hub.peer_message(2, true, &envelope(&state(0), MARKER), 10);   // a state on the reliable channel
    hub.peer_message(2, false, &envelope(&event(0), MARKER), 10);  // an event on the unreliable one
    assert!(hub.take().is_empty());
}

#[test]
fn a_new_hub_closes_every_link_and_its_predecessor_speaks_only_for_itself() {
    let mut f = star(3, 1, &[(1, ""), (2, "star"), (3, "sfu")], SLOTS_3);
    f.take();
    f.sfu_frame(1, state(1));
    assert_eq!(games(&f.take()).len(), 1);
    // The room moves the hub to member 2; member 1 is a member now, its links being made.
    f.room(&routes(2, &[(1, ""), (2, ""), (3, "")], SLOTS_3, &[]));
    assert_eq!(f.take(), vec![Action::Connect { member: 2, offerer: false }]);
    f.sfu_frame(1, state(1));  // the old hub's bridged frame
    assert!(f.take().is_empty());
    let mut s = linked(star(2, 1, LINKS, SLOTS_3), &[1]);
    s.room(&routes(2, &[(1, ""), (2, ""), (3, ""), (4, "")], SLOTS_3, &[]));
    let actions = s.take();
    assert!(actions.contains(&Action::Close(1)));
    for id in [1, 3, 4] {
        assert!(actions.contains(&Action::Connect { member: id, offerer: true }), "{actions:?}");
    }
}

#[test]
fn each_game_expects_the_sfu_channels_of_its_part_of_the_race() {
    assert_eq!(star(1, 1, LINKS, SLOTS_3).sfu_expect(), vec![3]);
    assert_eq!(star(3, 1, LINKS, SLOTS_3).sfu_expect(), vec![1]);
    assert!(star(2, 1, LINKS, SLOTS_3).sfu_expect().is_empty());
    let two = &[(1, ""), (2, "sfu"), (3, "sfu")];
    assert_eq!(star(2, 1, two, SLOTS_3).sfu_expect(), vec![1, 3]);
    assert!(star(1, 1, LINKS, SLOTS_3).on_sfu() && star(3, 1, LINKS, SLOTS_3).on_sfu() && !star(2, 1, LINKS, SLOTS_3).on_sfu());
    assert!(!star(1, 1, &[(1, ""), (2, "star")], &[1, 2]).on_sfu());
}

#[test]
fn a_hub_that_sits_the_race_out_still_forwards_it() {
    let links = &[(1, ""), (2, "star"), (3, "star")];
    let mut hub = linked(star(1, 1, links, &[2, 3]), &[2, 3]);
    assert_eq!(hub.race_start().unwrap()["players"], json!([{"slot": 0, "memberId": 2}, {"slot": 1, "memberId": 3}]));
    hub.peer_message(2, false, &envelope(&state(0), MARKER), 10);
    let out = hub.take();
    assert!(games(&out).is_empty());
    assert_eq!(peers(&out).iter().map(|(id, _, b)| (*id, b[10])).collect::<Vec<_>>(), vec![(3, 0)]);
    assert!(star(2, 1, links, &[2, 3]).race_start().is_none());
}

#[test]
fn a_silent_link_is_down_and_a_quiet_one_gets_keepalives() {
    let mut s = linked(star(2, 1, LINKS, &[]), &[1]);
    s.tick(ALIVE_MS);
    assert_eq!(s.take(), vec![Action::Peer(1, true, vec![KIND_ALIVE])]);
    s.peer_message(1, true, &[KIND_ALIVE], ALIVE_MS + 10);
    s.tick(ALIVE_MS + 10 + LINK_SILENT_MS + 1);
    assert_eq!(s.take(), vec![Action::Close(1), Action::Room(json!({"type": "p2p_link", "hub": 1, "member": 2, "up": false}))]);
    assert!(!s.any_up());
}

#[test]
fn without_direct_links_a_game_goes_to_the_sfu_at_once() {
    let mut member = Star::new(false);
    member.room(&json!({"type": "welcome", "memberId": 2}));
    member.room(&routes(1, &[(1, ""), (2, "")], &[], &[]));
    assert_eq!(member.take(), vec![Action::Room(json!({"type": "p2p_link", "hub": 1, "member": 2, "up": false}))]);
    member.room(&routes(1, &[(1, ""), (2, "")], &[], &[]));
    assert!(member.take().is_empty());  // once
    // The room puts it on the SFU, then lets it try again after the race: reported again.
    member.room(&routes(1, &[(1, ""), (2, "sfu")], &[], &[]));
    member.room(&routes(1, &[(1, ""), (2, "")], &[], &[]));
    assert_eq!(member.take(), vec![Action::Room(json!({"type": "p2p_link", "hub": 1, "member": 2, "up": false}))]);
    let mut hub = Star::new(false);
    hub.room(&json!({"type": "welcome", "memberId": 1}));
    hub.room(&routes(1, &[(1, ""), (2, ""), (3, "")], &[], &[]));
    assert_eq!(hub.take().len(), 2);  // each member's link
}

/// A star racer whose link brings keepalives but no race frame from the hub START_LEAD_MS +
/// SILENT_AFTER_MS after the race appeared treats the link as failed (once): the room puts it on the
/// SFU (finding 187's rule for the star). A frame from the hub, or no other racer on the star, says
/// nothing.
#[test]
fn a_star_racer_whose_hub_forwards_no_race_frame_falls_back() {
    let quiet = START_LEAD_MS + SILENT_AFTER_MS;
    let run = |frame_at: Option<u64>, links: &[(u64, &str)], slots: &[u64]| -> Vec<Action> {
        let mut s = linked(star(2, 1, links, &[]), &[1]);
        s.room(&routes(1, links, slots, &[]));
        s.take();
        let mut out = vec![];
        for t in (0..=quiet + 2000).step_by(500) {
            s.peer_message(1, true, &[KIND_ALIVE], t);  // the link is alive throughout
            if frame_at == Some(t) {
                s.peer_message(1, false, &envelope(&state(0), MARKER), t);
            }
            s.tick(t);
            out.extend(s.take().into_iter().filter(|a| !matches!(a, Action::Peer(..) | Action::Game(..))));
        }
        out
    };
    let links = &[(1, ""), (2, "star")];
    let out = run(None, links, &[1, 2]);
    assert!(matches!(&out[0], Action::Log(l) if l.contains("no race frame from the hub")), "{out:?}");
    assert_eq!(out[1..], [Action::Close(1), Action::Room(json!({"type": "p2p_link", "hub": 1, "member": 2, "up": false}))]);
    assert!(run(Some(1000), links, &[1, 2]).is_empty());
    // The hub sits the race out and the only other racer is on the SFU: nothing the hub would forward.
    assert!(run(None, &[(1, ""), (2, "star"), (3, "sfu")], &[2, 3]).is_empty());
}

/// A link to a new hub mid-race gets SILENT_AFTER_MS of its own before its silence counts, however
/// long the race has run (a game 30 s into its race that links to the new hub isn't sent to the SFU
/// at once).
#[test]
fn a_link_to_a_new_hub_mid_race_gets_its_own_time() {
    let links = &[(1, ""), (2, "star"), (3, "")];
    let mut s = linked(star(2, 1, links, &[1, 2, 3]), &[1]);
    s.tick(0);
    s.take();
    // The hub moves to member 3 at 30 s; this game links to it at 31 s, and hears nothing yet.
    s.room(&routes(3, &[(1, ""), (2, ""), (3, "")], &[1, 2, 3], &[0]));
    s.take();
    s.link(3, true, 31_000);
    s.take();
    let mut out = vec![];
    for t in (31_000..31_000 + SILENT_AFTER_MS).step_by(500) {
        s.peer_message(3, true, &[KIND_ALIVE], t);
        s.tick(t);
        out.extend(s.take().into_iter().filter(|a| !matches!(a, Action::Peer(..))));
    }
    assert!(out.is_empty(), "{out:?}");
    s.peer_message(3, true, &[KIND_ALIVE], 31_000 + SILENT_AFTER_MS + 500);
    s.tick(31_000 + SILENT_AFTER_MS + 500);
    assert!(s.take().contains(&Action::Close(3)));
}
