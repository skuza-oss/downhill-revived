# port/render/: the Rust library (renderer, settings, net, disc, presence)

The root and `port/AGENTS.md` rules apply.

- One static library, `dhd_render`, linked into every native build; the browser takes the
  renderer and `dhd_settings` as wasm. Crates: the renderer (`src/`, wgpu: exact `src/exact`,
  native `src/native`), `settings` (dhd.ini's only implementation, finding 101), `net` (`dhd_net`,
  WebSocket and HTTPS over rustls with bundled webpki roots; no OS certificate store, no proxy),
  `disc` (the USA disc's game-files hash, finding 098), `presence` (Discord IPC, finding 106).
- Build and test through the wrapper, not bare cargo: `python port/tools/rustgs.py build|test|
  check --target <triple>` (pinned toolchain `rust-toolchain.toml`, cache dirs from
  `DHD_DEVCACHES`). `rustgs.py check --target aarch64-apple-darwin` checks the Mac code anywhere.
- The C side talks to the library through `include/dhd_render.h` and `dhd_render_ext.h`; keep
  `src/abi.rs` and `port/gs/test_dhdr_abi.c` in step (`port/tools/test_dhdr_abi.py`).
- The exact renderer draws the PS2's pixels and answers the game's Z probe: its output is part of
  game state, so changes there need the state trace and the replay goldens
  (`port/tools/gs_golden.py`, recorded locally, never committed). The native renderer only shows
  pictures; it must not change what the game reads back.
- Shaders are WGSL (`port/gs/*.wgsl`, finding 084); DX12 compiles them with the pinned DXC
  (`port/tools/dxc.py`). `vendor/` holds patched upstream crates (`vendor/wgpu-hal/PATCH.md` says what
  and why); keep their licences and notices.
- Settings rows: `settings/src/table.rs`; `settings/tests/generated.rs` checks the generated C
  table (`DHD_SETTINGS_BLESS=1` regenerates it).
