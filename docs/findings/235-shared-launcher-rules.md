# 235: Shared launcher rules

Status: in progress

Branch `cleanup/shared-launcher-rules` from `main` 584ce03f, started 2026-11-14.

## Goal and acceptance

Share launcher links, phone update fetch and phone scale cap. No gameplay changes.
Run check.py quick, port, docs; desktop launcher build/tests; Android native/Java build
and unit tests; PC iOS host tests. Device and Simulator proof belongs to the lead.
Disc checks and deployment floors are out of scope.

## Progress

- A06: shared native launcher_link.h, desktop C ABI and Android JNI adapters; C fixtures.
- A04 and A07 pending.

## Decisions

The page uses discord/kofi aliases (ui/app.js:975), repository home (:648), release
pages (:810, :980), and disc help (:244; disc.rs:163). All remain accepted.
Keep the four prefix contracts; combine iOS character rejection with Android's
repository traversal/control rejection, applied consistently to all project links.
Desktop now refuses quotes, spaces, backslashes, controls and traversal; iOS also
refuses controls and traversal; Android rejects malformed repository links instead
of substituting releases/latest, and filters other links identically. No normal UI
link changes. The update manifest URL rule remains separate.
Research: https://url.spec.whatwg.org/ (dot segments include percent-encoded dots).
No external code copied.

## Surprises

None so far.

## Result

Pending gates and lead device proof.
