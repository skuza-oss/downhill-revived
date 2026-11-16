# 258: the game starts on iPhones with 3 GB of memory (guest memory in two pieces)

Status: current

Branch `fix/ios-guest-memory-258` from `main` ca519dde (0.8.6), started 2026-11-15. This file is
the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A player's iPhone XR (iPhone11,8, A12, 3 GB, iOS 18.7.11), 0.8.5 sideloaded with their own Apple
ID: the disc import works, PLAY closes the app at once, every time. Its dhd.log ends with
`host64 memory: reserve guest memory (12: Cannot allocate memory)`, and the `system:` line says
"macOS 18.7.11".

Done means: the game reserves its memory on iPhones and iPads with 3 GB or less (every device iOS
17 runs on), one code path; a failure says in dhd.log what was asked and what was free, and the
player sees a message; the `system:` line says iOS. Proof without the phone: a unit test of the
placement arithmetic (`test_runtime.py`'s `guest-place`), a Mac test that fills the address space
so the old reservation fails and the new one starts (`port/ios/tests/test_guest_space.py`), the
iOS simulator build to a race. Windows, Linux, macOS and Android keep their 8 GiB layout; boot and
race state traces on the PC equal the base. Out of scope: the extended-virtual-addressing
entitlement (a paid account's provisioning profile; free Apple IDs can't have it).

## The cause (from XNU's source; the phone itself is unproven)

iOS sizes an app's address space by the device's memory unless the app has the
extended-virtual-addressing entitlement (`pmap_max_64bit_offset`, `osfmk/arm/pmap/pmap.c`):
more than 3 GiB of RAM gets `ARM64_MAX_OFFSET_DEVICE_LARGE`, 1 to 3 GiB
`ARM64_MAX_OFFSET_DEVICE_SMALL` = the end of the shared region + 0x20000000 + 0x38000000. In
xnu-11215 (iOS 18) the shared region is `[0x180000000, 0x300000000)` (`shared_region.h`), so an
iPhone XR's app sees:

| range | what |
|---|---|
| `[0, 0x100000000)` | the 4 GiB page zero (arm64 binaries must have it) |
| `[0x100000000, 0x180000000)` | 2 GiB: the app's binary (33 MB, slid by at most about 80 MiB), dyld, its heap, thread stacks; then whatever the launcher, WebKit and Metal map |
| `[0x180000000, 0x300000000)` | the shared cache (6 GiB), no app mapping |
| `[0x300000000, 0x358000000)` | 1.375 GiB, the first 512 MiB malloc's nano zone |

A free range of 2 GiB + 64 KiB (what 0.8.5 reserved for the bounds-checked layout) cannot exist
there at any moment: it is the address-space cap, not fragmentation, not the `mmap` flags and not
the shared memory object (which is made before the reservation and is not what failed: the error
is the reservation's `ENOMEM`). Devices with more than 3 GiB of RAM get 4.875 GiB above the shared
cache, which is why newer iPhones start. iOS 17 and 18 devices with 3 GB: iPhone XR, iPhone SE
(2nd generation), iPad (7th to 9th generation), iPad Air (3rd), iPad mini (5th).

Sources: xnu-11215.1.10 `osfmk/arm/pmap/pmap.c` and `osfmk/mach/shared_region.h` (newosxbook.com
source browser); "Size Matters: An Exploration of Virtual Memory on iOS" (alwaysprocessing.blog,
2022) for the same arithmetic on iOS 15; emulator projects (MeloNX forks, the Apparition
sideloader's notes) for "iPhones under 4 GB need extended virtual addressing" and for it being a
paid-account entitlement.

## What the guest really needs

Not "far below 2 GiB": the scratchpad is at its PS2 address 0x70000000, the HLE window above it,
and the module's own memory starts at 0x70100000 (`--global-base`, `memory_layout.h`) with its
initial size ending at 0x70E00000 (`build_wasm.py`). So the guest spans at least 0x70E10000 bytes
(1.76 GiB) from its base: more than the 1.375 GiB above the shared cache, and it fits below the
shared cache only if the base is in `[0x100000000, 0x10F1F0000]` and the guest's two used parts,
`[0, 0x3a000000)` (the RAM views and mirrors, stack slots, bridge arena, hardware windows) and
`[0x70000000, end)`, are both free. Nothing is ever committed in `[0x3a000000, 0x70000000)`, so
that gap may hold other mappings. The high part reaches 0x72010000 at least (the core's access
test needs its first 32 MiB, below), so the guest needs 928 MiB free at the base and 33 MiB free
1792 MiB above it.

## Progress

- [x] 2026-11-15: the cause, from XNU's source (above)
- [x] the bounds-checked layout reserved in two parts at one base found from the process's free
      ranges (`guest_place.c`, `os_free_ranges`), at the process's start (a constructor in
      `guest_memory.c`) and again at the game's start if that failed; growth up to what the high
      part got (c58f259e)
- [x] the free ranges start at the task's `min_address`, not in the page zero (66ef7c46, found by
      the Mac test)
- [x] the failure's dhd.log lines (what was asked, the largest free range, every free range of
      16 MiB or more), the success line (base and room), the player's alert, the launcher's note
      after such a run (7497fe6f); the `system:` line says iOS or iPadOS (600bf48a)
- [x] tests: `guest-place` (Windows, `test_runtime.py`), `test_guest_space.py` on the Mac
      (29211a62 gives each mode ten minutes: a busy Mac took over two)
- [x] 2026-11-15 checks: `check.py port` (quick and docs in it) OK on the PC; boot (600 fields)
      and race (5000) state traces equal 0.8.6's (ca519dde) build; on the Mac (M1, macOS): the
      macOS host build and its fiber test, `test_guest_space.py` (start, late, none: OK), the iOS
      tests, the Simulator app on "DHD Lane 224" and "DHD Lane 224 iPad" from the launcher to a race
      (`system: iOS Simulator 27.3`, `host64 memory: guest memory at 0x7000000000, reserved at the
      start: [0, 0x3a000000) and [0x70000000, 0x80010000), room to grow to 2048 MiB`)
- [x] the unsigned test IPA (`package_release.py --platform ios`, its audit and the release's
      Mac-side audit: no signing file, no home folder, no team of the Mac's identities) for the
      player; the real iPhone XR is unproven
- [x] the review's must-fix: a wild guest access into the gap or above the high part reached
      whatever the process had mapped there (iOS builds skip wasm2c's checks, finding 224), where
      0.8.5's single reservation made it fault. Now it ends as that fault did (f3ef0f1e), proven
      by `guest-access` (`test_runtime.py`, every path around every 4 MiB edge, with negative
      controls) and `test_guest_space.py` (a readable, writable mapping in the gap and one above
      the high part, a child per path: each ends by SIGSEGV with the fault's line, the mapping
      untouched) (16bd33ce). Checks on the final tree: `check.py port` OK on the PC (`guest-access`
      in it); boot (600 fields) and race (5000) state traces equal 0.8.6's; on the Mac
      `test_guest_space.py` (start, late, none: OK, twelve wild accesses ended in each of start
      and late) and the iOS tests OK. The Simulator build was stopped: the shared Mac's load passed
      50 (other sessions)

What the player's next dhd.log must show (the test IPA, built before the guard, writes the same
lines): `system: iOS 18.7.11, ... (iPhone11,8)`, then
`host64 memory: guest memory at 0x..., reserved at the start: [0, 0x3a000000) and [0x70000000,
0x...), room to grow to N MiB` and the game running. If it shows `host64 memory: no room for the
guest's memory ...` instead, its `host64 memory: free 0x...-0x...` lines are the phone's address
space, and the fold below is the next step.

## Decisions

- Two parts at one base, not a smaller single reservation: the span from 0 to the module's memory
  is fixed by the PS2 scratchpad address and the core's link, so no single range is shorter than
  1.76 GiB; the gap between the parts is left to others.
- Reserved at the process's start: by the game's start the launcher (WKWebView), UIKit and Metal
  have mapped into the low 2 GiB, which fills from the bottom; at the start only the binary, dyld
  and a little heap are there. A run that never plays loses address space only (about 1.2 GiB of
  the high part's growth room and the low part), no memory.
- The high part takes as much growth room as the free range allows (at most the 2 GiB maximum);
  the core starts at 0x70E00000 and rarely grows, so even 100 MiB of room is plenty.
- One code path: the two-part placement is the bounds-checked layout's only reservation (iOS
  device and Simulator); the 8 GiB layout of the other platforms is untouched.
- Not done here: folding the guest's high part down next to the views on iOS (a compare and a
  subtract on every guest access and in `GUEST()`). It would make the span about 1 GiB and fit
  anywhere; it is the next step if the player's log still shows "no room".
- What lies in the gap and above the high part is kept out of the guest's reach without a new
  test on the common path (2026-11-15, the review's must-fix). `memory_access.h` already decides
  per access, by one bit of a 64-bit mask indexed by the address's 32 MiB window, whether the plain
  address may be used; on iOS the gap's windows (29..55) and the high part's windows above its
  first (57..63) are set in that mask too, and the out-of-line path ends the process for a span
  outside both parts (`host64_memory_wild`: the fault reporter's own line, `host64 memory: access
  of uncommitted guest address 0x...`, which `run_record.c` reads, then SIGSEGV). `GUEST()` tests
  the address against the gap and the end (host code, a compare per conversion), the bridge tests
  a span against the gap. Window 56 (the scratchpad and the module's first 31 MiB) stays plain, so
  the high part must reach 0x72010000. The cost, not measured (no timing runs in this lane):
  accesses with a small offset and the plain accesses run the same test as before; the 62,574 of
  the core's 205,774 split access sites whose offset is the module's data (30 %, before free)
  compare their base with 16 MiB, one compare and branch shared by every access through that base;
  604 sites (0.3 %) with an offset of 64 KiB to 32 MiB test their own address; module memory grown
  above 0x72000000 (it starts at 0x70E00000) is reached out of line. Weighed and not taken: a
  compare and subtract on every access (the same protection at a cost on every access, on the
  phones that are slowest per thread); filling the free gap and tail with inaccessible mappings at
  the start (it takes about 860 MiB of the 2 GiB below the shared cache from the launcher, WebKit
  and Metal, and the gap holds the main thread's stack, which iOS places just below 0x170000000
  and which would stay reachable and writable); the fold above (the same cost class as the
  compare, and more change, while the player's log is pending).
- What stays reachable, as before this lane: a wild address of 2 GiB or more (0x80010000 and up,
  where 0.8.5's reservation ended too) whose window, taken mod 64, is a plain one, and a host span
  that starts inside a part and runs past its end (`GUEST()` checks where a span starts; the
  bridge checks the whole span against the gap). Acceptable because both were reachable in every
  iOS build since finding 224 and need a wild address the game has never produced; everything the
  two-part placement newly left to others (the gap, the high part's shortened tail) is covered.
- `host_error_box` shows on iOS now (an alert the game thread waits for), for every error box,
  not only this one: before, iOS players saw nothing.

## Surprises

- The guest's highest address in use is not far below 2 GiB (the brief's reading): the module's
  memory is at 0x70100000.
- `vm_region_64` lists no region for the page zero: the first free range seemed to start at
  16 KiB, and a base there can never be mapped. TASK_VM_INFO's `min_address` (rev 2) is the
  task's real bottom.
- Darwin's linker wants a weak reference (`host_error_box`, `host_log_drain`) resolved; the game
  defines both, the address-space test gives its own.
- macOS ignores RLIMIT_AS, so the Mac test fills the address space with inaccessible mappings
  (every free range of 16 MiB or more) instead, in a constructor linked before guest_memory.c's
  for the modes that must fail at the start.
- The Linux variant of `host64_selftest.py` (`--linux`, a manual tool no check runs) builds with
  `WASM_RT_MEMCHECK_BOUNDS_CHECK=1` and so gets the two-part layout: its lists of unchanged and
  reserved addresses include 0x3a000000, in the gap, and fail there now. Not changed here; finding
  262 repaired it (and the Windows variant, which takes the same checks by default with clang-cl
  and no longer linked).
- The Mac (8 GB) reached a load of about 750 with four booted simulators' `locationd` spinning and
  restarted itself once during the IPA build; the builds were redone after it.

## Result

- **The cause** (from XNU's source, not seen on the phone): iOS gives an app on a device with 3 GB
  or less an address space capped just above its shared cache, so no free range of 2 GiB +
  64 KiB (0.8.5's single reservation) can exist there; it is the cap, not fragmentation.
- **The fix**: on iOS (wasm2c's bounds-checked layout) the guest is reserved in two parts at one
  base found from the process's free ranges, `[0, 0x3a000000)` and `[0x70000000, end)`, at the
  process's start and again at the game's start if that failed (`guest_place.c`,
  `guest_memory.c`); the gap between them is left to others and kept out of the guest's reach
  (`memory_access.h`'s window mask, `GUEST()`, the bridge, `host64_memory_wild`). Windows, Linux,
  macOS and Android keep their 8 GiB layout, unchanged. A failure says in dhd.log what was asked
  and every free range of 16 MiB or more, the player gets an alert and the launcher a note; the
  `system:` line says iOS or iPadOS.
- **Proven**: `guest-place` and `guest-access` in `test_runtime.py` (`check.py port`);
  `test_guest_space.py` on the Mac (start, late, none, and twelve wild accesses that end by
  SIGSEGV); boot (600 fields) and race (5000) state traces on the PC equal to 0.8.6's; the iOS
  Simulator from the launcher to a race (`host64 memory: guest memory at 0x7000000000, reserved
  at the start: [0, 0x3a000000) and [0x70000000, 0x80010000), room to grow to 2048 MiB`). The
  0.8.7 release job proved it again on the release's tree: `test_guest_space.py` OK (3 tests),
  and the Simulator app went from the launcher (`bridge.call("play")`) through the menus to a race
  67 s after launch with the same reservation line, 59.8/60 fps, no `freeze:` or `crash:` line.
- **Unproven**: a real phone with 3 GB. The player's next dhd.log decides: the reservation line
  above means it works; `host64 memory: no room for the guest's memory ...` with its free-range
  lines means the fold (Decisions) is the next step.
- **Left**: the player's log; the fold of the high part down next to the views if that log still
  shows no room.
