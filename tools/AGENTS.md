# tools/: the matching toolchain and repository checks

The root `AGENTS.md` rules apply.

- `configure.py`, `sncc.py`, `asm_sn.py`, `link.py`, `check_image.py` build the retail image from
  splat asm + C and compare it (finding 003). `try_func.py` (with `quick_check.py`, finding 007),
  `promote.py`, `queue.py`, `report.py` run the decomp workflow; `srclock.py` serialises writers of
  the shared build (finding 009). `setup_toolchain.py` fetches the pinned toolchain
  (`toolchain.lock.json`); `worktree_setup.py` prepares a git worktree.
- Never weaken a check: no new masks, tolerances or skipped sections in `check_image.py`,
  `try_func.py`, `promote.py` or `source_rules.py`. A tool that disagrees with retail is a bug in
  the tool or the source, not a reason to relax the comparison.
- Workers never edit this folder (`docs/WORKER.md`, "Forbidden").
- Repository checks live here too: `check.py` (the one entry point), `check_docs.py` (findings,
  citations, doc paths, machine paths), `findings_index.py` (the findings table),
  `check_tree.py` (the never-commit rules), `hooks/` (the tracked pre-commit hook).
- Output style for new checks: one line per problem, `ERROR <file>:<line> <reason>` or `WARN ...`,
  then one summary line; long logs go to files under `build/`.
- No machine paths in tools: find toolchains, scratch space and the test disc through the
  variables in `docs/TESTING.md`.
