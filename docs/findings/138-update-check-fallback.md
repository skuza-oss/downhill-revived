# 138: the update check falls back to our server, and its errors say what went wrong

Status: current
Checked: 2026-11-04

Why: until now the launcher checked for updates only through GitHub's releases API
(`https://api.github.com/repos/skuza-oss/downhill-revived/releases/latest`, `update.rs`
`OFFICIAL_SOURCE`). On 2026-11-02 the owner's own PC couldn't reach api.github.com for an hour
(connections timed out) while GitHub itself was up. Without a login, GitHub's API also allows only
60 requests an hour per IP address, so a shared or carrier-grade NAT address can use that up. In
both cases the launcher only said "Couldn't check for updates. You can still play 0.7.6.", and the
player stayed on a version that online play refuses (`MIN_VERSION`).

Branch `fix/update-check-fallback` (from main at 0.7.7 + finding 137's server fix).

## 1. The backup: `GET /v1/latest` on the online server

`online/server/src/latest.ts`, routed in `src/index.ts`. It answers with the newest release's
`latest.json`, the manifest the launcher already reads (`read_manifest`): `version`, the top-level
Windows `url`/`sha256`/`size` (what 0.6.x launchers read), `page` and `platforms`
(`{url, sha256, size}` for each platform). See `docs/online-protocol.md`, "Latest release".

- **Source.** The worker fetches
  `https://github.com/skuza-oss/downhill-revived/releases/latest/download/latest.json` itself and
  follows the redirects to GitHub's asset host. That is a plain download from github.com, not the
  API, so the API's 60-an-hour limit doesn't apply, and Cloudflare reaches GitHub fine. It has a
  10 s timeout, the body is read as bytes (GitHub serves the asset as `application/octet-stream`),
  and anything over 64 KB is refused: at once when its `Content-Length` says so, else read as a
  stream that is cancelled as soon as it passes 64 KB (`readCapped`; until 2026-11-03 the whole
  body was buffered first, review Muse #9).
- **Caching.** A copy is served for 5 minutes (`FRESH_MS`) before GitHub is asked again. The last
  good copy is kept in two places: in the isolate's memory, and in the edge cache of each Cloudflare
  location (`caches.default`, key `/v1/latest/kept` on the server's own host, `max-age` 30 days).
  When GitHub fails or gives a manifest that is refused, the kept copy is served with an
  `X-DHD-Stale: <why>` header. `X-DHD-Fetched-At` says when the copy was fetched. Responses carry
  `Cache-Control: public, max-age=60`.
- **No copy and no GitHub.** `503` (GitHub unreachable or an HTTP error) or `502` (its
  `latest.json` refused), with a JSON body `{"error": "The newest release's info can't be had right
  now: ...", "detail": "<what GitHub did>"}`.
- **Validation.** A manifest is served only when `version` is `MAJOR.MINOR.PATCH` (a leading `v`
  is allowed) and `platforms` names at least one platform (keys `[a-z0-9-]`, up to 32 characters).
  Every `url`, the top level's included, must be `https://` with no user, password or port, on
  `github.com` under `/skuza-oss/downhill-revived/releases/download/` (after URL normalisation, so
  `..` can't leave it), as `package_release.py` writes them. GitHub's release-asset hosts
  (`objects.githubusercontent.com`, `release-assets.githubusercontent.com`) were accepted until
  2026-11-03, but they serve every repository's assets, and the launcher refused them anyway
  (review Opus #14).
  Every `sha256` must be 64 hex digits (served in lower case), and `size`, when given, a byte
  count. The answer keeps only those fields plus `name`, and `page` only when it is on this
  repository (`isRepositoryPage`: the repository's address, then no dot segment, plain or `%2e`,
  and no backslash; until the second review round, 2026-11-03, a page starting with the address
  passed, so `.../downhill-revived/../../someone/else` went through and a browser opened another
  repository). Anything else is dropped.
- Only version info passes through the server. The download URLs are GitHub's release asset URLs,
  and nothing big is served or stored. **No release step is needed:** a published release reaches
  every location within 5 minutes.

Choices made here:
- The edge cache, not a KV namespace or a new Durable Object. It needs no new binding or migration,
  and with launchers checking on every start, the busy locations always hold a copy. A location
  nobody asked in 30 days has no copy until GitHub answers it once.
- No sharing of one GitHub fetch between requests: Workers don't let one request wait on another
  request's I/O. Launchers ask once per start, so at most a few fetches reach GitHub every 5 minutes
  per location.
- No release notes. Getting them would mean the API, with its limit shared across Cloudflare's
  addresses.

Tests (`test/latest.test.ts`, GitHub's fetch mocked): a good answer (and fresh/old copies, a new
release picked up), GitHub down with a kept copy (timeout, 429, 502), GitHub down with none (503
JSON), a bad manifest refused (not JSON, bad version, no platforms, http, another host, another
repository, short sha256, bad top-level url, an asset-host url: 502 with no copy, the kept copy
served with one), the address rule (asset hosts and `..` refused), the size cap (a 500 MB
`Content-Length` refused before the body is read; an endless body without one given up just past
64 KB; exactly 64 KB passes the size check), and the route through the worker (GET, 405 for POST).

## 2. The launcher: fallback, plain messages, launcher.log

`port/launcher/src-tauri/src/update.rs` `check()`:

1. dhd.ini's `update_url` (or `DHD_UPDATE_URL`), when the player set one: only that source, with
   no backup, as before.
2. Else GitHub's API (`OFFICIAL_SOURCE`). If it answers, its answer is used, and a release with no
   build for this platform is still "no update", not a failure.
3. Any failure (connect or timeout, DNS, HTTP 403/429 limit, 5xx, an answer that isn't JSON or
   names no usable release): `BACKUP_SOURCE` = `https://dhd.skuza.app/v1/latest`. Its answer is
   read with the same `read_manifest`. The launcher also refuses a backup answer whose download is
   not under `https://github.com/skuza-oss/downhill-revived/releases/download/`, as a second check
   after the server's. The download's SHA-256 is checked as always. Since 2026-11-03 (review Opus
   #14) the backup's `page`, which the Patch notes link opens in the browser, is kept only when it
   is under `https://github.com/skuza-oss/downhill-revived/`; anything else becomes the releases
   page. Neither address may hold a dot segment (`..`, also as `%2e`) or a backslash, so neither
   can climb out of the repository (`from_backup`, `under`).

A backup answer has no notes and no publish date. The home page then shows "Ready to ride"
instead of "New in X" (as before when notes were empty), and the Patch notes page says "The patch
notes for X couldn't be loaded here. Read them on GitHub." with the link to the release page (the
manifest's `page`).

The window's check and the `--play` check (`play_at_once`, the auto-update after the game) both go
through `check_for_update` in `main.rs`, so both fall back.

Worst case before the bar gives up: both servers time out, at up to 6 s to connect plus 20 s
reading each (the agent's existing timeouts). Usually it is one timeout (6 s) and then the backup's
quick answer.

### The messages (bottom bar, one line)

Errors are sorted into kinds (`Failure`) by pure functions: `status_failure` (HTTP status and
GitHub's rate-limit headers: 429, or 403 with `x-ratelimit-remaining: 0` or `retry-after`) and
`transport_failure` (ureq's error kind, the `io::ErrorKind` under it and its text). The kinds are
offline (DNS, network unreachable or down), timed out, refused, cut (reset or closed), TLS
(certificate, handshake: often a proxy or filter), rate limited, HTTP n, bad answer, and other.

Update check, both GitHub and the backup failed:
- both offline: "No internet connection. You can still play 0.7.6."
- GitHub rate limited and the backup answered (an HTTP error, a bad answer, refused, cut...):
  "GitHub is limiting update checks from your connection; try again in an hour. You can still play
  0.7.6."
- otherwise: "Couldn't reach GitHub or the backup server (REASON). You can still play 0.7.6.". The
  REASON is taken from whichever failure says more about the connection (offline, timeout, TLS,
  refused, cut before a server's error): "your connection timed out", "your network couldn't find
  them", "a secure connection couldn't be made", "the connection was refused", "the connection was
  cut", "a server error, HTTP 503", "the request was refused, HTTP 404", "the answer wasn't usable",
  "a network error".

The order is what the player can act on (2026-11-03, review Muse #8). Before, GitHub's rate limit
won over everything, so GitHub "limiting" plus a backup that was offline or timed out told the
player to wait an hour, without "You can still play", when they had no connection. A 403 with
`x-ratelimit-remaining: 0` can come from a captive portal or filter too. Now a backup that is
offline or timed out gives the "Couldn't reach" line ("your network couldn't find them", "your
connection timed out"), and the limit line, which now ends with "You can still play" like the
others, is for a backup that did answer.

The player's own source: "No internet connection. ..." / "Your update source is limiting checks;
try again later. ..." / "Couldn't reach your update source (REASON). You can still play 0.7.6."

Update download (the Try again button stays):
- "Update failed: the download couldn't reach GitHub (your connection timed out)." (the same
  reasons; "the download server" when the zip isn't on GitHub)
- "Update failed: GitHub is limiting downloads from your connection; try again later."
- "Update failed: the download arrived damaged; try again." (SHA-256 mismatch)
- Other install errors keep their plain text after "Update failed: " (a folder that can't be
  written, a file that couldn't be replaced).

Before this change the download error was ureq's own text, such as "download failed:
https://...: Connection Failed: Connect error: ...".

### launcher.log

There was no launcher.log yet, though `KEEP` and the uninstaller already kept the name.
`src/log.rs` appends lines to `launcher.log` in the data folder: UTC time, launcher version, text.
Past 256 KB the oldest half is dropped. It gets:
- a check that failed: GitHub's and the backup's failure kind and full error (or the own source's);
- a check the backup answered: GitHub's error and the backup's version;
- a failed update: the release and the full error (in the window and after `--play`).

Tests (`cargo test`, a local HTTP server as the existing tests use): falls back on 403 with the
limit used up, 429, 503, a non-JSON answer and a refused connection, and never when GitHub answers;
the own source has no backup; both failing gives the expected line and both errors in the detail;
a backup naming a download elsewhere is refused; the backup's page kept only under the repository
and its download refused with `..`, `%2E%2E`, a backslash or an asset host
(`backup_page_and_download`); the classification table; every message; a
download that can't connect says so in words; the log's date and trimming.

Not done here: no launcher window or game was run (by the brief), and the endpoint isn't deployed
yet (the lead deploys the server after review). Until it is, the fallback gets a 404 from the live
server, and the message says why GitHub failed as above.
