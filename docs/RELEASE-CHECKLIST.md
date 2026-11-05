# Doc gardening before each release

One agent runs this before the version commit (the `release` skill, step 2) and commits the fixes
in one docs commit. It keeps the docs an agent reads first true to the code. About an hour.

1. **Lint.** `python tools/check.py docs` (the docs linter and the never-commit check): no ERROR.
   Fix the warnings that are cheap (a code comment naming a gitignored note, a missing Status
   line of a merged lane).
2. **The release's findings.** List what the release touched:
   `git log --oneline v<last>..main` and the findings those commits name. For each one that is
   `current`: open the files and functions it names and confirm they exist and its claims still
   hold; fix the lines that don't, then set `Checked:` to today.
3. **What the release replaced.** A finding whose design this release replaced gets
   `superseded by NNN` (or `partly superseded by NNN (<part>)` and its stale lines edited); the
   new one gets `Supersedes:`. Regenerate the index: `python tools/findings_index.py`.
4. **STATE.md.** Read `docs/STATE.md` against the code, area by area; it names files instead of
   copying values. Move anything dated to `docs/STATE-LOG.md`.
5. **AGENTS.md files.** The current decisions still hold (the linter checks their findings are
   current; check the "Do not" column by reading). Each area file's commands still run as
   written. Optionally `/doctor prompt-audit` in Claude Code on the instruction files and skills.
6. **Players' text.** `port/release/README.txt`, `port/release/ANDROID-NOTES.md` and
   `docs/PLAYING.md` describe this release's settings and platforms.
7. **Crash symbols kept** (finding 152). After packaging, `build/release/` has the Windows link
   map (`-win.map`) and, with the APK, `-android-libmain.debug.so` (package_release.py prints
   "symbols ..." with its build ID; a "note: no libmain.debug.so" means the APK's crash frames
   can't be named: rebuild with `package_release.py --platform android` rather than `--apk`).
   Keep both with the release's files, never publish them: publish what `SHA256SUMS` lists
   (`docs/BUILDING.md`, "Release steps").
8. **After a major model upgrade:** delete rules that only worked around an older model's habits.
