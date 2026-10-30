# 122: The browser's disc check is the native game's, in wasm

## Why

Online rooms compare each player's disc hash, and the server turns a mismatch away. The hash is the
game files hash of finding 098: every ISO 9660 file's path, size and contents, in path order
(`dhd_disc`, port/render/disc). Finding 098 left the browser out because it had no online races. For
the browser to race, a browser player's hash has to equal a native player's for the same image.
The only safe way to make sure of that is to run the same code.

## What

- `port/render/disc/src/web.rs` (wasm32 only; `c.rs`, the native C interface, is now native
  only): a plain C interface with no wasm-bindgen, like dhd_settings's (finding 102). The module
  defines and exports its own memory and imports one function:
  - import `env.dhd_disc_read(offset_lo: u32, offset_hi: u32, ptr: *mut u8, len: u32) -> i32`:
    how many bytes were read (0 only at the end), < 0 if the read failed;
  - export `dhd_disc_web_check(size_lo, size_hi, digest: *mut u8, note: *mut u8, note_size) -> i32`:
    `crate::hash` through a Read + Seek adapter over the import. It returns `dhd_disc_check`'s
    codes without the cache: 1 hashed, -1 the image can't be read, -2 not the USA disc's file
    system (no ISO 9660 volume, or a cut dump). Notes are worded as natively;
  - exports `dhd_disc_alloc(len)` / `dhd_disc_free(ptr, len)`, plus `memory`. Two leftover symbols
    are exported too, because rustc exports every `#[no_mangle]`: `__getrandom_custom` and ring's
    `ring_core_0_17_14__bn_mul_mont`. Nothing calls them.
- ring's `getrandom` 0.2 refuses to build for wasm32-unknown-unknown unless it is given a source
  of random bytes. SHA-256 needs none, so the crate takes getrandom's `custom` feature, for that
  target only, and registers a source that always fails. This adds no crate: both locks
  (port/render, port/launcher/src-tauri) gain only the `getrandom 0.2.17` edge under `dhd_disc`.
  Native builds don't activate it.
- `port/web/www/disc.js`: `discCheck(wasm, read, size) -> Promise<{ code, digest, note }>`.
  `wasm` is the module's bytes or a compiled `WebAssembly.Module`. `read(offset, out: Uint8Array)`
  returns a byte count. `digest` is 64 lower-case hex digits (or '' when there is no hash).
  Instantiation is async; the hashing is one synchronous call, as FileReaderSync in the worker
  needs. Offsets are split into two u32s, so images past 4 GiB work. A `read` that throws gives
  -1, and its message is added to the note.
- `rustgs.py build --target wasm32-unknown-unknown --crate dhd_disc` builds build/web/dhd_disc.wasm.
  It uses the same path as dhd_settings (`build_module`, cdylib, its own memory, target dir
  `<target dir>/dhd_disc`) with the same gates: the include pre-check, the dep-info inputs, and
  check_rust_lib on the module. A dependency gate replaces settings' "no dependencies":
  - the graph (`cargo tree -e normal,build`) may hold only ring, getrandom, cfg-if, untrusted,
    cc, find-msvc-tools and shlex;
  - ring must be exactly 0.17.14;
  - no naga / wgpu-core / wgpu-hal, wasm-bindgen or js-sys.

  ring's C is built with LLVM's clang and llvm-ar for the target and `-g0`. ring's build.rs has a
  no-sysroot mode for wasm32 (`-nostdlibinc`, `RING_CORE_NOSTDLIBINC`), so the wasi-sdk isn't
  needed. ring asks for `-g3`, whose DWARF would name the cargo registry. clang's version is part
  of the cache key.
- `port/web/build.py`: `rust_module(crate, ...)` builds dhd_settings and now dhd_disc, then
  `wasm-opt -Oz`. dhd_disc.wasm is listed in manifest.json.
- `check_web_layout.py` rule P2: dhd_disc.wasm must be present. It imports exactly
  `env.dhd_disc_read` (a function), defines one unshared memory exported as `memory`, and has no
  start function. The selftest has a `disc_shared` fixture for it.

worker.js is not wired here: the main session does that, calling `discCheck` with discSource's
`read` and `size`.

## Sources

ring 0.17.14's build.rs (`configure_cc`: the wasm32 no-sysroot mode, `-g3`) and getrandom
0.2.17's lib.rs / custom.rs (the wasm32-unknown-unknown `compile_error!`,
`register_custom_getrandom!`), both read from the local cargo registry. dhd_settings's web.rs and
build path (finding 102) is the model for the interface and the build.

## Checks

- `rustgs.py build --target wasm32-unknown-unknown --crate dhd_disc`: built, check_rust_lib clean,
  93 672 bytes (71 577 after build.py's `-Oz`). The same bytes came out of two cargo builds, and a
  rerun is a cache hit. Refusals were tried: `--target x86_64-pc-windows-msvc`, ring off its pin,
  and a crate outside the allowed graph.
- `cargo test -p dhd_disc` (native): 9 of 9. The fixture test now pins its value,
  `0932bcbc...7864106`, which check_disc.mjs also wants.
- `node port/web/check_disc.mjs`: 6 of 6 PASS. The cases:
  - the crate's test image: the native value and the native note;
  - its files in another order on the disc: the same value;
  - a sparse 5.7 GiB image with the files from byte 6.1e9 on: 64-bit offsets give the
    definition's value;
  - a cut dump: -2, "A.BIN lies past the image's end";
  - no volume: -2;
  - a read that throws: -1.

  The script builds its images itself, with the crate's test layout, and computes the expected
  hash from the definition with node's crypto. It reads no game data. It passed before and after
  wasm-opt.
- The player's own USA image (2.36 GiB), read only:
  - native `dhd_disc_check` (a scratch program outside the tree): code 1,
    `c8c17bcae383daf9f2feadc7f6d7476c43d659ab8d66127a299b01311f2a3476`, note "" (7 s);
  - `check_disc.mjs --iso` (disc.js in node, fs reads): code 1, the same digest, note "", which
    is USA_FILES_SHA256 (17 s).
- `check_web_layout.py --selftest`: 8 fixtures OK. P1 and P2 hold for the built modules.
- Not run: the full `port/web/build.py` and a browser. The worker with FileReaderSync and a real
  File comes with the main session's wiring.
