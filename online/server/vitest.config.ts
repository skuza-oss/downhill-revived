import path from "node:path";
import { cloudflareTest, readD1Migrations } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig(async () => {
  // The moderation store (finding 161): wrangler.jsonc has no D1 binding until the operator makes
  // the database (README.md), so the tests add one and give each test file its tables
  // (test/apply-migrations.ts).
  const migrations = await readD1Migrations(path.join(__dirname, "migrations"));
  return {
    plugins: [
      cloudflareTest({
        wrangler: { configPath: "./wrangler.jsonc" },
        // The tests run with the version gate raised past 0.1.0 (wrangler.jsonc may still allow it):
        // the helpers' hello is release 0.2.0, and a hello without clientVersion (0.1.0) is refused.
        // The race room's countdown is short here (src/protocol.ts COUNTDOWN_MS). Rooms don't ask a
        // Cloudflare trace where they run (src/room.ts probeColo): tests set the colo themselves.
        // The members' caps count over 1 s windows here (src/guard.ts CAP_WINDOW_MS, 5 s), so a test
        // sees a second window soon. IP_HASH_KEY: a test key for the keyed address hash (a Worker
        // secret in production). GUARD_CACHE off: the kill switch and bans are read from the store
        // at every lookup (src/guard.ts keeps them 30 s and 60 s per isolate), so a test's change
        // bites at once; the cache's own tests pass an env without it.
        miniflare: {
          bindings: {
            MIN_VERSION: "0.2.0",
            COUNTDOWN_MS: "300",
            COLO_PROBE: "off",
            CAP_WINDOW_MS: "1000",
            IP_HASH_KEY: "test-ip-hash-key",
            GUARD_CACHE: "off",
            // Never the real Realtime or TURN API from a test, even with a filled .dev.vars:
            // tests that need them pass a fake fetch or stub it.
            REALTIME_APP_ID: "",
            REALTIME_APP_TOKEN: "",
            TURN_KEY_ID: "",
            TURN_KEY_API_TOKEN: "",
            TEST_MIGRATIONS: migrations,
          },
          d1Databases: ["MODERATION"],
        },
      }),
    ],
    test: { setupFiles: ["./test/apply-migrations.ts"] },
  };
});
