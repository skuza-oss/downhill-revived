// The moderation store's tables (migrations/), before each test file (vitest.config.ts).
import { applyD1Migrations } from "cloudflare:test";
import { env } from "cloudflare:workers";

await applyD1Migrations(env.MODERATION, env.TEST_MIGRATIONS);
