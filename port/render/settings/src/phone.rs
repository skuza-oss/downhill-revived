//! Which Android graphics chips start with lighter settings (findings 239, 263): one short list,
//! each rule with its source. No phone models, no numeric GPU ranking, no iPhone guesses: an
//! unknown or ambiguous chip keeps the usual defaults.
use crate::Ini;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class { Unchanged, Minimum, Below }

/// One rule: the adapter names it takes (compacted, see `compact`; "" = no adapter name), the
/// SoC identifiers it needs (`ro.soc.model`, else `ro.board.platform`; empty = any), what it
/// sets for missing settings, and why.
#[derive(Debug, PartialEq, Eq)]
pub struct Rule {
    /// Written in the log line.
    pub name: &'static str,
    pub gpus: &'static [&'static str],
    pub socs: &'static [&'static str],
    pub class: Class,
    /// The resolution a missing `scale` gets; None keeps the default (2x).
    pub scale: Option<&'static str>,
    pub source: &'static str,
}

/// The Helio P65/G70/G80/G85/G88: MT6768 and MT6769 variants, all with a Mali-G52 MC2. Android 11
/// and older have no `ro.soc.model`; their board platform is mt6768 (the Redmi 9's, finding 263).
const MT6768: &[&str] = &["mt6768", "mt6769", "mt6769h", "mt6769t", "mt6769v", "mt6769vcu", "mt6769vcz",
    "heliog80", "heliog85", "heliog88"];
const SM6225: &[&str] = &["sm6225", "sm6225ad", "snapdragon680", "snapdragon685"];

const G52_SOURCE: &str = "finding 259: Redmi 9 (Helio G80) at 2x races 48.6 fields/s, 61.1 with Performance Mode";
const ADRENO_610_SOURCE: &str = "not measured; Notebookcheck's GPU comparison puts the Adreno 610 at about 85 % \
    of the Mali-G52 MP2, so 1x as well (finding 239)";
const BELOW_SOURCE: &str = "finding 239 (issues #95, #101) and PLAYING.md's below-minimum list; Notebookcheck's \
    GPU comparison puts these well under the Mali-G52 MP2";

pub const RULES: &[Rule] = &[
    Rule { name: "Mali-G52 MC2", gpus: &["malig52mc2"], socs: &[], class: Class::Minimum, scale: None,
        source: G52_SOURCE },
    // A bare "Mali-G52" (the Redmi 9's driver says no more) needs a known SoC: larger G52
    // configurations (MC6 and up) exist in other chips.
    Rule { name: "Mali-G52 in a Helio P65/G70/G80/G85/G88 (MT6768/MT6769)", gpus: &["malig52", ""], socs: MT6768,
        class: Class::Minimum, scale: None, source: G52_SOURCE },
    Rule { name: "Adreno 610", gpus: &["adreno610"], socs: &[], class: Class::Minimum, scale: Some("1"),
        source: ADRENO_610_SOURCE },
    Rule { name: "Snapdragon 680/685 (SM6225, Adreno 610)", gpus: &[""], socs: SM6225, class: Class::Minimum,
        scale: Some("1"), source: ADRENO_610_SOURCE },
    Rule { name: "PowerVR GE8320/GE8322", gpus: &["powervrroguege8320", "powervrge8320", "powervrroguege8322",
        "powervrge8322"], socs: &[], class: Class::Below, scale: Some("1"), source: BELOW_SOURCE },
    Rule { name: "Adreno 306/308/505/506", gpus: &["adreno306", "adreno308", "adreno505", "adreno506"], socs: &[],
        class: Class::Below, scale: Some("1"), source: BELOW_SOURCE },
];

fn compact(s: &str) -> String {
    s.chars().filter(|c| c.is_ascii_alphanumeric()).flat_map(char::to_lowercase).collect()
}

/// The rule an adapter name and SoC identifier match, if any (exact names only).
pub fn rule(gpu: &str, soc: &str) -> Option<&'static Rule> {
    let gpu = compact(gpu).replace("adrenotm", "adreno");
    let soc = compact(soc);
    RULES.iter().find(|r| r.gpus.contains(&gpu.as_str()) && (r.socs.is_empty() || r.socs.contains(&soc.as_str())))
}

pub fn class(gpu: &str, soc: &str) -> Class {
    rule(gpu, soc).map_or(Class::Unchanged, |r| r.class)
}

/// Fill the rule's settings that dhd.ini lacks. Bits: 1 resolution, 2 effects (Performance Mode)
/// changed. A written key is never changed, and any effects key protects the whole Performance
/// Mode group: a player's custom mix is not turned into another one.
pub fn defaults(ini: &mut Ini, rule: Option<&Rule>) -> Result<u32, String> {
    let Some(rule) = rule else { return Ok(0) };
    let mut changed = 0;
    if let Some(scale) = rule.scale {
        if !ini.has("scale") {
            ini.set("scale", scale)?;
            changed |= 1;
        }
    }
    let effects = ["shadows", "lens_flare", "particles"];
    if !effects.iter().any(|k| ini.has(k)) {
        for k in effects { ini.set(k, "0")?; }
        changed |= 2;
    }
    Ok(changed)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn known_classes_only() {
        for (gpu, soc) in [("Mali-G52 MC2", "MT6769H"), ("Mali-G52 MC2", ""), ("Mali-G52", "MT6769T"),
            ("Mali-G52", "mt6768"), ("", "mt6768"), ("Adreno (TM) 610", "SM6225"), ("", "SM6225-AD")] {
            assert_eq!(class(gpu, soc), Class::Minimum, "{gpu} {soc}");
        }
        for gpu in ["PowerVR Rogue GE8320", "PowerVR GE8322", "Adreno 506"] {
            assert_eq!(class(gpu, ""), Class::Below);
        }
        // the Mi 11i's logged name (0.8.7's release check), ambiguous G52s, desktop and iOS names
        for (gpu, soc) in [("Adreno (TM) 660", ""), ("Adreno 660", "SM8350"), ("Mali-G52", "unknown"),
            ("Mali-G52", ""), ("Mali-G52 MC4", ""), ("Mali-G52", "kirin810"), ("Apple A12 GPU", ""),
            ("Apple M1", ""), ("NVIDIA GeForce RTX 3070", ""), ("AMD Radeon(TM) Graphics", ""),
            ("unknown", ""), ("", ""), ("Adreno 6100", ""), ("Mali-G57", ""), ("Adreno (TM) 619", "")] {
            assert_eq!(class(gpu, soc), Class::Unchanged, "{gpu} {soc}");
        }
    }

    #[test]
    fn rules_are_data_with_sources() {
        for r in RULES {
            assert!(!r.name.is_empty() && !r.source.is_empty() && !r.gpus.is_empty());
            for g in r.gpus { assert_eq!(compact(g), *g, "{}", r.name); }
            for s in r.socs { assert_eq!(compact(s), *s, "{}", r.name); }
            // a rule for "no adapter name" must name its SoCs
            assert!(!r.gpus.contains(&"") || !r.socs.is_empty(), "{}", r.name);
            assert!(r.class != Class::Unchanged, "{}", r.name);
            if r.class == Class::Below { assert_eq!(r.scale, Some("1"), "{}", r.name); }
        }
        // the Redmi 9 (Android 11: no ro.soc.model, board platform mt6768) keeps 2x
        let redmi = rule("Mali-G52", "mt6768").unwrap();
        assert_eq!((redmi.class, redmi.scale), (Class::Minimum, None));
        assert_eq!(rule("Adreno (TM) 610", "").unwrap().scale, Some("1"));
    }

    #[test]
    fn choices_and_restarts() {
        let g52 = rule("Mali-G52", "mt6768");
        let adreno = rule("Adreno (TM) 610", "");
        // a new dhd.ini on the Redmi 9's chip: Performance Mode on, the resolution left at 2x
        let mut ini = Ini::default();
        assert_eq!(defaults(&mut ini, g52).unwrap(), 2);
        assert!(!ini.has("scale"));
        assert_eq!(["shadows", "lens_flare", "particles"].map(|k| ini.value(k)), ["0", "0", "0"]);
        assert_eq!(defaults(&mut ini, g52).unwrap(), 0);
        let mut ini = Ini::default();
        assert_eq!(defaults(&mut ini, adreno).unwrap(), 3);
        assert_eq!(ini.value("scale"), "1");
        assert_eq!(defaults(&mut ini, rule("PowerVR GE8320", "")).unwrap(), 0);
        ini.set("scale", "2").unwrap();
        ini.set("shadows", "1").unwrap();
        let before = ini.clone();
        assert_eq!(defaults(&mut ini, adreno).unwrap(), 0);
        assert_eq!(ini, before);
        for key in ["shadows", "DHD_LENS_FLARE", "particles"] {
            let mut ini = Ini::parse(format!("{key} = on\nDHD_SCALE = 3\n").as_bytes());
            let before = ini.clone();
            assert_eq!(defaults(&mut ini, adreno).unwrap(), 0);
            assert_eq!(ini, before);
        }
        let mut ini = Ini::parse(b"scale = 2\n");
        assert_eq!(defaults(&mut ini, adreno).unwrap(), 2);
        assert_eq!(ini.value("scale"), "2");
        let mut unknown = Ini::default();
        assert_eq!(defaults(&mut unknown, rule("Adreno (TM) 660", "")).unwrap(), 0);
        assert!(unknown.bytes().is_empty());
    }

    #[cfg(not(target_arch = "wasm32"))]
    #[test]
    fn persisted_defaults_and_player_changes() {
        let dir = std::env::temp_dir().join(format!("dhd-phone-defaults-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("dhd.ini");
        let mut changed = 0;
        assert!(!crate::update_phone_defaults(&path, None, &mut changed).unwrap());
        assert!(!path.exists());
        let g52 = rule("Mali-G52", "mt6768");
        assert!(crate::update_phone_defaults(&path, g52, &mut changed).unwrap());
        assert_eq!(changed, 2);
        // the player turns Performance Mode off: never turned on again
        for k in ["shadows", "lens_flare", "particles"] { crate::save(&path, k, "1").unwrap(); }
        let before = std::fs::read(&path).unwrap();
        assert!(!crate::update_phone_defaults(&path, g52, &mut changed).unwrap());
        assert_eq!(std::fs::read(&path).unwrap(), before);
        // an existing dhd.ini with the effects written is never changed, on any rule
        let old = dir.join("old.ini");
        std::fs::write(&old, b"; the player's\nscale = 3\nparticles = 1\n").unwrap();
        for r in RULES {
            assert!(!crate::update_phone_defaults(&old, Some(r), &mut changed).unwrap());
            assert_eq!(std::fs::read(&old).unwrap(), b"; the player's\nscale = 3\nparticles = 1\n");
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
}
