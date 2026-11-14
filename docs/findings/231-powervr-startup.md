# 231: PowerVR startup black screen diagnostics

Status: in progress

Branch `fix/powervr-black-screen` from `main` a99bf961 (v0.8.1), started 2026-11-14.

## Goal and acceptance

Issues #92 and #82: distinguish an unexpected process exit, a driver call that does not return,
and returned presents with a black picture. No PowerVR phone is available. Required gates:
`python tools/check.py quick -j 6`, `python tools/check.py port -j 6`,
`python tools/check.py docs`, the Android native library build, and boot (600 fields) / race
(5000 fields) state traces equal to a99bf961. No APK signing, phone access or timing benchmarks.

## Progress

- [x] Reserved the finding (17d27c8d); read findings 170, 171, 176 and 201.
- [x] Read both issues, every comment and their supplied logs. #92 has two inline runs, 0.8.0
  and 0.7.16. #82's log attachment is also quoted in its body: the run is 0.7.14, not the share
  header's 0.7.15. No confirmation of test-powervr-3 / 0.8.0 there yet.
- [x] Trace startup and research related driver reports (below).
- [x] `check.py quick -j 6` and `check.py docs` passed. Fresh core build succeeded without
  the module-cache retry. Base host built; base boot and race traces have 600 and 5000 rows.
- [x] `rustgs.py test --target x86_64-pc-windows-msvc -- --lib startup::` passed (exit 0). Standalone harness compiled the actual Android diagnostic
  module on Windows (queue types stubbed, no GPU): immediate unexpected exit leaves died,
  a six-second call leaves hung, next process reports it, three presents disable writes/logs,
  a returned slow call clears that stage, and repeated acquire notices stop at 24. All passed.
- [x] LastExit.java and CrashReport.java compile against the installed Android SDK.
- [x] Final Android native library: `build_host64.py --target android-arm64 --jobs 6`
  passed (exit 0), including C/host linkage of the startup callback. No APK generated.
- [x] Boot state trace: all 600 rows match the base, state_diff PASS.
- [x] Race state trace: all 5000 rows match the base, state_diff PASS.
- [x] `python tools/check.py port -j 6` passed every step: core/runtime, host, retail-data scan,
  net, renderer WARP tests, settings, launcher, stack analysis and its negative-control gate.
  Android's first compile caught a missing diagnostic re-export; corrected before the successful
  final library build. The first core build did not need a cache-disabled retry.
- [x] Round 2: the lead's Mi 11i run of main d325a769 (including 9e022e91) exposed a
  diagnostic defect: Android cwd is the root directory, not the files folder. The relative record
  writes failed with EROFS 332 times; a force-stop at stage 10 left no record. In-run stages and
  retirement after three presents worked. This lane did not access the phone.
- [x] Round 2 implementation: android_main.c passes its existing absolute files folder by a
  copied UTF-8 C string into the existing Rust startup state. Reads, temporary writes, replacement
  and retirement use that folder; relative or missing host paths are rejected, never used as cwd.
  Record I/O failures log once per run, while later writes can still recover.
- [x] Round 2 startup unit tests: all four pass (`rustgs.py test --target
  x86_64-pc-windows-msvc -- --lib startup::`, exit 0). They cover an absolute folder away from
  cwd, replacement and kill/restart persistence, terminal markers, retirement, relative-path
  rejection and the once-only failure log with successful write recovery.
- [x] A standalone host harness compiled the actual Android diagnostic module (queue types
  stubbed, no GPU/phone) and passed a copied CString folder different from cwd. A simulated
  unexpected exit at stage 10 left that stage; the next process printed it and removed the
  record at three presents. A subsequent process printed no prior startup diagnosis; simulated
  background/return entry points did not recreate it. Repeated write failures logged once.
- [x] Round 2 Android native library build passed, exit 0, with the C-to-Rust path handoff linked.
- [x] Round 2 `python tools/check.py quick -j 6` passed; core build and runtime tests succeeded.
- [x] Round 2 `python tools/check.py docs` passed.
- [ ] PowerVR tester confirmation: still open; this lane does not claim a graphics fix.

## Candidate causes, ranked

1. **Exact creation: compiler crash, very slow compile or hang, especially raster `main`.**
   `gpu_rust.c` prints `gs: renderer` after device creation and its render probe succeed, then
   creates the exact renderer: shared raster shader, scanout pipeline, keyed main and local_copy
   pipelines, error scopes, buffers and display. #92 ends at that line in both runs. Native
   creation's completion line is absent. This is the same location as 170, but 170's uniform OR
   workaround already ships here; do not remove it or reapply it as a new fix. Driver 1.11 may
   have another compiler defect; there is no stack to prove one. A long compile can look exactly
   like a dead process in the old log. Watch the named shader / entry's begin and returned pair,
   elapsed seconds and watchdog, with its enclosing exact creation scope.
2. **Allocation failure or external kill during exact creation.** Buffers follow those pipelines;
   no crash handler runs on SIGKILL. The 3.7 GB RAM is total RAM, not available memory. Finding 176
   establishes that PowerVR can report compiler failure as OOM too, so OOM alone is not evidence
   of memory pressure. A record saying died, without a crash or Android exit record, still cannot
   distinguish a kill from an unreported native crash. The exact-creation scope isolates the
   buffer phase after every pipeline returns.
3. **Native shader/pipeline creation or CPU worker/queue wait.** Later than #92's earliest suspect;
   startup can fail there too. Named native object, shader, entry and draw-key scopes include
   error-scope waits. Finding 176's dead-colour pipeline handling remains intact. Native picture,
   finish/helper wait, actual submit and exact readback fence wait are separately delimited.
   One submit helper covers uploads, copies, scanout, Z reads and overlays as well as pictures.
4. **Surface creation/capabilities/configure/acquire/present or GPU/fence stall.** Later still;
   separate begin/returned pairs name them. Finding 201 already pads timeline submit values.
   #92 explicitly says VkFences, excluding that specific timeline-chain fault. #82 took the
   timeline path in the older build and still needs the offered fix tested. Finding 171 handles
   reported device loss; it cannot help if the driver never returns and reports no loss.
5. **The process runs and presents black pixels.** Three returned presents establish that the CPU
   reached presentation, not that the display or shader output is correct. A tester must report
   whether anything is visible and whether audio/input continue. No readback is added: it would
   introduce another fence wait and alter the path under investigation.

## Decisions

- **Established diagnostic defect fixed:** the old log has no stages inside exact creation;
  Android's asynchronous stdout/stderr log pipe also means fflush alone does not guarantee the
  file has the line before a driver call. Startup breadcrumbs drain that pipe. The durable private
  startup record is synced and replaced before entering each scope; it survives process death.
- **No speculative rendering change.** No documented Vulkan remedy found in this research
  matches 1.11@5425693 and the observed location closely enough to ship a workaround. No shaders,
  blend states, semaphores or present mode are changed. This is diagnostic instrumentation, not
  a proven cure for either issue.
- Android only, always on, through three returned presents. Then the watchdog exits and file/log
  writes cease; hot entry points retain only an inactive atomic check. Other platforms compile
  the calls away. Nested and concurrent scopes retain individual call ids and thread ids: a
  pipeline worker cannot overwrite a render thread's blocked submit.
- A five-second watchdog means slow/unreturned, not a proven permanent hang. It records `hung at
  <stage>` when that duration was observed. Otherwise the durable active scope reads `died at
  <stage>` on the next start: unexpected termination, including a kill, not necessarily a crash.
  A later returned call clears its timeout state. Known host shutdown retires the pending record.
- The record is included by LastExit in dhd-exit.txt (thus the next log's previous-run lines and
  Share log header), including Android 10. It contains operation names, no user or device data.

## How to read the new log

`gs startup: begin ID: STAGE (ThreadId(...))` is persisted and flushed before the operation.
`gs startup: returned ID: STAGE (SECONDS)` means control returned, including error returns;
existing validation/lost-device/error messages decide success. Allocation labels identify each
exact buffer; capped acquire details identify success, timeout, occlusion, outdated/lost/invalid
surfaces or a zero window size. The innermost unmatched begin on
that thread names the operation interrupted. `queue submit` separates the driver submit from
`native finish, helper wait and submit` and `exact finish and submitter wait`.

`gs startup: watchdog: ...; N presents returned` repeats at most every five seconds while startup
is delayed. With no active renderer call it says waiting for picture N: investigate the host/game
or an acquire repeatedly skipping frames, rather than inventing a driver crash.
`gs startup: picture N present returned` for N=1,2,3 means present returned. The record is
removed from the files folder and startup diagnostics retire. This does not prove nonblack
pixels or scanout completion. `previous run: startup: ...` carries the prior record on next play.

## Round 2: record lifecycle checked in code

The original standalone harness ran in a writable cwd, masking the Android root-cwd defect.
The new path tests use an absolute folder different from cwd and reject relative paths.

1. Before SDL starts, LastExit.java reads the last process's record from getFilesDir(), if present,
   and writes its startup diagnosis into dhd-exit.txt. android_main.c quotes that file into dhd.log.
   SDL_GetAndroidInternalStoragePath wraps that same getFilesDir() (SDL source below), so the C
   host's existing files buffer and Java agree on the directory. No chdir or new C global is used.
2. android_main.c passes that buffer after log setup, before launching the game. Rust copies it
   into the existing STATE's Record. The renderer's STARTED once guard reads a pending previous
   stage there, then syncs/replaces the new initial record; each startup scope updates it.
3. After a kill during startup there is no host end callback, so the pending stage remains.
   On the next launch Java's previous-run header and Rust's start can both name that stage.
   The next run replaces it with its own stages. Old terminal markers are ignored by Rust.
4. The third returned present sets ACTIVE false under the state lock and removes the record.
   Outstanding scope drops and the watchdog then see inactive and cannot save again. After a
   normal start and normal exit, the next launch has no startup hung-at/died-at line from that
   run: Java finds no record, and Rust finds none. The normal end callback sees inactive and
   does not recreate it. A known host shutdown before retirement also attempts removal.
5. A background/return reuses the process and its device. Neither STARTED nor STATE is reset;
   all stage, submit and present entry points stay inactive after retirement. Even another path
   handoff uses get_or_init and cannot rearm the recorder. A return cannot recreate the file.

Round 2 changes only path handling and diagnostic record lifecycle; no renderer operation or game
state changes. The requested gates are quick, the Android native library and docs, plus focused
startup unit tests; no new traces or phone runs.

## Research and sources

- [SDL internal storage API](https://wiki.libsdl.org/SDL3/SDL_GetAndroidInternalStoragePath):
  its C API wraps Android Context.getFilesDir(); reused for the Round 2 path handoff.

- [wgpu timeline submit fix discussion](https://github.com/gfx-rs/wgpu/pull/10677): unchecked
  timeline wait-value reads on PowerVR, already handled by 201. Not the VkFences path in #92.
- [wgpu pipeline error propagation](https://github.com/gfx-rs/wgpu/issues/10027): pipeline creation
  errors can lose a device; 176's vendored error mapping and scopes already address this here.
- [Imagination compiler discussion](https://forums.imgtec.com/t/use-glprogrambinary-in-powervrge8320-always-execute-glslcompiletouniflex-is-slow/4103):
  vendor explains full LLVM compilation versus binary loading; its test uses GE8320 driver
  1.11@5425693. This is GLES evidence for compile latency, not a Vulkan workaround.
- [Imagination GE8320 report](https://forums.imgtec.com/t/powervr-ge8320-gpu-graphics-bug-on-youtube-shorts/4249):
  same version string, video corruption; no matching Vulkan startup cause or remedy.
- [PPSSPP allocation fix](https://github.com/hrydgard/ppsspp/pull/11744) and
  [swapchain fix](https://github.com/hrydgard/ppsspp/pull/11747): historical PowerVR corruption
  reports, not evidence that our pre-surface exact creation needs a swapchain workaround.
- [Imagination memory requirements report](https://forums.imgtec.com/t/vulkan-vkgetimagememoryrequirements2-crashes-on-powervr-8300/3458):
  Vulkan 1.0 core-versus-KHR entry-point loading mistake, not a demonstrated match for this
  Vulkan 1.1 device; no loader workaround added.
- No offline compiler environment is configured through docs/TESTING.md's optional DHD_MALIOC;
  none installed or downloaded. Finding 170 already demonstrates that a newer offline compiler
  does not reproduce the older driver's failure. No upstream code copied.

## Surprises

The native renderer is the player's selected path, but the exact renderer still starts first for
its Z probe. Therefore the absence of a native pipeline log is meaningful: changing native VU or
its draw pipeline would not address the leading startup suspect.

## Result

The established fix is diagnostic: operation breadcrumbs, a bounded startup watchdog, durable
stage attribution after an unexpected exit and the Android log-pipe drain. The cause of #92
remains a hypothesis; no new rendering workaround ships. No phone was accessed and no APK was
built, signed, installed or published.

For the lead: build a test APK with its unstripped native library retained. Ask each reporter to
start it once, leave a black screen in the foreground for at least 10 seconds if it stays open,
and say whether it closed itself, remained open, or had sound/responded to input. Reopen once,
then Share log and send the entire file. Its current and previous run sections plus the startup
record distinguish a slow call, an unexpected exit, repeated skipped acquisitions and returned
presents. For #82 also ask whether the offered test/0.8.0 was ever tried; do not assume it was.
Keep the finding in progress until a PowerVR log confirms or rejects the ranked causes.
