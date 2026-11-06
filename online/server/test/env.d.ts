declare module "cloudflare:workers" {
  interface ProvidedEnv extends Env {}
}

// The tests' own bindings (vitest.config.ts): the moderation store, which wrangler.jsonc gets only
// once the operator has made the database, and its migrations.
declare namespace Cloudflare {
  interface Env {
    MODERATION: D1Database;
    TEST_MIGRATIONS: import("cloudflare:test").D1Migration[];
  }
}
