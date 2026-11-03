// GET /v1/latest: the launcher's backup of the newest release's latest.json (src/latest.ts, finding 138).
import { exports } from "cloudflare:workers";
import { afterEach, describe, expect, it, vi } from "vitest";
import { FRESH_MS, UPSTREAM, checkManifest, isReleaseAssetUrl, isRepositoryPage, latestResponse, type Kept, type Store } from "../src/latest";

const DL = "https://github.com/skuza-oss/downhill-revived/releases/download/v0.7.7/";
const sha = (c: string) => c.repeat(64);

/** latest.json as package_release.py writes it for 0.7.7. */
function manifest(version = "0.7.7"): Record<string, unknown> {
  const win = { url: `${DL}DownhillRevived-${version}-win.zip`, sha256: sha("a"), size: 30_000_000 };
  return {
    name: "Downhill Revived",
    version,
    ...win,
    page: `https://github.com/skuza-oss/downhill-revived/releases/tag/v${version}`,
    platforms: {
      "linux": { url: `${DL}DownhillRevived-${version}-linux-experimental.zip`, sha256: sha("c"), size: 3 },
      "linux-appimage": { url: `${DL}DownhillRevived-${version}-linux-experimental.AppImage`, sha256: sha("d"), size: 4 },
      "mac": { url: `${DL}DownhillRevived-${version}-mac-experimental.zip`, sha256: sha("b"), size: 2 },
      "win": win,
    },
  };
}

function memoryStore(start: Kept | null = null): Store & { kept: Kept | null } {
  return {
    kept: start,
    async get() { return this.kept; },
    async put(k) { this.kept = k; },
  };
}

const T0 = 1_800_000_000_000;

describe("GET /v1/latest", () => {
  it("serves GitHub's latest.json, checked, and keeps it", async () => {
    const store = memoryStore();
    const fetchUpstream = vi.fn(async () => new Response(JSON.stringify(manifest()), { headers: { "Content-Type": "application/octet-stream" } }));
    const res = await latestResponse({ fetchUpstream, store, now: () => T0 });
    expect(res.status).toBe(200);
    expect(res.headers.get("X-DHD-Stale")).toBeNull();
    const body = (await res.json()) as Record<string, any>;
    expect(body.version).toBe("0.7.7");
    expect(body.url).toBe(`${DL}DownhillRevived-0.7.7-win.zip`);
    expect(body.platforms.mac.sha256).toBe(sha("b"));
    expect(Object.keys(body.platforms).sort()).toEqual(["linux", "linux-appimage", "mac", "win"]);
    expect(store.kept?.fetchedAt).toBe(T0);

    // fresh: answered from the copy, GitHub not asked again
    const again = await latestResponse({ fetchUpstream, store, now: () => T0 + FRESH_MS - 1 });
    expect(((await again.json()) as any).version).toBe("0.7.7");
    expect(fetchUpstream).toHaveBeenCalledTimes(1);

    // old: GitHub asked again, a new release picked up
    fetchUpstream.mockImplementation(async () => new Response(JSON.stringify(manifest("0.7.8"))));
    const later = await latestResponse({ fetchUpstream, store, now: () => T0 + FRESH_MS + 1 });
    expect(((await later.json()) as any).version).toBe("0.7.8");
    expect(fetchUpstream).toHaveBeenCalledTimes(2);
  });

  it("answers with the kept copy while GitHub is down", async () => {
    const store = memoryStore({ manifest: checkManifest(manifest()), fetchedAt: T0 });
    for (const down of [
      async () => { throw new Error("The operation was aborted due to timeout"); },
      async () => new Response("rate limited", { status: 429 }),
      async () => new Response("bad gateway", { status: 502 }),
    ]) {
      const res = await latestResponse({ fetchUpstream: down, store, now: () => T0 + 3600_000 });
      expect(res.status).toBe(200);
      expect(res.headers.get("X-DHD-Stale")).toMatch(/GitHub/);
      expect(((await res.json()) as any).version).toBe("0.7.7");
    }
    expect(store.kept?.fetchedAt).toBe(T0);
  });

  it("says clearly when GitHub is down and nothing is kept", async () => {
    const res = await latestResponse({
      fetchUpstream: async () => { throw new TypeError("Network connection lost."); },
      store: memoryStore(),
      now: () => T0,
    });
    expect(res.status).toBe(503);
    expect(res.headers.get("Content-Type")).toMatch(/application\/json/);
    const body = (await res.json()) as Record<string, string>;
    expect(body.error).toMatch(/can't be had right now/);
    expect(body.detail).toMatch(/Network connection lost/);
  });

  it("refuses a bad manifest, and keeps serving the last good one", async () => {
    const bad: [string, Record<string, unknown> | string][] = [
      ["not JSON", "<html>"],
      ["bad version", { ...manifest(), version: "0.7" }],
      ["no platforms", { ...manifest(), platforms: {} }],
      ["http url", { ...manifest(), platforms: { win: { url: `http://github.com/skuza-oss/downhill-revived/releases/download/v1/x.zip`, sha256: sha("a") } } }],
      ["other host", { ...manifest(), platforms: { win: { url: "https://example.com/x.zip", sha256: sha("a") } } }],
      ["other repo", { ...manifest(), platforms: { win: { url: "https://github.com/someone/else/releases/download/v1/x.zip", sha256: sha("a") } } }],
      ["short sha", { ...manifest(), platforms: { win: { url: `${DL}x.zip`, sha256: "abc" } } }],
      ["bad top-level url", { ...manifest(), url: "https://example.com/x.zip" }],
      ["asset host", { ...manifest(), platforms: { win: { url: "https://objects.githubusercontent.com/github-production-release-asset/1/2", sha256: sha("a") } } }],
    ];
    for (const [what, body] of bad) {
      const fetchUpstream = async () => new Response(typeof body === "string" ? body : JSON.stringify(body));
      const res = await latestResponse({ fetchUpstream, store: memoryStore(), now: () => T0 });
      expect(res.status, what).toBe(502);
      expect(((await res.json()) as any).detail, what).toMatch(/latest\.json/);

      const kept = memoryStore({ manifest: checkManifest(manifest()), fetchedAt: T0 - FRESH_MS * 2 });
      const stale = await latestResponse({ fetchUpstream, store: kept, now: () => T0 });
      expect(stale.status, what).toBe(200);
      expect(stale.headers.get("X-DHD-Stale"), what).toMatch(/latest\.json/);
      expect(kept.kept?.fetchedAt, what).toBe(T0 - FRESH_MS * 2);
    }
  });

  it("refuses a body over 64 KB without reading it all", async () => {
    let pulled = 0;
    const endless = () => new ReadableStream<Uint8Array>({
      pull(c) { pulled += 4096; c.enqueue(new Uint8Array(4096).fill(0x20)); },
    });
    // Content-Length says too much: refused before any byte is read
    const declared = await latestResponse({
      fetchUpstream: async () => new Response(endless(), { headers: { "Content-Length": String(500 * 1024 * 1024) } }),
      store: memoryStore(),
      now: () => T0,
    });
    expect(declared.status).toBe(502);
    expect(((await declared.json()) as any).detail).toMatch(/too big/);
    expect(pulled).toBeLessThanOrEqual(4096 * 2);  // what the stream may queue on its own
    // no Content-Length: read as a stream, given up just past the limit
    pulled = 0;
    const streamed = await latestResponse({ fetchUpstream: async () => new Response(endless()), store: memoryStore(), now: () => T0 });
    expect(streamed.status).toBe(502);
    expect(((await streamed.json()) as any).detail).toMatch(/too big/);
    expect(pulled).toBeGreaterThan(64 * 1024);
    expect(pulled).toBeLessThan(64 * 1024 + 4096 * 4);
    // exactly at the limit still passes the size check (then fails as JSON here: all spaces)
    const full = await latestResponse({
      fetchUpstream: async () => new Response(new Uint8Array(64 * 1024).fill(0x20)), store: memoryStore(), now: () => T0,
    });
    expect(((await full.json()) as any).detail).toMatch(/not JSON/);
  });

  it("takes only this repository's release download addresses on github.com", () => {
    expect(isReleaseAssetUrl(`${DL}DownhillRevived-0.7.7-win.zip`)).toBe(true);
    // GitHub's asset hosts serve every repository's assets: never named in a manifest
    expect(isReleaseAssetUrl("https://objects.githubusercontent.com/github-production-release-asset/1/2")).toBe(false);
    expect(isReleaseAssetUrl("https://release-assets.githubusercontent.com/github-production-release-asset/1/2")).toBe(false);
    expect(isReleaseAssetUrl("https://github.com/skuza-oss/downhill-revived/releases/download/../../../someone/else/x")).toBe(false);
    expect(isReleaseAssetUrl("https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.7")).toBe(false);
    expect(isReleaseAssetUrl("https://github.com.evil.example/skuza-oss/downhill-revived/releases/download/x")).toBe(false);
    expect(isReleaseAssetUrl("https://user@github.com/skuza-oss/downhill-revived/releases/download/x")).toBe(false);
    expect(isReleaseAssetUrl("https://github.com:8443/skuza-oss/downhill-revived/releases/download/x")).toBe(false);
    expect(isReleaseAssetUrl("DownhillRevived-0.7.7-win.zip")).toBe(false);
  });

  it("serves the Android APK's entry like the others (finding 143)", () => {
    const m = manifest();
    const android = { url: `${DL}DownhillRevived-0.7.7-android-experimental.apk`, sha256: sha("e"), size: 5 };
    (m.platforms as Record<string, unknown>).android = android;
    const checked = checkManifest(m);
    expect(checked.platforms.android).toEqual(android);
    expect(Object.keys(checked.platforms).sort()).toEqual(["android", "linux", "linux-appimage", "mac", "win"]);
    expect(checked.page).toBe("https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.7");
  });

  it("drops fields the launcher doesn't read", () => {
    const m = checkManifest({ ...manifest(), extra: "x", page: "https://example.com/" });
    expect(m).not.toHaveProperty("extra");
    expect(m).not.toHaveProperty("page");
  });

  it("drops a page that leaves the repository through dot segments or backslashes", () => {
    const repo = "https://github.com/skuza-oss/downhill-revived/";
    for (const page of [
      `${repo}../../attacker/fake/releases/latest`,
      `${repo}%2e%2e/%2E%2E/attacker/fake`,
      `${repo}.%2e/attacker`,
      `${repo}releases/..\\..\\attacker`,
      `${repo}releases\\tag`,
    ]) {
      expect(isRepositoryPage(page)).toBe(false);
      expect(checkManifest({ ...manifest(), page })).not.toHaveProperty("page");
    }
    // where that page would have gone
    expect(new URL(`${repo}../../attacker/fake/releases/latest`).href).toBe("https://github.com/attacker/fake/releases/latest");
    expect(isRepositoryPage(`${repo}releases/tag/v0.7.8`)).toBe(true);
    expect(isRepositoryPage(`${repo}releases/latest`)).toBe(true);
    expect(isRepositoryPage("https://github.com/skuza-oss/downhill-revived-fork/")).toBe(false);
    expect(isRepositoryPage(42)).toBe(false);
  });

  describe("through the worker", () => {
    afterEach(() => vi.restoreAllMocks());

    it("routes /v1/latest, fetching GitHub's latest.json", async () => {
      const real = globalThis.fetch;
      const spy = vi.spyOn(globalThis, "fetch").mockImplementation(async (input, init) => {
        const url = input instanceof Request ? input.url : String(input);
        if (url === UPSTREAM) return new Response(JSON.stringify(manifest()));
        return real(input, init);
      });
      const res = await exports.default.fetch("https://dhd.test/v1/latest");
      expect(res.status).toBe(200);
      expect(((await res.json()) as any).platforms.win.sha256).toBe(sha("a"));
      expect(spy).toHaveBeenCalled();
      const post = await exports.default.fetch("https://dhd.test/v1/latest", { method: "POST" });
      expect(post.status).toBe(405);
    });
  });
});
