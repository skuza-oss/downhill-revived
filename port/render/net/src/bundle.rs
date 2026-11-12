//! A STATE message's layout as the game writes it (port/hle/state_bundle.h), read from that header
//! when this crate is built, so the two cannot drift apart (finding 220: after the bundle became
//! version 8, the SFU's flood limiter still looked for version 6 and never saw a stamp).

const STATE_BUNDLE_H: &str = include_str!("../../../hle/state_bundle.h");

/// A STATE bundle's payload version (`STATE_BUNDLE_VERSION`) and HOLD's (`STATE_HOLD_VERSION`).
pub(crate) const BUNDLE_VERSION: u8 = c_enum(STATE_BUNDLE_H, "STATE_BUNDLE_VERSION");
#[cfg_attr(not(test), allow(dead_code))]  // read by the tests
pub(crate) const HOLD_VERSION: u8 = c_enum(STATE_BUNDLE_H, "STATE_HOLD_VERSION");
/// The bundle's header (`STATE_BUNDLE_HEADER`): version, index, riders, then the f64 stamp at
/// STAMP_AT ("[3..10] f64" in the header's layout).
pub(crate) const BUNDLE_HEADER: usize = c_enum(STATE_BUNDLE_H, "STATE_BUNDLE_HEADER") as usize;
pub(crate) const STAMP_AT: usize = 3;
const _: () = assert!(STAMP_AT + 8 == BUNDLE_HEADER, "the stamp ends the bundle's header");

/// The value of a C enumerator `NAME = N` (a decimal number below 256) in `text`; a build error
/// when it is not there.
const fn c_enum(text: &str, name: &str) -> u8 {
    let (t, n) = (text.as_bytes(), name.as_bytes());
    let mut i = 0;
    while i + n.len() + 3 < t.len() {
        let mut j = 0;
        while j < n.len() && t[i + j] == n[j] { j += 1; }
        let before = if i == 0 { b' ' } else { t[i - 1] };
        let word = !(before.is_ascii_alphanumeric() || before == b'_');
        if j == n.len() && word && t[i + j] == b' ' && t[i + j + 1] == b'=' && t[i + j + 2] == b' ' {
            let (mut k, mut v) = (i + j + 3, 0u32);
            assert!(t[k].is_ascii_digit(), "state_bundle.h: not a number");
            while k < t.len() && t[k].is_ascii_digit() { v = v * 10 + (t[k] - b'0') as u32; k += 1; }
            assert!(v < 256, "state_bundle.h: not a byte");
            return v as u8;
        }
        i += 1;
    }
    panic!("state_bundle.h: an enumerator is missing")
}

/// A STATE payload's stamp (the server-clock moment its snapshots show, ms): a bundle's, as the
/// game writes it; None for anything else (a HOLD, a short or foreign message).
pub(crate) fn stamp(payload: &[u8]) -> Option<f64> {
    (payload.len() >= BUNDLE_HEADER && payload[0] == BUNDLE_VERSION)
        .then(|| f64::from_le_bytes(payload[STAMP_AT..STAMP_AT + 8].try_into().unwrap()))
}

#[cfg(test)]
pub(crate) mod fixture {
    //! Real bundles as the game makes them (port/hle/state_bundle_fixture.h, generated and checked by
    //! port/hle/test_state_bundle.c from the game's own code).
    const TEXT: &str = include_str!("../../../hle/state_bundle_fixture.h");

    fn array(name: &str) -> Vec<u8> {
        let start = TEXT.find(&format!("{name}[] = {{")).expect("the fixture's array") + name.len() + 6;
        let end = start + TEXT[start..].find('}').unwrap();
        TEXT[start..end].split(',').map(str::trim).filter(|x| !x.is_empty())
            .map(|x| u8::from_str_radix(x.trim_start_matches("0x"), 16).unwrap()).collect()
    }
    fn define(name: &str) -> f64 {
        let at = TEXT.find(&format!("#define {name} ")).unwrap() + name.len() + 9;
        TEXT[at..].lines().next().unwrap().trim().parse().unwrap()
    }
    /// The host's own rider's bundle and a computer rider's, each with its stamp; a HOLD.
    pub(crate) fn bundles() -> [(Vec<u8>, f64); 2] {
        [(array("STATE_BUNDLE_FIXTURE_0"), define("STATE_BUNDLE_FIXTURE_STAMP0")),
         (array("STATE_BUNDLE_FIXTURE_1"), define("STATE_BUNDLE_FIXTURE_STAMP1"))]
    }
    pub(crate) fn hold() -> Vec<u8> { array("STATE_HOLD_FIXTURE") }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_layout_is_the_games() {
        assert_eq!((BUNDLE_VERSION, HOLD_VERSION, BUNDLE_HEADER), (8, 7, 11));
        for (bytes, at) in fixture::bundles() {
            assert_eq!(bytes[0], BUNDLE_VERSION);
            assert_eq!(stamp(&bytes), Some(at), "a real bundle's stamp");
        }
        let hold = fixture::hold();
        assert_eq!((hold[0], hold.len()), (HOLD_VERSION, 13));
        assert_eq!(stamp(&hold), None, "a HOLD is no bundle");
        assert_eq!(stamp(&[BUNDLE_VERSION, 0, 1]), None, "too short");
    }
}
