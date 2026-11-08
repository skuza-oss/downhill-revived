# Contributing

Contributions are welcome: decompiling the last functions, naming and typing what is matched,
port fixes, widescreen split screens, testing on other PCs.

- Read `docs/STATE.md` and [AGENTS.md](../AGENTS.md) (the project's rules) first.
- To decompile a function, follow `docs/WORKER.md`. A function is accepted only when
  `tools/try_func.py` prints MATCH and `tools/promote.py` confirms the whole image still matches.
- Port changes go in `port/`; `src/` must keep matching.
- Never commit game data, secrets or server addresses other than the official server's. Install
  the pre-commit hook once (`python tools/hooks/install.py`): it checks these rules and the docs
  on every commit. `python tools/check.py docs` runs the same by hand.

How to build both the PC port and the matching build: [BUILDING.md](BUILDING.md).

## What "matching" means

"Matching" means that the C/C++ in `src/`, compiled with the game's original compiler (SN
Systems ProDG 3.01, gcc 2.95.3) and the original flags, gives exactly the bytes of the retail
executable. A function counts only when a tool confirms that its bytes and the whole rebuilt
executable are identical to retail; nobody's word is taken for it. Sony's SDK and the compiler's
own runtime are library code and are not decompiled. `python tools/report.py` refreshes the
README's progress table and `progress/report.json` (an [objdiff](https://github.com/encounter/objdiff) report, in
the format [decomp.dev](https://decomp.dev) reads).

## Repository layout

| Path | What |
|---|---|
| `src/` | The decompiled game and engine, in C/C++. Each file starts with the address range it replaces. |
| `include/` | Shared headers: structs and types of the game. |
| `config/` | splat configuration, symbol names, identified library functions. |
| `tools/` | The matching build and its checks (`configure.py`, `check_image.py`, `try_func.py`, `promote.py`, `report.py`). |
| `port/` | The PC port: build tools (`port/tools/`), PS2 system replacement (`port/hle/`), graphics (`port/gs/`, the Rust renderer in `port/render/`), CPU/VU runtime (`port/runtime/`), the 64-bit host (`port/host/`, `port/host64/`), the browser build (`port/web/`), network client (`port/net/`). |
| `online/` | The online race server (Cloudflare Workers + Durable Objects). |
| `docs/` | `PLAYING.md` (the players' guide), `CONTRIBUTING.md`, `CREDITS.md`, `STATE.md` (where each part stands), `findings/` (how things were worked out), `WORKER.md` (how to decompile a function), `BUILDING.md`, the online protocol. |
