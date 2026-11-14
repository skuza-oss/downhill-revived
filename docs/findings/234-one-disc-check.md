# 234: One boot-disc check

Status: in progress

Branch `cleanup/one-disc-check` from `main` 584ce03f, started 2026-11-14.

## Goal and acceptance

Use the callback-based C disc checker everywhere; preserve its whole-volume acceptance rule,
platform messages and Android import policy. No gameplay or copy-engine changes.
Gate: check.py quick, port and docs; launcher build/tests; Android native/Java build and tests;
iOS host disc/copy tests; boot trace equal to base. Device proof belongs to the lead after merge.

## Progress

- [x] Read the three parsers and callers; keep port/host/disc_check.c.
- [ ] Replace launcher and Java walks with reader adapters and share synthetic fixtures.
- [ ] Run local gates and document remaining platform proof.

## Decisions

C is already callback-based and used by game boot and iOS. Compile it into Tauri with cc;
Java passes a seekable FileChannel through JNI. Keep diagnostics in each platform adapter.
Research: cc build-script/FFI documentation, https://docs.rs/cc/latest/cc/ . No copied code.

## Surprises

None yet.

## Result

Pending verification and merge.
