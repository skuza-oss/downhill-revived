//! Conservative Android classes (239). No model names, numeric GPU ranking or iPhone guesses.
use crate::Ini;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class { Unchanged, Minimum, Below }

fn compact(s: &str) -> String {
    s.chars().filter(|c| c.is_ascii_alphanumeric()).flat_map(char::to_lowercase).collect()
}

pub fn class(gpu: &str, soc: &str) -> Class {
    let gpu = compact(gpu).replace("adrenotm", "adreno");
    let soc = compact(soc);
    // Exact families only. G52 without a core count needs a known G80/G85/G88 SoC:
    // larger G52 configurations also exist, so G52 alone is not a low-end verdict.
    if matches!(gpu.as_str(), "malig52mc2" | "adreno610")
        || (matches!(gpu.as_str(), "" | "malig52") && matches!(soc.as_str(),
            "mt6769" | "mt6769h" | "mt6769t" | "mt6769vcu" | "heliog80" | "heliog85" | "heliog88"))
        || (gpu.is_empty() && matches!(soc.as_str(), "sm6225" | "sm6225ad" | "snapdragon680" | "snapdragon685")) {
        Class::Minimum
    } else if matches!(gpu.as_str(), "powervrroguege8320" | "powervrge8320" | "powervrroguege8322" | "powervrge8322"
        | "adreno306" | "adreno308" | "adreno505" | "adreno506") {
        Class::Below
    } else { Class::Unchanged }
}

/// Bits: resolution and effects changed. An effects choice protects the whole Performance Mode
/// group: do not turn a player's custom combination into another combination.
pub fn defaults(ini: &mut Ini, class: Class) -> Result<u32, String> {
    if class == Class::Unchanged { return Ok(0); }
    let mut changed = 0;
    if !ini.has("scale") {
        ini.set("scale", "1")?;
        changed |= 1;
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
        for (gpu, soc) in [("Mali-G52 MC2", "MT6769H"), ("Mali-G52", "MT6769T"),
            ("Adreno (TM) 610", "SM6225"), ("", "SM6225-AD")] {
            assert_eq!(class(gpu, soc), Class::Minimum);
        }
        for gpu in ["PowerVR Rogue GE8320", "PowerVR GE8322", "Adreno 506"] {
            assert_eq!(class(gpu, ""), Class::Below);
        }
        for (gpu, soc) in [("Mali-G52", "unknown"), ("Mali-G52 MC4", ""), ("Adreno 660", "SM8350"),
            ("Apple A12 GPU", ""), ("unknown", ""), ("Adreno 6100", ""), ("Mali-G57", "")] {
            assert_eq!(class(gpu, soc), Class::Unchanged);
        }
    }
    #[test]
    fn choices_and_restarts() {
        let mut ini = Ini::default();
        assert_eq!(defaults(&mut ini, Class::Minimum).unwrap(), 3);
        assert_eq!(defaults(&mut ini, Class::Below).unwrap(), 0);
        ini.set("scale", "2").unwrap();
        ini.set("shadows", "1").unwrap();
        let before = ini.clone();
        assert_eq!(defaults(&mut ini, Class::Minimum).unwrap(), 0);
        assert_eq!(ini, before);
        for key in ["shadows", "DHD_LENS_FLARE", "particles"] {
            let mut ini = Ini::parse(format!("{key} = on\nDHD_SCALE = 3\n").as_bytes());
            let before = ini.clone();
            assert_eq!(defaults(&mut ini, Class::Minimum).unwrap(), 0);
            assert_eq!(ini, before);
        }
        let mut ini = Ini::parse(b"scale = 2\n");
        assert_eq!(defaults(&mut ini, Class::Minimum).unwrap(), 2);
        assert_eq!(ini.value("scale"), "2");
        let mut unknown = Ini::default();
        assert_eq!(defaults(&mut unknown, Class::Unchanged).unwrap(), 0);
        assert!(unknown.bytes().is_empty());
    }

    #[cfg(not(target_arch = "wasm32"))]
    #[test]
    fn persisted_defaults_and_player_changes() {
        let dir = std::env::temp_dir().join(format!("dhd-phone-defaults-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("dhd.ini");
        let mut changed = 0;
        assert!(!crate::update_phone_defaults(&path, Class::Unchanged, &mut changed).unwrap());
        assert!(!path.exists());
        assert!(crate::update_phone_defaults(&path, Class::Minimum, &mut changed).unwrap());
        assert_eq!(changed, 3);
        crate::save(&path, "scale", "2").unwrap();
        crate::save(&path, "shadows", "1").unwrap();
        let before = std::fs::read(&path).unwrap();
        assert!(!crate::update_phone_defaults(&path, Class::Minimum, &mut changed).unwrap());
        assert_eq!(std::fs::read(&path).unwrap(), before);
        std::fs::remove_dir_all(dir).unwrap();
    }
}
