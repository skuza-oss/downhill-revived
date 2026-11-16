//! Android's pre-launch adapter query: no surface, device or game is started (239).
use std::{ffi::{c_char, CStr}, path::Path};
use dhd_settings::phone::{self, Class};

/// Called before the launcher reads values, and before a direct game start loads settings.
/// `path` and `soc` are NUL-terminated strings supplied by Android's native glue (`soc`: the SoC
/// model, else the board platform). Returns the settings filled now (`phone::defaults`' bits: 1 the
/// resolution, 2 Performance Mode), 0 when nothing changed: the launcher's note says it once.
#[no_mangle]
pub unsafe extern "C" fn dhdr_android_defaults(path: *const c_char, soc: *const c_char) -> u32 {
    if path.is_null() || soc.is_null() { return 0; }
    let (Ok(path), Ok(soc)) = (unsafe { CStr::from_ptr(path) }.to_str(), unsafe { CStr::from_ptr(soc) }.to_str()) else { return 0 };
    let path = Path::new(path);
    if dhd_settings::disabled(path) { return 0; }
    if dhd_settings::read(path).is_some_and(|ini| ini.has("scale")
        && ["shadows", "lens_flare", "particles"].iter().any(|k| ini.has(k))) { return 0; }
    let mut desc = wgpu::InstanceDescriptor::new_without_display_handle();
    desc.backends = wgpu::Backends::VULKAN;
    desc.flags = wgpu::InstanceFlags::ALLOW_UNDERLYING_NONCOMPLIANT_ADAPTER;
    let instance = wgpu::Instance::new(desc);
    let adapters = pollster::block_on(instance.enumerate_adapters(wgpu::Backends::VULKAN));
    // Multiple adapters: don't guess which the game will select.
    if adapters.len() > 1 { return 0; }
    let gpu = adapters.first().map(|a| a.get_info().name).unwrap_or_default();
    let Some(rule) = phone::rule(&gpu, soc) else { return 0 };
    let mut changed = 0;
    if !dhd_settings::update_phone_defaults(path, Some(rule), &mut changed).is_ok_and(|saved| saved) { return 0; }
    let performance = if changed & 2 != 0 { "Performance Mode on" } else { "the player's effects" };
    let resolution = if changed & 1 != 0 { ", 1x" } else { "" };
    let below = if rule.class == Class::Below { " Below minimum: the game may run well under 60 fps; see docs/PLAYING.md system requirements via launcher Help > GitHub." } else { "" };
    let report = format!("settings: this phone's GPU \"{gpu}\" (SoC \"{soc}\") matched the rule \"{}\" ({}): this install starts with {performance}{resolution}. Change in Settings > Graphics.{below}
", rule.name, rule.source);
    // The launcher has no dhd.log yet. Consume this at game start after the log opens.
    if let Some(dir) = path.parent() { let _ = std::fs::write(dir.join("dhd-start-settings.txt"), report); }
    changed
}
