# 201: PowerVR GE8322 (driver 1.18) closes at the first picture's submit

Status: in progress

Branch `fix/powervr-ge8322-submit` from `main` f0971070 (0.7.16), started 2026-11-11. This file
is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #82: an itel A70 (itel A665L, Android 13, Unisoc T603 / SC9863A, PowerVR Rogue GE8322,
driver "PowerVR Rogue Vulkan Driver 1.18@6267915", 3.8 GB RAM) shows a black screen and closes
right after the game starts: `crash: SIGSEGV at 0000000000000000 in thread native`, a null pointer
dereference in the driver (`vulkan.sp9863a.so+0x6e058`) under `vulkan::driver::QueueSubmit`.

Goal: name the crash's place (our function and what it submits) and its cause with the evidence;
fix it with one code path where possible; if the cause cannot be pinned, log what pins it from the
next shared log. A release-signed test APK for the reporter (not published by the lane).

There is no PowerVR phone here; the cause comes from the log, the release's symbols, the code and
what others found.

## Progress

- [x] 2026-11-11: finding reserved (2ad22978).
- [x] 2026-11-11: the version. The crashing `libmain.so` has BuildId `69e6d2de...`, 0.7.14's kept
  debug library's (0.7.15's is `a9eb97f2...`, 0.7.16's `9bf143d8...`). Both run logs say 0.7.14
  (the earlier one: "no newer Android release than 0.7.14"). The share header says 0.7.15 because
  the share is written by the installed app: the player updated to 0.7.15 after the crash and
  shared from it without starting a game (a 0.7.15 run would have been the shared "last run").
  So 0.7.15 and 0.7.16 are untested on this phone; nothing they changed touches the cause (below).
- [x] 2026-11-11: the place. Symbolised against 0.7.14's library: `native_thread` ->
  `native_picture` -> `dhdr_native_picture_indexed` -> `Native::picture_indexed` ->
  `Pass::submit_now` -> `wgpu::Queue::submit` -> wgpu-core `Queue::submit` -> wgpu-hal Vulkan
  `Queue::submit` -> the loader's `QueueSubmit` -> the driver. The return address `+0x18681c8`
  is the call at `+0x18681c4`, the last `submit_now` call in `picture_indexed`, after `scan_out`
  and `zread_record` returned: the native renderer's first picture's own submit (`render.rs`
  `self.pass.submit`), not the present's (that one runs inside `scan_out`, which is not inlined
  and not on the stack). Nothing new is in that submit: the picture's command buffer(s).
- [x] 2026-11-11: the cause (Result): what wgpu-hal's Vulkan submit chains on a device with
  timeline semaphores, read by a driver that ignores the wait-value count.
- [x] 2026-11-11: the fix, 30e25104: the vendored wgpu-hal gives both value lists one entry per
  semaphore once the timeline info is chained (a binary wait's is 0). `tests/semaphore_values.rs`
  checks the submit info's lists; with upstream's `semaphore_list.rs` its two tests of a binary
  wait fail (wait values `[]` with one wait semaphore), with the fix all four pass.
- [x] 2026-11-11: the log line, c9e4d3ae: on Vulkan, `gs: Vulkan 1.4.357, submits signal a
  timeline semaphore` (or `VkFences`), after the GPU line. On this PC's RTX 3070 it prints that.
- [x] 2026-11-11 checks: `rustgs.py test --target x86_64-pc-windows-msvc` all green (device_smoke
  and framebuffer_cache run real Vulkan devices through the changed submit on the RTX 3070);
  `rustgs.py check --target aarch64-linux-android` OK; `check.py quick` OK on c9e4d3ae (after one
  `DHD_MODULE_CACHE=0` core build: a fresh worktree's cache hit leaves no `build/wasm/src` for
  test_runtime); `check.py docs` OK.
- [ ] the test APK: not built by the lane, the `DHD_ANDROID_*` signing variables are not set in
  its environment and a debug-signed APK blocks release updates. The lead builds it (Result).
- [ ] the reporter's run of the test build.

## Decisions

- **Pad the values for every driver, not a PowerVR switch.** The spec says a binary semaphore's
  entry in `pWaitSemaphoreValues` / `pSignalSemaphoreValues` is ignored, and a non-zero count must
  equal the semaphore count, which the padded lists meet; so the padded form is valid everywhere
  and one code path. The other known answer, timeline semaphores off on vendor 0x1010 (a wgpu fork,
  below), would be a second fence path kept for one vendor. Upstream PR #10677 started unconditional
  and was put behind a workaround flag on review; its testers asked for it unconditional too.
- **A binary wait's padding is 0, a signal's stays `!0`.** Vulkan ignores both. If a driver that
  reads the wait entry ever used it as a timeline wait, 0 is already reached and `!0` never is;
  signals were already padded with `!0` upstream and the first submit on this driver (relay and
  timeline signals, no waits) passed with them.
- **Written here, not taken from the PR**: its diff was not read (the page shows none); the change
  is ours, in the vendored crate under its MIT/Apache licence, marked `Downhill Revived` and listed
  in `vendor/wgpu-hal/PATCH.md`.
- **The test lives in the renderer crate** (`tests/semaphore_values.rs`, `ash` as a dev-dependency
  at the locked version): wgpu-hal's own tests need winit/glutin, outside the renderer's lock. The
  vendored crate re-exports `SemaphoreList` and its two types, doc-hidden, for it.
- **The log line instead of more diagnostics**: the crash's place is pinned; what a next report
  needs is whether its device took the timeline path. A per-submit trace was not added.
- **The Mali one-encoder rule (finding 198) is not extended to PowerVR**: it is about how many
  command buffers a picture submits, and the crash does not depend on that.

## Surprises

- **The share header's version is not the run's**: the header is the sharing app's version; the
  runs' own first lines and the BuildId are what ran.
- **Not a shader or pipeline** (findings 170, 176): every pipeline of the start was built and
  logged before the crash, and the fault is a read through null, not a compiler's table miss.
- **Driver 1.13 never took this path**: the public Android 10 SC9863A build of the same driver
  family lists no `VK_KHR_timeline_semaphore` among its extensions, so wgpu-hal used VkFences there
  and chained no timeline info. That is why the GE8320 phones of findings 170 and 176 (1.13)
  reached the menus and this GE8322 with 1.18 did not.

## Result

The crash's place: the native renderer's first picture's submit (`render.rs`
`Native::picture_indexed` -> `Pass::submit_now` -> `wgpu::Queue::submit`), which in wgpu-hal's
Vulkan backend (`vulkan/mod.rs` `Queue::submit`) is one `vkQueueSubmit` with the picture's command
buffers.

The cause, with the evidence (confidence: high, about 85 %; the 1.18 driver build itself was not
disassembled):
1. **What that submit carries.** On a device with timeline semaphores wgpu-hal signals its fence as
   a timeline semaphore in every submit, so every submit chains a `VkTimelineSemaphoreSubmitInfo`.
   Every submit but the queue's first also waits on a binary relay semaphore (`RelaySemaphores`,
   ordering submits), and a submit that draws to the swapchain waits on its binary acquire
   semaphore. With only binary waits, upstream set `waitSemaphoreValueCount = 0` and
   `pWaitSemaphoreValues = NULL`, which the spec allows.
2. **What PowerVR's driver does with it.** gfx-rs/wgpu PR #10677 (open, not merged when this was
   written) and its thread: on PowerVR's proprietary driver (B-Series BXM-4-64 and BXM-8-256,
   Rogue GE83xx named by the other fix there), "whenever a VkTimelineSemaphoreSubmitInfo is in the
   pNext chain, it loads pWaitSemaphoreValues[i] for every wait semaphore without checking
   waitSemaphoreValueCount" (found by disassembling `vulkan.jupiter.so`); "offscreen rendering
   survives exactly one submit because the relay semaphore's wait starts with the second" and the
   first submit to a surface crashes. A fork of wgpu (hellopaint-io/wgpu 3cffcf8) turns timeline
   semaphores off on vendor 0x1010 for the same crash, verified on a moto g54 (BXM-8-256) and a
   Galaxy A03s (GE8320, a newer driver than our 1.13 reports). wgpu issue #7669 is the moto g54's.
3. **The log fits it exactly.** A null pointer read (`fault addr 0x0`) inside the driver's
   `vkQueueSubmit`, at the picture's own submit, after earlier work but before any surface frame
   (`gs: window shown`, the audio start, then the crash): the second or a later submit of the queue,
   the first with a wait. The registers at the fault: `x1 = 0` (the null values pointer) and
   `x2 = 0x30`, the size of `VkTimelineSemaphoreSubmitInfo` on arm64.
4. **Why only driver 1.18**: 1.13 has no timeline semaphores (Surprises), so it never got the
   chained struct. The log could not say which path a device took; it now does (c9e4d3ae).
5. **0.7.15 and 0.7.16 crash the same way** (expected, not seen): no commit between v0.7.14 and
   f0971070 touches the vendored wgpu-hal or the renderer's `Cargo.toml`/`Cargo.lock`; finding
   195's encoder cut and finding 198's one-encoder rule change how many command buffers a submit
   has, not its semaphores.

Other candidates, and why they rank lower: a bad command buffer (the driver would fault in its
command processing, not on a null read straight from the submit; the same buffers run on Adreno,
Mali and the desktop drivers), memory (3.8 GB, the first picture), a lost device (Vulkan returns
an error; this is a fault in the call).

What changed: `port/render/vendor/wgpu-hal/src/vulkan/semaphore_list.rs` (`add_to_submit` pads
both lists; `dummy_value`), the re-export in `vulkan/mod.rs`, `Adapter::timeline_semaphores` in
`vulkan/adapter.rs`, `PATCH.md`; `port/render/src/device.rs` `log_vulkan`;
`port/render/tests/semaphore_values.rs` and `ash` as a dev-dependency (one line in
`Cargo.lock`). Every Vulkan device with timeline semaphores (Adreno, Mali, desktop Vulkan) now
passes values for its binary waits; nothing else in a submit changed. Metal, DX12 and WebGPU are
untouched.

Checked by the lane: the crate tests above (Windows, RTX 3070, Vulkan and DX12 devices), the
negative control (upstream's `semaphore_list.rs` fails two of the four tests), the Android
target's `rustgs.py check`; `check.py quick` and `check.py docs` green on c9e4d3ae. No game was
run (no state trace, no picture), no phone was used.

Not done by the lane (no phone, no signing key, the owner's PC in use):
- **The test APK.** `python port/android/build_apk.py --release` with the `DHD_ANDROID_*`
  variables set, from this branch (or the merged branch); give it to #82's reporter.
- **The lead's checks once on the merged branch**: `rustgs.py test`; `check.py port`; a boot
  trace against the base build on Windows with `DHD_WGPU_BACKEND=vulkan` (the hidden-desktop
  harness, `docs/TESTING.md`) to run the changed submit in the game; the test APK on an Adreno
  phone (the Mi 11i: timeline semaphores, so it takes the padded path every frame) to the title
  and into a race; a Mali phone (the Redmi 9) to the title if one is at hand. No game state can
  change (a submit's semaphore values only), so no race trace is needed beyond the boot trace.
- **From the reporter**: whether the test build starts and reaches the menus; if it still closes,
  Share log from the test build: its `gs: Vulkan ...` line says which path their driver took and a
  new crash's frames map with the test build's kept `libmain.so`.

Sources: gfx-rs/wgpu PR #10677 "vulkan: Give every semaphore a value when chaining timeline
submit info" and its comments; gfx-rs/wgpu issue #7669; hellopaint-io/wgpu commit 3cffcf8 (cited
in the PR); the Vulkan spec's `VkTimelineSemaphoreSubmitInfo` (values of binary semaphores are
ignored). The SC9863A driver's extension list: a public Android 10 firmware dump on GitHub (the
same family's `vendor/lib64/hw/vulkan.sp9863a.so`; its strings read with the NDK's
`llvm-strings`, never run, not kept in the repository). The 1.18@6267915 build is only in
repair-site firmware archives of the itel A665L (gigabytes), not fetched.
