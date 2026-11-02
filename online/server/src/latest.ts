// GET /v1/latest: a backup of the newest release's update manifest (latest.json, finding 138).
//
// The launcher asks GitHub's releases API first (port/launcher/src-tauri/src/update.rs). When a
// player's network can't reach api.github.com, or GitHub's unauthenticated limit (60 an hour per
// address) is used up, it asks this endpoint instead. The worker fetches latest.json from the newest
// release itself (GitHub's /releases/latest/download/ address, which redirects to the asset), keeps
// it fresh for FRESH_MS and keeps the last good copy, so a GitHub hiccup still gets an answer. Only
// version info passes through here: the download URLs stay GitHub's release asset URLs, checked
// below, and nothing big is served or stored. No release step: a published release is picked up
// within FRESH_MS.

export const UPSTREAM = "https://github.com/skuza-oss/downhill-revived/releases/latest/download/latest.json";
/** How long a fetched copy is served before GitHub is asked again. */
export const FRESH_MS = 5 * 60_000;
/** How long the edge cache keeps the last good copy (the answer while GitHub fails). */
export const KEEP_SECONDS = 30 * 24 * 3600;
const UPSTREAM_TIMEOUT_MS = 10_000;
const MAX_MANIFEST_BYTES = 64 * 1024;
const MAX_SIZE = 4 * 1024 ** 3;
const REPO_DOWNLOADS = "/skuza-oss/downhill-revived/releases/download/";
/** Hosts GitHub serves release assets from (github.com's download URL redirects there). */
const ASSET_HOSTS = ["objects.githubusercontent.com", "release-assets.githubusercontent.com"];

export interface Entry {
  url?: string;
  sha256?: string;
  size?: number;
}

/** latest.json as package_release.py writes it; only these fields are served. */
export interface Manifest extends Entry {
  name?: string;
  version: string;
  page?: string;
  platforms: Record<string, Entry>;
}

/** A copy and when it was fetched (ms since the Unix epoch). */
export interface Kept {
  manifest: Manifest;
  fetchedAt: number;
}

export interface Store {
  get(): Promise<Kept | null>;
  put(kept: Kept): Promise<void>;
}

export interface Deps {
  /** GitHub's latest.json: fetch(UPSTREAM) in the worker. */
  fetchUpstream: () => Promise<Response>;
  store: Store;
  now: () => number;
}

/** A release asset address the launcher may download: https, GitHub, this repository's releases. */
export function isReleaseAssetUrl(value: unknown): boolean {
  if (typeof value !== "string") return false;
  let u: URL;
  try {
    u = new URL(value);
  } catch {
    return false;
  }
  if (u.protocol !== "https:" || u.username || u.password || u.port) return false;
  if (u.hostname === "github.com") return u.pathname.startsWith(REPO_DOWNLOADS);
  return ASSET_HOSTS.includes(u.hostname);
}

function entryOf(v: unknown, where: string): Entry {
  if (typeof v !== "object" || v === null || Array.isArray(v)) throw new Error(`${where} is not an object`);
  const e = v as Record<string, unknown>;
  if (!isReleaseAssetUrl(e.url)) throw new Error(`${where}.url is not a GitHub release asset address`);
  if (typeof e.sha256 !== "string" || !/^[0-9a-fA-F]{64}$/.test(e.sha256)) throw new Error(`${where}.sha256 is not 64 hex digits`);
  const out: Entry = { url: e.url as string, sha256: e.sha256.toLowerCase() };
  if (e.size !== undefined) {
    if (!Number.isSafeInteger(e.size) || (e.size as number) < 0 || (e.size as number) > MAX_SIZE)
      throw new Error(`${where}.size is not a byte count`);
    out.size = e.size as number;
  }
  return out;
}

/** The manifest checked and reduced to the fields the launcher reads; throws what is wrong. */
export function checkManifest(v: unknown): Manifest {
  if (typeof v !== "object" || v === null || Array.isArray(v)) throw new Error("not a JSON object");
  const m = v as Record<string, unknown>;
  if (typeof m.version !== "string" || !/^v?\d{1,6}\.\d{1,6}\.\d{1,6}$/.test(m.version))
    throw new Error("version is not MAJOR.MINOR.PATCH");
  const p = m.platforms;
  if (typeof p !== "object" || p === null || Array.isArray(p) || Object.keys(p).length === 0)
    throw new Error("platforms is missing or empty");
  const platforms: Record<string, Entry> = {};
  for (const [key, entry] of Object.entries(p)) {
    if (!/^[a-z0-9-]{1,32}$/.test(key)) throw new Error(`platform name ${JSON.stringify(key).slice(0, 40)} is not one`);
    platforms[key] = entryOf(entry, `platforms.${key}`);
  }
  const out: Manifest = { version: m.version, platforms };
  if (typeof m.name === "string" && m.name.length <= 64) out.name = m.name;
  // the top level names the Windows zip (what 0.6.x launchers read), when there is one
  if (m.url !== undefined || m.sha256 !== undefined) Object.assign(out, entryOf(m, "the top level"));
  if (typeof m.page === "string" && m.page.startsWith("https://github.com/skuza-oss/downhill-revived/")) out.page = m.page;
  return out;
}

/** GitHub's latest.json, checked; throws what went wrong (said in the 502/503 answer). */
async function fetchManifest(deps: Deps): Promise<Manifest> {
  let res: Response;
  try {
    res = await deps.fetchUpstream();
  } catch (e) {
    throw new UpstreamError(`GitHub didn't answer (${e instanceof Error ? e.message : String(e)})`, 503);
  }
  if (!res.ok) {
    await res.body?.cancel();
    throw new UpstreamError(`GitHub answered HTTP ${res.status}`, 503);
  }
  // a release asset comes as application/octet-stream: read as bytes, then decoded
  const bytes = await res.arrayBuffer();
  if (bytes.byteLength > MAX_MANIFEST_BYTES) throw new UpstreamError("GitHub's latest.json is too big", 502);
  let parsed: unknown;
  try {
    parsed = JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(bytes));
  } catch {
    throw new UpstreamError("GitHub's latest.json is not JSON", 502);
  }
  try {
    return checkManifest(parsed);
  } catch (e) {
    throw new UpstreamError(`GitHub's latest.json was refused: ${(e as Error).message}`, 502);
  }
}

class UpstreamError extends Error {
  constructor(message: string, readonly status: 502 | 503) {
    super(message);
  }
}

const HEADERS = { "Access-Control-Allow-Origin": "*", "Cache-Control": "public, max-age=60" };

function answer(kept: Kept, stale: string | null): Response {
  const headers: Record<string, string> = { ...HEADERS, "X-DHD-Fetched-At": new Date(kept.fetchedAt).toISOString() };
  // stale: the reason GitHub's newer copy couldn't be had; the kept one is served instead
  if (stale) headers["X-DHD-Stale"] = stale.slice(0, 200);
  return Response.json(kept.manifest, { headers });
}

/** The answer to GET /v1/latest. Each request that finds the copy old asks GitHub itself (a promise
 * shared between requests would carry one request's I/O into another, which Workers refuse); launchers
 * ask once a start, so that is a few fetches every FRESH_MS per location at most. */
export async function latestResponse(deps: Deps): Promise<Response> {
  const kept = await deps.store.get().catch(() => null);
  if (kept && deps.now() - kept.fetchedAt < FRESH_MS) return answer(kept, null);
  try {
    const fresh = { manifest: await fetchManifest(deps), fetchedAt: deps.now() };
    await deps.store.put(fresh).catch(() => {});
    return answer(fresh, null);
  } catch (e) {
    const err = e instanceof UpstreamError ? e : new UpstreamError(String(e), 503);
    if (kept) return answer(kept, err.message);
    return Response.json(
      { error: "The newest release's info can't be had right now: GitHub gave no usable answer and no earlier copy is kept.", detail: err.message },
      { status: err.status, headers: { "Access-Control-Allow-Origin": "*", "Cache-Control": "no-store" } },
    );
  }
}

/** The worker's store: this isolate's copy, else the edge cache's (kept KEEP_SECONDS per location). */
let memory: Kept | null = null;

export function edgeStore(cacheKey: string): Store {
  return {
    async get() {
      if (memory) return memory;
      const hit = await caches.default.match(cacheKey);
      if (!hit) return null;
      const kept = (await hit.json()) as Kept;
      memory = { manifest: checkManifest(kept.manifest), fetchedAt: Number(kept.fetchedAt) || 0 };
      return memory;
    },
    async put(kept) {
      memory = kept;
      await caches.default.put(
        cacheKey,
        Response.json(kept, { headers: { "Cache-Control": `public, max-age=${KEEP_SECONDS}` } }),
      );
    },
  };
}

export function upstreamFetch(): Promise<Response> {
  return fetch(UPSTREAM, {
    redirect: "follow",
    signal: AbortSignal.timeout(UPSTREAM_TIMEOUT_MS),
    headers: { "User-Agent": "dhd-online-server", Accept: "application/octet-stream, application/json" },
  });
}
