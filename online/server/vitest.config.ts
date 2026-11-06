import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      // The tests run with the version gate raised past 0.1.0 (wrangler.jsonc may still allow it):
      // the helpers' hello is release 0.2.0, and a hello without clientVersion (0.1.0) is refused.
      // The race room's countdown is short here (src/protocol.ts COUNTDOWN_MS). Rooms don't ask a
      // Cloudflare trace where they run (src/room.ts probeColo): tests set the colo themselves.
      // The members' caps count over 1 s windows here (src/guard.ts CAP_WINDOW_MS, 5 s), so a test
      // sees a second window soon. IP_HASH_KEY: a test key for the keyed address hash (a Worker secret in production).
      miniflare: { bindings: { MIN_VERSION: "0.2.0", COUNTDOWN_MS: "300", COLO_PROBE: "off", CAP_WINDOW_MS: "1000", IP_HASH_KEY: "test-ip-hash-key" } },
    }),
  ],
});
