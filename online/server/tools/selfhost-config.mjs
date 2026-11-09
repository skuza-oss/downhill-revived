#!/usr/bin/env node
// wrangler.selfhost.jsonc: wrangler.jsonc plus P2P rooms (finding 180), for a self-hosted server and
// a local test server (port/tools/local_server.py, npm run dev). The official server runs P2P rooms
// as a Worker of their own (wrangler.p2p.jsonc), so wrangler.jsonc has neither the P2P binding nor
// the migration that creates its class: tools/deploy.mjs uploads versions (`wrangler versions
// upload`), and Cloudflare refuses a version that creates a Durable Object class. A server made
// from this config gets that class with `npx wrangler deploy --config wrangler.selfhost.jsonc`.
//
//   node tools/selfhost-config.mjs     writes wrangler.selfhost.jsonc (gitignored) when it changed
import { existsSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { stripJsonc } from "./moderate.mjs";

const SERVER = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
export const SELFHOST_CONFIG = "wrangler.selfhost.jsonc";
/** The migration that makes the P2P room class in a main Worker (src/p2p.ts). */
const P2P_MIGRATION = { tag: "v2-p2p", new_sqlite_classes: ["P2pRoom"] };

/** A main Worker's config with P2P rooms: the P2P_ROOM binding and its migration. */
export function withP2p(config) {
  const c = structuredClone(config);
  c.durable_objects ??= { bindings: [] };
  c.durable_objects.bindings = [...(c.durable_objects.bindings ?? []).filter((b) => b.name !== "P2P_ROOM"),
    { name: "P2P_ROOM", class_name: "P2pRoom" }];
  c.migrations = [...(c.migrations ?? []).filter((m) => m.tag !== P2P_MIGRATION.tag), P2P_MIGRATION];
  return c;
}

/** Writes wrangler.selfhost.jsonc from wrangler.jsonc. An unchanged file is left alone: a running
    `wrangler dev` reloads when its config is written. Returns the file's name. */
export function writeSelfhostConfig() {
  const config = withP2p(JSON.parse(stripJsonc(readFileSync(path.join(SERVER, "wrangler.jsonc"), "utf8"))));
  const text = `// Written by tools/selfhost-config.mjs: wrangler.jsonc with P2P rooms (finding 180). Edit\n` +
    `// wrangler.jsonc, not this file. Not the official server's config (tools/deploy.mjs).\n${JSON.stringify(config, null, 2)}\n`;
  const file = path.join(SERVER, SELFHOST_CONFIG);
  if (!existsSync(file) || readFileSync(file, "utf8") !== text) {
    const temporary = `${file}.${process.pid}.tmp`;
    writeFileSync(temporary, text);
    renameSync(temporary, file);
  }
  return SELFHOST_CONFIG;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  console.log(writeSelfhostConfig());
}
