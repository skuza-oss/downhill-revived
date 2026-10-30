import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      // The tests run with the version gate raised past 0.1.0 (wrangler.jsonc may still allow it):
      // the helpers' hello is release 0.2.0, and a hello without clientVersion (0.1.0) is refused.
      // The race room's countdown is short here (src/protocol.ts COUNTDOWN_MS), and so is a silent
      // racer's time (RACE_SILENT_MS) and a held slot's reconnect window (reconnect, finding 125).
      miniflare: { bindings: { MIN_VERSION: "0.2.0", COUNTDOWN_MS: "300", RACE_SILENT_MS: "1000", RECONNECT_WINDOW_MS: "300" } },
    }),
  ],
});
