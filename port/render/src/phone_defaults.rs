//! Android's pre-launch adapter query: no surface, device or game is started (239).
use std::{ffi::{c_char, CStr}, path::Path};
use dhd_settings::phone::{self, Class};

/// Called before the launcher reads values, and before a direct game start loads settings.
/// `path` and `soc` are NUL-terminated strings supplied by Android's native glue.
#[no_mangle]
pub unsafe extern "C" fn dhdr_android_defaults(path: *const c_char, soc: *const c_char) {
    if path.is_null() || soc.is_null() { return; }
    let (Ok(path), Ok(soc)) = (unsafe { CStr::from_ptr(path) }.to_str(), unsafe { CStr::from_ptr(soc) }.to_str()) else { return };
    let path = Path::new(path);
    if dhd_settings::disabled(path) { return; }
    if dhd_settings::read(path).is_some_and(|ini| ini.has("scale")
        && ["shadows", "lens_flare", "particles"].iter().any(|k| ini.has(k))) { return; }
    let mut desc = wgpu::InstanceDescriptor::new_without_display_handle();
    desc.backends = wgpu::Backends::VULKAN;
    desc.flags = wgpu::InstanceFlags::ALLOW_UNDERLYING_NONCOMPLIANT_ADAPTER;
    let instance = wgpu::Instance::new(desc);
    let adapters = pollster::block_on(instance.enumerate_adapters(wgpu::Backends::VULKAN));
    // Multiple adapters: don't guess which the game will select.
    let gpu = if adapters.len() == 1 { adapters[0].get_info().name } else { String::new() };
    if adapters.len() > 1 { return; }
    let class = phone::class(&gpu, soc);
    let mut changed = 0;
    let saved = dhd_settings::update_phone_defaults(path, class, &mut changed);
    if saved.is_ok_and(|saved| saved) {
        let resolution = if changed & 1 != 0 { "1x" } else { "the player's resolution" };
        let performance = if changed & 2 != 0 { "on" } else { "the player's effects" };
        let below = if class == Class::Below { " Below minimum: the game may run well under 60 fps; see docs/PLAYING.md system requirements via launcher Help > GitHub." } else { "" };
        let report = format!("settings: this phone ({gpu}; {soc}) starts at {resolution} with Performance Mode: {performance}. Change in launcher Settings > Graphics > Resolution / Performance Mode.{below}\n");
        // The launcher has no dhd.log yet. Consume this at game start after the log opens.
        if let Some(dir) = path.parent() { let _ = std::fs::write(dir.join("dhd-start-settings.txt"), report); }
    }
}
