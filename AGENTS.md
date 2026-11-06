# Downhill Revived: notes for contributors and coding agents

These rules apply to everyone who changes this repository, people and AI agents alike.

Read `docs/STATE.md` first: what is true now, per area. Then `docs/findings/README.md`: the
findings index with an area map. `docs/BUILDING.md` sets up a build, `docs/TESTING.md` lists the
test tools and their setup variables. Local-only notes (not in the repository, may be absent):
if `docs/private/STATE-ops.md` and `docs/LEAD.md` exist in your checkout, read them too. Keep run
logs, machine setup and account details there, never in tracked files.

## Rules

- A function counts as decompiled only when its compiled bytes match the retail
  executable exactly, as checked by the tools (`tools/try_func.py`, `tools/promote.py`,
  `tools/check_image.py`). Never weaken the check.
- Never commit game data: no ISO, executable, IRX modules, extracted assets, generated asm,
  captures, saves or BIOS files.
- Never commit a key or other secret, or a server address other than the official server
  (`wss://dhd.skuza.app`, the game's default). Any other server lives
  only in a player's `dhd.ini` or the `DHD_NET_SERVER` environment variable.
- Never commit a path of your own machine (drive letters, home folders, LAN addresses): tools
  find toolchains, scratch space and the test disc through the variables in `docs/TESTING.md`.
- Readable code is the goal: real names, structs and types once understood.
- Port changes go in `port/` and apply at build time; `src/` must keep matching.
- History on `main` is never rewritten. One clear commit per change; a release is its version
  commit (the last one), tagged `vX.Y.Z`.

The never-commit rules are also checked mechanically: `tools/check_tree.py` and
`tools/check_docs.py`, run by the pre-commit hook (install once per clone:
`python tools/hooks/install.py`) and by CI (`.github/workflows/checks.yml`).

## Done means

One entry point: `python tools/check.py <tier>` (one line per step, logs in `build/check/`).

| Change | Run before you hand it back |
|---|---|
| Docs, findings, AGENTS.md files | `check.py docs` |
| A matching function or anything in `src/`, `include/`, `config/`, `tools/` | `check.py matching` (and `try_func.py` printed MATCH) |
| Port code that compiles into the core or the host (`port/hle`, `port/runtime`, `port/gs`, patches) | `check.py quick`; `check.py port` when the host, renderer or network side changed |
| Anything that can change game state | the above, plus a boot and a race state trace against the base build (`docs/TESTING.md`) |
| The online server | `check.py server` |
| A release | the release checklist and `package_release.py`'s own checks |

The quick tier is compile plus unit tests: game runs, picture checks and benchmarks only when a
change needs them or someone asks.

## Current decisions (they override anything older)

| Decision | Since | Finding | Do not |
|---|---|---|---|
| Online races are state sync: each game runs its own one-player race, the others are puppet riders moved from snapshots (30 a second); hits are events; each game times its own finish | 2026-10-16 | 044 | add input lockstep, input delay, state hashes or tick agreement between PCs; peers share neither frame rate nor floats |
| Race frames travel through Cloudflare only (Realtime SFU, TURN over TLS on 443 as the fallback); the room sets them up; no player ever receives another's address | 2026-11-06 | 158 | add peer-to-peer links or a TURN mesh, or send a player another's address, SDP, TURN credentials, address hash or install id |
| One delta-time game clock at every frame rate, 60 included and online; deterministic runs keep the original clock | 2026-11-02 | 139 | add a fixed 1/60 step, a frame cap with repaid debt, or extra online fields |
| Our own GS renderer (Rust, wgpu) with an exact and a native front end; PCSX2 is the referee (replay the same GS stream), not the renderer | 2026-10-14 | 085 | swap in PCSX2's GS or blame the renderer before replaying the stream (025) |
| One code path for every platform, no legacy kept beside a new path; old code is deleted when replaced | 2026-10-27 | 099 | keep a second implementation "as a fallback" (the 32-bit build, D3D11, XInput, the game's updater are gone, 088) |
| Keep the original game's feel: no gameplay additions; QoL and online features are welcome | 2026-10-31 | | add custom music, taunts, assists or handling changes |
| No online play on Android or in the browser yet: the host does not offer it | 2026-11-03 | 143 | enable ONLINE there without a decision |

A lane's plan is its finding: copy `docs/findings/TEMPLATE.md` on the lane's first commit
(`Status: in progress`, which reserves the number) and keep its Progress, Decisions and Surprises
current; at the merge it becomes `current`. Findings carry a `Status:` line; only `current` findings and the still-true parts of `partly
superseded` ones describe the code. Older findings that mention lockstep (039-042, 049) describe
the dropped design.

## Where the area rules live

Each area has its own short AGENTS.md. Claude Code loads it when it reads a file in that folder;
Codex loads only the files from the repository root down to the folder it was started in. So
before you change files in an area, read that area's AGENTS.md yourself.

| You work on | Read |
|---|---|
| Matching decompilation (`src/`, `include/`, `config/`, `asm/`) | `src/AGENTS.md`, `docs/WORKER.md` |
| The matching tools (`tools/`) | `tools/AGENTS.md` |
| The port: HLE, runtime, host, GS front end, build scripts (`port/`) | `port/AGENTS.md` |
| The Rust library: renderer, settings, net, disc, presence (`port/render/`) | `port/render/AGENTS.md` |
| The Android app and APK (`port/android/`) | `port/android/AGENTS.md` |
| The online server (`online/server/`) | `online/server/AGENTS.md`, `docs/online-protocol.md` |
| A new finding, a lane, a release, decomp workers | new finding or lane plan: `docs/findings/TEMPLATE.md`; decomp workers: `docs/WORKER.md`; release: `docs/BUILDING.md` "Releases" and `docs/RELEASE-CHECKLIST.md` |

## Research

Search online before inventing a solution. Most PS2 problems are already solved somewhere:
- PCSX2 (source and wiki) for how the hardware behaves: GS, VU0/VU1, VIF, DMA, SPU2, IPU,
  timers.
- ps2sdk, ps2tek and Sony SDK documentation for libraries and protocols.
- Other PS2 decompilation, static recompilation and porting projects (ps2recomp, Play!,
  other decomps) for tools and approaches.
- vgmstream and similar projects for game file formats.

Note in the relevant `docs/findings/` file what you used. Code may be adapted only when
its licence allows it (it must be compatible with GPL-3.0-or-later); keep its copyright notice
and say where it came from.
