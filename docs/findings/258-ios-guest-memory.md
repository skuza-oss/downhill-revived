# 258: the game starts on iPhones with 3 GB of memory (guest memory in two pieces)

Status: in progress

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
like a small iPhone's so the old reservation fails and the new one starts, the iOS simulator build
to the menu. Windows, Linux, macOS and Android keep their 8 GiB layout; boot and race state traces
on the PC equal the base. Out of scope: the extended-virtual-addressing entitlement (needs a paid
account's provisioning profile).

## Progress

- [ ] 2026-11-15: the cause, from XNU's source
- [ ] the bounds-checked layout reserved in two pieces at a base found from the free ranges
- [ ] the failure's log line and message; the `system:` line
- [ ] tests: placement arithmetic, the Mac address-space test, the simulator
- [ ] the unsigned test IPA for the player

## Decisions

## Surprises

## Result
