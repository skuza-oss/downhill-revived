# 232: Keep non-gamepads out of controller slots

Status: in progress

Branch `fix/pad-slot-filter` from `main` 6d9d51d1, started 2026-11-14.

## Goal and acceptance

Filter devices before assigning player slots in the shared SDL host. Cover the reported
consumer HID, air mouse and real pads with device-description unit tests. Run quick and port
checks, build the Android native library, compare boot and race traces to the base, and check docs.
No phone use, timing benchmarks or changes to rebinding.

## Progress

- [x] Read the input path and controller findings; reserve the lane.
- [ ] Implement and test the filter; run the gates.

## Decisions

- Prefer known pad types or explicit mappings, otherwise require four face buttons and either
  a d-pad or left stick. Do not blacklist names or reject composite keyboard/gamepad devices.

## Surprises

- SDL's Android automatic mapping can describe a partial consumer device as a standard gamepad.

## Result

Pending validation; the lead closes this finding at merge.
