/* The C compiler's view of port/render/include/dhd_render.h, for port/tools/test_dhdr_abi.py
  . Never linked or run: the script compiles this file to LLVM IR for each target
   (x86_64, wasm32) and reads two constant arrays out of it:
     dhdr_abi_names   the row names, NUL-separated: "T#size", "T#align", "T.f" (offset),
                      "T.f#size" (the member's size), "=NAME" (a constant's value);
     dhdr_abi_values  one int64 per row, in the same order.
   It then requires src/abi.rs, compiled for the same target, to give the same value for every
   row. Every struct, member and constant of the header is listed; a row the Rust side lacks is
   a failure. */
#include "dhd_render.h"

#define ROWS \
    S(DhdrDeviceDesc) F(DhdrDeviceDesc, size) F(DhdrDeviceDesc, abi) F(DhdrDeviceDesc, flags) \
        F(DhdrDeviceDesc, vendor_id) F(DhdrDeviceDesc, device_id) F(DhdrDeviceDesc, log_level) \
        F(DhdrDeviceDesc, dxc_path) \
    S(DhdrExactStats) F(DhdrExactStats, submits) F(DhdrExactStats, reads) F(DhdrExactStats, read_ticks) \
        F(DhdrExactStats, dispatches) F(DhdrExactStats, bytes_written) F(DhdrExactStats, copies) \
    S(DhdrWindow) F(DhdrWindow, kind) F(DhdrWindow, reserved) F(DhdrWindow, a) F(DhdrWindow, b) \
    S(DhdrVertex) F(DhdrVertex, x) F(DhdrVertex, y) F(DhdrVertex, z) F(DhdrVertex, s) F(DhdrVertex, t) \
        F(DhdrVertex, q) F(DhdrVertex, fog) F(DhdrVertex, rgba) \
    S(DhdrNativeConstants) F(DhdrNativeConstants, target) F(DhdrNativeConstants, flags) \
        F(DhdrNativeConstants, atst) F(DhdrNativeConstants, aref) F(DhdrNativeConstants, alpha_scale) \
        F(DhdrNativeConstants, texa_alpha) F(DhdrNativeConstants, scale) F(DhdrNativeConstants, fog) \
        F(DhdrNativeConstants, tex_offset) F(DhdrNativeConstants, tex_size) F(DhdrNativeConstants, wrap) \
        F(DhdrNativeConstants, region) F(DhdrNativeConstants, mesh_offset) \
    S(DhdrConvertConstants) F(DhdrConvertConstants, tex) F(DhdrConvertConstants, size) \
        F(DhdrConvertConstants, slot) F(DhdrConvertConstants, pages) F(DhdrConvertConstants, palette) \
    S(DhdrCommand) F(DhdrCommand, kind) F(DhdrCommand, depth_key) F(DhdrCommand, color_mask) F(DhdrCommand, pad) \
        F(DhdrCommand, target) F(DhdrCommand, depth) F(DhdrCommand, texture_surface) F(DhdrCommand, convert) \
        F(DhdrCommand, blend_key) F(DhdrCommand, blend_fix) F(DhdrCommand, scissor) F(DhdrCommand, constants) \
        F(DhdrCommand, first) F(DhdrCommand, count) F(DhdrCommand, source) F(DhdrCommand, pad2) \
        F(DhdrCommand, x) F(DhdrCommand, y) F(DhdrCommand, w) F(DhdrCommand, h) F(DhdrCommand, sx) \
        F(DhdrCommand, sy) F(DhdrCommand, texture) F(DhdrCommand, replacement) F(DhdrCommand, pixels) \
    S(DhdrConvert) F(DhdrConvert, surface) F(DhdrConvert, constants) F(DhdrConvert, base) \
    S(DhdrSurfaceInfo) F(DhdrSurfaceInfo, page) F(DhdrSurfaceInfo, bw) F(DhdrSurfaceInfo, wide) \
        F(DhdrSurfaceInfo, depth) F(DhdrSurfaceInfo, zpsm) F(DhdrSurfaceInfo, scale) F(DhdrSurfaceInfo, size) \
        F(DhdrSurfaceInfo, extent) \
    S(DhdrPicture) F(DhdrPicture, size) F(DhdrPicture, vertex_count) F(DhdrPicture, command_count) \
        F(DhdrPicture, convert_count) F(DhdrPicture, surface_count) F(DhdrPicture, display_surface) \
        F(DhdrPicture, display) F(DhdrPicture, movie_width) F(DhdrPicture, movie_height) \
        F(DhdrPicture, movie_on) F(DhdrPicture, scale) F(DhdrPicture, vertices) F(DhdrPicture, commands) \
        F(DhdrPicture, converts) F(DhdrPicture, surfaces) F(DhdrPicture, movie) \
    S(DhdrPictureResult) F(DhdrPictureResult, shown) F(DhdrPictureResult, display_width) \
        F(DhdrPictureResult, display_height) F(DhdrPictureResult, pad) F(DhdrPictureResult, draws) \
        F(DhdrPictureResult, self_copies) F(DhdrPictureResult, self_copy_px) F(DhdrPictureResult, copies) \
        F(DhdrPictureResult, copy_px) F(DhdrPictureResult, imports) F(DhdrPictureResult, import_px) \
        F(DhdrPictureResult, two_pass_draws) \
    S(DhdrPresentRect) F(DhdrPresentRect, rect) F(DhdrPresentRect, cut) F(DhdrPresentRect, window_w) \
        F(DhdrPresentRect, window_h) F(DhdrPresentRect, resize) F(DhdrPresentRect, fill) \
    C(DHDR_ABI_VERSION) \
    C(DHDR_DEV_VULKAN) C(DHDR_DEV_FXC) C(DHDR_DEV_DEBUG) C(DHDR_DEV_VALIDATION) C(DHDR_DEV_FALLBACK) \
    C(DHDR_DEV_NO_DUAL_SOURCE) C(DHDR_DEV_DUAL_SOURCE) \
    C(DHDR_LOG_ERROR) C(DHDR_LOG_WARN) C(DHDR_LOG_INFO) \
    C(DHDR_FEAT_DUAL_SOURCE) C(DHDR_FEAT_RENDER_PIPELINES) \
    C(DHDR_B_VRAM) C(DHDR_B_SNAPSHOT) C(DHDR_B_STATES) C(DHDR_B_PRIMITIVES) C(DHDR_B_BINS) C(DHDR_B_DISPLAY) \
    C(DHDR_B_COPY) C(DHDR_B_READBACK) C(DHDR_B_STAGED) C(DHDR_B_BATCHES) C(DHDR_B_UPLOAD) C(DHDR_B_COUNT) \
    C(DHDR_P_LOCAL_COPY) C(DHDR_P_SCANOUT) \
    C(DHDR_RASTER_EARLY_DEPTH) C(DHDR_RASTER_ADDRESS_SHIFT) C(DHDR_RASTER_EXCLUSIVE_WORD_STORE) \
    C(DHDR_WINDOW_WIN32) C(DHDR_WINDOW_WEB_CANVAS) C(DHDR_WINDOW_XLIB) C(DHDR_WINDOW_WAYLAND) \
    C(DHDR_WINDOW_ANDROID) C(DHDR_WINDOW_METAL_LAYER) \
    C(DHDR_PRESENT_NO_TEARING) C(DHDR_PRESENT_VSYNC) C(DHDR_SURFACE_COPY_SRC) \
    C(DHDR_CMD_DRAW) C(DHDR_CMD_COPY) C(DHDR_CMD_IMPORT) C(DHDR_CMD_ALPHA_UPLOAD) C(DHDR_NO_INDEX) \
    C(DHDR_NATIVE_DUAL) C(DHDR_NATIVE_POINT_FILTER)

#define MEMBER_SIZE(T, f) sizeof(((T *)0)->f)

/* Names. */
#define S(T) #T "#size\0" #T "#align\0"
#define F(T, f) #T "." #f "\0" #T "." #f "#size\0"
#define C(N) "=" #N "\0"
const char dhdr_abi_names[] = ROWS;
#undef S
#undef F
#undef C

/* Values. */
#define S(T) (int64_t)sizeof(T), (int64_t)_Alignof(T),
#define F(T, f) (int64_t)offsetof(T, f), (int64_t)MEMBER_SIZE(T, f),
#define C(N) (int64_t)(N),
const int64_t dhdr_abi_values[] = { ROWS };
#undef S
#undef F
#undef C

/* Every export, so a compiler that sees the header also sees each prototype used. */
#define E(name) (const void *)&name,
const void *const dhdr_abi_functions[] = {
    E(dhdr_device_init) E(dhdr_adapter_name) E(dhdr_device_features) E(dhdr_version)
    E(dhdr_exact_create) E(dhdr_exact_destroy) E(dhdr_exact_write) E(dhdr_exact_copy) E(dhdr_exact_batch)
    E(dhdr_exact_record) E(dhdr_exact_submit) E(dhdr_exact_read_begin) E(dhdr_exact_read_finish)
    E(dhdr_exact_capture_begin) E(dhdr_exact_capture_finish) E(dhdr_exact_stats)
    E(dhdr_surface_create) E(dhdr_surface_destroy) E(dhdr_surface_usage) E(dhdr_present_exact)
    E(dhdr_surface_present)
    E(dhdr_native_create) E(dhdr_native_destroy) E(dhdr_texture_create) E(dhdr_replace_create)
    E(dhdr_texture_retain) E(dhdr_texture_release) E(dhdr_native_surface) E(dhdr_native_picture)
    E(dhdr_native_present) E(dhdr_native_read_begin) E(dhdr_native_read_finish) E(dhdr_native_options)
    E(dhdr_native_zread_start) E(dhdr_native_zread_take)
#if defined(__wasm__)
    E(dhdr_web_surface)
#else
    E(dhd_host_log)
#endif
#ifdef DHD_GS_TEST_BUILD
    E(dhdr_test_depth_probe) E(dhdr_test_presented)
#endif
};
#undef E
