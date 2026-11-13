# Credits

This project builds on the work of others:

- [PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0-or-later): the reference for how the hardware
  behaves. The GS memory layouts in `port/gs/memory.c` follow PCSX2's GSLocalMemory/GSTables.
  The test and comparison tools build PCSX2 from source at test time.
- The PCSX2 widescreen patch for this game, by Aced14
  ([pcsx2_patches](https://github.com/PCSX2/pcsx2_patches)): the map of values the 16:9 mode
  changes.
- [ps2sdk](https://github.com/ps2dev/ps2sdk): protocol and library references (no code
  included). The reverb presets are read from the player's own disc (Sony's libsd module).
- [OpenGOAL](https://github.com/open-goal/jak-project) (ISC): the SPU2 Gaussian interpolation
  table in `port/hle/sound_gaussian.h`.
- [SDL](https://libsdl.org) 3.4.18 (zlib): the window, sound and controllers, built from its
  pinned release source by `port/tools/sdl3.py` and linked in statically.
- [wgpu](https://github.com/gfx-rs/wgpu) (MIT/Apache-2.0) with naga and the Rust standard library:
  the renderer (`port/render`); the crates linked into `dhd.exe` are listed with their licences in
  the release's `THIRD-PARTY-NOTICES.txt`.
- [WABT](https://github.com/WebAssembly/wabt) (Apache-2.0): wasm2c turns the game core into C for
  the 64-bit build. [wasi-sdk / wasi-libc](https://github.com/WebAssembly/wasi-sdk) (MIT option) and
  [SIMDe](https://github.com/simd-everywhere/simde) (MIT) build the core.
- [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) (NCSA/MIT):
  `dxcompiler.dll`, shipped unchanged, compiles the Direct3D 12 shaders.
- [Binaryen](https://github.com/WebAssembly/binaryen) (Apache-2.0) and
  [wasm-bindgen](https://github.com/rustwasm/wasm-bindgen) (MIT/Apache-2.0): the browser build.
- [libmpeg2](https://libmpeg2.sourceforge.io) 0.5.1 (GPL-2.0-or-later): the movie decoder,
  `port/third_party/libmpeg2`, unchanged.
- [xxHash](https://github.com/Cyan4973/xxHash) (BSD-2-Clause): the XXH64 specification, from
  which `port/net/hash.c` is written, and `port/gs/xxhash.h` (XXH3, unchanged) for texture pack
  names.
- PCSX2's texture replacement names (its GSTextureCache / GSTextureReplacements hash scheme),
  reimplemented in `port/gs/texpack.c` so PCSX2 packs load unchanged.
- [splat](https://github.com/ethteck/splat), [spimdisasm](https://github.com/Decompollaborate/spimdisasm)
  and [rabbitizer](https://github.com/Decompollaborate/rabbitizer) (MIT): splitting and
  disassembling the executable.
- [objdiff](https://github.com/encounter/objdiff) (MIT/Apache-2.0) and [decomp.dev](https://decomp.dev):
  progress reports.
- [m2c](https://github.com/matt-kempster/m2c) (GPL-3.0): first drafts of functions.
- [decomp.me](https://decomp.me) and its [compilers](https://github.com/decompme/compilers)
  collection, and the SN ProDG mirror by AngheloAlf: the period compilers.
- [ps2dev](https://github.com/ps2dev/ps2dev) binutils (GPL-3.0): assembling and linking.
- [ps2tek](https://psi-rockin.github.io/ps2tek/), [Play!](https://github.com/jpd002/Play-),
  [vgmstream](https://github.com/vgmstream/vgmstream) and other PS2 projects: documentation and
  ideas.
- [ninja](https://ninja-build.org) (Apache-2.0), LLVM/clang (Apache-2.0 with LLVM exception),
  [wrangler](https://github.com/cloudflare/workers-sdk) (MIT/Apache-2.0).

The full notices of the code adapted into this repository are kept with it
(`port/hle/audio_licenses/`, file headers).

The touch controls' action pictures (`port/render/src/present/touch_icons.txt`, finding 229) are
the project's own drawings, written as vector shapes by hand and released under its licence; the
idea of touch buttons that show what they do comes from the community iOS fork
`bagasromadon/downhill-revived-ios-newbutton` (a fork of `ribeirogustav/downhill-revived-ios`),
whose pictures are not used.

The button icons in `port/res/buttons/` (A, B, X, Y, LB, RB, LT, RT, L, R, ZL, ZR) are the
project's own art, made for it with an image generator: no brand logos or colours, no images from
the game's disc. They are released with the project under its licence (GPL-3.0-or-later).
