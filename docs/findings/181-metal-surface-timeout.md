# 181: Metal surface drawable timeout on mobile platforms

Status: current
Checked: 2026-11-08

Branch `fix/shared-metal-surface` from `main` db3702d, started 2026-11-08.

## Goal and acceptance

On Apple mobile platforms (iOS, tvOS, visionOS), allow CAMetalLayer's native drawable timeout to remain
active (`setAllowsNextDrawableTimeout(true)`). Upstream wgpu-hal overrides this property to `false`, which
causes `nextDrawable` to block indefinitely if presentation halts when the application transitions into the
background or is suspended by the system compositor.

Acceptance:
- `port/render/vendor/wgpu-hal/src/metal/surface.rs` enables drawable timeout on mobile platforms (`target_os = "ios"`, `"tvos"`, `"visionos"`);
- macOS retains `false` (existing desktop behavior unchanged);
- `python tools/check.py docs` passes with 0 errors;
- Leaves Windows and Linux builds and state traces completely unchanged.

## Decisions

- **Conditional compilation via `cfg!`:** Used `cfg!(any(target_os = "ios", target_os = "tvos", target_os = "visionos"))`
  inside `setAllowsNextDrawableTimeout(...)`. This compiles cleanly on all platforms with zero overhead on non-Apple targets.

## Result

- `port/render/vendor/wgpu-hal/src/metal/surface.rs`: enabled timeout for mobile targets.
- `port/render/vendor/wgpu-hal/PATCH.md`: documented the change.
