# 134: rustgs.py's cargo target folder without DHD_CARGO_DIR

BUILDING.md says cargo's output goes under `DHD_DEVCACHES/dhd-cargo`. On Windows, `rustgs.py`
spelled that default as the literal string `'DHD_DEVCACHES/dhd-cargo'`, not the variable's value.
Every other tool reads `os.environ.get('DHD_DEVCACHES') or ~/DevCaches` (`build_wasm.py`,
`dxc.py`, objcache). With `DHD_CARGO_DIR` unset, the target folder was the relative path
`DHD_DEVCACHES\dhd-cargo\<worktree hash>`, which resolves in two places:

- cargo runs with `cwd=port/render`, so it built into `port/render/DHD_DEVCACHES/...` (an
  untracked folder in the worktree);
- rustgs checks from the directory it was started in (the repository root), found no dep-info
  there and refused: `dep-info DHD_DEVCACHES\dhd-cargo\...\dhd_render.d is missing`.

The build only worked where `DHD_CARGO_DIR` was set. Seen on a fresh Windows setup (2026-10-30;
`DHD_DEVCACHES=D:\DevCaches`, no `DHD_CARGO_DIR`). The default is now
`Path(DHD_DEVCACHES or ~/DevCaches) / 'dhd-cargo'`, as documented. With it, a cold
`rustgs.py build --target x86_64-pc-windows-msvc` built `dhd_render.lib` into
`D:\DevCaches\dhd-cargo\31c0788d\` and passed `check_rust_lib`.

A library built into the worktree folder (before the fix) can't be moved into the cache and
reused: `check_rust_lib` rightly refuses the worktree path compiled into its objects. Delete it
and build cold.

## The rest of a fresh Windows setup

The same setup (one 256 GB system drive, so the checkout and caches went onto a USB drive, D:)
needed four things that BUILDING.md didn't say. It says them now:

- **SIMDe.** The v0.8.2 release picks `SIMDE_FLOAT16_API_FLOAT16` for any non-x86 clang 6 or
  later, wasm32 included, and clang 23 refuses `_Float16` there. `build_wasm.py` then fails 4
  port units ("_Float16 is not supported on this target", reported at `runtime/ps2asm.c:98`, a
  line in an included header). SIMDe `master` asks for `__wasm_fp16__` on WASI (commit 28ab7b41c3,
  2025-09-19). Built with `master` at 65c15e6b (2026-10-29): 3815 game units and 35 port units,
  strict link and the offset scan clean.
- **The wasi-sdk's own clang.** `abi_check.py`, `testprog.py`, `postlink.py` and the web test
  builders run `<wasi-sdk>/bin/clang`, not LLVM's on PATH. Here the sysroot and builtins were
  installed alone (GitHub served the 619 MB SDK at a few KB/s), and `bin/` was rebuilt from LLVM
  23.1.3's `clang.exe` and `wasm-ld.exe`, clang's `include/`, and a `clang.cfg` with
  `--target=wasm32-wasip1` and the sysroot. That is the wasi-sdk 34 layout on the same clang
  major. The whole SDK is still the documented way.
- **wasm-bindgen-cli** at `Cargo.lock`'s 0.2.129 for `rustgs.py build --target
  wasm32-unknown-unknown`.
- **A rustup default** for the launcher, which has no `rust-toolchain.toml`, and its crates
  (`cargo fetch --locked` in `port/launcher/src-tauri`): `package_release.py` runs cargo there
  offline.

LLVM was in `C:\Program Files\LLVM` (its installer), not scoop's folder; `build_host64.py` and
nine other tools default to `~/scoop/apps/llvm/current/bin`, so that path was made a junction to
it.

With those, on this machine (RTX 3060 Laptop GPU):

- `package_release.py` wrote `DownhillRevived-0.7.2-win.zip` from committed source, all its
  checks passing.
- `test_runtime.py` and `test_gs_wgpu.py` passed (VERDICT PASS: `test_gpu`, `test_depth` and
  `test_present` on WARP, DX12 and Vulkan).
- `port/net/build_tests.py` passed (`test_hash`, `test_json`, `test_loopback`).
