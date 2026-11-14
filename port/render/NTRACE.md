# `.ntrace`: the native renderer's picture trace

A change that breaks old traces bumps `version` and keeps the reader for the old one while a
recorder still writes it.

**Versions.** 2 (now): a command's head is `DhdrCommand` up to its first pointer, 184 bytes; the
replayer takes the size from the struct (`offset_of!(DhdrCommand, texture)`), so it follows the
layout `abi.rs` pins. 1: the same with the 168-byte head of before the native mesh draws grew
`DhdrNativeConstants` (finding 168). Version 1 traces came only from the retired recorder below and
none is kept, so the replayer refuses them (exit 2) instead of keeping a reader nothing can feed
(finding 242).

An `.ntrace` records, picture by picture, exactly what the native renderer's GPU half is given (the
`DhdrPicture` that `port/gs/native_snapshot.inc` builds) and what the C path drew from it on
wgpu-native: the display picture, the counters and every surface's contents before and after the
picture. A replayer draws the same pictures on the Rust renderer
(`port/render/tools/src/bin/native_replay.rs`) and compares every byte. This tests the Rust native
modules before any C glue exists, one picture at a time.

- **Recorder:** the `DHD_NATIVE_RTRACE` block in `port/gs/gpu_wgpu_native.inc` (task R2). It
  belonged to the retired C renderer (C wgpu-native), removed by cleanup CL2 (finding 088): it is
  last at render/rust 3882d5576. The tag `v0.3.1` does not have it (the 0.3.1 release never carried the C
  wgpu-native renderer). Nothing records new traces now; the goldens below stay as recorded. It
  was off unless `DHD_NATIVE_RTRACE=<path>` was set. When off it cost one branch per picture and
  changed nothing. When on it did not change what was drawn either: `test_native_transfers.py
  --backend both` and its golden compare stayed the same with recording on.
  - It built each picture with `native_snapshot.inc` (with its own `NATIVE_SNAPSHOT_TEXTURE` /
    `NATIVE_SNAPSHOT_VIEW` handles), so the trace holds the very structure the Rust path receives.
  - It recorded every picture `native_picture` drew (DHD_INTERP pictures included), not "present
    again".
  - `DHD_NATIVE_RTRACE_RANGE=first-last` recorded only pictures `first` to `last` (indices count
    every picture drawn, recorded or not).
- **Goldens:** `$GOLD/ntrace/{transfers-dual, transfers-nodual, g+3600}.ntrace`. They are
  game-derived (g+3600) or adapter-tied, so they are never committed.
- **Determinism:** the same program, input, environment and adapter must record byte-identical
  traces (R2's referee records twice and compares). The trace holds no time, pid, pointer,
  absolute path or uninitialised byte.

## Layout

All integers are little-endian; floats are IEEE binary32 bit patterns. Offsets are in bytes. Every
record starts on a 4-byte boundary. `NONE` is `0xFFFFFFFF`.

### Header

| Offset | Type | Field |
|---|---|---|
| 0 | `u8[8]` | magic `"DHDNTRC\0"` |
| 8 | `u32` | `version` = 2 |
| 12 | `u32` | `header_bytes`: total header size, a multiple of 4, padding included |
| 16 | `u32` | `flags`: bit 0 `DUAL`, set when the recording renderer drew with dual-source blending (its `dual` and its dual module); every other bit 0 |
| 20 | `u32` | `api`: 1 DX12, 2 Vulkan, 3 browser WebGPU |
| 24 | `u32` | `vendor_id` (PCI) |
| 28 | `u32` | `device_id` (PCI) |
| 32 | `u32` | `compiler`: 0 none / unknown, 1 FXC, 2 DXC |
| 36 | `u32` | `fallback_adapter`: 1 when the adapter is a CPU / fallback one (WARP), else 0 |
| 40 | `u64` | `driver_version`: DXGI UMD version, Vulkan `driverVersion`, or 0 |
| 48 | `u8[40]` | `recorder_sha`: ASCII hex git blob SHA-1 of the `gpu_wgpu_native.inc` that recorded (the build passes it in as `DHD_NATIVE_RTRACE_SHA`; 40 `'0'` when unknown) |
| 88 | `u32` | `adapter_bytes`, then that many bytes of UTF-8 adapter name (no NUL) |
| ... | `u32` | `source_bytes`, then that many bytes of UTF-8: the program's base name and the caller's label, for example `test_native_transfers wgpu` (no paths) |
| ... | | zero padding up to `header_bytes` |

A replayer must check `magic`, `version` and `header_bytes`, and print the header. It refuses (exit
2, not a pass) a trace with a `version` it does not know.

### Records

Each record is a `u32 tag`, a `u32 payload_bytes`, then the payload, then zero padding up to a
multiple of 4. `payload_bytes` excludes the padding. Struct names are `dhd_render.h`'s; "as is"
means the struct's bytes exactly as `dhd_render.h` lays them out (no implicit padding exists, and
pad members are 0).

| Tag | Record | Payload |
|---|---|---|
| 1 | `TEXTURE` | `u32 id, u32 kind, u32 width, u32 height, u32 levels`, then the pixels. `kind` 1: a decoded texture (`dhdr_texture_create`; `levels` = 1; `width * height * 4` RGBA8 bytes, rows unpadded). `kind` 2: a replacement (`dhdr_replace_create`; each level's rows after the previous level's, sides halved down to 1: the sum over levels `l` of `max(1, width >> l) * max(1, height >> l) * 4` bytes). |
| 2 | `BLOB` | `u32 id, u32 format, u32 width, u32 height`, then `width * height * 4` bytes, rows unpadded: a surface's contents. `format` 1: RGBA8 (colour surface); 2: depth32float bit patterns (depth surface). |
| 3 | `PICTURE` | Starts picture `index`: `u32 index` (pictures drawn so far in the process, from 0), `u32 picture_flags` (bit 0: DHD_INTERP picture, its vertices mixed), `u64 field` (`NativeFrame.field`), `u32 scale`, `i32 display_surface`, `u32 display[12]`, `u32 movie_on, u32 movie_width, u32 movie_height`, `u32 vertex_count, u32 command_count, u32 convert_count, u32 surface_count`, then `u32 label_bytes` and that many bytes of UTF-8 (the recording program's name for the picture, or empty; no filter depends on it). The values are the `DhdrPicture`'s. |
| 4 | `SURFACES` | `u32 count` (= `surface_count`), then per surface `i` (from 0): `DhdrSurfaceInfo` as is (40 bytes), `u32 before`: the `BLOB` id of its contents when the picture's commands start (after `native_prepare_surfaces`: grown, old texels kept, new ones zero), `NONE` exactly when its `size` is 0 x 0. The blob's size equals `size`, its format follows `depth`. |
| 5 | `VERTICES` | `u32 count` (= `vertex_count`), then `count` `DhdrVertex` as is (32 bytes each). |
| 6 | `CONVERTS` | `u32 count` (= `convert_count`), then per conversion: `i16 surface[4]`, `DhdrConvertConstants` as is (1632 bytes, `tex[3]` as recorded), `u32 base` (`TEXTURE` id or `NONE`). |
| 7 | `COMMANDS` | `u32 count` (= `command_count`), then per command: the first 184 bytes of `DhdrCommand` as is (everything before its pointers), `u32 texture`, `u32 replacement` (`TEXTURE` ids or `NONE`), `u32 pixel_bytes`, then that many bytes: `IMPORT` `w * h * 4`, `ALPHA_UPLOAD` `64 * 32 * 4`, every other kind 0. |
| 8 | `MOVIE` | Only when `movie_on`: `u32 width, u32 height`, then `width * height * 4` RGBA8 bytes. |
| 9 | `RESULT` | Ends the picture: `DhdrPictureResult` as is (80 bytes; the counters as the C path counted them for this picture alone), then, when `shown` is 1, `display_width * display_height * 4` bytes (the display texture after the picture, as `native_read_display` returns it), then `u32 count` (= `surface_count`) and per surface `u32 after`: the `BLOB` id of its contents after the picture was submitted, `NONE` when it has no texture. When `shown` is 0, `display_width` and `display_height` are 0. |
| 15 | `END` | `u64 records` (records before END), `u64 fnv`, `u32 counts[16]` (records with tags 0-15; 0 is unused). |

**Order.** A picture is exactly `PICTURE`, `SURFACES`, `VERTICES`, `CONVERTS`, `COMMANDS`,
[`MOVIE`], `RESULT`, with nothing else between them except `TEXTURE` and `BLOB` records. Pictures
follow each other with increasing `index` (gaps only where `DHD_NATIVE_RTRACE_RANGE` skipped).

**Ids.** `TEXTURE` ids and `BLOB` ids each count from 0 in order of definition. Every id is defined
by its record before the first record that refers to it, and only once. Equal contents are stored
once: two textures with the same `kind`, size, `levels` and bytes share one id (the renderer's
output does not depend on texture identity), and so do two equal blobs (a surface's `after` is
usually the next picture's `before`). The recorder may key on a SHA-256 of the record's payload
after the id.

Only tags 1-9 and 15 are valid in version 2. Any other tag is an error.

### END and integrity

- `fnv` is FNV-1a 64 (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`) over every byte
  from the first record's tag up to, but not including, END's tag. Padding is included.
- A trace is invalid when it has no END (truncated), bytes after END, a wrong `records`, `counts`
  or `fnv`, a record that runs past the end of the file, records out of the order above, a
  reference to an id not yet defined, an id defined twice, a count that disagrees with
  `PICTURE`, or a size that disagrees with its payload. The replayer fails it before drawing
  anything, so a reordered, dropped or edited record can never pass.

## Replay rule

Each picture is replayed on its own: its surfaces start from their `before` blobs, so a picture does
not depend on the pictures before it, and a module can be tested on the pictures that use it alone.

1. The header and integrity checks pass.
2. **Same adapter.** The replay device must match the header's `api`, `vendor_id`, `device_id`,
   `driver_version`, `compiler` and `fallback_adapter`. Otherwise the replayer prints both and
   refuses (exit 2, not a pass): native pictures are compared at 0 differing pixels (decision
   D5), which holds for one adapter and driver.
3. **Same blending.** The `DhdrNative` is created with `DHDR_NATIVE_DUAL` exactly when the header
   has `DUAL`. A `DUAL` trace on a device without dual-source blending is refused (exit 2).
4. For every selected picture, in trace order: make each surface with `size` not 0 x 0 at its
   `scale`, `size` and `depth`, filled with its `before` blob, and no other surface; make every
   referenced texture from its `TEXTURE` record (once per replay); draw the picture as
   `dhdr_native_picture` does (commands, scanout or movie, submit); then compare:
   - the `DhdrPictureResult`: `shown`, `display_width`, `display_height` and every counter,
     exactly;
   - the display pixels, every byte (0 differing pixels; alpha included);
   - every surface's contents after the picture with its `after` blob, every byte (depth as bit
     patterns).
5. It prints `compared N pictures (P display pixels, S surfaces)`, with N the number of selected
   pictures. A selection of 0 pictures is a failure (exit 1).

There is no tolerance option. On the first mismatch the replayer prints the picture index and label,
what differs (a result field, the display, or surface `i`), the first differing pixel (x, y and both
values) and, as a hint, the last command that wrote there (index and kind: the last `DRAW`, `COPY`,
`IMPORT` or `ALPHA_UPLOAD` whose target is that surface, or the display surface for the display).
Then it exits 1. It keeps going only with `--all`, and still exits 1.

**Selections** (the same for every replayer, decided from the trace's content only):

| Option | Pictures |
|---|---|
| (none) | all |
| `--picture N` | the picture with `index` N |
| `--only no-draw` | pictures with no `DRAW` command |
| `--only convert-scenes` | pictures with at least one `DRAW` whose `convert` is not `DHDR_NO_INDEX` |
| `--only draw` | pictures with at least one `DRAW` command |

The replayer runs with IEEE float control on every thread that calls the device (MXCSR `0x1F80`
class), as the renderer's exports do. 
