//! The player's picture settings (finding 275): Brightness, Contrast, Saturation and Gamma
//! (dhd.ini `brightness`, `contrast`, `saturation`, `gamma`; the rows are `dhd_settings`' table).
//! C reads the four variables as numbers and hands them over with `dhdr_picture`
//! (`dhd_render_ext.h`) whenever they may have changed (the first picture, the SETTINGS page;
//! numbers, not text: the browser's renderer has a memory of its own); both present passes (the exact one, `pipe.rs`,
//! and the native one, `native/present.rs`) put `uniform()` into their constants, and
//! `picture.wgsl`'s `picture_adjust` applies it to the game's picture only: the touch controls and
//! the FPS counter are drawn after it, untouched.
//!
//! All four neutral (100 %, 100 %, 100 %, 1.0) make the uniform all zeros, and the shader then
//! returns the sampled colour as it is: today's picture, byte for byte.
use std::sync::atomic::{AtomicU32, Ordering};

/// The settings' ranges: percentages (brightness, contrast, saturation) and gamma.
pub const PERCENT_MIN: f32 = 50.0;
pub const PERCENT_MAX: f32 = 150.0;
pub const GAMMA_MIN: f32 = 0.6;
pub const GAMMA_MAX: f32 = 1.4;

/// The four settings as factors (1.0 neutral each).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Picture {
    pub brightness: f32,
    pub contrast: f32,
    pub saturation: f32,
    pub gamma: f32,
}

impl Picture {
    pub const NEUTRAL: Picture = Picture { brightness: 1.0, contrast: 1.0, saturation: 1.0, gamma: 1.0 };

    /// From the settings as numbers: percentages for the first three (120), the gamma (1.2). One
    /// that is not a number (NaN: missing or unreadable) is neutral; the rest are held to the
    /// ranges.
    pub fn new(brightness: f32, contrast: f32, saturation: f32, gamma: f32) -> Picture {
        let percent = |v: f32| if v.is_nan() { 1.0 } else { v.clamp(PERCENT_MIN, PERCENT_MAX) / 100.0 };
        Picture {
            brightness: percent(brightness),
            contrast: percent(contrast),
            saturation: percent(saturation),
            gamma: if gamma.is_nan() { 1.0 } else { gamma.clamp(GAMMA_MIN, GAMMA_MAX) },
        }
    }

    /// From the variables' text, as C reads it (a number at the start, "120%" too); missing or
    /// unreadable is neutral.
    pub fn parse(brightness: Option<&str>, contrast: Option<&str>, saturation: Option<&str>, gamma: Option<&str>) -> Picture {
        let number = |s: Option<&str>| {
            let s = s.unwrap_or("").trim();
            let end = s.find(|c: char| !(c.is_ascii_digit() || c == '.' || c == '-' || c == '+')).unwrap_or(s.len());
            s[..end].parse::<f32>().unwrap_or(f32::NAN)
        };
        Picture::new(number(brightness), number(contrast), number(saturation), number(gamma))
    }

    pub fn is_neutral(&self) -> bool {
        *self == Picture::NEUTRAL
    }

    /// `picture_adjust`'s `adjust`: saturation, contrast, brightness, 1 / gamma; zeros when neutral.
    pub fn uniform(&self) -> [f32; 4] {
        if self.is_neutral() {
            [0.0; 4]
        } else {
            [self.saturation, self.contrast, self.brightness, 1.0 / self.gamma]
        }
    }
}

/// `picture_adjust` on the CPU (the same steps, for the tests and the finding's numbers).
pub fn adjust(colour: [f32; 3], adjust: [f32; 4]) -> [f32; 3] {
    if adjust[3] == 0.0 {
        return colour;
    }
    let luma = 0.2126 * colour[0] + 0.7152 * colour[1] + 0.0722 * colour[2];
    colour.map(|c| {
        let saturated = luma + (c - luma) * adjust[0];
        let contrasted = (saturated - 0.5) * adjust[1] + 0.5;
        (contrasted * adjust[2]).clamp(0.0, 1.0).powf(adjust[3])
    })
}

/// The settings now, as the uniform's four f32 bit patterns (zeros: neutral).
static NOW: [AtomicU32; 4] = [AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0)];

pub fn set(p: Picture) {
    for (slot, v) in NOW.iter().zip(p.uniform()) {
        slot.store(v.to_bits(), Ordering::Relaxed);
    }
}

/// `dhdr_picture` (`dhd_render_ext.h`): the player's picture settings for the next pictures, as
/// numbers (NaN: not set). Any thread.
#[no_mangle]
pub extern "C" fn dhdr_picture(brightness: f32, contrast: f32, saturation: f32, gamma: f32) {
    let _g = crate::guard::Guard::enter("dhdr_picture");
    let p = Picture::new(brightness, contrast, saturation, gamma);
    set(p);
}

/// The uniform the present passes draw with now.
pub fn uniform() -> [f32; 4] {
    NOW.each_ref().map(|slot| f32::from_bits(slot.load(Ordering::Relaxed)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_or_neutral_is_the_identity() {
        assert_eq!(Picture::parse(None, None, None, None), Picture::NEUTRAL);
        assert_eq!(Picture::parse(Some("100"), Some("100%"), Some(" 100 "), Some("1.0")), Picture::NEUTRAL);
        assert_eq!(Picture::parse(Some(""), Some("bright"), Some("-"), Some(".")), Picture::NEUTRAL);
        assert_eq!(Picture::new(f32::NAN, f32::NAN, f32::NAN, f32::NAN), Picture::NEUTRAL);
        assert_eq!(Picture::NEUTRAL.uniform(), [0.0; 4]);
        for c in [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0], [0.25, 0.5, 0.75], [1.0 / 255.0, 128.0 / 255.0, 254.0 / 255.0]] {
            assert_eq!(adjust(c, Picture::NEUTRAL.uniform()), c);
        }
    }

    #[test]
    fn values_are_held_to_the_ranges() {
        assert_eq!(Picture::new(f32::INFINITY, f32::NEG_INFINITY, 70.0, 1.2),
                   Picture { brightness: 1.5, contrast: 0.5, saturation: 0.7, gamma: 1.2 });
        let p = Picture::parse(Some("10"), Some("400"), Some("-5"), Some("9"));
        assert_eq!(p, Picture { brightness: 0.5, contrast: 1.5, saturation: 0.5, gamma: 1.4 });
        assert_eq!(Picture::parse(None, None, None, Some("0.1")).gamma, 0.6);
        assert_eq!(Picture::parse(Some("120"), None, None, Some("1.3")),
                   Picture { brightness: 1.2, contrast: 1.0, saturation: 1.0, gamma: 1.3 });
    }

    #[test]
    fn known_values() {
        let close = |a: [f32; 3], b: [f32; 3]| a.iter().zip(b).all(|(x, y)| (x - y).abs() < 1e-5);
        let only = |b: &str, c: &str, s: &str, g: &str| Picture::parse(Some(b), Some(c), Some(s), Some(g)).uniform();
        // brightness 150 %: mid-grey to 0.75, white held at 1
        assert!(close(adjust([0.5, 0.5, 1.0], only("150", "100", "100", "1")), [0.75, 0.75, 1.0]));
        // contrast 50 % around mid-grey: black to 0.25, white to 0.75, grey stays
        assert!(close(adjust([0.0, 0.5, 1.0], only("100", "50", "100", "1")), [0.25, 0.5, 0.75]));
        // saturation 50 %: pure red halfway to its luma
        let luma = 0.2126;
        assert!(close(adjust([1.0, 0.0, 0.0], only("100", "100", "50", "1")), [(1.0 + luma) / 2.0, luma / 2.0, luma / 2.0]));
        // gamma 1.4 lifts 0.25 to 0.25^(1/1.4) (about 0.372); black and white stay
        assert!(close(adjust([0.0, 0.25, 1.0], only("100", "100", "100", "1.4")), [0.0, 0.25f32.powf(1.0 / 1.4), 1.0]));
    }

    #[test]
    fn set_and_read_back() {
        set(Picture::parse(Some("120"), None, None, None));
        assert_eq!(uniform(), [1.0, 1.0, 1.2, 1.0]);
        set(Picture::NEUTRAL);
        assert_eq!(uniform(), [0.0; 4]);
    }
}
